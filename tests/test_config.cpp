#include <cassert>

#include "../src/control/app_config.hpp"
#include "helpers/print.hpp"

int main(int argc, char** argv) {
  nat::io::println("========================================");
  nat::io::println("  Running Unit Test: Config Reflection  ");
  nat::io::println("========================================");

  const std::string json_path = (argc > 1) ? argv[1] : "config/config.json";
  const std::string yaml_path = (argc > 2) ? argv[2] : "config/config.yaml";

  // 1. Test JSON deserialization via Glaze
  nat::io::println("[+] Testing JSON config parse: {}", json_path);
  const auto json_cfg_res = nat::config::LoadConfig(json_path);
  assert(json_cfg_res.has_value());
  const auto& json_cfg = *json_cfg_res;

  assert(json_cfg.version == "1.0");
  assert(json_cfg.nat_engine.sessions_per_worker >= 250000);
  assert(json_cfg.nat_engine.max_translations_per_thread >= 300000);
  assert(json_cfg.nat_engine.vector_batch_size == 256);
  assert(json_cfg.nat_engine.prefetch_distance == 4);
  assert(json_cfg.nat_engine.port_block_allocation.enabled == true);
  assert(json_cfg.nat_engine.port_block_allocation.block_size == 64);
  assert(json_cfg.nat_engine.timeouts.udp >= 15);
  assert(json_cfg.nat_engine.timeouts.tcp_established == 3600);
  assert(json_cfg.topology.inside.interface_name == "inside0");
  assert(json_cfg.topology.inside.vrrp_vip == "10.10.0.254");
  assert(json_cfg.topology.outside.vrrp_vip == "198.18.0.254");
  assert(json_cfg.public_pool.start_ip == "203.0.113.1");
  assert(json_cfg.public_pool.end_ip == "203.0.113.32");
  assert(json_cfg.nodes.node1.ha_local_ip == "172.30.0.1");
  assert(json_cfg.nodes.node1.ha_peer_ip == "172.30.0.2");
  assert(json_cfg.nodes.node1.vrrp_priority == 200);
  assert(json_cfg.nodes.node2.vrrp_priority == 100);

  nat::io::println("[+] JSON configuration reflection verified successfully.");

  // 2. Test YAML deserialization directly via Glaze (zero yaml-cpp dependency)
  nat::io::println("[+] Testing YAML config parse via Glaze: {}", yaml_path);
  const auto yaml_cfg_res = nat::config::LoadConfig(yaml_path);
  assert(yaml_cfg_res.has_value());
  const auto& yaml_cfg = *yaml_cfg_res;

  assert(yaml_cfg.version == "1.0");
  assert(yaml_cfg.nat_engine.sessions_per_worker >= 250000);
  assert(yaml_cfg.nat_engine.vector_batch_size == 256);
  assert(yaml_cfg.nat_engine.prefetch_distance == 4);
  assert(yaml_cfg.nat_engine.port_block_allocation.block_size == 64);
  assert(yaml_cfg.topology.inside.vrrp_vip == "10.10.0.254");
  assert(yaml_cfg.public_pool.start_ip == "203.0.113.1");
  assert(yaml_cfg.nodes.node1.ha_local_ip == "172.30.0.1");

  nat::io::println("[+] YAML configuration reflection verified successfully via Glaze.");

  nat::io::println("[+] All configuration reflection tests passed!");
  return 0;
}
