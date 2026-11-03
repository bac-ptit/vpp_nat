#include <stdio.h>

#include <vlib/unix/plugin.h>
#include <vlib/vlib.h>
#include <vnet/feature/feature.h>
#include <vnet/interface.h>
#include <vnet/ip/ip.h>
#include <vnet/ip/ip4_packet.h>
#include <vnet/vnet.h>

#include "nat/nat_plugin.h"
#include "nat_ha_sync.h"

VLIB_PLUGIN_REGISTER() = {
    .version = "0.1.0",
    .description = "High-Performance HA NAT44 PBA Data Plane Plugin",
};

typedef enum {
  NAT_NEXT_DROP,
  NAT_NEXT_LOOKUP,
  NAT_NEXT_SLOW_PATH,
  NAT_N_NEXT,
} nat_next_t;

/* Global NAT engine instance */
nat_engine_t* global_nat_engine = NULL;

static clib_error_t* nat_pba_init(vlib_main_t* vm) {
  (void)vm;
  u32 n_threads = vlib_get_n_threads();
  const u32 n_worker_slots = vlib_num_workers() + 1;
  if (n_worker_slots > n_threads) {
    n_threads = n_worker_slots;
  }
  global_nat_engine = nat_engine_create(n_threads);
  return global_nat_engine ? 0 : clib_error_return(0, "Failed to allocate C++ NAT Engine");
}

VLIB_INIT_FUNCTION(nat_pba_init);

/* Trivial standard-layout struct wrapping packet state (Zero-cost abstraction) */
typedef struct {
  u32 bi;
  vlib_buffer_t* b;
  ip4_header_t* ip;
  void* l4;
  u32 next;
} nat_pkt_t;

typedef nat_action_t (*nat_translate_fn_t)(nat_engine_t*, u32, void*, void*, u8);
typedef void (*nat_translate_batch_fn_t)(nat_engine_t*, u32, void**, void**, u8*,
                                         nat_action_t*, u32);

static inline void nat_prefetch_pipeline(vlib_main_t* vm, const u32* from, u32 n_left_from) {
  /* Stage 1: If at least 8 packets remain in frame, prefetch packet payload data for from[4..7] */
  if (PREDICT_TRUE(n_left_from >= 8)) {
    for (int i = 4; i < 8; ++i) {
      vlib_buffer_t* b = vlib_get_buffer(vm, from[i]);
      CLIB_PREFETCH(b->data, CLIB_CACHE_LINE_BYTES, LOAD);
    }
  }
  /* Stage 2: If at least 12 packets remain in frame, prefetch buffer header descriptors for from[8..11] */
  if (PREDICT_TRUE(n_left_from >= 12)) {
    for (int i = 8; i < 12; ++i) {
      vlib_buffer_t* b = vlib_get_buffer(vm, from[i]);
      vlib_prefetch_buffer_header(b, LOAD);
    }
  }
}

static inline nat_pkt_t nat_pkt_init(vlib_main_t* vm, u32 bi) {
  vlib_buffer_t* b = vlib_get_buffer(vm, bi);
  ip4_header_t* ip = (ip4_header_t*)vlib_buffer_get_current(b);
  return (nat_pkt_t){
      .bi = bi,
      .b = b,
      .ip = ip,
      .l4 = (void*)((u8*)ip + ip4_header_bytes(ip)),
      .next = NAT_NEXT_DROP,
  };
}

/* Frame cursor encapsulating VPP ring pointers and counters */
typedef struct {
  u32* from;
  u32* to_next;
  u32 n_left_from;
  u32 n_left_to_next;
  u32 next_index;
} nat_frame_cursor_t;

static inline u32 nat_action_to_next(nat_action_t act) {
  if (PREDICT_TRUE(act == NAT_TRANSLATE_OK)) return NAT_NEXT_LOOKUP;
  if (act == NAT_TRANSLATE_SLOW_PATH) return NAT_NEXT_SLOW_PATH;
  return NAT_NEXT_DROP;
}

