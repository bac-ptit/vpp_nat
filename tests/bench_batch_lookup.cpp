#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
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
#include <thread>
#include <vector>

#include "nat/nat_plugin.h"

namespace {

constexpr std::uint32_t kOutsideIp = 0xC6120002;
constexpr std::size_t kSubscribersPerWorker = 500;

struct alignas(64) Packet {
  struct iphdr ip;
  struct tcphdr tcp;
  std::uint8_t payload[24];
};

struct Flow {
  std::uint32_t inside_ip{0};
  std::uint16_t inside_port{0};
};

void PinThread(std::size_t worker_idx, std::size_t total_workers) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  int cpu_id = 0;
  if (total_workers <= 8) {
    cpu_id = static_cast<int>(worker_idx * 2);
  } else if (worker_idx < 8) {
    cpu_id = static_cast<int>(worker_idx * 2);
  } else {
    cpu_id = static_cast<int>((worker_idx - 8) * 2 + 1);
  }
  CPU_SET(cpu_id, &cpuset);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  (void)setpriority(PRIO_PROCESS, 0, -20);
}

void InitPacket(Packet& pkt, std::uint32_t src_ip, std::uint16_t src_port) {
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.ip.version = 4;
  pkt.ip.ihl = 5;
  pkt.ip.tot_len = htons(sizeof(pkt));
  pkt.ip.ttl = 64;
  pkt.ip.protocol = IPPROTO_TCP;
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(kOutsideIp);
  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(80);
  pkt.tcp.doff = 5;
  pkt.tcp.syn = 1;
}

void RebuildPacket(Packet& pkt, const Flow& flow) {
  pkt.ip.saddr = htonl(flow.inside_ip);
  pkt.ip.daddr = htonl(kOutsideIp);
  pkt.tcp.source = htons(flow.inside_port);
  pkt.tcp.dest = htons(80);
  pkt.tcp.check = 0;
}

void DumpRecord(void* ctx, const nat_session_record_t* record) {
  auto* seen = static_cast<int*>(ctx);
  if (*seen == 0) {
    std::printf("record: in=%08x:%u -> %08x:%u out=%08x:%u\n", record->inside_src_ip,
                record->inside_src_port, record->inside_dst_ip, record->inside_dst_port,
                record->outside_src_port, record->outside_dst_port);
  }
  ++*seen;
}

