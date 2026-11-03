#include <arpa/inet.h>
#include <linux/perf_event.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "helpers/print.hpp"
#include "nat/nat_plugin.h"

namespace {

constexpr std::size_t kDefaultNumWorkers = 16;
constexpr std::size_t kSubscribersPerWorker = 20'000;
constexpr std::size_t kNewSubscribersPerWorker = 10'000;
constexpr std::size_t kPoolSizePerWorker = 30'000;

std::uint32_t ChurnPercent() {
  const char* env = std::getenv("NAT_BENCH_CHURN");
  if (env == nullptr) return 15;
  const unsigned long value = std::strtoul(env, nullptr, 10);
  return static_cast<std::uint32_t>(std::min<unsigned long>(value, 100));
}

std::size_t BatchSize() {
  const char* env = std::getenv("NAT_BENCH_BATCH");
  if (env == nullptr) return 1;
  const unsigned long value = std::strtoul(env, nullptr, 10);
  if (value < 1) return 1;
  return std::min<unsigned long>(value, 256);
}

bool EngineOnly() {
  const char* env = std::getenv("NAT_BENCH_ENGINE_ONLY");
  return env != nullptr && std::strtoul(env, nullptr, 10) != 0;
}

bool LazyUpdates() {
  const char* env = std::getenv("NAT_BENCH_LAZY");
  return env != nullptr && std::strtoul(env, nullptr, 10) != 0;
}

bool InlineValues() {
  const char* env = std::getenv("NAT_BENCH_INLINE");
  return env != nullptr && std::strtoul(env, nullptr, 10) != 0;
}

std::size_t HotCacheSize() {
  const char* env = std::getenv("NAT_BENCH_HOT");
  if (env == nullptr) return 0;
  return std::strtoul(env, nullptr, 10);
}

std::size_t PublicIpCount() {
  const char* env = std::getenv("NAT_BENCH_PUBLIC_COUNT");
  if (env == nullptr) return 512;
  const unsigned long value = std::strtoul(env, nullptr, 10);
  return value == 0 ? 512 : value;
}

bool ForceEvict() {
  const char* env = std::getenv("NAT_BENCH_FORCE_EVICT");
  if (env != nullptr) return std::strtoul(env, nullptr, 10) != 0;
  return ChurnPercent() > 0;
}

double TargetRatePerWorker(std::size_t workers) {
  const char* weak_env = std::getenv("NAT_BENCH_WEAK_RATE");
  if (weak_env != nullptr) {
    const double rate = std::strtod(weak_env, nullptr);
    if (rate > 0.0) return rate;
  }
  const char* env = std::getenv("NAT_BENCH_TARGET_RATE");
  if (env != nullptr) {
    const double total = std::strtod(env, nullptr);
    if (total > 0.0 && workers > 0) {
      return total / static_cast<double>(workers);
    }
  }
  return 0.0;
}

std::size_t PoolSizePerWorker(std::size_t workers) {
  const auto parse = [](const char* text) -> std::size_t {
    if (text == nullptr) return 0;
    char* end{nullptr};
    const unsigned long value = std::strtoul(text, &end, 10);
    return (end == text) ? 0 : static_cast<std::size_t>(value);
  };

  if (const std::size_t total = parse(std::getenv("NAT_BENCH_TOTAL_SESSIONS"));
      total != 0) {
    return std::max<std::size_t>(1, total / workers);
  }
  if (const std::size_t per_worker = parse(std::getenv("NAT_BENCH_POOL"));
      per_worker != 0) {
    return std::min<std::size_t>(per_worker, 2'500'000);
  }
  return kPoolSizePerWorker;
}
constexpr std::uint32_t kCapacityPerWorker = 2'500'000;
constexpr std::uint32_t kOutsideIp = 0xC6120002;  // 198.18.0.2

class PerfCounter {
 public:
  explicit PerfCounter(std::uint64_t config) noexcept { Open(config); }
  ~PerfCounter() { Close(); }
  PerfCounter(const PerfCounter&) = delete;
  PerfCounter& operator=(const PerfCounter&) = delete;

  void Enable() noexcept {
    if (fd_ >= 0) (void)::ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
  }
  void Disable() noexcept {
    if (fd_ >= 0) (void)::ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
  }
  [[nodiscard]] bool Available() const noexcept { return fd_ >= 0; }
  [[nodiscard]] std::uint64_t Read() noexcept {
    std::uint64_t value{0};
    if (fd_ >= 0 &&
        ::read(fd_, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) {
      value = 0;
    }
    return value;
  }

 private:
  void Open(std::uint64_t config) noexcept {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = config;
    attr.disabled = 1;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
  }
  void Close() noexcept {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  int fd_{-1};
};

[[nodiscard]] inline std::uint64_t ReadTsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}

template <std::size_t FrameSize>
struct alignas(64) PacketBuffer {
  static_assert(FrameSize >= sizeof(struct iphdr) + sizeof(struct tcphdr),
                "FrameSize must be at least 40 bytes");
  struct iphdr ip;
  struct tcphdr tcp;
  static constexpr std::size_t kPayloadBytes =
      FrameSize > (sizeof(struct iphdr) + sizeof(struct tcphdr))
          ? (FrameSize - sizeof(struct iphdr) - sizeof(struct tcphdr))
          : 0;
  std::uint8_t payload[kPayloadBytes > 0 ? kPayloadBytes : 1];
};

// Sum 16-bit words as they are stored in memory. Both the data plane and the
// verifier use this representation, so the folded value can be compared
// directly with the checksum field stored in the packet.
std::uint32_t SumWords(const void* data, std::size_t len, std::uint32_t sum = 0) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (len > 1) {
    std::uint16_t word{0};
    std::memcpy(&word, p, sizeof(word));
    sum += word;
    p += 2;
    len -= 2;
  }
  if (len != 0) {
    sum += *p;
  }
  return sum;
}

std::uint16_t FoldChecksum(std::uint32_t sum) {
  while ((sum >> 16) != 0) {
    sum = (sum & 0xFFFFU) + (sum >> 16);
  }
  const auto result = static_cast<std::uint16_t>(~sum & 0xFFFFU);
  return result == 0 ? 0xFFFF : result;
}

std::uint16_t CalculateIpChecksum(const struct iphdr& ip) {
  struct iphdr copy = ip;
  copy.check = 0;
  return FoldChecksum(SumWords(&copy, sizeof(copy)));
}

template <std::size_t FrameSize>
std::uint32_t PayloadSum() {
  static const std::uint32_t sum = []() {
    constexpr std::size_t kPayloadLen =
        FrameSize - sizeof(struct iphdr) - sizeof(struct tcphdr);
    if constexpr (kPayloadLen == 0) {
      return 0U;
    } else {
      std::array<std::uint8_t, kPayloadLen> payload{};
      for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i & 0xFFU);
      }
      return SumWords(payload.data(), payload.size());
    }
  }();
  return sum;
}

