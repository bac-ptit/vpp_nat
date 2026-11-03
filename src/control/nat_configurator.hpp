#pragma once

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>

#include "app_config.hpp"
#include "vpp_client.hpp"

namespace nat::control {

/**
 * @brief Drives VPP NAT44 data-plane configuration via C++ VAPI.
 * Employs std::expected for robust, zero-overhead error reporting.
 */
class NatConfigurator final {
 public:
  explicit NatConfigurator(VppClient& client, const nat::config::ApplicationConfig& cfg)
      : client_{client}, cfg_{cfg} {}

  [[nodiscard]] std::expected<void, std::string> ApplyConfiguration(
      const std::string& node_id) noexcept;

  [[nodiscard]] std::expected<void, std::string> PromoteHa(
      const std::string& cli_socket) noexcept;
  [[nodiscard]] std::expected<void, std::string> FlushHa() noexcept;
  [[nodiscard]] std::expected<void, std::string> ResyncHa() noexcept;

 private:
  [[nodiscard]] std::expected<void, std::string> SetupNat44Engine(
      const std::string& cli_socket) noexcept;
  [[nodiscard]] std::expected<void, std::string> ConfigureTimeouts(
      const std::string& cli_socket) noexcept;
  [[nodiscard]] std::expected<void, std::string> AddNatAddressPool(
      const std::string& cli_socket) noexcept;
  [[nodiscard]] std::expected<void, std::string> SetupHaPeering(
      const nat::config::NodeEndpointConfig& node_cfg) noexcept;

  [[nodiscard]] static std::expected<std::array<std::uint8_t, 4>, std::string> ParseIp4ToArray(
      std::string_view ip_str) noexcept {
    if (ip_str.empty() || ip_str.size() >= INET_ADDRSTRLEN) {
      return std::unexpected(std::format("Invalid IPv4 address format: {}", ip_str));
    }

    std::array<char, INET_ADDRSTRLEN> text{};
    std::ranges::copy(ip_str, text.begin());

    struct in_addr addr {};
    if (inet_pton(AF_INET, text.data(), &addr) != 1) {
      return std::unexpected(std::format("Invalid IPv4 address format: {}", ip_str));
    }
    return std::bit_cast<std::array<std::uint8_t, 4>>(addr.s_addr);
  }

  VppClient& client_;
  const nat::config::ApplicationConfig& cfg_;
};

}  // namespace nat::control
