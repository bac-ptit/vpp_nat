#include "nat_ha_sync.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint32_t kMagic{0x4E504241};  // "NPBA"
constexpr std::uint16_t kVersion{1};
constexpr std::uint32_t kMaxBatch{256};

enum class MessageType : std::uint16_t {
  kSnapshotBegin = 1,
  kSnapshotRecords = 2,
  kSnapshotEnd = 3,
  kHeartbeat = 4,
};

struct HaHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t type;
  std::uint32_t count;
  std::uint32_t payload_bytes;
};

struct ShadowKey {
  std::uint32_t thread;
  std::uint32_t src_ip;
  std::uint32_t dst_ip;
  std::uint16_t src_port;
  std::uint16_t dst_port;
  std::uint8_t protocol;

  bool operator==(const ShadowKey&) const = default;
};

struct ShadowHash {
  [[nodiscard]] std::size_t operator()(const ShadowKey& key) const noexcept {
    std::size_t h{key.thread};
    auto mix = [&h](std::uint32_t value) {
      h ^= static_cast<std::size_t>(value) + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    };
    mix(key.src_ip);
    mix(key.dst_ip);
    mix(static_cast<std::uint32_t>(key.src_port) << 16 | key.dst_port);
    mix(key.protocol);
    return h;
  }
};

std::mutex g_config_mutex;
std::string g_local_ip{"0.0.0.0"};
std::string g_peer_ip{"127.0.0.1"};
std::uint16_t g_local_port{3234};
std::uint16_t g_peer_port{3235};
std::uint32_t g_interval_ms{1000};
nat_engine_t* g_engine{nullptr};

std::atomic<int> g_role{0};
std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_stop{false};
std::atomic<bool> g_force_snapshot{false};
std::atomic<std::uint64_t> g_shadow_count{0};
std::atomic<std::uint64_t> g_last_sync_ms{0};
std::atomic<std::uint64_t> g_snapshots_sent{0};
std::atomic<std::uint64_t> g_records_sent{0};
std::atomic<std::uint64_t> g_records_imported{0};
std::atomic<std::uint64_t> g_records_failed{0};

std::mutex g_shadow_mutex;
std::unordered_map<ShadowKey, nat_session_record_t, ShadowHash> g_shadow;
std::thread g_thread;

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool SendAll(int fd, const void* data, std::size_t len) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t offset{0};
  while (offset < len) {
    const ssize_t written = ::send(fd, bytes + offset, len - offset, MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
      continue;
    }
    return false;
  }
  return true;
}

bool RecvAll(int fd, void* data, std::size_t len) noexcept {
  auto* bytes = static_cast<std::uint8_t*>(data);
  std::size_t offset{0};
  while (offset < len && !g_stop.load(std::memory_order_relaxed)) {
    const ssize_t received = ::recv(fd, bytes + offset, len - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received == 0) return false;
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
    return false;
  }
  return offset == len;
}

bool SendHeader(int fd, MessageType type, std::uint32_t count,
                std::uint32_t payload_bytes) noexcept {
  const HaHeader header{
      .magic = kMagic,
      .version = kVersion,
      .type = static_cast<std::uint16_t>(type),
      .count = count,
      .payload_bytes = payload_bytes,
  };
  return SendAll(fd, &header, sizeof(header));
}

bool SendRecords(int fd, const std::vector<nat_session_record_t>& records) noexcept {
  if (records.empty()) return true;
  const std::uint32_t count = static_cast<std::uint32_t>(records.size());
  const std::uint32_t bytes = count * static_cast<std::uint32_t>(sizeof(nat_session_record_t));
  return SendHeader(fd, MessageType::kSnapshotRecords, count, bytes) &&
         SendAll(fd, records.data(), bytes);
}

int ConnectToPeer() noexcept {
  in_addr peer{};
  if (::inet_pton(AF_INET, g_peer_ip.c_str(), &peer) != 1) return -1;

  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_peer_port);
  addr.sin_addr = peer;

  const int flags = ::fcntl(fd, F_GETFL, 0);
  (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 &&
      errno != EINPROGRESS) {
    ::close(fd);
    return -1;
  }

  pollfd pfd{.fd = fd, .events = POLLOUT, .revents = 0};
  if (::poll(&pfd, 1, 300) <= 0) {
    ::close(fd);
    return -1;
  }
  int error{0};
  socklen_t len{sizeof(error)};
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
    ::close(fd);
    return -1;
  }
  (void)::fcntl(fd, F_SETFL, flags);
  const int one{1};
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  timeval timeout{.tv_sec = 1, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  return fd;
}

