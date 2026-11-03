/*
 * Experimental flat-hash backend for the NAT session table.
 *
 * Same ABI as bihash_wrapper.c so the engine can be built with either
 * backend. This is a research path: it is per-worker, not thread-safe across
 * writers, and uses a 32-bit fingerprint plus full 128-bit key comparison.
 */
#include "bihash_wrapper.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FLAT_EMPTY 0xFFFFFFFFu

typedef struct {
  uint64_t hash;
  uint32_t value;
  uint32_t pad;
} flat_slot_t;

struct nat_bihash_16_8 {
  flat_slot_t *slots;
  uint32_t capacity;
  uint32_t mask;
  uint32_t size;
};

static inline uint64_t flat_hash(const uint64_t key[2]) {
  uint64_t h = key[0] * 0x9E3779B97F4A7C15ULL;
  h ^= key[1] + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h;
}

static inline int hash_equal(const flat_slot_t *slot, uint64_t hash) {
  return slot->hash == hash;
}

static uint32_t next_pow2(uint32_t value) {
  uint32_t result = 1;
  while (result < value) result <<= 1;
  return result;
}

nat_bihash_16_8_t *nat_bihash_create(const char *name, uint32_t nbuckets,
                                     uint64_t memory_size) {
  (void)name;
  (void)memory_size;
  const uint32_t requested = nbuckets < 1024 ? 1024 : nbuckets;
  const uint32_t capacity = next_pow2(requested + (requested / 2));
  nat_bihash_16_8_t *wrap = (nat_bihash_16_8_t *)calloc(1, sizeof(*wrap));
  if (wrap == NULL) return NULL;
  wrap->slots = (flat_slot_t *)malloc(sizeof(flat_slot_t) * capacity);
  if (wrap->slots == NULL) {
    free(wrap);
    return NULL;
  }
  wrap->capacity = capacity;
  wrap->mask = capacity - 1;
  for (uint32_t i = 0; i < capacity; ++i) wrap->slots[i].value = FLAT_EMPTY;
  return wrap;
}

void nat_bihash_destroy(nat_bihash_16_8_t *wrap) {
  if (wrap == NULL) return;
  free(wrap->slots);
  free(wrap);
}

static int flat_insert(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *kv) {
  const uint64_t hash = flat_hash(kv->key);
  uint32_t pos = (uint32_t)hash & wrap->mask;
  for (uint32_t probe = 0; probe < wrap->capacity; ++probe) {
    flat_slot_t *slot = &wrap->slots[pos];
    if (slot->value == FLAT_EMPTY) {
      slot->value = (uint32_t)kv->value;
      slot->hash = hash;
      ++wrap->size;
      return 0;
    }
    if (hash_equal(slot, hash)) {
      slot->value = (uint32_t)kv->value;
      return 0;
    }
    pos = (pos + 1) & wrap->mask;
  }
  return -1;
}

static int flat_search(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *key,
                       uint64_t hash, nat_bihash_kv_t *value_result) {
  (void)key;
  uint32_t pos = (uint32_t)hash & wrap->mask;
  for (uint32_t probe = 0; probe < wrap->capacity; ++probe) {
    const flat_slot_t *slot = &wrap->slots[pos];
    if (slot->value == FLAT_EMPTY) return -1;
    if (hash_equal(slot, hash)) {
      if (value_result != NULL) {
        value_result->key[0] = hash;
        value_result->key[1] = 0;
        value_result->value = slot->value;
      }
      return 0;
    }
    pos = (pos + 1) & wrap->mask;
  }
  return -1;
}

static int flat_delete(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *key) {
  const uint64_t hash = flat_hash(key->key);
  uint32_t hole = (uint32_t)hash & wrap->mask;
  for (uint32_t probe = 0; probe < wrap->capacity; ++probe) {
    flat_slot_t *slot = &wrap->slots[hole];
    if (slot->value == FLAT_EMPTY) return -1;
    if (hash_equal(slot, hash)) break;
    hole = (hole + 1) & wrap->mask;
  }
  if (wrap->slots[hole].value == FLAT_EMPTY) return -1;

  uint32_t next = (hole + 1) & wrap->mask;
  while (wrap->slots[next].value != FLAT_EMPTY) {
    const uint32_t ideal = wrap->slots[next].hash & wrap->mask;
    int move;
    if (hole <= next) {
      move = ideal > hole && ideal <= next;
    } else {
      move = ideal > hole || ideal <= next;
    }
    if (!move) {
      wrap->slots[hole] = wrap->slots[next];
      hole = next;
    }
    next = (next + 1) & wrap->mask;
  }
  wrap->slots[hole].value = FLAT_EMPTY;
  --wrap->size;
  return 0;
}

int nat_bihash_search(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *search_key,
                      nat_bihash_kv_t *value_result) {
  if (wrap == NULL || search_key == NULL) return -1;
  return flat_search(wrap, search_key, flat_hash(search_key->key), value_result);
}

int nat_bihash_add_del(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *kv,
                       int is_add) {
  if (wrap == NULL || kv == NULL) return -1;
  return is_add ? flat_insert(wrap, kv) : flat_delete(wrap, kv);
}

int nat_bihash_add_del_with_hash(nat_bihash_16_8_t *wrap, const nat_bihash_kv_t *kv,
                                 uint64_t hash, int is_add) {
  (void)hash;
  return nat_bihash_add_del(wrap, kv, is_add);
}

uint64_t nat_bihash_hash(const nat_bihash_kv_t *key) {
  return key == NULL ? 0 : flat_hash(key->key);
}

int nat_bihash_search_with_hash(nat_bihash_16_8_t *wrap,
                                const nat_bihash_kv_t *search_key, uint64_t hash,
                                nat_bihash_kv_t *value_result) {
  if (wrap == NULL || search_key == NULL) return -1;
  return flat_search(wrap, search_key, hash, value_result);
}

void nat_bihash_prefetch_bucket(nat_bihash_16_8_t *wrap,
                                const nat_bihash_kv_t *search_key) {
  if (wrap == NULL || search_key == NULL) return;
  const uint64_t hash = flat_hash(search_key->key);
  __builtin_prefetch(&wrap->slots[(uint32_t)hash & wrap->mask], 0, 3);
}

void nat_bihash_prefetch_data(nat_bihash_16_8_t *wrap,
                              const nat_bihash_kv_t *search_key) {
  nat_bihash_prefetch_bucket(wrap, search_key);
}

void nat_bihash_prefetch_bucket_hash(nat_bihash_16_8_t *wrap, uint64_t hash) {
  if (wrap == NULL) return;
  __builtin_prefetch(&wrap->slots[(uint32_t)hash & wrap->mask], 0, 3);
}

void nat_bihash_prefetch_data_hash(nat_bihash_16_8_t *wrap, uint64_t hash) {
  nat_bihash_prefetch_bucket_hash(wrap, hash);
}
