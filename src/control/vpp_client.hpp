#pragma once

#include <expected>
#include <format>
#include <memory>
#include <string>
#include <vapi/vapi.hpp>

namespace nat::control {

/**
 * @brief RAII wrapper around VPP C++ binary API (vapi::Connection).
 * Uses std::expected for modern C++23 error handling without exceptions.
 */
class VppClient final {
 public:
  VppClient() : con_{std::make_unique<vapi::Connection>()} {}
  ~VppClient() { Disconnect(); }

  VppClient(const VppClient&) = delete;
  VppClient& operator=(const VppClient&) = delete;
  VppClient(VppClient&&) noexcept = default;
  VppClient& operator=(VppClient&&) noexcept = default;

  [[nodiscard]] static std::expected<VppClient, std::string> Create(
      const std::string& app_name, const std::string& socket_path = "",
      int max_outstanding_requests = 32, int response_queue_size = 32) noexcept {
    VppClient client;
    auto res = client.Connect(app_name, socket_path, max_outstanding_requests, response_queue_size);
    if (!res) {
      return std::unexpected(res.error());
    }
    return client;
  }

  [[nodiscard]] std::expected<void, std::string> Connect(const std::string& app_name,
                                                         const std::string& socket_path = "",
                                                         int max_outstanding_requests = 32,
                                                         int response_queue_size = 32) noexcept {
    if (connected_) {
      Disconnect();
    }
    app_name_ = app_name;
    socket_path_ = socket_path;
    const bool use_uds = !socket_path_.empty();
    const char* path = use_uds ? socket_path_.c_str() : nullptr;

    const vapi_error_e rv = con_->connect(app_name_.c_str(), path, max_outstanding_requests,
                                          response_queue_size, true, use_uds);
    if (rv != VAPI_OK) {
      return std::unexpected(std::format("Failed to connect to VPP ({}), error code: {}",
                                         use_uds ? socket_path_ : "shm", static_cast<int>(rv)));
    }
    connected_ = true;
    return {};
  }

  void Disconnect() noexcept {
    if (connected_ && con_) {
      con_->disconnect();
      connected_ = false;
    }
  }

  [[nodiscard]] vapi::Connection& Connection() noexcept { return *con_; }
  [[nodiscard]] const vapi::Connection& Connection() const noexcept { return *con_; }

  [[nodiscard]] bool IsConnected() const noexcept { return connected_; }
  [[nodiscard]] const std::string& AppName() const noexcept { return app_name_; }
  [[nodiscard]] const std::string& SocketPath() const noexcept { return socket_path_; }

 private:
  std::string app_name_{};
  std::string socket_path_{};
  std::unique_ptr<vapi::Connection> con_{};
  bool connected_{false};
};

}  // namespace nat::control