struct SnapshotContext {
  int fd{-1};
  bool ok{true};
  std::uint64_t sent{0};
  std::vector<nat_session_record_t> batch;
};

void SnapshotCallback(void* opaque, const nat_session_record_t* record) {
  auto* context = static_cast<SnapshotContext*>(opaque);
  if (!context->ok) return;
  context->batch.push_back(*record);
  if (context->batch.size() >= kMaxBatch) {
    if (!SendRecords(context->fd, context->batch)) {
      context->ok = false;
      return;
    }
    context->sent += context->batch.size();
    context->batch.clear();
  }
}

bool SendSnapshot(int fd) noexcept {
  if (g_engine == nullptr) return false;
  if (!SendHeader(fd, MessageType::kSnapshotBegin, 0, 0)) return false;

  SnapshotContext context{};
  context.fd = fd;
  context.batch.reserve(kMaxBatch);
  nat_engine_export_sessions(g_engine, SnapshotCallback, &context);

  if (context.ok && !context.batch.empty()) {
    if (SendRecords(fd, context.batch)) {
      context.sent += context.batch.size();
    } else {
      context.ok = false;
    }
  }
  if (!context.ok) return false;
  if (!SendHeader(fd, MessageType::kSnapshotEnd, 0, 0)) return false;

  g_records_sent.fetch_add(context.sent, std::memory_order_relaxed);
  g_snapshots_sent.fetch_add(1, std::memory_order_relaxed);
  return true;
}

void ActiveLoop() noexcept {
  int fd{-1};
  std::uint64_t last_snapshot{0};

  while (!g_stop.load(std::memory_order_relaxed)) {
    if (fd < 0) {
      fd = ConnectToPeer();
      if (fd < 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        continue;
      }
      g_connected.store(true, std::memory_order_relaxed);
      g_force_snapshot.store(true, std::memory_order_relaxed);
    }

    const std::uint64_t now = NowMs();
    const bool forced = g_force_snapshot.exchange(false, std::memory_order_relaxed);
    if (forced || now - last_snapshot >= g_interval_ms) {
      last_snapshot = now;
      if (!SendSnapshot(fd)) {
        ::close(fd);
        fd = -1;
        g_connected.store(false, std::memory_order_relaxed);
        continue;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  if (fd >= 0) ::close(fd);
  g_connected.store(false, std::memory_order_relaxed);
}

void ApplyRecords(const std::vector<nat_session_record_t>& records) {
  std::lock_guard lock(g_shadow_mutex);
  for (const auto& record : records) {
    const ShadowKey key{
        .thread = record.thread_index,
        .src_ip = record.inside_src_ip,
        .dst_ip = record.inside_dst_ip,
        .src_port = record.inside_src_port,
        .dst_port = record.inside_dst_port,
        .protocol = record.protocol,
    };
    g_shadow[key] = record;
  }
  g_shadow_count.store(g_shadow.size(), std::memory_order_relaxed);
}

void StandbyLoop() noexcept {
  in_addr local{};
  if (::inet_pton(AF_INET, g_local_ip.c_str(), &local) != 1) {
    local.s_addr = htonl(INADDR_ANY);
  }

  const int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd < 0) return;

  const int one{1};
  (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(g_local_port);
  addr.sin_addr = local;
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
      ::listen(listen_fd, 1) < 0) {
    ::close(listen_fd);
    return;
  }

  timeval accept_timeout{.tv_sec = 0, .tv_usec = 200000};
  (void)::setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &accept_timeout,
                     sizeof(accept_timeout));

  while (!g_stop.load(std::memory_order_relaxed)) {
    const int client = ::accept(listen_fd, nullptr, nullptr);
    if (client < 0) continue;

    g_connected.store(true, std::memory_order_relaxed);
    timeval recv_timeout{.tv_sec = 0, .tv_usec = 200000};
    (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout,
                       sizeof(recv_timeout));
    (void)::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    while (!g_stop.load(std::memory_order_relaxed)) {
      HaHeader header{};
      if (!RecvAll(client, &header, sizeof(header))) break;
      if (header.magic != kMagic || header.version != kVersion) break;

      const auto type = static_cast<MessageType>(header.type);
      if (type == MessageType::kSnapshotBegin) {
        std::lock_guard lock(g_shadow_mutex);
        g_shadow.clear();
        g_shadow_count.store(0, std::memory_order_relaxed);
      } else if (type == MessageType::kSnapshotRecords) {
        if (header.count > 4096 ||
            header.payload_bytes != header.count * sizeof(nat_session_record_t)) {
          break;
        }
        std::vector<nat_session_record_t> records(header.count);
        if (header.payload_bytes > 0 &&
            !RecvAll(client, records.data(), header.payload_bytes)) {
          break;
        }
        ApplyRecords(records);
      } else if (type == MessageType::kSnapshotEnd) {
        g_last_sync_ms.store(NowMs(), std::memory_order_relaxed);
      }
    }

    ::close(client);
    g_connected.store(false, std::memory_order_relaxed);
  }

  ::close(listen_fd);
}

}  // namespace

