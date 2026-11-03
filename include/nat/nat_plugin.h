#ifndef __NAT_PLUGIN_H__
#define __NAT_PLUGIN_H__

#ifdef __cplusplus
extern "C" {
#define NAT_NOEXCEPT noexcept
#else
#define NAT_NOEXCEPT
#endif

#include <stddef.h>
#include <stdint.h>

/* Opaque pointer to C++ NAT Engine */
typedef struct nat_engine nat_engine_t;

/* Translation outcomes for packet processing graph nodes */
typedef enum {
  NAT_TRANSLATE_OK = 0,
  NAT_TRANSLATE_DROP = 1,
  NAT_TRANSLATE_SLOW_PATH = 2,
} nat_action_t;

/* Plugin and engine lifecycle */
nat_engine_t* nat_engine_create(uint32_t num_threads) NAT_NOEXCEPT;
nat_engine_t* nat_engine_create_with_capacity(uint32_t num_threads,
                                              uint32_t sessions_per_worker) NAT_NOEXCEPT;
nat_engine_t* nat_engine_create_with_capacity_and_active(
    uint32_t num_threads, uint32_t sessions_per_worker,
    uint32_t expected_active) NAT_NOEXCEPT;
void nat_engine_destroy(nat_engine_t* engine) NAT_NOEXCEPT;

/* Packet translation bridges called from VPP graph nodes */
nat_action_t nat_translate_in2out(nat_engine_t* engine, uint32_t thread_index, void* ip4_hdr,
                                  void* l4_hdr, uint8_t proto) NAT_NOEXCEPT;

nat_action_t nat_translate_out2in(nat_engine_t* engine, uint32_t thread_index, void* ip4_hdr,
                                  void* l4_hdr, uint8_t proto) NAT_NOEXCEPT;

/* Batched translation: prefetches buckets/data for the whole batch before lookup. */
void nat_translate_in2out_batch(nat_engine_t* engine, uint32_t thread_index,
                                void** ip4_headers, void** l4_headers, uint8_t* protos,
                                nat_action_t* actions, uint32_t count) NAT_NOEXCEPT;
void nat_translate_out2in_batch(nat_engine_t* engine, uint32_t thread_index,
                                void** ip4_headers, void** l4_headers, uint8_t* protos,
                                nat_action_t* actions, uint32_t count) NAT_NOEXCEPT;

bool nat_engine_delete_session(nat_engine_t* engine, uint32_t thread_index, void* ip4_hdr,
                               void* l4_hdr, uint8_t proto) NAT_NOEXCEPT;

/* Expire idle sessions from the worker's LRU head. Call once per frame/tick. */
void nat_engine_process_tick(nat_engine_t* engine, uint32_t thread_index) NAT_NOEXCEPT;

/* HA session record exchanged between active and standby. */
typedef struct {
  uint64_t last_active_ts;
  uint32_t thread_index;
  uint32_t inside_src_ip;
  uint32_t inside_dst_ip;
  uint32_t outside_src_ip;
  uint32_t outside_dst_ip;
  uint16_t inside_src_port;
  uint16_t inside_dst_port;
  uint16_t outside_src_port;
  uint16_t outside_dst_port;
  uint16_t block_base_port;
  uint16_t block_size;
  uint16_t flags;
  uint16_t generation;
  uint8_t protocol;
  uint8_t pad[3];
} nat_session_record_t;

typedef void (*nat_session_export_cb_t)(void* ctx, const nat_session_record_t* record);

void nat_engine_set_ha_enabled(nat_engine_t* engine, bool enabled) NAT_NOEXCEPT;
void nat_engine_set_lazy_updates(nat_engine_t* engine, bool enabled) NAT_NOEXCEPT;
void nat_engine_set_inline_values(nat_engine_t* engine, bool enabled) NAT_NOEXCEPT;
void nat_engine_set_hot_cache(nat_engine_t* engine, uint32_t entries) NAT_NOEXCEPT;
void nat_engine_export_sessions(const nat_engine_t* engine, nat_session_export_cb_t cb,
                                void* ctx) NAT_NOEXCEPT;
bool nat_engine_import_session(nat_engine_t* engine,
                               const nat_session_record_t* record) NAT_NOEXCEPT;
void nat_engine_flush_sessions(nat_engine_t* engine) NAT_NOEXCEPT;

/* Telemetry and stats */
typedef struct {
  uint64_t packets_processed;
  uint64_t new_sessions_created;
  uint64_t create_failures;
  uint64_t existing_session_hits;
  uint64_t fast_path_hits;
  uint64_t hot_cache_hits;
  uint64_t lru_evictions;
  uint64_t expired_reclaims;
  uint64_t active_sessions_high_watermark;
  uint64_t pba_blocks_allocated;
  uint64_t pba_blocks_freed;
  uint64_t pba_port_alloc_failures;
  uint64_t bihash_insert_failures;
  uint64_t fragment_drops;
  uint64_t malformed_drops;
  uint64_t out2in_misses;
  uint64_t slow_path_packets;
} nat_telemetry_t;

uint64_t nat_engine_get_total_sessions(const nat_engine_t* engine) NAT_NOEXCEPT;
uint64_t nat_engine_get_total_translations(const nat_engine_t* engine) NAT_NOEXCEPT;
nat_telemetry_t nat_engine_get_telemetry(const nat_engine_t* engine) NAT_NOEXCEPT;
void nat_engine_set_timeouts(nat_engine_t* engine, uint32_t udp_sec,
                             uint32_t tcp_est_sec, uint32_t tcp_trans_sec,
                             uint32_t icmp_sec) NAT_NOEXCEPT;

/* Runtime configuration. Public pool addresses are in host byte order. */
void nat_engine_set_pool(nat_engine_t* engine, uint32_t start_ip, uint32_t end_ip) NAT_NOEXCEPT;
void nat_engine_set_block_size(nat_engine_t* engine, uint16_t block_size) NAT_NOEXCEPT;
void nat_engine_set_max_blocks_per_user(nat_engine_t* engine, uint16_t max_blocks) NAT_NOEXCEPT;
void nat_engine_set_vector_config(nat_engine_t* engine, uint16_t batch_size,
                                  uint8_t prefetch_distance) NAT_NOEXCEPT;
/* Rebuilds all worker tables; fails if any session already exists. */
bool nat_engine_set_sessions_per_worker(nat_engine_t* engine,
                                        uint32_t sessions_per_worker) NAT_NOEXCEPT;
void nat_engine_set_max_sessions_per_worker(nat_engine_t* engine,
                                            uint32_t max_sessions) NAT_NOEXCEPT;
void nat_engine_set_force_eviction(nat_engine_t* engine, bool enabled) NAT_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* __NAT_PLUGIN_H__ */
