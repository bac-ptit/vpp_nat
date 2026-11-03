#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <string_view>
#include <thread>

#include "app_config.hpp"
#include "ha_manager.hpp"
#include "helpers/print.hpp"
#include "nat_configurator.hpp"
#include "vpp_client.hpp"

namespace {
using namespace std::chrono_literals;

std::atomic<bool> g_running{true};

void HandleSignal(int) {
  g_running.store(false, std::memory_order_relaxed);
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::string config_path{"config/config.yaml"};
  std::string node_id{"node1"};

  // Parse command line arguments
  constexpr std::array kNodeFlags{std::string_view{"--node"}, std::string_view{"-n"}};
  constexpr std::array kConfigFlags{std::string_view{"--config"}, std::string_view{"-c"}};
  constexpr std::array kHelpFlags{std::string_view{"--help"}, std::string_view{"-h"}};

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (std::ranges::contains(kNodeFlags, arg) && i + 1 < argc) {
      node_id = argv[++i];
    } else if (std::ranges::contains(kConfigFlags, arg)) {
      if (i + 1 < argc) {
        config_path = argv[++i];
      }
    } else if (std::ranges::contains(kHelpFlags, arg)) {
      nat::io::println("Usage: nat-ctl [options]");
      nat::io::println("Options:");
      nat::io::println("  -c, --config <file>   Path to config.yaml or config.json");
      nat::io::println("  -n, --node <node_id>  Target node identifier (node1, node2)");
      nat::io::println("  -h, --help            Show this help message");
      return 0;
    } else if (!arg.starts_with('-')) {
      config_path = arg;
    }
  }

  nat::io::println("====================================================");
  nat::io::println("  Starting NAT44 Control Plane Daemon (nat-ctl)     ");
  nat::io::println("  Configuration file: {}", config_path);
  nat::io::println("  Target Node ID:     {}", node_id);
  nat::io::println("====================================================");

  // 1. Parse configuration via Glaze reflection
  nat::io::println("[nat-ctl] Parsing {} via Glaze reflection...", config_path);
  const auto config_res = nat::config::LoadConfig(config_path);
  if (!config_res) {
    nat::io::println(stderr, "[nat-ctl] Fatal configuration error: {}", config_res.error());
    return 1;
  }
  const auto& app_config = *config_res;

  const auto& node_cfg = (node_id == "node2") ? app_config.nodes.node2 : app_config.nodes.node1;

  // 2. Connect to VPP Binary API socket
  nat::io::println("[nat-ctl] Connecting to VPP socket: {}", node_cfg.vpp_api_socket);
  auto client_res = nat::control::VppClient::Create("nat_control_daemon", node_cfg.vpp_api_socket);
  if (!client_res) {
    nat::io::println(stderr, "[nat-ctl] Fatal VPP connection failure: {}", client_res.error());
    return 1;
  }
  auto& client = *client_res;

  // 3. Apply NAT44 configuration and HA peering
  nat::control::NatConfigurator configurator(client, app_config);
  if (auto cfg_res = configurator.ApplyConfiguration(node_id); !cfg_res) {
    nat::io::println(stderr, "[nat-ctl] Fatal configuration application failure: {}", cfg_res.error());
    return 1;
  }

#if NAT_ENABLE_HA
  // 4. Initialize and start HA Manager
  nat::control::HaManager ha_mgr(client, app_config, node_id, &configurator);
  ha_mgr.Start();
  nat::io::println("[nat-ctl] Daemon operational with HA Manager active (Ctrl+C to exit)...");
#else
  nat::io::println("[nat-ctl] Daemon operational in Standalone High-Performance mode (HA disabled via #if NAT_ENABLE_HA=0).");
#endif

  auto last_stats_report{std::chrono::steady_clock::now()};

  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(500ms);

#if NAT_ENABLE_HA
    const auto now{std::chrono::steady_clock::now()};
    if (now - last_stats_report >= 10s) {
      last_stats_report = now;
      const auto st{ha_mgr.Stats()};
      const auto state_str =
          (ha_mgr.CurrentState() == nat::control::HaState::kActive) ? "ACTIVE" : "STANDBY";
      nat::io::println(
          "[nat-ctl] [HA State: {}] Sent: {}, Received: {}, Missed: {}, "
          "Failovers: {}",
          state_str, st.heartbeats_sent, st.heartbeats_received, st.missed_heartbeats,
          st.failover_transitions);
    }
#else
    (void)last_stats_report;
#endif
  }

#if NAT_ENABLE_HA
  nat::io::println("[nat-ctl] Stopping HA Manager...");
  ha_mgr.Stop();
#endif

  nat::io::println("[nat-ctl] Shutdown procedure finished.");
  return 0;
}
