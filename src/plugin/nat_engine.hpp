#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <new>
#include <optional>
#include <sys/mman.h>
#include <vector>

#include "bihash_wrapper.h"
#include "nat/nat_engine_types.hpp"
#include "nat/nat_plugin.h"
#include "pba_bitmap.h"

#if defined(NAT_PROFILE_NOINLINE)
#define NAT_FORCE_INLINE gnu::noinline
#else
#define NAT_FORCE_INLINE gnu::always_inline
#endif

#if !defined(NDEBUG) || defined(DEBUG) || defined(NAT_DEBUG)
#define NAT_DEBUG_CHECKS 1
#else
#define NAT_DEBUG_CHECKS 0
#endif

namespace nat::engine {

#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t kCacheLineSize{std::hardware_destructive_interference_size};
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 14
#pragma GCC diagnostic pop
#endif
#else
inline constexpr std::size_t kCacheLineSize{64};
#endif

inline constexpr std::uint32_t kInvalidIndex{0xFFFFFFFF};

enum class LruClass : std::uint8_t {
  kTcpTransitory = 0,
  kTcpEstablished = 1,
  kUdp = 2,
  kIcmp = 3,
  kUnknown = 4,
};
inline constexpr std::size_t kLruClassCount{5};
inline constexpr std::uint32_t kSessionFlagLruShift{3};
inline constexpr std::uint16_t kSessionFlagLruMask{
    static_cast<std::uint16_t>(0x7U << kSessionFlagLruShift)};

/**
 * @brief Telemetry metrics on hash table probe depth and collisions.
 */
struct ProbeStats {
  std::size_t total_entries{0};
  std::size_t total_probes{0};
  std::size_t max_probe_length{0};
  std::size_t collisions{0};

  [[nodiscard]] double AverageProbeLength() const noexcept {
    return total_entries > 0 ? static_cast<double>(total_probes) / static_cast<double>(total_entries) : 1.0;
  }
  [[nodiscard]] double CollisionRatePct() const noexcept {
    return total_entries > 0 ? (static_cast<double>(collisions) / static_cast<double>(total_entries)) * 100.0 : 0.0;
  }
};

/**
 * @brief FiveTuple to 16-byte Bihash key converter.
 */
static inline nat_bihash_kv_t FiveTupleToKv(const FiveTuple& key, std::uint64_t val = 0) noexcept {
  nat_bihash_kv_t kv{};
  kv.key[0] = (static_cast<std::uint64_t>(key.src_ip) << 32) | static_cast<std::uint64_t>(key.dst_ip);
  kv.key[1] = (static_cast<std::uint64_t>(key.src_port) << 48) |
              (static_cast<std::uint64_t>(key.dst_port) << 32) |
              (static_cast<std::uint64_t>(key.protocol) << 24);
  kv.value = val;
  return kv;
}

enum class LookupResult : std::uint8_t { Hit, Miss, TableFull };

struct LookupOutcome {
  LookupResult result{LookupResult::Miss};
  std::uint32_t session_idx{kInvalidIndex};
  std::uint32_t translated_ip{0};
  std::uint16_t translated_port{0};
  bool fast_path{false};
  nat_bihash_kv_t kv{};
  NatSession* session{nullptr};
};

struct PendingCreate {
  LookupOutcome outcome{};
  FiveTuple in_key{};
  std::uint32_t entry_index{kInvalidIndex};
  std::uint32_t session_idx{kInvalidIndex};
  std::uint32_t public_ip{0};
  std::uint64_t in_hash{0};
  std::uint64_t out_hash{0};
  std::uint16_t port{0};
  bool port_allocated{false};
  NatSession* session{nullptr};
};

struct BatchLookupEntry {
  FiveTuple key{};
  std::uint64_t hash{0};
  LookupOutcome outcome{};
  bool valid{false};
};

struct PortAllocation {
  std::uint16_t port{0};
  std::uint32_t public_ip{0};

  operator std::uint16_t() const noexcept { return port; }
};

inline std::uint64_t current_timestamp_ns(std::uint32_t* out_sec = nullptr) noexcept {
  thread_local std::uint64_t cached_ts = 0;
  thread_local std::uint32_t cached_sec = 0;
  thread_local std::uint32_t pkt_counter = 0;
  if ((pkt_counter++ & 0x3F) == 0 || cached_ts == 0) [[unlikely]] {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
    cached_sec = static_cast<std::uint32_t>(ts.tv_sec);
    cached_ts = static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                static_cast<std::uint64_t>(ts.tv_nsec);
  }
  if (out_sec) *out_sec = cached_sec;
  return cached_ts;
}

struct SessionTimeouts {
  std::uint32_t udp_sec{120};
  std::uint32_t tcp_established_sec{3600};
  std::uint32_t tcp_transitory_sec{60};
  std::uint32_t icmp_sec{30};
};

struct RetireEntry {
  std::uint32_t session_idx{kInvalidIndex};
  std::uint16_t generation{0};
};

struct EngineConfig {
  std::uint32_t sessions_per_worker{2'000'000};
  std::uint32_t pool_start_ip{0xCB007101};  // 203.0.113.1
  std::uint32_t pool_end_ip{0xCB007120};    // 203.0.113.32
  std::uint16_t block_size{64};
  std::uint16_t max_blocks_per_user{8};
  std::uint16_t vector_batch_size{256};
  std::uint8_t prefetch_distance{4};
  SessionTimeouts timeouts{};
};

inline constexpr std::size_t kLowWatermark{512};
inline constexpr std::size_t kDefaultReclaimBatchSize{64};

struct alignas(64) TableTelemetry {
  std::uint64_t packets_processed{0};
  std::uint64_t new_sessions_created{0};
  std::uint64_t create_failures{0};
  std::uint64_t existing_session_hits{0};
  std::uint64_t fast_path_hits{0};
  std::uint64_t hot_cache_hits{0};
  std::uint64_t lru_evictions{0};
  std::uint64_t expired_reclaims{0};
  std::uint64_t active_sessions_high_watermark{0};
  std::uint64_t pba_blocks_allocated{0};
  std::uint64_t pba_blocks_freed{0};
  std::uint64_t pba_port_alloc_failures{0};
  std::uint64_t bihash_insert_failures{0};
  std::uint64_t fragment_drops{0};
  std::uint64_t malformed_drops{0};
  std::uint64_t out2in_misses{0};
  std::uint64_t slow_path_packets{0};
};

struct alignas(8) LruNode {
  std::uint32_t prev_idx{kInvalidIndex};
  std::uint32_t next_idx{kInvalidIndex};
};

enum class SubscriberState : std::uint8_t {
  kEmpty = 0,
  kOccupied = 1,
  kTombstone = 2,
};

struct alignas(64) PbaSubscriber {
  static constexpr std::size_t kMaxBlocks = 8;
  std::uint32_t user_ip{0};
  std::uint8_t num_blocks{0};
  SubscriberState state{SubscriberState::kEmpty};
  std::uint16_t pad{0};
  pba_block_t blocks[kMaxBlocks]{};
};

static_assert(sizeof(pba_block_t) == 24, "pba_block_t must be 24 bytes");
static_assert(sizeof(PbaSubscriber) == 256, "PbaSubscriber must be 256 bytes");

/**
 * @brief High-performance contiguous vector backed by 2MB Hugepages.
 *
 * Drastically cuts dTLB misses from hundreds of thousands of 4KB pages to a handful
 * of 2MB hugepages, eliminating hardware page-table walk overhead on millions of sessions.
 */
template <typename T>
class HugepageVector {
 public:
  HugepageVector() = default;
  explicit HugepageVector(std::size_t count, const T& val = T{}) {
    resize(count, val);
  }
  ~HugepageVector() { clear(); }

