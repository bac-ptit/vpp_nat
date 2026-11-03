#ifndef __PBA_BITMAP_H__
#define __PBA_BITMAP_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

#if !defined(NDEBUG) || defined(DEBUG) || defined(NAT_DEBUG)
#define PBA_DEBUG_CHECKS 1
#else
#define PBA_DEBUG_CHECKS 0
#endif

#define PBA_DEFAULT_BLOCK_SIZE 64
#define PBA_MIN_PORT 1024
#define PBA_MAX_PORT 65535

/**
 * @brief Represents a contiguous block of allocated ports (32 or 64) for a subscriber.
 */
typedef struct {
  uint16_t base_port;    /* Starting port of this contiguous block */
  uint16_t block_size;   /* Sizing: 32 or 64 ports */
  uint32_t pad;
  uint64_t bitmap;       /* 64-bit allocation bitmask: 1 = allocated, 0 = available */
  uint64_t last_used_ts; /* Timestamp of most recent packet/session */
} pba_block_t;

/**
 * @brief Initializes a PBA block descriptor.
 */
static inline void pba_block_init(pba_block_t* block, uint16_t base_port,
                                  uint16_t block_size) {
#ifdef __cplusplus
  *block = pba_block_t{
      .base_port = base_port,
      .block_size = (block_size == 32) ? (uint16_t)32 : (uint16_t)64,
      .pad = 0,
      .bitmap = 0ULL,
      .last_used_ts = 0,
  };
#else
  *block = (pba_block_t){
      .base_port = base_port,
      .block_size = (block_size == 32) ? (uint16_t)32 : (uint16_t)64,
      .pad = 0,
      .bitmap = 0ULL,
      .last_used_ts = 0,
  };
#endif
}

/**
 * @brief Fast bitwise port acquisition from a PBA block.
 *
 * Uses hardware CTZ (count trailing zeros) for O(1) allocation without branch loops.
 *
 * @param block Pointer to PBA block
 * @param out_port Pointer to store the allocated port number
 * @return true if allocation succeeded, false if block is fully consumed
 */
static inline bool pba_alloc_port(pba_block_t* block, uint16_t* out_port) {
#if PBA_DEBUG_CHECKS
  if (__builtin_expect(!block || !out_port, 0)) {
    return false;
  }
#endif
  const uint64_t mask = (block->block_size == 32) ? 0xFFFFFFFFULL : ~0ULL;
  const uint64_t free_bits = (~block->bitmap) & mask;

  if (__builtin_expect(free_bits == 0, 0)) {
    return false; /* Block is saturated */
  }

  const int port_index = __builtin_ctzll(free_bits);
  if (__builtin_expect(port_index >= block->block_size, 0)) {
    return false;
  }

  block->bitmap |= (1ULL << port_index);
  *out_port = block->base_port + (uint16_t)port_index;
  return true;
}

/**
 * @brief Bitwise port release back to the PBA block.
 *
 * @param block Pointer to PBA block
 * @param port Port number to release
 * @return true if port belonged to this block and was released, false otherwise
 */
static inline bool pba_free_port(pba_block_t* block, uint16_t port) {
#if PBA_DEBUG_CHECKS
  if (__builtin_expect(!block, 0)) {
    return false;
  }
#endif
  if (__builtin_expect(port < block->base_port || port >= block->base_port + block->block_size,
                       0)) {
    return false;
  }

  const int port_index = (int)(port - block->base_port);
  const uint64_t bit = (1ULL << port_index);

  if ((block->bitmap & bit) == 0) {
    return false; /* Not allocated / double free */
  }

  block->bitmap &= ~bit;
  return true;
}

/**
 * @brief Checks if all ports in the block are free.
 */
static inline bool pba_is_empty(const pba_block_t* block) {
  return block->bitmap == 0ULL;
}

/**
 * @brief Checks if all ports in the block are allocated.
 */
static inline bool pba_is_full(const pba_block_t* block) {
  const uint64_t mask = (block->block_size == 32) ? 0xFFFFFFFFULL : ~0ULL;
  return (block->bitmap & mask) == mask;
}

/**
 * @brief Returns the count of allocated ports in this block.
 */
static inline uint32_t pba_used_count(const pba_block_t* block) {
  const uint64_t mask = (block->block_size == 32) ? 0xFFFFFFFFULL : ~0ULL;
  return (uint32_t)__builtin_popcountll(block->bitmap & mask);
}

/**
 * @brief Returns the count of remaining available ports in this block.
 */
static inline uint32_t pba_available_count(const pba_block_t* block) {
  return (uint32_t)block->block_size - pba_used_count(block);
}

#ifdef __cplusplus
}
#endif

#endif /* __PBA_BITMAP_H__ */
