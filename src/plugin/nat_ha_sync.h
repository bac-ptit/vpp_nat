#ifndef NAT_HA_SYNC_H
#define NAT_HA_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "nat/nat_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int role;  // 0 = off, 1 = active, 2 = standby
  bool running;
  bool connected;
  uint64_t shadow_sessions;
  uint64_t last_sync_ms;
  uint64_t snapshots_sent;
  uint64_t records_sent;
  uint64_t records_imported;
  uint64_t records_failed;
} nat_ha_status_t;

void nat_ha_sync_start(nat_engine_t* engine, int role, const char* local_ip,
                       uint16_t local_port, const char* peer_ip, uint16_t peer_port,
                       uint32_t interval_ms);
void nat_ha_sync_stop(void);
bool nat_ha_sync_promote(nat_engine_t* engine);
void nat_ha_sync_force_snapshot(void);
void nat_ha_sync_get_status(nat_ha_status_t* out);

#ifdef __cplusplus
}
#endif

#endif  // NAT_HA_SYNC_H