  HugepageVector(const HugepageVector&) = delete;
  HugepageVector& operator=(const HugepageVector&) = delete;
  HugepageVector(HugepageVector&& other) noexcept
      : data_{other.data_}, size_{other.size_}, capacity_bytes_{other.capacity_bytes_} {
    other.data_ = nullptr;
    other.size_ = 0;
    other.capacity_bytes_ = 0;
  }
  HugepageVector& operator=(HugepageVector&& other) noexcept {
    if (this != &other) {
      clear();
      data_ = other.data_;
      size_ = other.size_;
      capacity_bytes_ = other.capacity_bytes_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.capacity_bytes_ = 0;
    }
    return *this;
  }

  void resize(std::size_t new_size, const T& val = T{}) {
    if (new_size == size_) return;
    clear();
    if (new_size == 0) return;
    size_ = new_size;
    constexpr std::size_t kHugepageSize = 2 * 1024 * 1024;
    const std::size_t raw_bytes = sizeof(T) * new_size;
    capacity_bytes_ = ((raw_bytes + kHugepageSize - 1) / kHugepageSize) * kHugepageSize;

    void* ptr = mmap(nullptr, capacity_bytes_, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    static bool logged_backing = false;
    if (ptr == MAP_FAILED) {
      ptr = mmap(nullptr, capacity_bytes_, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#if defined(MADV_HUGEPAGE)
      if (ptr != MAP_FAILED) {
        madvise(ptr, capacity_bytes_, MADV_HUGEPAGE);
      }
#endif
      if (!logged_backing && ptr != MAP_FAILED) {
        logged_backing = true;
        std::fprintf(stderr,
                     "[nat] MAP_HUGETLB unavailable; THP fallback for %zu bytes\n",
                     capacity_bytes_);
      }
    } else if (!logged_backing) {
      logged_backing = true;
      std::fprintf(stderr, "[nat] hugepages active for %zu bytes\n", capacity_bytes_);
    }
    if (ptr == MAP_FAILED || !ptr) {
      data_ = nullptr;
      size_ = 0;
      capacity_bytes_ = 0;
      return;
    }
    data_ = static_cast<T*>(ptr);
    for (std::size_t i = 0; i < size_; ++i) {
      new (&data_[i]) T(val);
    }
  }

  void clear() noexcept {
    if (data_ && data_ != MAP_FAILED) {
      for (std::size_t i = 0; i < size_; ++i) {
        data_[i].~T();
      }
      munmap(data_, capacity_bytes_);
      data_ = nullptr;
    }
    size_ = 0;
    capacity_bytes_ = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard, NAT_FORCE_INLINE]] inline T& operator[](std::size_t idx) noexcept { return data_[idx]; }
  [[nodiscard, NAT_FORCE_INLINE]] inline const T& operator[](std::size_t idx) const noexcept { return data_[idx]; }
  [[nodiscard]] T* data() noexcept { return data_; }
  [[nodiscard]] const T* data() const noexcept { return data_; }

 private:
  T* data_{nullptr};
  std::size_t size_{0};
  std::size_t capacity_bytes_{0};
};

/**
 * @brief Thread-local session table and PBA port allocator.
 *
 * Cache-line aligned (alignas(64)) to prevent false sharing between worker threads.
 * Uses VPP clib_bihash_16_8, per-thread LRU doubly-linked list, and Flat Subscriber Array.
 */
class alignas(kCacheLineSize) ThreadSessionTable final {
 public:
  static constexpr std::size_t kSubscribersPerWorker = 65536;
  static constexpr std::uint32_t kSubscriberMask = 65535;
  static constexpr std::size_t kDefaultReclaimBatchSize = 64;
  static constexpr std::size_t kBlocksPerPublicIp =
      (PBA_MAX_PORT - PBA_MIN_PORT + 1) / 64;
  static constexpr std::size_t kMaxPublicIps = 32;
  static constexpr std::size_t kTotalBlocks = kBlocksPerPublicIp * kMaxPublicIps;
  static constexpr std::size_t kBlockBitmapWords = (kTotalBlocks + 63) / 64;
  static constexpr std::uint32_t kFastActive{1U << 31};
  static constexpr std::uint32_t kFastRetiring{1U << 30};
  static constexpr std::uint32_t kFastSecMask{(1U << 29) - 1};