#define NAT_STEP_QUAD(dir, batch_fn)                                                          \
  static inline void nat_step_quad_##dir(vlib_main_t* vm, vlib_node_runtime_t* node,          \
                                         nat_frame_cursor_t* cur, nat_engine_t* eng, u32 tid) { \
    nat_prefetch_pipeline(vm, cur->from, cur->n_left_from);                                   \
    nat_pkt_t p[4];                                                                            \
    for (int i = 0; i < 4; ++i) {                                                             \
      p[i] = nat_pkt_init(vm, cur->from[i]);                                                  \
      cur->to_next[i] = p[i].bi;                                                              \
    }                                                                                          \
    cur->from += 4;                                                                            \
    cur->n_left_from -= 4;                                                                     \
    cur->to_next += 4;                                                                         \
    cur->n_left_to_next -= 4;                                                                  \
    void* ips[4];                                                                              \
    void* l4s[4];                                                                              \
    u8 protos[4];                                                                              \
    nat_action_t acts[4];                                                                      \
    for (int i = 0; i < 4; ++i) {                                                             \
      ips[i] = p[i].ip;                                                                        \
      l4s[i] = p[i].l4;                                                                        \
      protos[i] = p[i].ip->protocol;                                                           \
    }                                                                                          \
    batch_fn(eng, tid, ips, l4s, protos, acts, 4);                                            \
    u32 next[4];                                                                               \
    for (int i = 0; i < 4; ++i) {                                                             \
      next[i] = nat_action_to_next(acts[i]);                                                  \
    }                                                                                          \
    vlib_validate_buffer_enqueue_x4(vm, node, cur->next_index, cur->to_next,                  \
                                    cur->n_left_to_next, p[0].bi, p[1].bi, p[2].bi, p[3].bi,  \
                                    next[0], next[1], next[2], next[3]);                       \
  }

#define NAT_STEP_DUAL(dir, batch_fn)                                                          \
  static inline void nat_step_dual_##dir(vlib_main_t* vm, vlib_node_runtime_t* node,          \
                                         nat_frame_cursor_t* cur, nat_engine_t* eng, u32 tid) { \
    if (PREDICT_TRUE(cur->n_left_from >= 4)) {                                                \
      for (int i = 2; i < 4; ++i) {                                                           \
        vlib_buffer_t* b = vlib_get_buffer(vm, cur->from[i]);                                 \
        CLIB_PREFETCH(b->data, CLIB_CACHE_LINE_BYTES, LOAD);                                  \
      }                                                                                        \
    }                                                                                          \
    nat_pkt_t p[2];                                                                            \
    for (int i = 0; i < 2; ++i) {                                                             \
      p[i] = nat_pkt_init(vm, cur->from[i]);                                                  \
      cur->to_next[i] = p[i].bi;                                                              \
    }                                                                                          \
    cur->from += 2;                                                                            \
    cur->n_left_from -= 2;                                                                     \
    cur->to_next += 2;                                                                         \
    cur->n_left_to_next -= 2;                                                                  \
    void* ips[2] = {p[0].ip, p[1].ip};                                                         \
    void* l4s[2] = {p[0].l4, p[1].l4};                                                         \
    u8 protos[2] = {p[0].ip->protocol, p[1].ip->protocol};                                     \
    nat_action_t acts[2];                                                                       \
    batch_fn(eng, tid, ips, l4s, protos, acts, 2);                                              \
    u32 next0 = nat_action_to_next(acts[0]);                                                   \
    u32 next1 = nat_action_to_next(acts[1]);                                                   \
    vlib_validate_buffer_enqueue_x2(vm, node, cur->next_index, cur->to_next,                  \
                                    cur->n_left_to_next, p[0].bi, p[1].bi, next0, next1);      \
  }

#define NAT_STEP_SINGLE(dir, translate_fn)                                                    \
  static inline void nat_step_single_##dir(vlib_main_t* vm, vlib_node_runtime_t* node,        \
                                           nat_frame_cursor_t* cur, nat_engine_t* eng,         \
                                           u32 tid) {                                          \
    nat_pkt_t p = nat_pkt_init(vm, cur->from[0]);                                             \
    cur->to_next[0] = p.bi;                                                                    \
    cur->from += 1;                                                                            \
    cur->n_left_from -= 1;                                                                     \
    cur->to_next += 1;                                                                         \
    cur->n_left_to_next -= 1;                                                                  \
    nat_action_t act = translate_fn(eng, tid, p.ip, p.l4, p.ip->protocol);                     \
    u32 next = nat_action_to_next(act);                                                        \
    vlib_validate_buffer_enqueue_x1(vm, node, cur->next_index, cur->to_next,                  \
                                    cur->n_left_to_next, p.bi, next);                          \
  }