template <std::size_t FrameSize>
std::uint16_t CalculateTcpChecksum(const struct iphdr& ip, const struct tcphdr& tcp) {
  struct __attribute__((packed)) PseudoHeader {
    std::uint32_t src_ip;
    std::uint32_t dst_ip;
    std::uint8_t zero;
    std::uint8_t protocol;
    std::uint16_t tcp_len;
  } pseudo{};

  pseudo.src_ip = ip.saddr;
  pseudo.dst_ip = ip.daddr;
  pseudo.zero = 0;
  pseudo.protocol = IPPROTO_TCP;
  constexpr std::size_t kPayloadLen = FrameSize - sizeof(struct iphdr) - sizeof(struct tcphdr);
  pseudo.tcp_len = htons(static_cast<std::uint16_t>(sizeof(struct tcphdr) + kPayloadLen));

  struct tcphdr tcp_copy = tcp;
  tcp_copy.check = 0;

  std::uint32_t sum = SumWords(&pseudo, sizeof(pseudo));
  sum = SumWords(&tcp_copy, sizeof(tcp_copy), sum);
  sum += PayloadSum<FrameSize>();
  return FoldChecksum(sum);
}

template <std::size_t FrameSize>
void InitializePacket(PacketBuffer<FrameSize>& pkt, std::uint32_t src_ip, std::uint32_t dst_ip,
                      std::uint16_t src_port, std::uint16_t dst_port) {
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.ip.version = 4;
  pkt.ip.ihl = 5;
  pkt.ip.tos = 0;
  pkt.ip.tot_len = htons(static_cast<std::uint16_t>(FrameSize));
  pkt.ip.id = htons(1234);
  pkt.ip.frag_off = 0;
  pkt.ip.ttl = 64;
  pkt.ip.protocol = IPPROTO_TCP;
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(dst_ip);
  pkt.ip.check = 0;
  pkt.ip.check = CalculateIpChecksum(pkt.ip);

  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(dst_port);
  pkt.tcp.seq = htonl(100000);
  pkt.tcp.ack_seq = 0;
  pkt.tcp.doff = 5;
  pkt.tcp.syn = 1;
  pkt.tcp.window = htons(65535);
  pkt.tcp.check = 0;
  pkt.tcp.urg_ptr = 0;

  for (std::size_t i = 0; i < sizeof(pkt.payload); ++i) {
    pkt.payload[i] = static_cast<std::uint8_t>(i & 0xFFU);
  }

  pkt.tcp.check = CalculateTcpChecksum<FrameSize>(pkt.ip, pkt.tcp);
}

template <std::size_t FrameSize>
void RebuildUplink(PacketBuffer<FrameSize>& pkt, std::uint32_t src_ip, std::uint16_t src_port) {
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(kOutsideIp);
  pkt.ip.check = 0;
  pkt.ip.check = CalculateIpChecksum(pkt.ip);

  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(80);
  pkt.tcp.check = 0;
  pkt.tcp.check = CalculateTcpChecksum<FrameSize>(pkt.ip, pkt.tcp);
}

template <std::size_t FrameSize>
void RebuildUplinkNoChecksum(PacketBuffer<FrameSize>& pkt, std::uint32_t src_ip,
                             std::uint16_t src_port) {
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(kOutsideIp);
  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(80);
}

template <std::size_t FrameSize>
void RebuildDownlink(PacketBuffer<FrameSize>& pkt, std::uint32_t public_ip,
                     std::uint16_t public_port) {
  pkt.ip.saddr = htonl(kOutsideIp);
  pkt.ip.daddr = htonl(public_ip);
  pkt.ip.check = 0;
  pkt.ip.check = CalculateIpChecksum(pkt.ip);

  pkt.tcp.source = htons(80);
  pkt.tcp.dest = htons(public_port);
  pkt.tcp.check = 0;
  pkt.tcp.check = CalculateTcpChecksum<FrameSize>(pkt.ip, pkt.tcp);
}

template <std::size_t FrameSize>
void RebuildDownlinkNoChecksum(PacketBuffer<FrameSize>& pkt, std::uint32_t public_ip,
                               std::uint16_t public_port) {
  pkt.ip.saddr = htonl(kOutsideIp);
  pkt.ip.daddr = htonl(public_ip);
  pkt.tcp.source = htons(80);
  pkt.tcp.dest = htons(public_port);
}

template <std::size_t FrameSize>
bool VerifyChecksums(PacketBuffer<FrameSize>& pkt) {
  const std::uint16_t actual_ip = pkt.ip.check;
  const std::uint16_t actual_tcp = pkt.tcp.check;

  pkt.ip.check = 0;
  const std::uint16_t expected_ip = CalculateIpChecksum(pkt.ip);
  pkt.tcp.check = 0;
  const std::uint16_t expected_tcp = CalculateTcpChecksum<FrameSize>(pkt.ip, pkt.tcp);

  pkt.ip.check = actual_ip;
  pkt.tcp.check = actual_tcp;
  return actual_ip == expected_ip && actual_tcp == expected_tcp;
}

void PinThread(std::size_t worker_idx, std::size_t total_workers = 16) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  int cpu_id = 0;
  if (total_workers <= 8) {
    cpu_id = static_cast<int>(worker_idx * 2);
  } else {
    if (worker_idx < 8) {
      cpu_id = static_cast<int>(worker_idx * 2);
    } else {
      cpu_id = static_cast<int>((worker_idx - 8) * 2 + 1);
    }
  }
  CPU_SET(cpu_id, &cpuset);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  (void)setpriority(PRIO_PROCESS, 0, -20);
}

enum class TrafficDirection {
  kUplink,
  kDownlink,
};

const char* DirectionName(TrafficDirection dir) {
  return dir == TrafficDirection::kUplink ? "UPLINK (In2Out)" : "DOWNLINK (Out2In)";
}

struct FlowInfo {
  std::uint32_t inside_ip{0};
  std::uint16_t inside_port{0};
  std::uint32_t public_ip{0};
  std::uint16_t public_port{0};
};

struct BatchItem {
  bool is_new{false};
  bool sample{false};
  std::size_t pool_idx{0};
  std::uint32_t src_ip{0};
  std::uint16_t user_port{0};
  std::chrono::steady_clock::time_point sample_start{};
};

struct BenchmarkResult {
  std::size_t frame_size{0};
  TrafficDirection direction{TrafficDirection::kUplink};
  std::size_t num_workers{16};
  double elapsed_sec{0.0};
  std::uint64_t total_attempts{0};
  std::uint64_t new_attempts{0};
  std::uint64_t new_success{0};
  std::uint64_t failures{0};
  std::uint64_t hits{0};
  std::uint64_t evictions{0};
  std::uint64_t peak_sessions{0};
  std::uint64_t in_ram_sessions{0};
  double ram_footprint_mb{0.0};
  std::string working_set_tier{};
  std::uint64_t slow_path_packets{0};
  std::uint64_t pba_blocks_allocated{0};
  std::uint64_t pba_blocks_freed{0};
  std::uint64_t pba_failures{0};
  double mpps_attempted{0.0};
  double mpps_success{0.0};
  double total_sessions_handled_rate{0.0};
  double cps_success{0.0};
  double established_session_rate{0.0};
  double churn_attempt_pct{0.0};
  double churn_success_pct{0.0};
  double p50_new_ns{0.0};
  double p99_new_ns{0.0};
  double p50_old_ns{0.0};
  double p99_old_ns{0.0};
  double l3_gbps{0.0};
  double wire_gbps{0.0};
  std::uint64_t csum_checks{0};
  std::uint64_t csum_errors{0};
  double cycles_per_packet{0.0};
  double instructions_per_packet{0.0};
  double cache_misses_per_packet{0.0};
  double cache_miss_pct{0.0};
  double average_core_ghz{0.0};
  double worker_rate_min_mpps{0.0};
  double worker_rate_max_mpps{0.0};
  double worker_rate_cv_pct{0.0};
  bool perf_available{false};
};