extern "C" {

void nat_ha_sync_start(nat_engine_t* engine, int role, const char* local_ip,
                       uint16_t local_port, const char* peer_ip, uint16_t peer_port,
                       uint32_t interval_ms) {
  nat_ha_sync_stop();

  {
    std::lock_guard lock(g_config_mutex);
    g_engine = engine;
    g_local_ip = (local_ip != nullptr) ? local_ip : "0.0.0.0";
    g_peer_ip = (peer_ip != nullptr) ? peer_ip : "127.0.0.1";
    g_local_port = local_port;
    g_peer_port = peer_port;
    g_interval_ms = (interval_ms == 0) ? 1000 : interval_ms;
  }

  g_role.store(role);
  if (role != 1 && role != 2) return;

  g_stop.store(false);
  g_force_snapshot.store(true);
  g_running.store(true);
  if (role == 1) {
    g_thread = std::thread(ActiveLoop);
  } else {
    g_thread = std::thread(StandbyLoop);
  }
}

void nat_ha_sync_stop(void) {
  g_stop.store(true);
  if (g_thread.joinable()) {
    g_thread.join();
  }
  g_running.store(false);
  g_connected.store(false);
  g_role.store(0);
}

bool nat_ha_sync_promote(nat_engine_t* engine) {
  if (engine == nullptr) return false;

  std::string local_ip;
  std::string peer_ip;
  std::uint16_t local_port{0};
  std::uint16_t peer_port{0};
  std::uint32_t interval{1000};
  {
    std::lock_guard lock(g_config_mutex);
    local_ip = g_local_ip;
    peer_ip = g_peer_ip;
    local_port = g_local_port;
    peer_port = g_peer_port;
    interval = g_interval_ms;
  }

  nat_ha_sync_stop();

  std::vector<nat_session_record_t> records;
  {
    std::lock_guard lock(g_shadow_mutex);
    records.reserve(g_shadow.size());
    for (const auto& [key, record] : g_shadow) {
      (void)key;
      records.push_back(record);
    }
    g_shadow.clear();
    g_shadow_count.store(0, std::memory_order_relaxed);
  }

  nat_engine_flush_sessions(engine);
  std::uint64_t imported{0};
  std::uint64_t failed{0};
  for (const auto& record : records) {
    if (nat_engine_import_session(engine, &record)) {
      ++imported;
    } else {
      ++failed;
    }
  }
  g_records_imported.store(imported, std::memory_order_relaxed);
  g_records_failed.store(failed, std::memory_order_relaxed);

  // The promoted node becomes the new active sender.
  nat_ha_sync_start(engine, 1, local_ip.c_str(), local_port, peer_ip.c_str(), peer_port,
                    interval);
  return failed == 0;
}

void nat_ha_sync_force_snapshot(void) {
  g_force_snapshot.store(true, std::memory_order_relaxed);
}

void nat_ha_sync_get_status(nat_ha_status_t* out) {
  if (out == nullptr) return;
  out->role = g_role.load(std::memory_order_relaxed);
  out->running = g_running.load(std::memory_order_relaxed);
  out->connected = g_connected.load(std::memory_order_relaxed);
  out->shadow_sessions = g_shadow_count.load(std::memory_order_relaxed);
  out->last_sync_ms = g_last_sync_ms.load(std::memory_order_relaxed);
  out->snapshots_sent = g_snapshots_sent.load(std::memory_order_relaxed);
  out->records_sent = g_records_sent.load(std::memory_order_relaxed);
  out->records_imported = g_records_imported.load(std::memory_order_relaxed);
  out->records_failed = g_records_failed.load(std::memory_order_relaxed);
}

}  // extern "C"
