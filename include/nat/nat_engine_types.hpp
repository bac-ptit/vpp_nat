#pragma once

#include <cstdint>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#endif

namespace nat::engine {

struct FiveTuple {
  std::uint32_t src_ip{0};
  std::uint32_t dst_ip{0};
  std::uint16_t src_port{0};
  std::uint16_t dst_port{0};
  std::uint8_t protocol{0};

  bool operator==(const FiveTuple&) const noexcept = default;
};

struct NatSession {
  FiveTuple inside{};
  FiveTuple outside{};
  std::uint64_t last_active_ts{0};
  std::uint64_t total_packets{0};
  std::uint64_t total_bytes{0};
  std::uint16_t flags{0};
  std::uint16_t generation{0};
  std::uint32_t last_lru_move_sec{0};
};

static_assert(sizeof(NatSession) == 64, "NatSession must be exactly 64 bytes (1 cache line)");

inline constexpr std::uint32_t kSessionFlagFree = 0;
inline constexpr std::uint32_t kSessionFlagActive = 1U << 0;
inline constexpr std::uint32_t kSessionFlagRetiring = 1U << 1;
inline constexpr std::uint32_t kSessionFlagTcpEstablished = 1U << 2;

struct FiveTupleHash {
  [[nodiscard]] std::size_t operator()(const FiveTuple& t) const noexcept {
#if defined(__SSE4_2__)
    const std::uint64_t w0{(static_cast<std::uint64_t>(t.src_ip) << 32) |
                           static_cast<std::uint64_t>(t.dst_ip)};
    const std::uint64_t w1{(static_cast<std::uint64_t>(t.src_port) << 48) |
                           (static_cast<std::uint64_t>(t.dst_port) << 32) |
                           static_cast<std::uint64_t>(t.protocol)};
    return _mm_crc32_u64(_mm_crc32_u64(0, w0), w1);
#else
    // Fallback portable hash combination
    std::size_t h{14695981039346656037ULL};
    auto mix = [&h](std::uint64_t val) {
      h ^= val;
      h *= 1099511628211ULL;
    };
    mix(t.src_ip);
    mix(t.dst_ip);
    mix((static_cast<std::uint32_t>(t.src_port) << 16) | t.dst_port);
    mix(t.protocol);
    return h;
#endif
  }
};

}  // namespace nat::engine

namespace std {
template <>
struct hash<nat::engine::FiveTuple> {
  std::size_t operator()(const nat::engine::FiveTuple& t) const noexcept {
    return nat::engine::FiveTupleHash{}(t);
  }
};
}  // namespace std
