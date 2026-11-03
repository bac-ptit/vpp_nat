#ifndef NAT_BIHASH_WRAPPER_H
#define NAT_BIHASH_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nat_bihash_16_8 nat_bihash_16_8_t;

typedef struct {
  uint64_t key[2];
  uint64_t value;
} nat_bihash_kv_t;

/**
 * @brief Creates a VPP clib_bihash_16_8 table instance.
 */
nat_bihash_16_8_t* nat_bihash_create(const char* name, uint32_t nbuckets, uint64_t memory_size);

/**
 * @brief Destroys and frees a bihash instance.
 */
void nat_bihash_destroy(nat_bihash_16_8_t* wrap);

/**
 * @brief Performs inline search for a key.
 * @return 0 on HIT (value_result populated), non-zero on MISS.
 */
int nat_bihash_search(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key, nat_bihash_kv_t* value_result);

/**
 * @brief Inserts or removes a key-value pair from bihash.
 * @param is_add 1 to insert, 0 to delete.
 * @return 0 on success, non-zero on failure.
 */
int nat_bihash_add_del(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* kv, int is_add);
int nat_bihash_add_del_with_hash(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* kv,
                                 uint64_t hash, int is_add);

/**
 * @brief Prefetches the bihash bucket corresponding to search_key.
 */
void nat_bihash_prefetch_bucket(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key);

/**
 * @brief Prefetches the bihash data/value corresponding to search_key.
 */
void nat_bihash_prefetch_data(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key);

/**
 * @brief Computes the bihash hash for a key so it can be reused across pipeline stages.
 */
uint64_t nat_bihash_hash(const nat_bihash_kv_t* key);

/**
 * @brief Searches using a precomputed hash (avoids hashing twice in a burst).
 */
int nat_bihash_search_with_hash(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key,
                                uint64_t hash, nat_bihash_kv_t* value_result);

void nat_bihash_prefetch_bucket_hash(nat_bihash_16_8_t* wrap, uint64_t hash);
void nat_bihash_prefetch_data_hash(nat_bihash_16_8_t* wrap, uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif  // NAT_BIHASH_WRAPPER_H