double Percentile(std::vector<double> samples, double fraction) {
  if (samples.empty()) return 0.0;
  std::ranges::sort(samples);
  const auto idx = static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1));
  return samples[std::min(idx, samples.size() - 1)];
}

template <std::size_t FrameSize, TrafficDirection Dir>
BenchmarkResult RunChurnBenchmark(std::uint32_t duration_sec, std::size_t num_workers = 16) {
  nat::io::println("================================================================================");
  const std::uint32_t churn = ChurnPercent();
  const std::size_t batch_size = BatchSize();
  const bool engine_only = EngineOnly();
  const bool lazy_updates = LazyUpdates();
  const bool inline_values = InlineValues();
  const std::size_t hot_cache = HotCacheSize();
  const std::size_t public_count = PublicIpCount();
  nat::io::println(" NAT44 BENCHMARK: {} B, {}, {} workers, {} s, {}% churn, batch {}, "
                   "engine_only={}, lazy={}, inline={}, hot={}, public_ips={}",
                   FrameSize, DirectionName(Dir), num_workers, duration_sec, churn, batch_size,
                   engine_only, lazy_updates, inline_values, hot_cache, public_count);
  const double target_rate_worker = TargetRatePerWorker(num_workers);
  if (target_rate_worker > 0.0) {
    nat::io::println(" RATE PACING: Target {:.2f} Mpps total ({:.2f} Mpps/worker, {:.2f} ns/pkt)",
                     target_rate_worker * static_cast<double>(num_workers) / 1e6,
                     target_rate_worker / 1e6, 1e9 / target_rate_worker);
  }
  nat::io::println("================================================================================");

  const std::size_t pool_size = PoolSizePerWorker(num_workers);
  std::size_t engine_capacity = kCapacityPerWorker;
  if (const char* capacity_env = std::getenv("NAT_BENCH_CAPACITY");
      capacity_env != nullptr) {
    const std::size_t requested = static_cast<std::size_t>(std::strtoul(capacity_env, nullptr, 10));
    if (requested >= pool_size) {
      engine_capacity = std::min<std::size_t>(requested, kCapacityPerWorker);
    }
  }

  std::size_t max_sessions = pool_size;
  if (const char* max_env = std::getenv("NAT_BENCH_MAX_SESSIONS");
      max_env != nullptr) {
    const std::size_t requested = static_cast<std::size_t>(std::strtoul(max_env, nullptr, 10));
    max_sessions = (requested == 0) ? 0 : requested;
  }
  const std::size_t expected_active =
      max_sessions != 0 ? max_sessions : engine_capacity;

  nat_engine_t* engine = nat_engine_create_with_capacity_and_active(
      static_cast<std::uint32_t>(num_workers), static_cast<std::uint32_t>(engine_capacity),
      static_cast<std::uint32_t>(expected_active));
  if (engine == nullptr) {
    nat::io::println(stderr, "[-] Failed to create NAT engine");
    return {};
  }
  nat_engine_set_max_sessions_per_worker(engine, static_cast<std::uint32_t>(max_sessions));
  nat_engine_set_force_eviction(engine, ForceEvict());
  nat_engine_set_lazy_updates(engine, lazy_updates);
  nat_engine_set_inline_values(engine, inline_values);
  nat_engine_set_hot_cache(engine, static_cast<std::uint32_t>(hot_cache));
  nat_engine_set_pool(engine, 0xCB007101,
                      static_cast<std::uint32_t>(0xCB007101 + public_count - 1));

  nat::io::println("[*] Session pool: {} per worker ({} total), engine capacity {} per worker, "
                   "max sessions {} per worker, churn {}%",
                   pool_size, pool_size * num_workers, engine_capacity, max_sessions, churn);

  std::vector<std::vector<FlowInfo>> flows(
      num_workers, std::vector<FlowInfo>(pool_size));
  std::vector<std::thread> setup_threads;
  setup_threads.reserve(num_workers);
  for (std::size_t w = 0; w < num_workers; ++w) {
    setup_threads.emplace_back([engine, w, num_workers, &flows, pool_size]() {
      PinThread(w, num_workers);
      const std::uint32_t net_prefix =
          (10U << 24) | (static_cast<std::uint32_t>(w + 1) << 16);
      PacketBuffer<FrameSize> pkt;
      for (std::size_t i = 0; i < pool_size; ++i) {
        const std::uint32_t user_idx = static_cast<std::uint32_t>(i % kSubscribersPerWorker);
        const std::uint16_t user_port =
            static_cast<std::uint16_t>(1024 + (i / kSubscribersPerWorker));
        const std::uint32_t src_ip = net_prefix | user_idx;
        InitializePacket(pkt, src_ip, kOutsideIp, user_port, 80);
        const auto act = nat_translate_in2out(engine, static_cast<std::uint32_t>(w), &pkt.ip,
                                              &pkt.tcp, IPPROTO_TCP);
        if (act == NAT_TRANSLATE_OK) {
          flows[w][i] = FlowInfo{src_ip, user_port, ntohl(pkt.ip.saddr), ntohs(pkt.tcp.source)};
        } else {
          flows[w][i] = FlowInfo{src_ip, user_port, 0, 0};
        }
      }
    });
  }
  for (auto& thread : setup_threads) thread.join();

  const nat_telemetry_t tel_before = nat_engine_get_telemetry(engine);
  nat::io::println("[*] Prewarmed sessions: {}", nat_engine_get_total_sessions(engine));
  nat::io::println("[*] Pre-benchmark telemetry: created={}, failures={}, pba_failures={}",
                   tel_before.new_sessions_created, tel_before.create_failures,
                   tel_before.pba_port_alloc_failures);

  std::vector<std::uint64_t> thread_total(num_workers, 0);
  std::vector<std::uint64_t> thread_new(num_workers, 0);
  std::vector<std::uint64_t> thread_old(num_workers, 0);
  std::vector<std::uint64_t> thread_csum_checks(num_workers, 0);
  std::vector<std::uint64_t> thread_csum_errors(num_workers, 0);
  std::vector<std::uint64_t> thread_cycles(num_workers, 0);
  std::vector<std::uint64_t> thread_instructions(num_workers, 0);
  std::vector<std::uint64_t> thread_cache_misses(num_workers, 0);
  std::vector<std::uint64_t> thread_cache_refs(num_workers, 0);
  std::vector<std::uint8_t> thread_perf_available(num_workers, 0);
  std::vector<std::vector<double>> new_latencies(num_workers);
  std::vector<std::vector<double>> old_latencies(num_workers);
  for (auto& samples : new_latencies) samples.reserve(4096);
  for (auto& samples : old_latencies) samples.reserve(4096);

  const double ns_per_packet =
      target_rate_worker > 0.0 ? (1e9 / target_rate_worker) : 0.0;
  std::atomic<bool> start_gate{false};
  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(num_workers);
  for (std::size_t w = 0; w < num_workers; ++w) {
    workers.emplace_back([&, w]() {
      PinThread(w, num_workers);
      const std::uint32_t net_prefix =
          (10U << 24) | (static_cast<std::uint32_t>(w + 1) << 16);
      const std::uint32_t thread_index = static_cast<std::uint32_t>(w);

      std::uint64_t total_count = 0;
      std::uint64_t new_count = 0;
      std::uint64_t old_count = 0;
      std::uint64_t csum_checks = 0;
      std::uint64_t csum_errors = 0;
      std::uint32_t churn_counter = 0;

      PacketBuffer<FrameSize> packet;
      PacketBuffer<FrameSize> setup_packet;
      InitializePacket(packet, net_prefix | 1, kOutsideIp, 1024, 80);
      if constexpr (Dir == TrafficDirection::kDownlink) {
        InitializePacket(setup_packet, net_prefix | 1, kOutsideIp, 1024, 80);
      }

      PerfCounter cycle_counter(PERF_COUNT_HW_CPU_CYCLES);
      PerfCounter instruction_counter(PERF_COUNT_HW_INSTRUCTIONS);
      PerfCounter cache_miss_counter(PERF_COUNT_HW_CACHE_MISSES);
      PerfCounter cache_ref_counter(PERF_COUNT_HW_CACHE_REFERENCES);

      while (!start_gate.load(std::memory_order_acquire)) {
#if defined(__x86_64__) || defined(_M_X64)
        _mm_pause();
#endif
      }

      const auto deadline = start + std::chrono::seconds(duration_sec);
      const std::uint64_t tsc_start = ReadTsc();
      cycle_counter.Enable();
      instruction_counter.Enable();
      cache_miss_counter.Enable();
      cache_ref_counter.Enable();

      if (batch_size > 1 && Dir == TrafficDirection::kUplink) {
        std::array<PacketBuffer<FrameSize>, 256> batch_packets{};
        std::array<BatchItem, 256> batch_items{};
        std::array<void*, 256> batch_ips{};
        std::array<void*, 256> batch_l4s{};
        std::array<std::uint8_t, 256> batch_protos{};
        std::array<nat_action_t, 256> batch_actions{};
        for (std::size_t i = 0; i < batch_packets.size(); ++i) {
          InitializePacket(batch_packets[i], net_prefix | 1, kOutsideIp, 1024, 80);
        }

        while (true) {
          if ((total_count & 0x7F) == 0 && std::chrono::steady_clock::now() >= deadline) {
            break;
          }

          if (ns_per_packet > 0.0) {
            const auto target_time =
                start + std::chrono::nanoseconds(
                            static_cast<std::int64_t>(total_count * ns_per_packet));
            while (std::chrono::steady_clock::now() < target_time) {
#if defined(__x86_64__) || defined(_M_X64)
              _mm_pause();
#endif
            }
          }

          std::size_t batch_count = 0;
          while (batch_count < batch_size) {
            const bool is_new = churn_counter < churn;
            if (++churn_counter == 100) churn_counter = 0;

            if (is_new) {
              ++new_count;
              const std::size_t pool_idx = new_count % pool_size;
              const std::uint32_t user_idx =
                  static_cast<std::uint32_t>(new_count % kNewSubscribersPerWorker);
              const std::uint16_t user_port = static_cast<std::uint16_t>(
                  2000 + ((new_count / kNewSubscribersPerWorker) % 60000));
              const std::uint32_t src_ip = net_prefix | user_idx;
              const bool sample = (new_count & 0x1FFU) == 0;

              auto& pkt = batch_packets[batch_count];
              if (engine_only) {
                RebuildUplinkNoChecksum(pkt, src_ip, user_port);
              } else {
                RebuildUplink(pkt, src_ip, user_port);
              }
              batch_items[batch_count] = BatchItem{
                  .is_new = true,
                  .sample = sample,
                  .pool_idx = pool_idx,
                  .src_ip = src_ip,
                  .user_port = user_port,
                  .sample_start = sample ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{},
              };
              batch_ips[batch_count] = &pkt.ip;
              batch_l4s[batch_count] = &pkt.tcp;
              batch_protos[batch_count] = IPPROTO_TCP;
              batch_actions[batch_count] = NAT_TRANSLATE_DROP;
              ++batch_count;
              continue;
            }

            ++old_count;
            const std::size_t pool_idx = (total_count + old_count) % pool_size;
            const FlowInfo& flow = flows[w][pool_idx];
            if (flow.public_ip == 0) {
              ++total_count;
              continue;
            }

            const bool sample = (old_count & 0x7FFU) == 0;
            auto& pkt = batch_packets[batch_count];
            if (engine_only) {
              RebuildUplinkNoChecksum(pkt, flow.inside_ip, flow.inside_port);
            } else {
              RebuildUplink(pkt, flow.inside_ip, flow.inside_port);
            }
            batch_items[batch_count] = BatchItem{
                .is_new = false,
                .sample = sample,
                .pool_idx = pool_idx,
                .sample_start = sample ? std::chrono::steady_clock::now()
                                       : std::chrono::steady_clock::time_point{},
            };
            batch_ips[batch_count] = &pkt.ip;
            batch_l4s[batch_count] = &pkt.tcp;
            batch_protos[batch_count] = IPPROTO_TCP;
            batch_actions[batch_count] = NAT_TRANSLATE_DROP;
            ++batch_count;
          }

          if (batch_count == 0) break;
          nat_translate_in2out_batch(engine, thread_index, batch_ips.data(),
                                     batch_l4s.data(), batch_protos.data(),
                                     batch_actions.data(),
                                     static_cast<std::uint32_t>(batch_count));

          for (std::size_t i = 0; i < batch_count; ++i) {
            ++total_count;
            const BatchItem& item = batch_items[i];
            const nat_action_t action = batch_actions[i];
            if (action == NAT_TRANSLATE_OK) {
              if (item.is_new) {
                flows[w][item.pool_idx] =
                    FlowInfo{item.src_ip, item.user_port,
                             ntohl(batch_packets[i].ip.saddr),
                             ntohs(batch_packets[i].tcp.source)};
              }
              if (item.sample) {
                const auto end = std::chrono::steady_clock::now();
                const double latency_ns =
                    std::chrono::duration<double, std::nano>(end - item.sample_start).count();
                if (item.is_new) {
                  new_latencies[w].push_back(latency_ns);
                } else {
                  old_latencies[w].push_back(latency_ns);
                }
                ++csum_checks;
                if (!engine_only && !VerifyChecksums(batch_packets[i])) ++csum_errors;
              }
            }
          }
        }
      } else {
        while (true) {
        if ((total_count & 0x7F) == 0 && std::chrono::steady_clock::now() >= deadline) {
          break;
        }

        if (ns_per_packet > 0.0 && (total_count & 0x1FU) == 0) {
          const auto target_time =
              start + std::chrono::nanoseconds(
                          static_cast<std::int64_t>(total_count * ns_per_packet));
          while (std::chrono::steady_clock::now() < target_time) {
#if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
#endif
          }
        }

        const bool is_new = churn_counter < churn;
        if (++churn_counter == 100) churn_counter = 0;

        if (is_new) {
          ++new_count;
          const std::size_t pool_idx = new_count % pool_size;
          const std::uint32_t user_idx =
              static_cast<std::uint32_t>(new_count % kNewSubscribersPerWorker);
          const std::uint16_t user_port =
              static_cast<std::uint16_t>(2000 + ((new_count / kNewSubscribersPerWorker) % 60000));
          const std::uint32_t src_ip = net_prefix | user_idx;

          const bool sample = (new_count & 0x1FFU) == 0;
          const auto sample_start = sample ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};

          nat_action_t action = NAT_TRANSLATE_DROP;
          FlowInfo flow{src_ip, user_port, 0, 0};
          if constexpr (Dir == TrafficDirection::kUplink) {
            if (engine_only) {
              RebuildUplinkNoChecksum(packet, src_ip, user_port);
            } else {
              RebuildUplink(packet, src_ip, user_port);
            }
            action =
                nat_translate_in2out(engine, thread_index, &packet.ip, &packet.tcp, IPPROTO_TCP);
            if (action == NAT_TRANSLATE_OK) {
              flow = FlowInfo{src_ip, user_port, ntohl(packet.ip.saddr),
                              ntohs(packet.tcp.source)};
            }
          } else {
            if (engine_only) {
              RebuildUplinkNoChecksum(setup_packet, src_ip, user_port);
            } else {
              RebuildUplink(setup_packet, src_ip, user_port);
            }
            const auto setup_action = nat_translate_in2out(
                engine, thread_index, &setup_packet.ip, &setup_packet.tcp, IPPROTO_TCP);
            if (setup_action == NAT_TRANSLATE_OK) {
              flow = FlowInfo{src_ip, user_port, ntohl(setup_packet.ip.saddr),
                              ntohs(setup_packet.tcp.source)};
              if (engine_only) {
                RebuildDownlinkNoChecksum(packet, flow.public_ip, flow.public_port);
              } else {
                RebuildDownlink(packet, flow.public_ip, flow.public_port);
              }
              action =
                  nat_translate_out2in(engine, thread_index, &packet.ip, &packet.tcp, IPPROTO_TCP);
            }
          }

          if (sample) {
            const auto end = std::chrono::steady_clock::now();
            new_latencies[w].push_back(
                std::chrono::duration<double, std::nano>(end - sample_start).count());
          }

          if (flow.public_ip != 0) {
            flows[w][pool_idx] = flow;
          }
          if (action == NAT_TRANSLATE_OK && sample) {
            ++csum_checks;
            if (!engine_only && !VerifyChecksums(packet)) ++csum_errors;
          }
          ++total_count;
          continue;
        }

        ++old_count;
        const std::size_t pool_idx = (total_count + old_count) % pool_size;
        const FlowInfo& flow = flows[w][pool_idx];
        if (flow.public_ip == 0) {
          ++total_count;
          continue;
        }

        const bool sample = (old_count & 0x7FFU) == 0;
        const auto sample_start = sample ? std::chrono::steady_clock::now()
                                         : std::chrono::steady_clock::time_point{};

        nat_action_t action = NAT_TRANSLATE_DROP;
        if constexpr (Dir == TrafficDirection::kUplink) {
          if (engine_only) {
            RebuildUplinkNoChecksum(packet, flow.inside_ip, flow.inside_port);
          } else {
            RebuildUplink(packet, flow.inside_ip, flow.inside_port);
          }
          action =
              nat_translate_in2out(engine, thread_index, &packet.ip, &packet.tcp, IPPROTO_TCP);
        } else {
          if (engine_only) {
            RebuildDownlinkNoChecksum(packet, flow.public_ip, flow.public_port);
          } else {
            RebuildDownlink(packet, flow.public_ip, flow.public_port);
          }
          action =
              nat_translate_out2in(engine, thread_index, &packet.ip, &packet.tcp, IPPROTO_TCP);
        }

        if (sample) {
          const auto end = std::chrono::steady_clock::now();
          old_latencies[w].push_back(
              std::chrono::duration<double, std::nano>(end - sample_start).count());
          if (action == NAT_TRANSLATE_OK) {
            ++csum_checks;
            if (!engine_only && !VerifyChecksums(packet)) ++csum_errors;
          }
        }
        ++total_count;
        }
      }

      cycle_counter.Disable();
      instruction_counter.Disable();
      cache_miss_counter.Disable();
      cache_ref_counter.Disable();
      thread_cycles[w] = cycle_counter.Available() ? cycle_counter.Read()
                                                   : (ReadTsc() - tsc_start);
      thread_instructions[w] = instruction_counter.Read();
      thread_cache_misses[w] = cache_miss_counter.Read();
      thread_cache_refs[w] = cache_ref_counter.Read();
      thread_perf_available[w] = static_cast<std::uint8_t>(
          cycle_counter.Available() || instruction_counter.Available() ||
          cache_miss_counter.Available());

      thread_total[w] = total_count;
      thread_new[w] = new_count;
      thread_old[w] = old_count;
      thread_csum_checks[w] = csum_checks;
      thread_csum_errors[w] = csum_errors;
    });
  }
  start = std::chrono::steady_clock::now();
  start_gate.store(true, std::memory_order_release);
  for (auto& thread : workers) thread.join();
  const auto end = std::chrono::steady_clock::now();

  const double elapsed_sec =
      std::chrono::duration<double>(end - start).count();
  const std::uint64_t total_attempts =
      std::accumulate(thread_total.begin(), thread_total.end(), std::uint64_t{0});
  const std::uint64_t new_attempts =
      std::accumulate(thread_new.begin(), thread_new.end(), std::uint64_t{0});
  const std::uint64_t old_attempts =
      std::accumulate(thread_old.begin(), thread_old.end(), std::uint64_t{0});
  const std::uint64_t csum_checks =
      std::accumulate(thread_csum_checks.begin(), thread_csum_checks.end(), std::uint64_t{0});
  const std::uint64_t csum_errors =
      std::accumulate(thread_csum_errors.begin(), thread_csum_errors.end(), std::uint64_t{0});

  std::vector<double> all_new_latencies;
  std::vector<double> all_old_latencies;
  for (const auto& samples : new_latencies) {
    all_new_latencies.insert(all_new_latencies.end(), samples.begin(), samples.end());
  }
  for (const auto& samples : old_latencies) {
    all_old_latencies.insert(all_old_latencies.end(), samples.begin(), samples.end());
  }

  const nat_telemetry_t tel_after = nat_engine_get_telemetry(engine);
  const std::uint64_t new_success =
      tel_after.new_sessions_created - tel_before.new_sessions_created;
  const std::uint64_t failures = tel_after.create_failures - tel_before.create_failures;
  const std::uint64_t hits = tel_after.existing_session_hits - tel_before.existing_session_hits;
  const std::uint64_t fast_hits =
      tel_after.fast_path_hits - tel_before.fast_path_hits;
  const std::uint64_t hot_hits =
      tel_after.hot_cache_hits - tel_before.hot_cache_hits;
  const std::uint64_t evictions = tel_after.lru_evictions - tel_before.lru_evictions;
  const std::uint64_t pba_alloc =
      tel_after.pba_blocks_allocated - tel_before.pba_blocks_allocated;
  const std::uint64_t pba_freed =
      tel_after.pba_blocks_freed - tel_before.pba_blocks_freed;
  const std::uint64_t pba_failures =
      tel_after.pba_port_alloc_failures - tel_before.pba_port_alloc_failures;

  if (new_success + failures != new_attempts) {
    nat::io::println(stderr,
                     "[!] Accounting warning: new_attempts={} success={} failures={}",
                     new_attempts, new_success, failures);
  }

  const std::uint64_t successful_packets =
      total_attempts >= failures ? total_attempts - failures : 0;

  const std::uint64_t total_cycles =
      std::accumulate(thread_cycles.begin(), thread_cycles.end(), std::uint64_t{0});
  const std::uint64_t total_instructions =
      std::accumulate(thread_instructions.begin(), thread_instructions.end(), std::uint64_t{0});
  const std::uint64_t total_cache_misses =
      std::accumulate(thread_cache_misses.begin(), thread_cache_misses.end(), std::uint64_t{0});
  const std::uint64_t total_cache_refs =
      std::accumulate(thread_cache_refs.begin(), thread_cache_refs.end(), std::uint64_t{0});
  const bool perf_available =
      std::any_of(thread_perf_available.begin(), thread_perf_available.end(),
                  [](std::uint8_t value) { return value != 0; });
  const std::uint64_t perf_denominator = successful_packets > 0 ? successful_packets : 1;

  std::vector<double> worker_rates(num_workers, 0.0);
  for (std::size_t w = 0; w < num_workers; ++w) {
    worker_rates[w] = static_cast<double>(thread_total[w]) / elapsed_sec / 1e6;
  }
  const double rate_mean =
      std::accumulate(worker_rates.begin(), worker_rates.end(), 0.0) /
      static_cast<double>(num_workers);
  double rate_variance = 0.0;
  for (const double rate : worker_rates) {
    rate_variance += (rate - rate_mean) * (rate - rate_mean);
  }
  rate_variance /= static_cast<double>(num_workers);
  const double rate_stddev = std::sqrt(rate_variance);
  const auto [rate_min_it, rate_max_it] =
      std::minmax_element(worker_rates.begin(), worker_rates.end());

  const double mpps_attempted =
      static_cast<double>(total_attempts) / elapsed_sec / 1e6;
  const double mpps_success =
      static_cast<double>(successful_packets) / elapsed_sec / 1e6;
  const double total_sessions_handled_rate =
      static_cast<double>(new_success + hits) / elapsed_sec;
  const double cps_success =
      static_cast<double>(new_success) / elapsed_sec / 1e6;
  const double established_session_rate =
      static_cast<double>(hits) / elapsed_sec;
  const double churn_attempt_pct =
      total_attempts == 0 ? 0.0
                          : 100.0 * static_cast<double>(new_attempts) /
                                static_cast<double>(total_attempts);
  const double churn_success_pct =
      total_attempts == 0 ? 0.0
                          : 100.0 * static_cast<double>(new_success) /
                                static_cast<double>(total_attempts);

  const std::uint64_t in_ram_sessions = nat_engine_get_total_sessions(engine);
  constexpr std::size_t kBytesPerSessionInRam = 144;
  const double ram_footprint_mb =
      static_cast<double>(in_ram_sessions * kBytesPerSessionInRam) / (1024.0 * 1024.0);
  const double per_worker_ram_kb =
      num_workers > 0 ? (ram_footprint_mb * 1024.0) / static_cast<double>(num_workers) : 0.0;

  std::string cache_tier_str;
  if (per_worker_ram_kb <= 512.0) {
    cache_tier_str = "L2 Cache Resident (<= 512 KB/core, 0 DRAM traffic)";
  } else if (ram_footprint_mb <= 16.0) {
    cache_tier_str = "L3 Cache Resident (<= 16 MB shared LLC)";
  } else {
    const double excess_ratio = ram_footprint_mb / 16.0;
    cache_tier_str = std::format("DRAM Resident / LLC Misses Bound ({:.1f} MB exceeds 16 MB L3 by {:.1f}x)",
                                 ram_footprint_mb, excess_ratio);
  }

  BenchmarkResult result{};
  result.frame_size = FrameSize;
  result.direction = Dir;
  result.num_workers = num_workers;
  result.elapsed_sec = elapsed_sec;
  result.total_attempts = total_attempts;
  result.new_attempts = new_attempts;
  result.new_success = new_success;
  result.failures = failures;
  result.hits = hits;
  result.evictions = evictions;
  result.peak_sessions = tel_after.active_sessions_high_watermark;
  result.in_ram_sessions = in_ram_sessions;
  result.ram_footprint_mb = ram_footprint_mb;
  result.working_set_tier = cache_tier_str;
  result.slow_path_packets = tel_after.slow_path_packets - tel_before.slow_path_packets;
  result.pba_blocks_allocated = pba_alloc;
  result.pba_blocks_freed = pba_freed;
  result.pba_failures = pba_failures;
  result.mpps_attempted = mpps_attempted;
  result.mpps_success = mpps_success;
  result.total_sessions_handled_rate = total_sessions_handled_rate;
  result.cps_success = cps_success;
  result.established_session_rate = established_session_rate;
  result.churn_attempt_pct = churn_attempt_pct;
  result.churn_success_pct = churn_success_pct;
  result.p50_new_ns = Percentile(all_new_latencies, 0.50);
  result.p99_new_ns = Percentile(all_new_latencies, 0.99);
  result.p50_old_ns = Percentile(all_old_latencies, 0.50);
  result.p99_old_ns = Percentile(all_old_latencies, 0.99);
  result.l3_gbps = mpps_attempted * static_cast<double>(FrameSize) * 8.0 / 1000.0;
  result.wire_gbps =
      mpps_attempted * static_cast<double>(FrameSize + 20) * 8.0 / 1000.0;
  result.csum_checks = csum_checks;
  result.csum_errors = csum_errors;
  result.cycles_per_packet =
      static_cast<double>(total_cycles) / static_cast<double>(perf_denominator);
  result.instructions_per_packet =
      perf_available
          ? static_cast<double>(total_instructions) / static_cast<double>(perf_denominator)
          : 0.0;
  result.cache_misses_per_packet =
      perf_available
          ? static_cast<double>(total_cache_misses) / static_cast<double>(perf_denominator)
          : 0.0;
  result.cache_miss_pct =
      total_cache_refs > 0
          ? 100.0 * static_cast<double>(total_cache_misses) /
                static_cast<double>(total_cache_refs)
          : 0.0;
  result.average_core_ghz =
      perf_available && num_workers > 0
          ? static_cast<double>(total_cycles) /
                (elapsed_sec * static_cast<double>(num_workers) * 1e9)
          : 0.0;
  result.worker_rate_min_mpps = (rate_min_it != worker_rates.end()) ? *rate_min_it : 0.0;
  result.worker_rate_max_mpps = (rate_max_it != worker_rates.end()) ? *rate_max_it : 0.0;
  result.worker_rate_cv_pct =
      rate_mean > 0.0 ? (rate_stddev / rate_mean) * 100.0 : 0.0;
  result.perf_available = perf_available;

  nat::io::println("--------------------------------------------------------------------------------");
  nat::io::println("Elapsed: {:.4f} s", elapsed_sec);
  nat::io::println("Attempted packets: {}  ({} new attempts + {} hits)",
                   total_attempts, new_attempts, old_attempts);
  nat::io::println("Successful packets: {}  Failures: {}", successful_packets, failures);
  nat::io::println("Packet forwarding attempted: {:.2f} Mpps", mpps_attempted);
  nat::io::println("Packet forwarding successful: {:.2f} Mpps", mpps_success);
  if (perf_available) {
    nat::io::println("Cycles/packet (HW): {:.2f}  Instructions/packet (HW): {:.2f}",
                     result.cycles_per_packet, result.instructions_per_packet);
    nat::io::println(
        "Avg core frequency: {:.2f} GHz  Cache misses/packet: {:.2f} ({:.2f}% of refs)",
        result.average_core_ghz, result.cache_misses_per_packet,
        result.cache_miss_pct);
  } else {
    nat::io::println("Cycles/packet (TSC, nominal): {:.2f}", result.cycles_per_packet);
    nat::io::println("Instructions/packet: n/a (perf_event_open denied; use `perf stat`)");
  }
  nat::io::println(
      "Per-worker packet rate: mean {:.2f} Mpps, min {:.2f}, max {:.2f}, CV {:.1f}%",
      rate_mean, result.worker_rate_min_mpps, result.worker_rate_max_mpps,
      result.worker_rate_cv_pct);
  nat::io::println("--------------------------------------------------------------------------------");
  nat::io::println("💾 IN-MEMORY WORKING SET & ACTIVE SESSIONS IN RAM:");
  nat::io::println("   ├─ Active Sessions in RAM: {:L} sessions ({:.2f} MB footprint in RAM)",
                   in_ram_sessions, ram_footprint_mb);
  nat::io::println("   ├─ Average per worker: {:L} sessions/worker ({:.2f} KB/worker)",
                   num_workers > 0 ? in_ram_sessions / num_workers : 0, per_worker_ram_kb);
  nat::io::println("   ├─ Peak Active Sessions (HWM): {:L} sessions",
                   result.peak_sessions);
  nat::io::println("   └─ Cache Placement Status: {}", cache_tier_str);
  nat::io::println("⚡ SESSIONS HANDLED & FORWARDED (BREAKDOWN):");
  nat::io::println("   ├─ Total Sessions Processed / sec: {:L} session/s ({:.2f} M sessions/s)",
                   static_cast<uint64_t>(total_sessions_handled_rate), total_sessions_handled_rate / 1e6);
  nat::io::println("   ├─ ⭐ New Session Creation (CPS): {:L} session/s ({:.2f} M CPS) [Allocated PBA + Added to RAM]",
                   static_cast<uint64_t>(new_success / elapsed_sec), cps_success);
  nat::io::println("   ├─ 🔹 In-RAM Established Forwarding: {:L} session/s ({:.2f} M sessions/s) [Lookup in RAM + Rewrite]",
                   static_cast<uint64_t>(hits / elapsed_sec), established_session_rate / 1e6);
  nat::io::println("   ├─ 🚦 Slow-Path Redirected Packets: {:L} packets",
                   result.slow_path_packets);
  nat::io::println("   └─ 🔄 Evicted / Replaced from RAM: {:L} sessions (LRU quota reclaims)",
                   evictions);
  nat::io::println("Churn attempts: {:.2f}%  Churn success: {:.2f}%",
                   churn_attempt_pct, churn_success_pct);
  nat::io::println("Hits: {}  Fast-path hits: {}  Hot-cache hits: {}  Evictions: {}",
                   hits, fast_hits, hot_hits, evictions);
  nat::io::println("Peak sessions (sum of per-worker HWM): {}", result.peak_sessions);
  nat::io::println("PBA blocks allocated: {}  freed: {}  allocation failures: {}",
                   pba_alloc, pba_freed, pba_failures);
  nat::io::println("Latency new  P50/P99: {:.2f} / {:.2f} ns",
                   result.p50_new_ns, result.p99_new_ns);
  nat::io::println("Latency hit  P50/P99: {:.2f} / {:.2f} ns",
                   result.p50_old_ns, result.p99_old_ns);
  nat::io::println("L3: {:.2f} Gbps  Wire-equivalent: {:.2f} Gbps",
                   result.l3_gbps, result.wire_gbps);
  nat::io::println("Checksum checks: {}  errors: {}", csum_checks, csum_errors);
  nat::io::println("--------------------------------------------------------------------------------");

  nat_engine_destroy(engine);
  return result;
}