  struct alignas(32) HotEntry {
    FiveTuple key{};
    std::uint64_t hash{0};
    std::uint32_t session_idx{kInvalidIndex};
    std::uint16_t generation{0};
    std::uint16_t pad{0};
  };

  explicit ThreadSessionTable(std::size_t initial_capacity = 250'000,
                              std::size_t expected_active = 0) noexcept;
  ~ThreadSessionTable() noexcept;

  ThreadSessionTable(const ThreadSessionTable&) = delete;
  ThreadSessionTable& operator=(const ThreadSessionTable&) = delete;
  ThreadSessionTable(ThreadSessionTable&& other) noexcept;
  ThreadSessionTable& operator=(ThreadSessionTable&& other) noexcept;

  void SetTimeouts(const SessionTimeouts& t) noexcept { timeouts_ = t; }
  [[nodiscard]] const SessionTimeouts& GetTimeouts() const noexcept { return timeouts_; }

  void SetThreadIndex(std::uint32_t index) noexcept { thread_index_ = index; }
  [[nodiscard]] std::uint32_t ThreadIndex() const noexcept { return thread_index_; }

  void SetMaxSessions(std::size_t max_sessions) noexcept { max_sessions_ = max_sessions; }
  [[nodiscard]] std::size_t MaxSessions() const noexcept { return max_sessions_; }

  void SetForceEvictOnOverflow(bool enabled) noexcept { force_evict_on_overflow_ = enabled; }
  [[nodiscard]] bool ForceEvictOnOverflow() const noexcept { return force_evict_on_overflow_; }

  void SetPool(std::uint32_t start, std::uint32_t end) noexcept {
    pool_start_ip_ = start;
    pool_end_ip_ = end;
  }
  void SetPublicIp(std::uint32_t ip) noexcept {
    public_ips_[0] = ip;
    public_ip_count_ = 1;
    public_ip_ = ip;
  }
  void SetPublicIps(std::uint32_t start_ip, std::size_t count) noexcept {
    const std::size_t n = std::min<std::size_t>(count, kMaxPublicIps);
    public_ip_count_ = static_cast<std::uint8_t>(n == 0 ? 1 : n);
    for (std::size_t i = 0; i < public_ip_count_; ++i) {
      public_ips_[i] = start_ip + static_cast<std::uint32_t>(i);
    }
    public_ip_ = public_ips_[0];
  }
  [[nodiscard]] std::uint32_t PublicIp() const noexcept { return public_ip_; }
  [[nodiscard]] std::uint32_t PublicIpAt(std::size_t index) const noexcept {
    return public_ips_[index & (kMaxPublicIps - 1)];
  }
  [[nodiscard]] std::uint8_t PublicIpCount() const noexcept { return public_ip_count_; }
  [[nodiscard]] std::size_t PublicIpIndex(std::uint32_t ip) const noexcept {
    for (std::size_t i = 0; i < public_ip_count_; ++i) {
      if (public_ips_[i] == ip) return i;
    }
    return 0;
  }
  [[nodiscard]] static constexpr std::size_t PortSlot(std::size_t ip_index,
                                                      std::uint16_t port) noexcept {
    return ip_index * 65536 + port;
  }

  void SetBlockSize(std::uint16_t size) noexcept {
    block_size_ = (size == 32) ? static_cast<std::uint16_t>(32) : static_cast<std::uint16_t>(64);
  }
  [[nodiscard]] std::uint16_t BlockSize() const noexcept { return block_size_; }

  void SetMaxBlocksPerUser(std::uint16_t count) noexcept {
    const std::uint16_t capacity = static_cast<std::uint16_t>(PbaSubscriber::kMaxBlocks);
    max_blocks_per_user_ = (count == 0) ? static_cast<std::uint16_t>(1)
                                        : (count > capacity ? capacity : count);
  }
  [[nodiscard]] std::uint16_t MaxBlocksPerUser() const noexcept { return max_blocks_per_user_; }

  void SetVectorConfig(std::uint16_t batch, std::uint8_t prefetch) noexcept {
    vector_batch_size_ = batch;
    prefetch_distance_ = prefetch;
  }
  [[nodiscard]] std::uint16_t VectorBatchSize() const noexcept { return vector_batch_size_; }
  [[nodiscard]] std::uint8_t PrefetchDistance() const noexcept { return prefetch_distance_; }

  void SetLazyUpdates(bool enabled) noexcept { lazy_updates_ = enabled; }
  [[nodiscard]] bool LazyUpdates() const noexcept { return lazy_updates_; }
  void SetInlineValues(bool enabled) noexcept {
    inline_values_ = enabled;
    if (enabled && fast_state_.size() < sessions_.size()) {
      fast_state_.resize(sessions_.size(), 0);
    }
  }
  [[nodiscard]] bool InlineValues() const noexcept { return inline_values_; }

  void SetHotCache(std::size_t entries) noexcept {
    if (entries < 2) {
      hot_cache_.clear();
      return;
    }
    std::size_t size = 1;
    while (size < entries) size <<= 1;
    hot_cache_.resize(size, HotEntry{});
    hot_mask_ = size - 1;
  }
  [[nodiscard]] std::size_t HotCacheSize() const noexcept { return hot_cache_.size(); }

