#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "app_config.hpp"
#include "coro_helpers.hpp"
#include "vpp_client.hpp"

namespace nat::control {

class NatConfigurator;

enum class NodeRole : std::uint8_t { kPrimary, kSecondary };

enum class HaState : std::uint8_t { kInit, kStandby, kActive, kFailed, kStandbySyncing };

struct HaStatistics {
  std::uint64_t heartbeats_sent{0};
  std::uint64_t heartbeats_received{0};
  std::uint64_t missed_heartbeats{0};
  std::uint64_t failover_transitions{0};
};

/**
 * @brief Manages HA heartbeats, epoll event-driven monitoring, and C++23 coroutine failback.
 */
class HaManager final {
 public:
  using StateCallback = std::move_only_function<void(HaState old_state, HaState new_state)>;

  HaManager(VppClient& client, const nat::config::ApplicationConfig& cfg,
            const std::string& node_id, NatConfigurator* configurator = nullptr);
  ~HaManager();

  HaManager(const HaManager&) = delete;
  HaManager& operator=(const HaManager&) = delete;
  HaManager(HaManager&&) = delete;
  HaManager& operator=(HaManager&&) = delete;

  void Start();
  void Stop();

  void RegisterStateChangeCallback(StateCallback cb);

  [[nodiscard]] HaState CurrentState() const noexcept {
    return current_state_.load(std::memory_order_acquire);
  }
  [[nodiscard]] NodeRole Role() const noexcept { return role_; }
  [[nodiscard]] HaStatistics Stats() const noexcept;

  void TriggerFailoverToActive();
  DetachedTask AsyncGracefulFailback();

 private:
  void EventLoopThread(std::stop_token stop_token);
  void OnTimerTick();
  void OnHeartbeatReceived();

  void SetState(HaState new_state);
  void FlushHaSync();
  void BroadcastGratuitousArp();

  VppClient& client_;
  const nat::config::ApplicationConfig& cfg_;
  std::string node_id_;
  NodeRole role_{NodeRole::kPrimary};
  NatConfigurator* configurator_{nullptr};
  std::atomic<HaState> current_state_{HaState::kInit};

  std::atomic<std::uint64_t> last_peer_heartbeat_ts_{0};
  std::atomic<bool> failback_in_progress_{false};
  std::atomic<uint64_t> boot_time_ms_{0};

  // Heartbeat statistics
  std::atomic<std::uint64_t> heartbeats_sent_{0};
  std::atomic<std::uint64_t> heartbeats_received_{0};
  std::atomic<std::uint64_t> missed_heartbeats_{0};
  std::atomic<std::uint64_t> failover_transitions_{0};

  StateCallback on_state_change_;

  int send_sock_{-1};
  int recv_sock_{-1};
  int epoll_fd_{-1};
  int timer_fd_{-1};
  int event_fd_{-1};

  std::jthread event_loop_thread_;
};

}  // namespace nat::control