void PrintSummary(const char* title, const BenchmarkResult& up, const BenchmarkResult& down) {
  nat::io::println("================================================================================");
  nat::io::println(" {} ({} WORKERS, {:.2f}s, 15% CHURN)", title, up.num_workers, up.elapsed_sec);
  nat::io::println("================================================================================");
  nat::io::println("Metric                                         Uplink          Downlink");
  nat::io::println("🚀 Packet Forwarding Rate (Mpps)           {:>10.2f}        {:>10.2f}",
                   up.mpps_success, down.mpps_success);
  nat::io::println("💾 Active Sessions in RAM                  {:>10L}        {:>10L}",
                   up.in_ram_sessions, down.in_ram_sessions);
  nat::io::println("RAM Footprint (MB)                         {:>10.2f}        {:>10.2f}",
                   up.ram_footprint_mb, down.ram_footprint_mb);
  nat::io::println("Cycles/packet (TSC or HW)                  {:>10.2f}        {:>10.2f}",
                   up.cycles_per_packet, down.cycles_per_packet);
  nat::io::println("Instructions/packet (HW, 0 = n/a)          {:>10.2f}        {:>10.2f}",
                   up.instructions_per_packet, down.instructions_per_packet);
  nat::io::println("Avg core frequency (GHz)                   {:>10.2f}        {:>10.2f}",
                   up.average_core_ghz, down.average_core_ghz);
  nat::io::println("Cache misses/packet                        {:>10.2f}        {:>10.2f}",
                   up.cache_misses_per_packet, down.cache_misses_per_packet);
  nat::io::println("Per-worker rate CV (%)                     {:>10.1f}        {:>10.1f}",
                   up.worker_rate_cv_pct, down.worker_rate_cv_pct);
  nat::io::println("⚡ Sessions/s (total)                      {:>10.2f}        {:>10.2f}",
                   up.total_sessions_handled_rate / 1e6, down.total_sessions_handled_rate / 1e6);
  nat::io::println("   ├─ ⭐ New Session Creation (CPS)        {:>10.2f}        {:>10.2f}",
                   up.cps_success, down.cps_success);
  nat::io::println("   └─ 🔹 Established Session Handling      {:>10.2f}        {:>10.2f}",
                   up.established_session_rate / 1e6, down.established_session_rate / 1e6);
  nat::io::println("      (all session values above are M sessions/s)");
  nat::io::println("Failures                                   {:>10}        {:>10}",
                   up.failures, down.failures);
  nat::io::println("P99 new latency (ns)                       {:>10.2f}        {:>10.2f}",
                   up.p99_new_ns, down.p99_new_ns);
  nat::io::println("P99 hit latency (ns)                       {:>10.2f}        {:>10.2f}",
                   up.p99_old_ns, down.p99_old_ns);
  nat::io::println("L3 Data Throughput (Gbps)                  {:>10.2f}        {:>10.2f}",
                   up.l3_gbps, down.l3_gbps);
  nat::io::println("Physical Wire Rate (Gbps)                  {:>10.2f}        {:>10.2f}",
                   up.wire_gbps, down.wire_gbps);
  nat::io::println("Checksum errors                            {:>10}        {:>10}",
                   up.csum_errors, down.csum_errors);
  nat::io::println("================================================================================");
}

