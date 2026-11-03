#include "nat_configurator.hpp"

#include <cerrno>
#include <cstring>
#include <expected>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <vapi/nat44_ei.api.vapi.hpp>

#include "helpers/print.hpp"

DEFINE_VAPI_MSG_IDS_NAT44_EI_API_JSON;
DEFINE_VAPI_MSG_IDS_VPE_API_JSON;

namespace {

std::expected<void, std::string> RunVppCli(const std::string& socket_path,
                                           const std::string& command) noexcept {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return std::unexpected(std::format("socket: {}", std::strerror(errno)));
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.empty() || socket_path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return std::unexpected("invalid VPP CLI socket path");
  }
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    const std::string message =
        std::format("connect {}: {}", socket_path, std::strerror(errno));
    ::close(fd);
    return std::unexpected(message);
  }

  const std::string line = command + "\n";
  std::size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t written =
        ::send(fd, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
    if (written <= 0) {
      const std::string message = std::format("write {}: {}", socket_path, std::strerror(errno));
      ::close(fd);
      return std::unexpected(message);
    }
    sent += static_cast<std::size_t>(written);
  }

  char buffer[4096];
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
  while (::poll(&pfd, 1, 200) > 0) {
    if (::read(fd, buffer, sizeof(buffer)) <= 0) {
      break;
    }
  }
  ::close(fd);
  return {};
}

}  // namespace