  [[nodiscard, NAT_FORCE_INLINE]] inline NatSession* HotLookup(
      const FiveTuple& key, std::uint64_t hash) noexcept {
    if (hot_cache_.empty()) return nullptr;
    const HotEntry& entry = hot_cache_[hash & hot_mask_];
    if (entry.session_idx == kInvalidIndex || entry.hash != hash ||
        !(entry.key == key)) {
      return nullptr;
    }
    NatSession* session = &sessions_[entry.session_idx];
    if (session->generation != entry.generation ||
        (session->flags & (kSessionFlagActive | kSessionFlagRetiring)) !=
            kSessionFlagActive) {
      return nullptr;
    }
    ++telemetry_.hot_cache_hits;
    return session;
  }

  [[NAT_FORCE_INLINE]] inline void HotInsert(const FiveTuple& key,
                                             std::uint64_t hash,
                                             std::uint32_t session_idx) noexcept {
    if (hot_cache_.empty()) return;
    HotEntry& entry = hot_cache_[hash & hot_mask_];
    entry.key = key;
    entry.hash = hash;
    entry.session_idx = session_idx;
    entry.generation = sessions_[session_idx].generation;
  }

  [[nodiscard]] std::uint64_t EncodeIn2OutValue(std::uint32_t session_idx,
                                                std::uint32_t public_ip,
                                                std::uint16_t public_port) const noexcept {
    if (!inline_values_) return session_idx;
    const std::uint64_t ip_index = PublicIpIndex(public_ip) & (kMaxPublicIps - 1);
    return static_cast<std::uint64_t>(session_idx) |
           (static_cast<std::uint64_t>(public_port) << 32) |
           (ip_index << 48);
  }