struct ScalingPoint {
  std::size_t workers{0};
  BenchmarkResult up{};
  BenchmarkResult down{};
};

void PrintScalingCurve(const std::vector<ScalingPoint>& points) {
  if (points.empty()) return;
  const double base_up = points.front().up.total_sessions_handled_rate > 0
                             ? points.front().up.total_sessions_handled_rate
                             : 1.0;
  const double base_down = points.front().down.total_sessions_handled_rate > 0
                               ? points.front().down.total_sessions_handled_rate
                               : 1.0;

  nat::io::println("\n=========================================================================================================================");
  nat::io::println("   SCALING CURVE: 64B, 15% CHURN (sessions/s is the standard; Mpps is packet forwarding only)");
  nat::io::println("=========================================================================================================================");
  nat::io::println("  Workers | Up sessions/s (M) | Up scaling | Up cyc/pkt | Down sessions/s (M) | Down scaling | Down cyc/pkt");
  nat::io::println("  --------+-------------------+------------+------------+---------------------+--------------+-------------");
  for (const auto& point : points) {
    const double up_rate = point.up.total_sessions_handled_rate / 1e6;
    const double down_rate = point.down.total_sessions_handled_rate / 1e6;
    nat::io::println(
        "  {:>7} | {:>17.2f} | {:>9.2f}x | {:>10.2f} | {:>19.2f} | {:>11.2f}x | {:>11.2f}",
        point.workers, up_rate, point.up.total_sessions_handled_rate / base_up,
        point.up.cycles_per_packet, down_rate,
        point.down.total_sessions_handled_rate / base_down, point.down.cycles_per_packet);
  }
  nat::io::println("  [*] Cycles/packet is HW cycles when perf_event_open is permitted, otherwise TSC (nominal).");
  nat::io::println("=========================================================================================================================\n");
}


}  // namespace