namespace nat::control {

std::expected<void, std::string> NatConfigurator::ApplyConfiguration(
    const std::string& node_id) noexcept {
  const auto& nodes{cfg_.nodes};
  const auto& node_cfg{(node_id == "node2") ? nodes.node2 : nodes.node1};

  nat::io::println("[nat-ctl] Applying NAT44 configuration for [{}]...", node_id);

  const std::string& cli_socket{node_cfg.vpp_cli_socket};
#if NAT_ENABLE_HA
  // C++23 std::expected monadic chaining with HA peering
  auto result{SetupNat44Engine(cli_socket)
                  .and_then([this, &cli_socket] { return ConfigureTimeouts(cli_socket); })
                  .and_then([this, &cli_socket] { return AddNatAddressPool(cli_socket); })
                  .and_then([this, &node_cfg] { return SetupHaPeering(node_cfg); })};
#else
  // Standalone configuration without HA replication overhead
  auto result{SetupNat44Engine(cli_socket)
                  .and_then([this, &cli_socket] { return ConfigureTimeouts(cli_socket); })
                  .and_then([this, &cli_socket] { return AddNatAddressPool(cli_socket); })};
#endif
  if (!result) {
    return result;
  }

  nat::io::println("[nat-ctl] Configuration successfully committed to VPP.");
  return {};
}

std::expected<void, std::string> NatConfigurator::SetupNat44Engine(
    const std::string& cli_socket) noexcept {
  auto result = RunVppCli(
      cli_socket,
      std::format("nat pba set sessions {}", cfg_.nat_engine.sessions_per_worker));
  if (!result) {
    return std::unexpected(std::format("Failed to configure sessions: {}", result.error()));
  }
  nat::io::println("[nat-ctl] NAT PBA engine configured with {} sessions per worker.",
                   cfg_.nat_engine.sessions_per_worker);
  return {};
}

std::expected<void, std::string> NatConfigurator::ConfigureTimeouts(
    const std::string& cli_socket) noexcept {
  auto result = RunVppCli(
      cli_socket,
      std::format("nat pba set timeouts udp {} tcp-established {} tcp-transitory {} icmp {}",
                  cfg_.nat_engine.timeouts.udp, cfg_.nat_engine.timeouts.tcp_established,
                  cfg_.nat_engine.timeouts.tcp_transitory, cfg_.nat_engine.timeouts.icmp));
  if (!result) {
    return std::unexpected(std::format("Failed to set NAT timeouts: {}", result.error()));
  }
  nat::io::println("[nat-ctl] Configured timeouts: UDP={}s, TCP_EST={}s, TCP_TRANS={}s, ICMP={}s.",
                   cfg_.nat_engine.timeouts.udp, cfg_.nat_engine.timeouts.tcp_established,
                   cfg_.nat_engine.timeouts.tcp_transitory, cfg_.nat_engine.timeouts.icmp);
  return {};
}

std::expected<void, std::string> NatConfigurator::AddNatAddressPool(
    const std::string& cli_socket) noexcept {
  auto pool = RunVppCli(
      cli_socket,
      std::format("nat pba set pool {} {}", cfg_.public_pool.start_ip,
                  cfg_.public_pool.end_ip));
  if (!pool) {
    return std::unexpected(std::format("Failed to set NAT pool: {}", pool.error()));
  }

  auto blocks = RunVppCli(
      cli_socket,
      std::format("nat pba set block-size {}",
                  cfg_.nat_engine.port_block_allocation.block_size));
  if (!blocks) {
    return std::unexpected(std::format("Failed to set block size: {}", blocks.error()));
  }

  auto max_blocks = RunVppCli(
      cli_socket,
      std::format("nat pba set max-blocks {}",
                  cfg_.nat_engine.port_block_allocation.max_blocks_per_user));
  if (!max_blocks) {
    return std::unexpected(std::format("Failed to set max blocks: {}", max_blocks.error()));
  }

  auto vector = RunVppCli(
      cli_socket,
      std::format("nat pba set vector batch-size {} prefetch {}",
                  cfg_.nat_engine.vector_batch_size, cfg_.nat_engine.prefetch_distance));
  if (!vector) {
    return std::unexpected(std::format("Failed to set vector config: {}", vector.error()));
  }

  nat::io::println(
      "[nat-ctl] Public pool {} - {}, block size {}, max blocks {}, batch {}, prefetch {}.",
      cfg_.public_pool.start_ip, cfg_.public_pool.end_ip,
      cfg_.nat_engine.port_block_allocation.block_size,
      cfg_.nat_engine.port_block_allocation.max_blocks_per_user,
      cfg_.nat_engine.vector_batch_size, cfg_.nat_engine.prefetch_distance);
  return {};
}

std::expected<void, std::string> NatConfigurator::SetupHaPeering(
    const nat::config::NodeEndpointConfig& node_cfg) noexcept {
  const bool primary{node_cfg.vrrp_priority >= 150};
  const std::uint16_t local_port{
      static_cast<std::uint16_t>(primary ? 3234 : 3235)};
  const std::uint16_t peer_port{
      static_cast<std::uint16_t>(primary ? 3235 : 3234)};
  const std::string& cli_socket{node_cfg.vpp_cli_socket};

  const auto run = [&cli_socket](const std::string& command)
      -> std::expected<void, std::string> {
    return RunVppCli(cli_socket, command);
  };

  // Both nodes start as standby. Promotion happens only after the heartbeat
  // election (cold boot on the primary, or peer timeout on the secondary).
  if (auto result = run("nat pba ha set role standby"); !result) {
    return std::unexpected(std::format("Failed to set HA role: {}", result.error()));
  }
  if (auto result = run(std::format("nat pba ha set local 127.0.0.1 {}", local_port));
      !result) {
    return std::unexpected(std::format("Failed to set HA local endpoint: {}", result.error()));
  }
  if (auto result = run(std::format("nat pba ha set peer 127.0.0.1 {}", peer_port));
      !result) {
    return std::unexpected(std::format("Failed to set HA peer endpoint: {}", result.error()));
  }
  if (auto result = run("nat pba ha set interval 1000"); !result) {
    return std::unexpected(std::format("Failed to set HA interval: {}", result.error()));
  }
  if (auto result = run("nat pba ha start"); !result) {
    return std::unexpected(std::format("Failed to start HA sync: {}", result.error()));
  }

  nat::io::println("[nat-ctl] NAT PBA HA synchronization active: 127.0.0.1:{} -> 127.0.0.1:{} "
                   "(role {})",
                   local_port, peer_port, primary ? "active" : "standby");
  return {};
}

std::expected<void, std::string> NatConfigurator::PromoteHa(
    const std::string& cli_socket) noexcept {
  auto result = RunVppCli(cli_socket, "nat pba ha promote");
  if (!result) {
    return std::unexpected(std::format("Failed to promote HA: {}", result.error()));
  }
  nat::io::println("[nat-ctl] HA promotion command sent to VPP.");
  return {};
}

std::expected<void, std::string> NatConfigurator::FlushHa() noexcept {
  vapi::Nat44_ei_ha_flush req(client_.Connection());
  if (req.execute() == VAPI_OK) {
    client_.Connection().wait_for_response(req);
    return {};
  }
  return std::unexpected("Nat44_ei_ha_flush execution failed");
}

std::expected<void, std::string> NatConfigurator::ResyncHa() noexcept {
  vapi::Nat44_ei_ha_resync req(client_.Connection());
  auto& p = req.get_request().get_payload();
  p.want_resync_event = 1;
  p.pid = static_cast<u32>(getpid());
  if (req.execute() == VAPI_OK) {
    client_.Connection().wait_for_response(req);
    return {};
  }
  return std::unexpected("Nat44_ei_ha_resync execution failed");
}

}  // namespace nat::control