  [[nodiscard]] static std::uint32_t DecodeSessionIndex(std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(value & 0xFFFFFFFFULL);
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline bool FastUsable(
      std::uint32_t session_idx, std::uint32_t now_sec) const noexcept {
    if (!inline_values_ || session_idx >= fast_state_.size()) return false;
    const std::uint32_t state = fast_state_[session_idx];
    return (state & (kFastActive | kFastRetiring)) == kFastActive &&
           (state & kFastSecMask) == (now_sec & kFastSecMask);
  }

  [[NAT_FORCE_INLINE]] inline void MarkFastActive(std::uint32_t session_idx,
                                                  std::uint32_t now_sec) noexcept {
    if (!inline_values_ || session_idx >= fast_state_.size()) return;
    fast_state_[session_idx] = kFastActive | (now_sec & kFastSecMask);
  }

  [[NAT_FORCE_INLINE]] inline void MarkFastSec(std::uint32_t session_idx,
                                               std::uint32_t now_sec) noexcept {
    if (!inline_values_ || session_idx >= fast_state_.size()) return;
    std::uint32_t& state = fast_state_[session_idx];
    state = (state & ~kFastSecMask) | (now_sec & kFastSecMask);
  }

  [[NAT_FORCE_INLINE]] inline void MarkFastRetiring(std::uint32_t session_idx) noexcept {
    if (!inline_values_ || session_idx >= fast_state_.size()) return;
    std::uint32_t& state = fast_state_[session_idx];
    state = (state & ~kFastActive) | kFastRetiring;
  }

  [[NAT_FORCE_INLINE]] inline void MarkFastFree(std::uint32_t session_idx) noexcept {
    if (!inline_values_ || session_idx >= fast_state_.size()) return;
    fast_state_[session_idx] = 0;
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline std::uint64_t GetTimeoutNs(
      const NatSession& session) const noexcept {
    std::uint32_t sec = timeouts_.udp_sec;
    switch (session.inside.protocol) {
      case 17:  // IPPROTO_UDP
        sec = timeouts_.udp_sec;
        break;
      case 6:   // IPPROTO_TCP
        sec = (session.flags & kSessionFlagTcpEstablished)
                  ? timeouts_.tcp_established_sec
                  : timeouts_.tcp_transitory_sec;
        break;
      case 1:   // IPPROTO_ICMP
        sec = timeouts_.icmp_sec;
        break;
      default:
        sec = timeouts_.udp_sec;
        break;
    }
    return static_cast<std::uint64_t>(sec) * 1'000'000'000ULL;
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline bool IsExpired(
      const NatSession& session, std::uint64_t now_ns) const noexcept {
    const std::uint64_t timeout_ns = GetTimeoutNs(session);
    return (now_ns > session.last_active_ts) &&
           (now_ns - session.last_active_ts >= timeout_ns);
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline std::uint64_t HashKey(
      const FiveTuple& key) const noexcept {
    const auto kv = FiveTupleToKv(key);
    return nat_bihash_hash(&kv);
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchIn2OutHash(std::uint64_t hash) const noexcept {
    nat_bihash_prefetch_bucket_hash(in2out_bihash_, hash);
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchIn2OutDataHash(std::uint64_t hash) const noexcept {
    nat_bihash_prefetch_data_hash(in2out_bihash_, hash);
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchOut2InHash(std::uint64_t hash) const noexcept {
    nat_bihash_prefetch_bucket_hash(out2in_bihash_, hash);
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchOut2InDataHash(std::uint64_t hash) const noexcept {
    nat_bihash_prefetch_data_hash(out2in_bihash_, hash);
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline LookupOutcome LookupIn2OutOutcome(
      const FiveTuple& key, std::uint64_t now_ns = 0, std::uint32_t now_sec = 0) noexcept {
    return LookupIn2OutOutcomeHashed(key, HashKey(key), now_ns, now_sec);
  }

  template <typename EntryType>
  void LookupIn2OutBatch(EntryType* entries, std::size_t count,
                         std::uint64_t now_ns, std::uint32_t now_sec) noexcept {
    if (entries == nullptr || count == 0) return;

    if (now_sec == 0 && now_ns > 0) {
      now_sec = static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL);
    }

    for (std::size_t i = 0; i < count; ++i) {
      auto& entry = entries[i];
      entry.outcome = LookupOutcome{};
      if (!entry.valid) continue;
      if (auto* hot = HotLookup(entry.key, entry.hash)) {
        entry.outcome.result = LookupResult::Hit;
        entry.outcome.session_idx =
            static_cast<std::uint32_t>(hot - sessions_.data());
        __builtin_prefetch(hot, 0, 3);
        continue;
      }
      entry.outcome.kv = FiveTupleToKv(entry.key);
      nat_bihash_kv_t result{};
      if (nat_bihash_search_with_hash(in2out_bihash_, &entry.outcome.kv, entry.hash,
                                      &result) != 0) {
        entry.outcome.result = LookupResult::Miss;
        continue;
      }
      entry.outcome.session_idx = DecodeSessionIndex(result.value);
      if (inline_values_ && lazy_updates_ &&
          FastUsable(entry.outcome.session_idx, now_sec)) {
        entry.outcome.result = LookupResult::Hit;
        entry.outcome.fast_path = true;
        entry.outcome.translated_ip =
            PublicIpAt(static_cast<std::size_t>((result.value >> 48) & (kMaxPublicIps - 1)));
        entry.outcome.translated_port =
            static_cast<std::uint16_t>((result.value >> 32) & 0xFFFFU);
        ++telemetry_.existing_session_hits;
        ++telemetry_.fast_path_hits;
        ++total_translations_;
        continue;
      }
      __builtin_prefetch(&sessions_[entry.outcome.session_idx], 0, 3);
    }

    for (std::size_t i = 0; i < count; ++i) {
      auto& entry = entries[i];
      if (!entry.valid || entry.outcome.session_idx == kInvalidIndex) continue;
      if (entry.outcome.fast_path) continue;
      const std::uint32_t idx = entry.outcome.session_idx;
      auto* session = &sessions_[idx];

      if (session->flags & kSessionFlagRetiring) [[unlikely]] {
        DeleteSessionByIdxIfCurrent(idx);
        entry.outcome.result = LookupResult::Miss;
        entry.outcome.session = nullptr;
        continue;
      }

      if (now_ns > 0 && IsExpired(*session, now_ns)) [[unlikely]] {
        ++telemetry_.expired_reclaims;
        session->flags |= kSessionFlagRetiring;
        MarkFastRetiring(idx);
        retire_queue_.push_back(RetireEntry{idx, session->generation});
        LruRemove(idx);
        if (retire_queue_.size() >= kDefaultReclaimBatchSize) {
          ReclaimBatch(kDefaultReclaimBatchSize);
        }
        entry.outcome.result = LookupResult::Miss;
        entry.outcome.session = nullptr;
        continue;
      }

      entry.outcome.result = LookupResult::Hit;
      entry.outcome.session = session;
      ++telemetry_.existing_session_hits;
      if (now_sec != session->last_lru_move_sec) [[unlikely]] {
        LruTouch(idx);
        session->last_lru_move_sec = now_sec;
        if (lazy_updates_) session->last_active_ts = now_ns;
        if (inline_values_) MarkFastSec(idx, now_sec);
      }
      ++total_translations_;
    }
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline LookupOutcome LookupIn2OutOutcomeHashed(
      const FiveTuple& key, std::uint64_t hash, std::uint64_t now_ns = 0,
      std::uint32_t now_sec = 0) noexcept {
    LookupOutcome outcome{};
    outcome.kv = FiveTupleToKv(key);
    nat_bihash_kv_t res{};
    if (nat_bihash_search_with_hash(in2out_bihash_, &outcome.kv, hash, &res) == 0) [[likely]] {
      outcome.session_idx = DecodeSessionIndex(res.value);
      auto* s = &sessions_[outcome.session_idx];

      if (s->flags & kSessionFlagRetiring) [[unlikely]] {
        DeleteSessionByIdxIfCurrent(outcome.session_idx);
        outcome.result = LookupResult::Miss;
        outcome.session = nullptr;
        return outcome;
      }

      if (now_ns > 0) [[likely]] {
        if (IsExpired(*s, now_ns)) [[unlikely]] {
          ++telemetry_.expired_reclaims;
          s->flags |= kSessionFlagRetiring;
          MarkFastRetiring(outcome.session_idx);
          retire_queue_.push_back(RetireEntry{outcome.session_idx, s->generation});
          LruRemove(outcome.session_idx);
          if (retire_queue_.size() >= kDefaultReclaimBatchSize) {
            ReclaimBatch(kDefaultReclaimBatchSize);
          }
          outcome.result = LookupResult::Miss;
          outcome.session = nullptr;
          return outcome;
        }
      }

      outcome.result = LookupResult::Hit;
      outcome.session = s;
      ++telemetry_.existing_session_hits;
      if (now_sec == 0 && now_ns > 0) [[unlikely]] {
        now_sec = static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL);
      }
      if (now_sec != s->last_lru_move_sec) [[unlikely]] {
        LruTouch(outcome.session_idx);
        s->last_lru_move_sec = now_sec;
        if (lazy_updates_) s->last_active_ts = now_ns;
        if (inline_values_) MarkFastSec(outcome.session_idx, now_sec);
      }
      ++total_translations_;
      return outcome;
    }
    outcome.result = LookupResult::Miss;
    return outcome;
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline NatSession* LookupIn2Out(
      const FiveTuple& key, std::uint64_t now_ns = 0, std::uint32_t now_sec = 0) noexcept {
    auto outcome = LookupIn2OutOutcome(key, now_ns, now_sec);
    return outcome.session;
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline NatSession* LookupOut2In(
      const FiveTuple& key, std::uint64_t now_ns = 0, std::uint32_t now_sec = 0) noexcept {
    return LookupOut2InHashed(key, HashKey(key), now_ns, now_sec);
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline NatSession* LookupOut2InHashed(
      const FiveTuple& key, std::uint64_t hash, std::uint64_t now_ns = 0,
      std::uint32_t now_sec = 0) noexcept {
    const auto kv = FiveTupleToKv(key);
    nat_bihash_kv_t res{};
    if (nat_bihash_search_with_hash(out2in_bihash_, &kv, hash, &res) == 0) [[likely]] {
      const auto idx = static_cast<std::uint32_t>(res.value);
      auto* s = &sessions_[idx];
      if (s->flags & kSessionFlagRetiring) [[unlikely]] {
        DeleteSessionByIdxIfCurrent(idx);
        return nullptr;
      }
      if (now_ns > 0) [[likely]] {
        if (IsExpired(*s, now_ns)) [[unlikely]] {
          ++telemetry_.expired_reclaims;
          s->flags |= kSessionFlagRetiring;
          MarkFastRetiring(idx);
          retire_queue_.push_back(RetireEntry{idx, s->generation});
          LruRemove(idx);
          if (retire_queue_.size() >= kDefaultReclaimBatchSize) {
            ReclaimBatch(kDefaultReclaimBatchSize);
          }
          return nullptr;
        }
      }
      ++telemetry_.existing_session_hits;
      if (now_sec == 0 && now_ns > 0) [[unlikely]] {
        now_sec = static_cast<std::uint32_t>(now_ns / 1'000'000'000ULL);
      }
      if (now_sec != s->last_lru_move_sec) [[unlikely]] {
        LruTouch(idx);
        s->last_lru_move_sec = now_sec;
        if (lazy_updates_) s->last_active_ts = now_ns;
        if (inline_values_) MarkFastSec(idx, now_sec);
      }
      ++total_translations_;
      return s;
    }
    return nullptr;
  }

  void LookupIn2OutBurst(const FiveTuple* keys, NatSession** out_sessions, std::size_t count,
                         std::size_t prefetch_dist = 4) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (i + prefetch_dist < count) {
        PrefetchIn2Out(keys[i + prefetch_dist]);
      }
      out_sessions[i] = LookupIn2Out(keys[i]);
    }
  }

  void LookupOut2InBurst(const FiveTuple* keys, NatSession** out_sessions, std::size_t count,
                         std::size_t prefetch_dist = 4) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      if (i + prefetch_dist < count) {
        PrefetchOut2In(keys[i + prefetch_dist]);
      }
      out_sessions[i] = LookupOut2In(keys[i]);
    }
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchIn2Out(const FiveTuple& key) const noexcept {
    const auto kv = FiveTupleToKv(key);
    nat_bihash_prefetch_bucket(in2out_bihash_, &kv);
  }

  [[NAT_FORCE_INLINE]] inline void PrefetchOut2In(const FiveTuple& key) const noexcept {
    const auto kv = FiveTupleToKv(key);
    nat_bihash_prefetch_bucket(out2in_bihash_, &kv);
  }

  [[gnu::hot]] NatSession* CreateSessionWithOutcome(
      const LookupOutcome& outcome, const FiveTuple& in_key, const FiveTuple& out_key) noexcept;

  [[gnu::hot]] void CreateSessionsBatch(PendingCreate* pending, std::size_t count,
                                        std::uint64_t now_ns) noexcept;

  [[gnu::hot]] NatSession* CreateSession(const FiveTuple& in_key,
                                         const FiveTuple& out_key) noexcept;

  [[gnu::cold]] bool DeleteSession(const FiveTuple& in_key) noexcept;
  [[gnu::cold]] bool DeleteSessionByIdx(std::uint32_t session_idx) noexcept;
  [[gnu::cold]] bool DeleteSessionByIdxIfCurrent(std::uint32_t session_idx) noexcept;

  void LruTouch(std::uint32_t session_idx) noexcept;
  void LruPushTail(std::uint32_t session_idx, LruClass cls) noexcept;
  void LruRemove(std::uint32_t session_idx) noexcept;
  std::uint32_t LruPopOldestHead() noexcept;
  std::uint32_t LruFindExpiredHead(std::uint64_t now_ns) noexcept;
  void ReclaimBatch(std::size_t batch_size = kDefaultReclaimBatchSize) noexcept;

  [[nodiscard]] std::optional<PortAllocation> AllocatePortForUser(
      std::uint32_t user_ip, std::uint32_t public_ip) noexcept;

  void ReleasePortForUser(std::uint32_t user_ip, std::uint16_t port) noexcept;
  void ReleasePortForUser(std::uint32_t user_ip, std::uint32_t public_ip,
                          std::uint16_t port) noexcept;

  [[nodiscard]] bool IsValid() const noexcept {
    return in2out_bihash_ != nullptr && out2in_bihash_ != nullptr &&
           sessions_.data() != nullptr && lru_nodes_.data() != nullptr;
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline std::uint32_t SessionIndex(
      const NatSession* session) const noexcept {
    return static_cast<std::uint32_t>(session - sessions_.data());
  }

  void ProcessTick(std::uint64_t now_ns) noexcept;

  void SetHaEnabled(bool enabled) noexcept { ha_enabled_ = enabled; }
  [[nodiscard]] bool HaEnabled() const noexcept { return ha_enabled_; }
  [[nodiscard]] std::uint64_t HaDropped() const noexcept { return ha_dropped_; }

  void ExportSessions(nat_session_export_cb_t cb, void* ctx) const noexcept;
  bool ImportSession(const nat_session_record_t& record) noexcept;
  void FlushSessions() noexcept;
  bool ReservePortForUser(std::uint32_t user_ip, std::uint32_t public_ip,
                          std::uint16_t base_port, std::uint16_t block_size,
                          std::uint16_t port) noexcept;

  [[nodiscard]] std::size_t SessionCount() const noexcept { return session_count_; }
  [[nodiscard]] std::uint64_t TotalTranslations() const noexcept { return total_translations_; }
  [[nodiscard]] std::size_t RetireQueueDepth() const noexcept { return retire_queue_.size(); }
  [[nodiscard]] const TableTelemetry& GetTelemetry() const noexcept { return telemetry_; }
  TableTelemetry& MutableTelemetry() noexcept { return telemetry_; }
  void RecordPacketProcessed() noexcept { ++telemetry_.packets_processed; }
  void RecordPackets(std::size_t count) noexcept { telemetry_.packets_processed += count; }
  void RecordCreateFailure() noexcept { ++telemetry_.create_failures; }

  [[nodiscard]] ProbeStats GetIn2OutProbeStats() const noexcept {
    return ProbeStats{session_count_, 0, 0, 1};
  }

  [[nodiscard]] ProbeStats GetOut2InProbeStats() const noexcept {
    return ProbeStats{session_count_, 0, 0, 1};
  }

 private:
  [[gnu::hot]] bool MakeRoomForCreates(std::size_t count, std::uint64_t now_ns) noexcept;
  bool ReleasePortForUserIp(std::uint32_t user_ip, std::uint32_t public_ip,
                            std::uint16_t port) noexcept;

  [[nodiscard]] static LruClass ClassOfProtocol(std::uint8_t protocol,
                                                bool tcp_established) noexcept {
    switch (protocol) {
      case 6:
        return tcp_established ? LruClass::kTcpEstablished : LruClass::kTcpTransitory;
      case 17:
        return LruClass::kUdp;
      case 1:
        return LruClass::kIcmp;
      default:
        return LruClass::kUnknown;
    }
  }

  [[nodiscard]] static LruClass ClassOfSession(const NatSession& session) noexcept {
    return ClassOfProtocol(session.inside.protocol,
                           (session.flags & kSessionFlagTcpEstablished) != 0);
  }

  [[nodiscard]] static std::uint16_t LruBits(LruClass cls) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(cls) << kSessionFlagLruShift);
  }

  static void SetLruClass(NatSession& session, LruClass cls) noexcept {
    session.flags = static_cast<std::uint16_t>(
        (session.flags & static_cast<std::uint16_t>(~kSessionFlagLruMask)) | LruBits(cls));
  }

  nat_bihash_16_8_t* in2out_bihash_{nullptr};
  nat_bihash_16_8_t* out2in_bihash_{nullptr};

  HugepageVector<NatSession> sessions_;
  HugepageVector<LruNode> lru_nodes_;
  HugepageVector<std::uint32_t> port_to_session_;
  HugepageVector<std::uint32_t> fast_state_;
  HugepageVector<HotEntry> hot_cache_;
  std::size_t hot_mask_{0};
  std::vector<std::uint32_t> free_indices_;
  std::vector<RetireEntry> retire_queue_;

  std::array<std::uint32_t, kLruClassCount> lru_head_{};
  std::array<std::uint32_t, kLruClassCount> lru_tail_{};

  std::array<PbaSubscriber, kSubscribersPerWorker> subscribers_{};
  std::array<std::uint64_t, kBlockBitmapWords> pba_block_bitmap_{};
  std::array<std::uint32_t, kMaxPublicIps> public_ips_{};
  std::uint8_t public_ip_count_{1};

  std::size_t session_count_{0};
  std::uint64_t total_translations_{0};
  TableTelemetry telemetry_{};
  SessionTimeouts timeouts_{};
  std::uint32_t pool_start_ip_{0xCB007101};
  std::uint32_t pool_end_ip_{0xCB007120};
  std::uint32_t public_ip_{0xCB007101};
  std::uint16_t block_size_{64};
  std::uint16_t max_blocks_per_user_{PbaSubscriber::kMaxBlocks};
  std::uint16_t vector_batch_size_{256};
  std::uint8_t prefetch_distance_{4};
  std::uint32_t thread_index_{0};
  std::size_t max_sessions_{0};
  bool lazy_updates_{false};
  bool inline_values_{false};
  bool ha_enabled_{false};
  bool force_evict_on_overflow_{false};
  std::uint64_t ha_dropped_{0};
};

/**
 * @brief Master NAT Engine holding isolated per-worker session tables.
 */
class NatEngine final {
 public:
  explicit NatEngine(std::size_t num_threads, std::size_t sessions_per_worker = 250'000,
                     std::size_t expected_active = 0) noexcept;
  ~NatEngine() = default;

  NatEngine(const NatEngine&) = delete;
  NatEngine& operator=(const NatEngine&) = delete;
  NatEngine(NatEngine&&) noexcept = default;
  NatEngine& operator=(NatEngine&&) noexcept = default;

  [[nodiscard, NAT_FORCE_INLINE]] inline ThreadSessionTable& GetThreadTable(
      std::uint32_t thread_index) noexcept {
#if NAT_DEBUG_CHECKS
    assert(thread_index < thread_tables_.size());
#else
    [[assume(thread_index < thread_tables_.size())]];
#endif
    return thread_tables_[thread_index];
  }

  [[nodiscard, NAT_FORCE_INLINE]] inline const ThreadSessionTable& GetThreadTable(
      std::uint32_t thread_index) const noexcept {
#if NAT_DEBUG_CHECKS
    assert(thread_index < thread_tables_.size());
#else
    [[assume(thread_index < thread_tables_.size())]];
#endif
    return thread_tables_[thread_index];
  }

  [[nodiscard]] std::size_t ThreadCount() const noexcept { return thread_tables_.size(); }

  [[nodiscard]] bool IsValid() const noexcept {
    for (const auto& table : thread_tables_) {
      if (!table.IsValid()) return false;
    }
    return !thread_tables_.empty();
  }

  [[nodiscard]] std::uint64_t GetTotalSessions() const noexcept;
  [[nodiscard]] std::uint64_t GetTotalTranslations() const noexcept;

  void SetTimeouts(const SessionTimeouts& t) noexcept {
    timeouts_ = t;
    for (auto& table : thread_tables_) {
      table.SetTimeouts(t);
    }
  }

  void SetPool(std::uint32_t start, std::uint32_t end) noexcept {
    pool_start_ip_ = start;
    pool_end_ip_ = (end >= start) ? end : start;
    const std::uint32_t pool_size = pool_end_ip_ - pool_start_ip_ + 1U;
    const std::size_t threads = std::max<std::size_t>(thread_tables_.size(), 1);
    for (std::size_t i{0}; i < thread_tables_.size(); ++i) {
      const std::size_t begin = (i * pool_size) / threads;
      const std::size_t finish = ((i + 1) * pool_size) / threads;
      const std::size_t count = std::max<std::size_t>(finish - begin, 1);
      thread_tables_[i].SetPool(pool_start_ip_, pool_end_ip_);
      thread_tables_[i].SetPublicIps(
          pool_start_ip_ + static_cast<std::uint32_t>(begin), count);
    }
  }

  void SetBlockSize(std::uint16_t size) noexcept {
    for (auto& table : thread_tables_) {
      table.SetBlockSize(size);
    }
  }

  void SetMaxBlocksPerUser(std::uint16_t count) noexcept {
    for (auto& table : thread_tables_) {
      table.SetMaxBlocksPerUser(count);
    }
  }

  void SetVectorConfig(std::uint16_t batch, std::uint8_t prefetch) noexcept {
    for (auto& table : thread_tables_) {
      table.SetVectorConfig(batch, prefetch);
    }
  }

  void SetMaxSessions(std::size_t max_sessions) noexcept {
    for (auto& table : thread_tables_) {
      table.SetMaxSessions(max_sessions);
    }
  }

  void SetLazyUpdates(bool enabled) noexcept {
    for (auto& table : thread_tables_) {
      table.SetLazyUpdates(enabled);
    }
  }

  void SetInlineValues(bool enabled) noexcept {
    for (auto& table : thread_tables_) {
      table.SetInlineValues(enabled);
    }
  }

  void SetHotCache(std::size_t entries) noexcept {
    for (auto& table : thread_tables_) {
      table.SetHotCache(entries);
    }
  }

  void SetHaEnabled(bool enabled) noexcept {
    for (auto& table : thread_tables_) {
      table.SetHaEnabled(enabled);
    }
  }

  [[nodiscard]] std::uint64_t HaDropped() const noexcept {
    std::uint64_t total{0};
    for (const auto& table : thread_tables_) {
      total += table.HaDropped();
    }
    return total;
  }

  void ExportSessions(nat_session_export_cb_t cb, void* ctx) const noexcept {
    for (const auto& table : thread_tables_) {
      table.ExportSessions(cb, ctx);
    }
  }

  bool ImportSession(const nat_session_record_t& record) noexcept {
    if (record.thread_index >= thread_tables_.size()) return false;
    return thread_tables_[record.thread_index].ImportSession(record);
  }

  void FlushSessions() noexcept {
    for (auto& table : thread_tables_) {
      table.FlushSessions();
    }
  }

  bool ReconfigureSessions(std::size_t sessions_per_worker) noexcept {
    if (sessions_per_worker == 0) return false;
    if (sessions_per_worker == sessions_per_worker_) return true;
    for (const auto& table : thread_tables_) {
      if (table.SessionCount() != 0) return false;
    }

    std::vector<ThreadSessionTable> tables;
    tables.reserve(thread_tables_.size());
    for (std::size_t i{0}; i < thread_tables_.size(); ++i) {
      tables.emplace_back(sessions_per_worker, expected_active_);
      tables.back().SetThreadIndex(static_cast<std::uint32_t>(i));
      tables.back().SetMaxSessions(sessions_per_worker);
    }
    for (const auto& table : tables) {
      if (!table.IsValid()) return false;
    }
    thread_tables_ = std::move(tables);
    sessions_per_worker_ = sessions_per_worker;
    SetPool(pool_start_ip_, pool_end_ip_);
    for (auto& table : thread_tables_) {
      table.SetTimeouts(timeouts_);
    }
    return true;
  }

  [[nodiscard]] TableTelemetry GetAggregatedTelemetry() const noexcept {
    TableTelemetry total{};
    for (const auto& t : thread_tables_) {
      const auto& tel = t.GetTelemetry();
      total.packets_processed += tel.packets_processed;
      total.new_sessions_created += tel.new_sessions_created;
      total.create_failures += tel.create_failures;
      total.existing_session_hits += tel.existing_session_hits;
      total.fast_path_hits += tel.fast_path_hits;
      total.hot_cache_hits += tel.hot_cache_hits;
      total.lru_evictions += tel.lru_evictions;
      total.expired_reclaims += tel.expired_reclaims;
      total.active_sessions_high_watermark += tel.active_sessions_high_watermark;
      total.pba_blocks_allocated += tel.pba_blocks_allocated;
      total.pba_blocks_freed += tel.pba_blocks_freed;
      total.pba_port_alloc_failures += tel.pba_port_alloc_failures;
      total.bihash_insert_failures += tel.bihash_insert_failures;
      total.fragment_drops += tel.fragment_drops;
      total.malformed_drops += tel.malformed_drops;
      total.out2in_misses += tel.out2in_misses;
      total.slow_path_packets += tel.slow_path_packets;
    }
    return total;
  }

  void SetForceEvictOnOverflow(bool enabled) noexcept {
    force_evict_on_overflow_ = enabled;
    for (auto& table : thread_tables_) {
      table.SetForceEvictOnOverflow(enabled);
    }
  }
  [[nodiscard]] bool ForceEvictOnOverflow() const noexcept { return force_evict_on_overflow_; }

 private:
  std::vector<ThreadSessionTable> thread_tables_;
  std::size_t sessions_per_worker_{2'000'000};
  std::size_t expected_active_{0};
  SessionTimeouts timeouts_{};
  std::uint32_t pool_start_ip_{0xCB007101};
  std::uint32_t pool_end_ip_{0xCB007120};
  bool force_evict_on_overflow_{false};
};

}  // namespace nat::engine
