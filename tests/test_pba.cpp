#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "../src/plugin/pba_bitmap.h"
#include "helpers/print.hpp"

int main() {
  nat::io::println("========================================");
  nat::io::println("  Running Unit Test: PBA 64-bit Bitmap  ");
  nat::io::println("========================================");

  constexpr std::uint16_t kBasePort = 5000;
  constexpr std::uint16_t kBlockSize = 64;

  pba_block_t block{
      .base_port{kBasePort},
      .block_size{kBlockSize},
      .pad{0},
      .bitmap{0ULL},
      .last_used_ts{0},
  };

  assert(pba_is_empty(&block));
  assert(!pba_is_full(&block));
  assert(pba_used_count(&block) == 0);
  assert(pba_available_count(&block) == 64);

  // Allocate all 64 ports
  std::array<std::uint16_t, 64> allocated_ports{};
  for (std::size_t i{0}; i < allocated_ports.size(); ++i) {
    std::uint16_t p{0};
    const bool ok{pba_alloc_port(&block, &p)};
    assert(ok);
    assert(p == kBasePort + i);
    allocated_ports[i] = p;
  }
  (void)allocated_ports;

  // Block must be full
  assert(pba_is_full(&block));
  assert(!pba_is_empty(&block));
  assert(pba_used_count(&block) == 64);
  assert(pba_available_count(&block) == 0);

  // 65th allocation must fail
  uint16_t overflow_port{0};
  assert(!pba_alloc_port(&block, &overflow_port));

  // Free port 5 (5005) and port 42 (5042)
  assert(pba_free_port(&block, 5005));
  assert(pba_free_port(&block, 5042));
  assert(!pba_is_full(&block));
  assert(pba_used_count(&block) == 62);
  assert(pba_available_count(&block) == 2);

  // Double free must fail
  assert(!pba_free_port(&block, 5005));

  // Out of range free must fail
  assert(!pba_free_port(&block, 4999));
  assert(!pba_free_port(&block, 5064));

  // Re-allocate should return 5005 first (lowest free bit index = 5)
  uint16_t realloc_port1{0};
  assert(pba_alloc_port(&block, &realloc_port1));
  assert(realloc_port1 == 5005);

  // Next re-allocate should return 5042
  uint16_t realloc_port2{0};
  assert(pba_alloc_port(&block, &realloc_port2));
  assert(realloc_port2 == 5042);

  assert(pba_is_full(&block));

  nat::io::println("[+] All PBA Bitmap allocation and bitmask tests passed!");
  return 0;
}