NAT_STEP_QUAD(in2out, nat_translate_in2out_batch)
NAT_STEP_DUAL(in2out, nat_translate_in2out_batch)
NAT_STEP_SINGLE(in2out, nat_translate_in2out)

NAT_STEP_QUAD(out2in, nat_translate_out2in_batch)
NAT_STEP_DUAL(out2in, nat_translate_out2in_batch)
NAT_STEP_SINGLE(out2in, nat_translate_out2in)

static inline uword nat_node_process_frame_in2out(vlib_main_t* vm, vlib_node_runtime_t* node,
                                                  vlib_frame_t* frame) {
  nat_frame_cursor_t cur = {
      .from = vlib_frame_vector_args(frame),
      .n_left_from = frame->n_vectors,
      .next_index = node->cached_next_index,
  };
  u32 tid = vm->thread_index;

  while (cur.n_left_from > 0) {
    vlib_get_next_frame(vm, node, cur.next_index, cur.to_next, cur.n_left_to_next);

    while (cur.n_left_from >= 8 && cur.n_left_to_next >= 4) {
      nat_step_quad_in2out(vm, node, &cur, global_nat_engine, tid);
    }
    while (cur.n_left_from >= 4 && cur.n_left_to_next >= 2) {
      nat_step_dual_in2out(vm, node, &cur, global_nat_engine, tid);
    }
    while (cur.n_left_from > 0 && cur.n_left_to_next > 0) {
      nat_step_single_in2out(vm, node, &cur, global_nat_engine, tid);
    }
    vlib_put_next_frame(vm, node, cur.next_index, cur.n_left_to_next);
  }
  nat_engine_process_tick(global_nat_engine, tid);
  return frame->n_vectors;
}

static inline uword nat_node_process_frame_out2in(vlib_main_t* vm, vlib_node_runtime_t* node,
                                                  vlib_frame_t* frame) {
  nat_frame_cursor_t cur = {
      .from = vlib_frame_vector_args(frame),
      .n_left_from = frame->n_vectors,
      .next_index = node->cached_next_index,
  };
  u32 tid = vm->thread_index;

  while (cur.n_left_from > 0) {
    vlib_get_next_frame(vm, node, cur.next_index, cur.to_next, cur.n_left_to_next);

    while (cur.n_left_from >= 8 && cur.n_left_to_next >= 4) {
      nat_step_quad_out2in(vm, node, &cur, global_nat_engine, tid);
    }
    while (cur.n_left_from >= 4 && cur.n_left_to_next >= 2) {
      nat_step_dual_out2in(vm, node, &cur, global_nat_engine, tid);
    }
    while (cur.n_left_from > 0 && cur.n_left_to_next > 0) {
      nat_step_single_out2in(vm, node, &cur, global_nat_engine, tid);
    }
    vlib_put_next_frame(vm, node, cur.next_index, cur.n_left_to_next);
  }
  nat_engine_process_tick(global_nat_engine, tid);
  return frame->n_vectors;
}

VLIB_NODE_FN(nat_pba_in2out_node)(vlib_main_t* vm, vlib_node_runtime_t* node, vlib_frame_t* frame) {
  return nat_node_process_frame_in2out(vm, node, frame);
}