std::uint64_t ReadTsc() {
#if defined(__x86_64__) || defined(__i386__)
  return __rdtsc();
#else
  return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t workers = (argc > 1) ? std::strtoul(argv[1], nullptr, 10) : 16;
  std::size_t batch = (argc > 2) ? std::strtoul(argv[2], nullptr, 10) : 4;
  std::uint32_t duration = (argc > 3) ? std::strtoul(argv[3], nullptr, 10) : 5;
  std::size_t pool = (argc > 4) ? std::strtoul(argv[4], nullptr, 10) : 30'000;
  std::size_t churn = (argc > 5) ? std::strtoul(argv[5], nullptr, 10) : 0;
  if (workers == 0) workers = 16;
  if (batch == 0 || batch > 32) batch = 4;
  if (duration == 0) duration = 5;
  if (pool == 0) pool = 30'000;
  if (churn > 100) churn = 100;

  std::printf("batch=%zu workers=%zu pool=%zu duration=%u churn=%zu%%\n", batch, workers, pool,
              duration, churn);

  const std::uint32_t capacity = static_cast<std::uint32_t>(
      std::min<std::size_t>(1'000'000, pool * 2 + 4096));
  nat_engine_t* engine = nat_engine_create_with_capacity(
      static_cast<std::uint32_t>(workers), capacity);
  if (engine == nullptr) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }

  std::vector<std::vector<Flow>> flows(workers, std::vector<Flow>(pool));
  std::vector<std::thread> setup;
  setup.reserve(workers);
  for (std::size_t w = 0; w < workers; ++w) {
    setup.emplace_back([&, w]() {
      PinThread(w, workers);
      const std::uint32_t prefix = (10U << 24) | (static_cast<std::uint32_t>(w + 1) << 16);
      Packet pkt;
      for (std::size_t i = 0; i < pool; ++i) {
        const std::uint32_t user = static_cast<std::uint32_t>(i % kSubscribersPerWorker);
        const std::uint16_t port = static_cast<std::uint16_t>(1024 + (i / kSubscribersPerWorker));
        const std::uint32_t src_ip = prefix | user;
        InitPacket(pkt, src_ip, port);
        if (nat_translate_in2out(engine, static_cast<std::uint32_t>(w), &pkt.ip, &pkt.tcp,
                                 IPPROTO_TCP) == NAT_TRANSLATE_OK) {
          flows[w][i] = Flow{src_ip, port};
        }
      }
    });
  }
  for (auto& thread : setup) thread.join();
  std::printf("prewarmed=%llu\n",
              static_cast<unsigned long long>(nat_engine_get_total_sessions(engine)));
  int seen = 0;
  nat_engine_export_sessions(engine, DumpRecord, &seen);
  std::printf("flow0: in=%08x:%u\n", flows[0][0].inside_ip, flows[0][0].inside_port);
  Packet debug_packet;
  InitPacket(debug_packet, flows[0][0].inside_ip, flows[0][0].inside_port);
  RebuildPacket(debug_packet, flows[0][0]);
  std::printf("debug pkt: src=%08x dst=%08x sport=%u dport=%u proto=%u ihl=%u tot=%u\n",
              ntohl(debug_packet.ip.saddr), ntohl(debug_packet.ip.daddr),
              ntohs(debug_packet.tcp.source), ntohs(debug_packet.tcp.dest),
              debug_packet.ip.protocol, debug_packet.ip.ihl,
              ntohs(debug_packet.ip.tot_len));
  std::printf("debug action=%d\n",
              static_cast<int>(nat_translate_in2out(engine, 0, &debug_packet.ip,
                                                    &debug_packet.tcp, IPPROTO_TCP)));

  std::vector<std::uint64_t> counts(workers, 0);
  std::vector<std::uint64_t> tsc_cycles(workers, 0);
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  threads.reserve(workers);

  for (std::size_t w = 0; w < workers; ++w) {
    threads.emplace_back([&, w]() {
      PinThread(w, workers);
      const auto deadline = start + std::chrono::seconds(duration);
      std::array<Packet, 32> packets{};
      for (std::size_t i = 0; i < packets.size(); ++i) {
        InitPacket(packets[i], flows[w][0].inside_ip, flows[w][0].inside_port);
      }
      void* ips[32];
      void* l4s[32];
      std::uint8_t protos[32];
      nat_action_t actions[32];
      std::size_t cursor = 0;
      std::uint64_t total = 0;
      std::uint64_t new_flow = 0;
      std::uint32_t churn_counter = 0;
      const std::uint32_t prefix = (10U << 24) | (static_cast<std::uint32_t>(w + 1) << 16);
      const std::uint64_t tsc_start = ReadTsc();

      while (true) {
        for (std::size_t i = 0; i < batch; ++i) {
          const bool is_new = churn_counter < churn;
          if (++churn_counter == 100) churn_counter = 0;
          if (is_new) {
            const std::uint32_t src_ip = prefix | static_cast<std::uint32_t>(new_flow % 500);
            const std::uint16_t src_port =
                static_cast<std::uint16_t>(1024 + ((new_flow / 500) % 60000));
            ++new_flow;
            packets[i].ip.saddr = htonl(src_ip);
            packets[i].ip.daddr = htonl(kOutsideIp);
            packets[i].tcp.source = htons(src_port);
            packets[i].tcp.dest = htons(80);
            packets[i].tcp.check = 0;
          } else {
            const Flow& flow = flows[w][cursor++ % pool];
            RebuildPacket(packets[i], flow);
          }
          ips[i] = &packets[i].ip;
          l4s[i] = &packets[i].tcp;
          protos[i] = IPPROTO_TCP;
          actions[i] = NAT_TRANSLATE_DROP;
        }
        if (batch == 1) {
          actions[0] = nat_translate_in2out(engine, static_cast<std::uint32_t>(w), ips[0], l4s[0],
                                            protos[0]);
        } else {
          nat_translate_in2out_batch(engine, static_cast<std::uint32_t>(w), ips, l4s, protos,
                                     actions, static_cast<std::uint32_t>(batch));
        }
        for (std::size_t i = 0; i < batch; ++i) {
          if (actions[i] == NAT_TRANSLATE_OK) ++total;
        }
        if ((total & 0xFFF) == 0 && std::chrono::steady_clock::now() >= deadline) break;
      }

      if (w == 0) {
        std::printf("worker0 total=%llu last_action=%d\n",
                    static_cast<unsigned long long>(total),
                    static_cast<int>(actions[batch - 1]));
      }
      counts[w] = total;
      tsc_cycles[w] = ReadTsc() - tsc_start;
    });
  }
  for (auto& thread : threads) thread.join();
  const auto end = std::chrono::steady_clock::now();
  const double elapsed = std::chrono::duration<double>(end - start).count();

  const std::uint64_t total = std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
  const std::uint64_t cycles = std::accumulate(tsc_cycles.begin(), tsc_cycles.end(), std::uint64_t{0});
  const double mpps = static_cast<double>(total) / elapsed / 1e6;
  const double cycles_per_packet =
      total > 0 ? static_cast<double>(cycles) / static_cast<double>(total) : 0.0;
  double mean = 0.0;
  for (const auto rate : counts) mean += static_cast<double>(rate) / elapsed / 1e6;
  mean /= static_cast<double>(workers);
  double variance = 0.0;
  for (const auto rate : counts) {
    const double value = static_cast<double>(rate) / elapsed / 1e6;
    variance += (value - mean) * (value - mean);
  }
  variance /= static_cast<double>(workers);
  std::printf("total=%llu mpps=%.2f tsc_cyc_pkt=%.2f mean_worker=%.2f cv=%.1f%%\n",
              static_cast<unsigned long long>(total), mpps, cycles_per_packet, mean,
              mean > 0 ? 100.0 * std::sqrt(variance) / mean : 0.0);
  nat_engine_destroy(engine);
  return 0;
}
