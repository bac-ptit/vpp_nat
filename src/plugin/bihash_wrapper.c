#include "bihash_wrapper.h"

#include <stdlib.h>
#include <string.h>
#include <vppinfra/mem.h>
#include <vppinfra/os.h>

#define clib_bihash_init2_16_8 clib_bihash_init2_16_8_orig
#include <vppinfra/bihash_16_8.h>
#include <vppinfra/bihash_template.c>
#undef clib_bihash_init2_16_8

struct nat_bihash_16_8 {
  BVT (clib_bihash) h;
};

static __thread int s_thread_mem_initialized = 0;

nat_bihash_16_8_t* nat_bihash_create(const char* name, uint32_t nbuckets, uint64_t memory_size) {
  if (!s_thread_mem_initialized) {
    if (!clib_mem_get_heap()) {
      clib_mem_init(0, 6ULL << 30); // 6GB memory heap for Bihash allocations
      clib_mem_thread_init();
    }
    s_thread_mem_initialized = 1;
  }

  nat_bihash_16_8_t* wrap = (nat_bihash_16_8_t*)malloc(sizeof(nat_bihash_16_8_t));
  if (!wrap) return NULL;
  memset(&wrap->h, 0, sizeof(wrap->h));

  BVT (clib_bihash_init2_args) a;
  memset(&a, 0, sizeof(a));
  a.h = &wrap->h;
  a.name = (char*)name;
  a.nbuckets = 1 << (max_log2 (nbuckets));
  a.memory_size = memory_size;
  a.dont_add_to_all_bihash_list = 1;

  wrap->h.name = (u8 *) a.name;
  wrap->h.nbuckets = a.nbuckets;
  wrap->h.log2_nbuckets = (u32)max_log2 (a.nbuckets);
  wrap->h.memory_size = a.memory_size;
  wrap->h.instantiated = 0;
  wrap->h.dont_add_to_all_bihash_list = 1;
  alloc_arena (&wrap->h) = 0;
  wrap->h.alloc_lock = clib_mem_alloc_aligned (CLIB_CACHE_LINE_BYTES, CLIB_CACHE_LINE_BYTES);
  wrap->h.alloc_lock[0] = 0;
  BV (clib_bihash_instantiate) (&wrap->h);
  return wrap;
}

void nat_bihash_destroy(nat_bihash_16_8_t* wrap) {
  if (wrap) {
    BV (clib_bihash_free) (&wrap->h);
    free(wrap);
  }
}

int nat_bihash_search(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key, nat_bihash_kv_t* value_result) {
  clib_bihash_kv_16_8_t in_out_kv;
  in_out_kv.key[0] = search_key->key[0];
  in_out_kv.key[1] = search_key->key[1];
  in_out_kv.value = 0;
  int rv = BV (clib_bihash_search_inline) (&wrap->h, &in_out_kv);
  if (rv == 0 && value_result) {
    value_result->key[0] = in_out_kv.key[0];
    value_result->key[1] = in_out_kv.key[1];
    value_result->value = in_out_kv.value;
  }
  return rv;
}

int nat_bihash_add_del(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* kv, int is_add) {
  clib_bihash_kv_16_8_t in_kv;
  in_kv.key[0] = kv->key[0];
  in_kv.key[1] = kv->key[1];
  in_kv.value = kv->value;
  return BV (clib_bihash_add_del) (&wrap->h, &in_kv, is_add);
}

int nat_bihash_add_del_with_hash(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* kv,
                                 uint64_t hash, int is_add) {
  clib_bihash_kv_16_8_t in_kv;
  in_kv.key[0] = kv->key[0];
  in_kv.key[1] = kv->key[1];
  in_kv.value = kv->value;
  return BV (clib_bihash_add_del_with_hash) (&wrap->h, &in_kv, hash, is_add);
}

void nat_bihash_prefetch_bucket(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key) {
  if (!wrap || !search_key) return;
  clib_bihash_kv_16_8_t in_kv;
  in_kv.key[0] = search_key->key[0];
  in_kv.key[1] = search_key->key[1];
  u64 hash = BV (clib_bihash_hash) (&in_kv);
  BV (clib_bihash_prefetch_bucket) (&wrap->h, hash);
}

void nat_bihash_prefetch_data(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key) {
  if (!wrap || !search_key) return;
  clib_bihash_kv_16_8_t in_kv;
  in_kv.key[0] = search_key->key[0];
  in_kv.key[1] = search_key->key[1];
  u64 hash = BV (clib_bihash_hash) (&in_kv);
  BV (clib_bihash_prefetch_data) (&wrap->h, hash);
}

uint64_t nat_bihash_hash(const nat_bihash_kv_t* key) {
  if (!key) return 0;
  clib_bihash_kv_16_8_t in_kv;
  in_kv.key[0] = key->key[0];
  in_kv.key[1] = key->key[1];
  in_kv.value = 0;
  return BV (clib_bihash_hash) (&in_kv);
}

int nat_bihash_search_with_hash(nat_bihash_16_8_t* wrap, const nat_bihash_kv_t* search_key,
                                uint64_t hash, nat_bihash_kv_t* value_result) {
  if (!wrap || !search_key) return -1;
  clib_bihash_kv_16_8_t in_out_kv;
  in_out_kv.key[0] = search_key->key[0];
  in_out_kv.key[1] = search_key->key[1];
  in_out_kv.value = 0;
  const int rv = BV (clib_bihash_search_inline_with_hash) (&wrap->h, hash, &in_out_kv);
  if (rv == 0 && value_result) {
    value_result->value = in_out_kv.value;
  }
  return rv;
}

void nat_bihash_prefetch_bucket_hash(nat_bihash_16_8_t* wrap, uint64_t hash) {
  if (!wrap) return;
  BV (clib_bihash_prefetch_bucket) (&wrap->h, hash);
}

void nat_bihash_prefetch_data_hash(nat_bihash_16_8_t* wrap, uint64_t hash) {
  if (!wrap) return;
  BV (clib_bihash_prefetch_data) (&wrap->h, hash);
}