int main(int argc, char* argv[]) {
  std::uint32_t duration_sec = 30;
  std::string mode = "all";
  std::size_t num_workers = 16;

  if (argc > 1) {
    mode = argv[1];
  }
  if (argc > 2) {
    duration_sec = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    if (duration_sec == 0) duration_sec = 30;
  }
  if (argc > 3) {
    num_workers = static_cast<std::size_t>(std::strtoul(argv[3], nullptr, 10));
    if (num_workers == 0) num_workers = 16;
    if (num_workers > 64) num_workers = 64;
  }

  if (mode == "scaling" || mode == "threads") {
    nat::io::println("================================================================================");
    nat::io::println(" STARTING SCALING CURVE: 1/2/4/8/16 WORKERS ({}s per test, 15% churn)", duration_sec);
    nat::io::println("================================================================================\n");

    const std::array<std::size_t, 5> worker_points{1, 2, 4, 8, 16};
    std::vector<ScalingPoint> points;
    points.reserve(worker_points.size());
    for (const std::size_t workers : worker_points) {
      nat::io::println("\n>>> {} WORKER(S)", workers);
      auto up = RunChurnBenchmark<64, TrafficDirection::kUplink>(duration_sec, workers);
      auto down = RunChurnBenchmark<64, TrafficDirection::kDownlink>(duration_sec, workers);
      points.push_back(ScalingPoint{workers, up, down});
    }
    PrintScalingCurve(points);
    return 0;
  }

  const bool run_40_up = mode == "40" || mode == "40-up";
  const bool run_40_down = mode == "40" || mode == "40-down";
  const bool run_64_up =
      mode == "all" || mode == "64" || mode == "uplink" || mode == "64-up";
  const bool run_64_down =
      mode == "all" || mode == "64" || mode == "downlink" || mode == "64-down";
  const bool run_1500_up =
      mode == "all" || mode == "1500" || mode == "uplink" || mode == "1500-up";
  const bool run_1500_down =
      mode == "all" || mode == "1500" || mode == "downlink" || mode == "1500-down";

  BenchmarkResult res40_up{}, res40_down{}, res64_up{}, res64_down{}, res1500_up{}, res1500_down{};
  if (run_40_up) res40_up = RunChurnBenchmark<40, TrafficDirection::kUplink>(duration_sec, num_workers);
  if (run_40_down) res40_down = RunChurnBenchmark<40, TrafficDirection::kDownlink>(duration_sec, num_workers);
  if (run_64_up) res64_up = RunChurnBenchmark<64, TrafficDirection::kUplink>(duration_sec, num_workers);
  if (run_64_down) res64_down = RunChurnBenchmark<64, TrafficDirection::kDownlink>(duration_sec, num_workers);
  if (run_1500_up) res1500_up = RunChurnBenchmark<1500, TrafficDirection::kUplink>(duration_sec, num_workers);
  if (run_1500_down) res1500_down = RunChurnBenchmark<1500, TrafficDirection::kDownlink>(duration_sec, num_workers);

  if (run_40_up && run_40_down) PrintSummary("40 B SUMMARY", res40_up, res40_down);

  if (run_64_up && run_64_down) PrintSummary("64 B SUMMARY", res64_up, res64_down);
  if (run_1500_up && run_1500_down) PrintSummary("1500 B SUMMARY", res1500_up, res1500_down);

  return 0;
}
