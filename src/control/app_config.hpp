#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NAT_ENABLE_HA
#define NAT_ENABLE_HA 0
#endif

namespace nat::config {

struct PortBlockConfig {
  bool enabled{true};
  std::uint32_t block_size{64};
  std::uint32_t max_blocks_per_user{8};
};

struct TimeoutsConfig {
  std::uint32_t udp{120};
  std::uint32_t tcp_established{3600};
  std::uint32_t tcp_transitory{60};
  std::uint32_t icmp{30};
};

struct NatEngineConfig {
  std::uint32_t sessions_per_worker{250000};
  std::uint32_t max_translations_per_thread{300000};
  std::uint32_t vector_batch_size{256};
  std::uint32_t prefetch_distance{4};
  PortBlockConfig port_block_allocation{};
  TimeoutsConfig timeouts{};
};

struct InterfaceConfig {
  std::string interface_name{};
  std::string pci_address{};
  std::string subnet_cidr{};
  std::string gateway_ip{};
  std::string vrrp_vip{};
  std::uint8_t vrrp_vrid{0};
};

struct HasyncInterfaceConfig {
  std::string interface_name{};
  std::string pci_address{};
  std::string subnet_cidr{};
};

struct TopologyConfig {
  InterfaceConfig inside{};
  InterfaceConfig outside{};
  HasyncInterfaceConfig hasync{};
};

struct PublicPoolConfig {
  std::string start_ip{};
  std::string end_ip{};
  bool vrrp_managed{true};
};

struct NodeEndpointConfig {
  std::string vpp_api_socket{};
  std::string vpp_cli_socket{};
  std::string ha_local_ip{};
  std::uint16_t ha_local_port{1234};
  std::string ha_peer_ip{};
  std::uint16_t ha_peer_port{1234};
  std::uint32_t ha_refresh_interval_sec{10};
  std::uint8_t vrrp_priority{100};
};

struct NodesConfig {
  NodeEndpointConfig node1{};
  NodeEndpointConfig node2{};
};

struct ApplicationConfig {
  std::string version{"1.0"};
  NatEngineConfig nat_engine{};
  TopologyConfig topology{};
  PublicPoolConfig public_pool{};
  NodesConfig nodes{};
};

/**
 * @brief Reads and validates configuration directly from YAML or JSON using Glaze reflection.
 * Returns std::expected with ApplicationConfig or an error message.
 */
[[nodiscard]] inline std::expected<ApplicationConfig, std::string> LoadConfig(
    const std::string& config_file_path) noexcept {
  ApplicationConfig cfg{};
  std::string buffer{};

  const bool is_json{config_file_path.ends_with(".json")};

  if (is_json) {
    const auto ec{glz::read_file_json(cfg, config_file_path, buffer)};
    if (ec) {
      return std::unexpected(std::format("Glaze failed to parse JSON configuration file [{}]: {}",
                                         config_file_path, glz::format_error(ec, buffer)));
    }
    return cfg;
  }

  // Parse YAML directly with Glaze reflection without yaml-cpp
  const auto ec{glz::read_file_yaml(cfg, config_file_path, buffer)};
  if (ec) {
    return std::unexpected(std::format("Glaze failed to parse YAML configuration file [{}]: {}",
                                       config_file_path, glz::format_error(ec, buffer)));
  }
  return cfg;
}

}  // namespace nat::config