VLIB_NODE_FN(nat_pba_out2in_node)(vlib_main_t* vm, vlib_node_runtime_t* node, vlib_frame_t* frame) {
  return nat_node_process_frame_out2in(vm, node, frame);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
VLIB_REGISTER_NODE(nat_pba_in2out_node) = {
    .name = "nat-pba-in2out",
    .vector_size = sizeof(u32),
    .type = VLIB_NODE_TYPE_INTERNAL,
    .n_next_nodes = NAT_N_NEXT,
    .next_nodes =
        {
            [NAT_NEXT_DROP] = "error-drop",
            [NAT_NEXT_LOOKUP] = "ip4-lookup",
            [NAT_NEXT_SLOW_PATH] = "error-drop",
        },
};

VLIB_REGISTER_NODE(nat_pba_out2in_node) = {
    .name = "nat-pba-out2in",
    .vector_size = sizeof(u32),
    .type = VLIB_NODE_TYPE_INTERNAL,
    .n_next_nodes = NAT_N_NEXT,
    .next_nodes =
        {
            [NAT_NEXT_DROP] = "error-drop",
            [NAT_NEXT_LOOKUP] = "ip4-lookup",
            [NAT_NEXT_SLOW_PATH] = "error-drop",
        },
};
#pragma GCC diagnostic pop

VNET_FEATURE_INIT(nat_pba_in2out_feature, static) = {
    .arc_name = "ip4-unicast",
    .node_name = "nat-pba-in2out",
    .runs_before = VNET_FEATURES("ip4-lookup"),
};

VNET_FEATURE_INIT(nat_pba_out2in_feature, static) = {
    .arc_name = "ip4-unicast",
    .node_name = "nat-pba-out2in",
    .runs_before = VNET_FEATURES("ip4-lookup"),
};

typedef struct {
  u32 sessions_per_worker;
  u32 pool_start_ip;
  u32 pool_end_ip;
  u16 block_size;
  u16 max_blocks_per_user;
  u16 vector_batch_size;
  u8 prefetch_distance;
  u32 udp_sec;
  u32 tcp_est_sec;
  u32 tcp_trans_sec;
  u32 icmp_sec;
} nat_pba_config_t;

static nat_pba_config_t g_nat_pba_cfg = {
    .sessions_per_worker = 2000000,
    .pool_start_ip = 0xCB007101,
    .pool_end_ip = 0xCB007120,
    .block_size = 64,
    .max_blocks_per_user = 8,
    .vector_batch_size = 256,
    .prefetch_distance = 4,
    .udp_sec = 120,
    .tcp_est_sec = 3600,
    .tcp_trans_sec = 60,
    .icmp_sec = 30,
};

static int g_ha_role = 0;
static char g_ha_local_ip[32] = "0.0.0.0";
static u16 g_ha_local_port = 3234;
static char g_ha_peer_ip[32] = "127.0.0.1";
static u16 g_ha_peer_port = 3235;
static u32 g_ha_interval_ms = 1000;

static void nat_ha_ip4_to_string(ip4_address_t* addr, char* out, size_t out_size) {
  const u32 host = clib_net_to_host_u32(addr->as_u32);
  snprintf(out, out_size, "%u.%u.%u.%u", (host >> 24) & 0xFF, (host >> 16) & 0xFF,
           (host >> 8) & 0xFF, host & 0xFF);
}

static clib_error_t* nat_pba_ha_set_role_fn(vlib_main_t* vm, unformat_input_t* input,
                                            vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  if (unformat(input, "active")) {
    g_ha_role = 1;
  } else if (unformat(input, "standby")) {
    g_ha_role = 2;
  } else if (unformat(input, "off")) {
    g_ha_role = 0;
  } else {
    return clib_error_return(0, "expected active|standby|off");
  }
  return 0;
}

static clib_error_t* nat_pba_ha_set_local_fn(vlib_main_t* vm, unformat_input_t* input,
                                             vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  ip4_address_t addr;
  u32 port = 0;
  if (!unformat(input, "%U %u", unformat_ip4_address, &addr, &port) || port == 0 ||
      port > 65535) {
    return clib_error_return(0, "expected <local-ip> <local-port>");
  }
  nat_ha_ip4_to_string(&addr, g_ha_local_ip, sizeof(g_ha_local_ip));
  g_ha_local_port = (u16)port;
  return 0;
}

static clib_error_t* nat_pba_ha_set_peer_fn(vlib_main_t* vm, unformat_input_t* input,
                                            vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  ip4_address_t addr;
  u32 port = 0;
  if (!unformat(input, "%U %u", unformat_ip4_address, &addr, &port) || port == 0 ||
      port > 65535) {
    return clib_error_return(0, "expected <peer-ip> <peer-port>");
  }
  nat_ha_ip4_to_string(&addr, g_ha_peer_ip, sizeof(g_ha_peer_ip));
  g_ha_peer_port = (u16)port;
  return 0;
}

static clib_error_t* nat_pba_ha_set_interval_fn(vlib_main_t* vm, unformat_input_t* input,
                                                vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 interval = 0;
  if (!unformat(input, "%u", &interval) || interval < 50) {
    return clib_error_return(0, "expected interval in ms (>= 50)");
  }
  g_ha_interval_ms = interval;
  return 0;
}

static clib_error_t* nat_pba_ha_start_fn(vlib_main_t* vm, unformat_input_t* input,
                                         vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  if (!global_nat_engine) {
    return clib_error_return(0, "NAT PBA engine is not initialized");
  }
  if (g_ha_role != 1 && g_ha_role != 2) {
    return clib_error_return(0, "set role active|standby first");
  }
  nat_ha_sync_start(global_nat_engine, g_ha_role, g_ha_local_ip, g_ha_local_port,
                    g_ha_peer_ip, g_ha_peer_port, g_ha_interval_ms);
  vlib_cli_output(vm, "HA sync started: role=%s local=%s:%u peer=%s:%u interval=%u ms\n",
                  g_ha_role == 1 ? "active" : "standby", g_ha_local_ip, g_ha_local_port,
                  g_ha_peer_ip, g_ha_peer_port, g_ha_interval_ms);
  return 0;
}

static clib_error_t* nat_pba_ha_stop_fn(vlib_main_t* vm, unformat_input_t* input,
                                        vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  nat_ha_sync_stop();
  vlib_cli_output(vm, "HA sync stopped\n");
  return 0;
}

static clib_error_t* nat_pba_ha_promote_fn(vlib_main_t* vm, unformat_input_t* input,
                                           vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  if (!global_nat_engine) {
    return clib_error_return(0, "NAT PBA engine is not initialized");
  }
  const bool ok = nat_ha_sync_promote(global_nat_engine);
  vlib_cli_output(vm, "HA promotion %s\n", ok ? "complete" : "completed with import failures");
  return 0;
}

static clib_error_t* nat_pba_ha_snapshot_fn(vlib_main_t* vm, unformat_input_t* input,
                                            vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  nat_ha_sync_force_snapshot();
  vlib_cli_output(vm, "HA snapshot requested\n");
  return 0;
}

static clib_error_t* nat_pba_ha_show_fn(vlib_main_t* vm, unformat_input_t* input,
                                        vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  nat_ha_status_t status = {0};
  nat_ha_sync_get_status(&status);
  const char* role = status.role == 1 ? "active" : (status.role == 2 ? "standby" : "off");
  vlib_cli_output(vm, "HA sync: role=%s running=%d connected=%d\n", role, status.running,
                  status.connected);
  vlib_cli_output(vm,
                  "  local=%s:%u peer=%s:%u interval=%u ms\n", g_ha_local_ip,
                  g_ha_local_port, g_ha_peer_ip, g_ha_peer_port, g_ha_interval_ms);
  vlib_cli_output(vm,
                  "  shadow_sessions=%llu last_sync=%llu snapshots_sent=%llu "
                  "records_sent=%llu imported=%llu failed=%llu\n",
                  (unsigned long long)status.shadow_sessions,
                  (unsigned long long)status.last_sync_ms,
                  (unsigned long long)status.snapshots_sent,
                  (unsigned long long)status.records_sent,
                  (unsigned long long)status.records_imported,
                  (unsigned long long)status.records_failed);
  return 0;
}

static clib_error_t* nat_pba_set_timeouts_fn(vlib_main_t* vm, unformat_input_t* input,
                                             vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 udp = g_nat_pba_cfg.udp_sec;
  u32 tcp_est = g_nat_pba_cfg.tcp_est_sec;
  u32 tcp_trans = g_nat_pba_cfg.tcp_trans_sec;
  u32 icmp = g_nat_pba_cfg.icmp_sec;

  while (unformat_check_input(input) != UNFORMAT_END_OF_INPUT) {
    if (unformat(input, "udp %u", &udp)) {
      continue;
    }
    if (unformat(input, "tcp-established %u", &tcp_est)) {
      continue;
    }
    if (unformat(input, "tcp-transitory %u", &tcp_trans)) {
      continue;
    }
    if (unformat(input, "icmp %u", &icmp)) {
      continue;
    }
    return clib_error_return(0, "unknown input `%U'", format_unformat_error, input);
  }

  g_nat_pba_cfg.udp_sec = udp;
  g_nat_pba_cfg.tcp_est_sec = tcp_est;
  g_nat_pba_cfg.tcp_trans_sec = tcp_trans;
  g_nat_pba_cfg.icmp_sec = icmp;
  nat_engine_set_timeouts(global_nat_engine, udp, tcp_est, tcp_trans, icmp);
  return 0;
}

static clib_error_t* nat_pba_set_pool_fn(vlib_main_t* vm, unformat_input_t* input,
                                         vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  ip4_address_t start;
  ip4_address_t end;
  if (!unformat(input, "%U %U", unformat_ip4_address, &start, unformat_ip4_address, &end)) {
    return clib_error_return(0, "expected <pool-start-ip> <pool-end-ip>");
  }
  const u32 start_host = clib_net_to_host_u32(start.as_u32);
  const u32 end_host = clib_net_to_host_u32(end.as_u32);
  if (end_host < start_host) {
    return clib_error_return(0, "pool end address must be >= start address");
  }

  g_nat_pba_cfg.pool_start_ip = start_host;
  g_nat_pba_cfg.pool_end_ip = end_host;
  nat_engine_set_pool(global_nat_engine, start_host, end_host);
  return 0;
}

static clib_error_t* nat_pba_set_block_size_fn(vlib_main_t* vm, unformat_input_t* input,
                                               vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 block_size = 0;
  if (!unformat(input, "%u", &block_size) || (block_size != 32 && block_size != 64)) {
    return clib_error_return(0, "expected block size 32 or 64");
  }
  g_nat_pba_cfg.block_size = (u16)block_size;
  nat_engine_set_block_size(global_nat_engine, (u16)block_size);
  return 0;
}

static clib_error_t* nat_pba_set_max_blocks_fn(vlib_main_t* vm, unformat_input_t* input,
                                               vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 max_blocks = 0;
  if (!unformat(input, "%u", &max_blocks) || max_blocks == 0 || max_blocks > 8) {
    return clib_error_return(0, "expected max blocks per user in range 1..8");
  }
  g_nat_pba_cfg.max_blocks_per_user = (u16)max_blocks;
  nat_engine_set_max_blocks_per_user(global_nat_engine, (u16)max_blocks);
  return 0;
}

static clib_error_t* nat_pba_set_sessions_fn(vlib_main_t* vm, unformat_input_t* input,
                                             vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 sessions = 0;
  if (!unformat(input, "%u", &sessions) || sessions == 0) {
    return clib_error_return(0, "expected sessions per worker > 0");
  }
  if (!nat_engine_set_sessions_per_worker(global_nat_engine, sessions)) {
    return clib_error_return(0, "cannot resize while sessions exist; flush sessions first");
  }
  g_nat_pba_cfg.sessions_per_worker = sessions;
  return 0;
}

static clib_error_t* nat_pba_set_vector_fn(vlib_main_t* vm, unformat_input_t* input,
                                           vlib_cli_command_t* cmd) {
  (void)vm;
  (void)cmd;
  u32 batch_size = 0;
  u32 prefetch = 0;
  if (!unformat(input, "batch-size %u prefetch %u", &batch_size, &prefetch)) {
    return clib_error_return(0, "expected batch-size <n> prefetch <n>");
  }
  g_nat_pba_cfg.vector_batch_size = (u16)batch_size;
  g_nat_pba_cfg.prefetch_distance = (u8)prefetch;
  nat_engine_set_vector_config(global_nat_engine, (u16)batch_size, (u8)prefetch);
  return 0;
}

static clib_error_t* nat_pba_set_interface_fn(vlib_main_t* vm, unformat_input_t* input,
                                              vlib_cli_command_t* cmd) {
  (void)cmd;
  vnet_main_t* vnm = vnet_get_main();
  u32 sw_if_index = ~0U;
  int in2out = 1;
  int is_add = 1;

  if (unformat(input, "in %U", unformat_vnet_sw_interface, vnm, &sw_if_index)) {
    in2out = 1;
  } else if (unformat(input, "out %U", unformat_vnet_sw_interface, vnm, &sw_if_index)) {
    in2out = 0;
  } else {
    return clib_error_return(0, "expected in|out <interface> [del]");
  }
  if (unformat(input, "del")) {
    is_add = 0;
  }

  const char* node_name = in2out ? "nat-pba-in2out" : "nat-pba-out2in";
  const int rv =
      vnet_feature_enable_disable("ip4-unicast", node_name, sw_if_index, is_add, 0, 0);
  if (rv != 0) {
    return clib_error_return(0, "failed to %s %s on sw_if_index %u",
                             is_add ? "enable" : "disable", node_name, sw_if_index);
  }
  vlib_cli_output(vm, "%s %s on sw_if_index %u\n", is_add ? "enabled" : "disabled",
                  node_name, sw_if_index);
  return 0;
}

static clib_error_t* show_nat_pba_config_fn(vlib_main_t* vm, unformat_input_t* input,
                                            vlib_cli_command_t* cmd) {
  (void)input;
  (void)cmd;
  ip4_address_t pool_start = {.as_u32 = clib_host_to_net_u32(g_nat_pba_cfg.pool_start_ip)};
  ip4_address_t pool_end = {.as_u32 = clib_host_to_net_u32(g_nat_pba_cfg.pool_end_ip)};

  vlib_cli_output(vm, "NAT PBA configuration:\n");
  vlib_cli_output(vm, "  sessions/worker:  %u\n", g_nat_pba_cfg.sessions_per_worker);
  vlib_cli_output(vm, "  public pool:      %U - %U\n", format_ip4_address, &pool_start,
                  format_ip4_address, &pool_end);
  vlib_cli_output(vm, "  block size:       %u\n", g_nat_pba_cfg.block_size);
  vlib_cli_output(vm, "  max blocks/user:  %u\n", g_nat_pba_cfg.max_blocks_per_user);
  vlib_cli_output(vm, "  vector batch:     %u\n", g_nat_pba_cfg.vector_batch_size);
  vlib_cli_output(vm, "  prefetch:         %u\n", g_nat_pba_cfg.prefetch_distance);
  vlib_cli_output(vm, "  timeouts:         udp=%u tcp-est=%u tcp-trans=%u icmp=%u\n",
                  g_nat_pba_cfg.udp_sec, g_nat_pba_cfg.tcp_est_sec,
                  g_nat_pba_cfg.tcp_trans_sec, g_nat_pba_cfg.icmp_sec);
  return 0;
}

static clib_error_t* show_nat_pba_command_fn(vlib_main_t* vm, unformat_input_t* input,
                                             vlib_cli_command_t* cmd) {
  (void)vm;
  (void)input;
  (void)cmd;
  if (!global_nat_engine) {
    vlib_cli_output(vm, "NAT PBA Engine: Not initialized\n");
    return 0;
  }
  u64 sessions = nat_engine_get_total_sessions(global_nat_engine);
  u64 pkts = nat_engine_get_total_translations(global_nat_engine);
  nat_telemetry_t tel = nat_engine_get_telemetry(global_nat_engine);
  vlib_cli_output(vm, "===============================================\n");
  vlib_cli_output(vm, "  NAT44 PBA High-Performance Engine Status     \n");
  vlib_cli_output(vm, "===============================================\n");
  vlib_cli_output(vm, "Total Active Sessions:      %llu\n", (unsigned long long)sessions);
  vlib_cli_output(vm, "Total Processed Packets:    %llu\n", (unsigned long long)pkts);
  vlib_cli_output(vm, "Worker Threads Configured:  %u\n", vlib_get_n_threads());
  vlib_cli_output(vm, "Port Block Sizing:          %u ports/block\n", g_nat_pba_cfg.block_size);
  vlib_cli_output(vm, "Out2in misses:              %llu\n",
                  (unsigned long long)tel.out2in_misses);
  vlib_cli_output(vm, "Fragment drops:             %llu\n",
                  (unsigned long long)tel.fragment_drops);
  vlib_cli_output(vm, "Malformed drops:            %llu\n",
                  (unsigned long long)tel.malformed_drops);
  vlib_cli_output(vm, "Slow-path packets:          %llu\n",
                  (unsigned long long)tel.slow_path_packets);
  return 0;
}

VLIB_CLI_COMMAND(show_nat_pba_command, static) = {
    .path = "show nat pba",
    .short_help = "show nat pba",
    .function = show_nat_pba_command_fn,
};

VLIB_CLI_COMMAND(show_nat_pba_config_command, static) = {
    .path = "show nat pba config",
    .short_help = "show nat pba config",
    .function = show_nat_pba_config_fn,
};

VLIB_CLI_COMMAND(set_interface_nat_pba_command, static) = {
    .path = "set interface nat pba",
    .short_help = "set interface nat pba in|out <interface> [del]",
    .function = nat_pba_set_interface_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_timeouts_command, static) = {
    .path = "nat pba set timeouts",
    .short_help = "nat pba set timeouts [udp <s>] [tcp-established <s>] [tcp-transitory <s>] [icmp <s>]",
    .function = nat_pba_set_timeouts_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_pool_command, static) = {
    .path = "nat pba set pool",
    .short_help = "nat pba set pool <start-ip> <end-ip>",
    .function = nat_pba_set_pool_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_block_size_command, static) = {
    .path = "nat pba set block-size",
    .short_help = "nat pba set block-size <32|64>",
    .function = nat_pba_set_block_size_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_max_blocks_command, static) = {
    .path = "nat pba set max-blocks",
    .short_help = "nat pba set max-blocks <1..8>",
    .function = nat_pba_set_max_blocks_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_sessions_command, static) = {
    .path = "nat pba set sessions",
    .short_help = "nat pba set sessions <per-worker>",
    .function = nat_pba_set_sessions_fn,
};

VLIB_CLI_COMMAND(nat_pba_set_vector_command, static) = {
    .path = "nat pba set vector",
    .short_help = "nat pba set vector batch-size <n> prefetch <n>",
    .function = nat_pba_set_vector_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_set_role_command, static) = {
    .path = "nat pba ha set role",
    .short_help = "nat pba ha set role active|standby|off",
    .function = nat_pba_ha_set_role_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_set_local_command, static) = {
    .path = "nat pba ha set local",
    .short_help = "nat pba ha set local <ip> <port>",
    .function = nat_pba_ha_set_local_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_set_peer_command, static) = {
    .path = "nat pba ha set peer",
    .short_help = "nat pba ha set peer <ip> <port>",
    .function = nat_pba_ha_set_peer_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_set_interval_command, static) = {
    .path = "nat pba ha set interval",
    .short_help = "nat pba ha set interval <ms>",
    .function = nat_pba_ha_set_interval_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_start_command, static) = {
    .path = "nat pba ha start",
    .short_help = "nat pba ha start",
    .function = nat_pba_ha_start_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_stop_command, static) = {
    .path = "nat pba ha stop",
    .short_help = "nat pba ha stop",
    .function = nat_pba_ha_stop_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_promote_command, static) = {
    .path = "nat pba ha promote",
    .short_help = "nat pba ha promote",
    .function = nat_pba_ha_promote_fn,
};

VLIB_CLI_COMMAND(nat_pba_ha_snapshot_command, static) = {
    .path = "nat pba ha snapshot",
    .short_help = "nat pba ha snapshot",
    .function = nat_pba_ha_snapshot_fn,
};

VLIB_CLI_COMMAND(show_nat_pba_ha_command, static) = {
    .path = "show nat pba ha",
    .short_help = "show nat pba ha",
    .function = nat_pba_ha_show_fn,
};
