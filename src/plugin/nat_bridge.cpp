#include <arpa/inet.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>

#include <array>
#include <cstdint>
#include <new>
#include <optional>

#include "nat/nat_plugin.h"
#include "nat_engine.hpp"

struct nat_engine {
  nat::engine::NatEngine cpp_engine;
  explicit nat_engine(std::size_t num_threads, std::size_t sessions_per_worker = 2'000'000,
                      std::size_t expected_active = 0) noexcept
      : cpp_engine(num_threads, sessions_per_worker, expected_active) {}
};

namespace {

enum class NatDirection : std::uint8_t { kIn2Out, kOut2In };

struct PacketView {
  struct iphdr* ip{nullptr};
  void* l4{nullptr};
  std::uint32_t src_ip{0};
  std::uint32_t dst_ip{0};
  std::uint16_t src_port{0};
  std::uint16_t dst_port{0};
  std::uint16_t* l4_csum{nullptr};
  std::uint8_t proto{0};
};

[[NAT_FORCE_INLINE]] inline std::uint16_t IncrementalChecksumUpdate(
    std::uint16_t old_csum, std::uint32_t old_val_net, std::uint32_t new_val_net) noexcept {
  std::uint32_t sum{~static_cast<std::uint32_t>(old_csum) & 0xFFFF};
  sum += (~(old_val_net & 0xFFFF) & 0xFFFF) + (new_val_net & 0xFFFF);
  sum += (~(old_val_net >> 16) & 0xFFFF) + (new_val_net >> 16);
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  const auto res = static_cast<std::uint16_t>(~sum & 0xFFFF);
  return res == 0 ? 0xFFFF : res;
}

[[NAT_FORCE_INLINE]] inline std::uint16_t IncrementalChecksumUpdate16(
    std::uint16_t old_csum, std::uint16_t old_val_net, std::uint16_t new_val_net) noexcept {
  std::uint32_t sum{~static_cast<std::uint32_t>(old_csum) & 0xFFFF};
  sum += (~old_val_net & 0xFFFF) + (new_val_net & 0xFFFF);
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  const auto res = static_cast<std::uint16_t>(~sum & 0xFFFF);
  return res == 0 ? 0xFFFF : res;
}

[[NAT_FORCE_INLINE]] inline std::uint16_t IncrementalChecksumUpdateCombined(
    std::uint16_t old_csum, std::uint32_t old_ip_net, std::uint32_t new_ip_net,
    std::uint16_t old_port_net, std::uint16_t new_port_net) noexcept {
  std::uint32_t sum{~static_cast<std::uint32_t>(old_csum) & 0xFFFF};
  sum += (~(old_ip_net & 0xFFFF) & 0xFFFF) + (new_ip_net & 0xFFFF);
  sum += (~(old_ip_net >> 16) & 0xFFFF) + (new_ip_net >> 16);
  sum += (~static_cast<std::uint32_t>(old_port_net) & 0xFFFF) + static_cast<std::uint32_t>(new_port_net);
  while (sum >> 16)
    sum = (sum & 0xFFFF) + (sum >> 16);
  const auto res = static_cast<std::uint16_t>(~sum & 0xFFFF);
  return res == 0 ? 0xFFFF : res;
}

enum class IpCheck : std::uint8_t { kOk, kMalformed, kNonFirstFragment };

[[NAT_FORCE_INLINE]] inline IpCheck CheckIpv4(const struct iphdr* ip) noexcept {
  if (ip == nullptr || ip->version != 4) [[unlikely]] {
    return IpCheck::kMalformed;
  }
  const std::uint32_t ihl = static_cast<std::uint32_t>(ip->ihl) * 4U;
  if (ihl < sizeof(struct iphdr)) [[unlikely]] {
    return IpCheck::kMalformed;
  }
  const std::uint16_t tot_len = ntohs(ip->tot_len);
  if (tot_len < static_cast<std::uint16_t>(ihl)) [[unlikely]] {
    return IpCheck::kMalformed;
  }
  const std::uint16_t frag_off = ntohs(ip->frag_off);
  if ((frag_off & 0x1FFFU) != 0) {
    return IpCheck::kNonFirstFragment;
  }
  return IpCheck::kOk;
}

[[NAT_FORCE_INLINE]] inline std::optional<PacketView> ParsePacketHeaders(
    void* ip4_hdr, void* l4_hdr, std::uint8_t proto) noexcept {
  (void)l4_hdr;
  if (ip4_hdr == nullptr) [[unlikely]] {
    return std::nullopt;
  }
  auto* ip = static_cast<struct iphdr*>(ip4_hdr);
  const std::uint32_t ihl = static_cast<std::uint32_t>(ip->ihl) * 4U;
  if (ihl < sizeof(struct iphdr)) [[unlikely]] {
    return std::nullopt;
  }
  const std::uint16_t tot_len = ntohs(ip->tot_len);
  auto* l4 = reinterpret_cast<std::uint8_t*>(ip) + ihl;

  PacketView v{.ip{ip},
               .l4{l4},
               .src_ip{ntohl(ip->saddr)},
               .dst_ip{ntohl(ip->daddr)},
               .proto{proto}};

  if (proto == IPPROTO_TCP) {
    if (tot_len < static_cast<std::uint16_t>(ihl + sizeof(struct tcphdr))) [[unlikely]] {
      return std::nullopt;
    }
    auto* tcp = reinterpret_cast<struct tcphdr*>(l4);
    v.src_port = ntohs(tcp->source);
    v.dst_port = ntohs(tcp->dest);
    v.l4_csum = &tcp->check;
  } else if (proto == IPPROTO_UDP) {
    if (tot_len < static_cast<std::uint16_t>(ihl + sizeof(struct udphdr))) [[unlikely]] {
      return std::nullopt;
    }
    auto* udp = reinterpret_cast<struct udphdr*>(l4);
    v.src_port = ntohs(udp->source);
    v.dst_port = ntohs(udp->dest);
    v.l4_csum = &udp->check;
  } else if (proto == IPPROTO_ICMP) {
    if (tot_len < static_cast<std::uint16_t>(ihl + sizeof(struct icmphdr))) [[unlikely]] {
      return std::nullopt;
    }
    auto* icmp = reinterpret_cast<struct icmphdr*>(l4);
    if (icmp->type != ICMP_ECHO && icmp->type != ICMP_ECHOREPLY) [[unlikely]] {
      return std::nullopt;
    }
    v.src_port = ntohs(icmp->un.echo.id);
    v.dst_port = 0;
    v.l4_csum = &icmp->checksum;
  } else {
    return std::nullopt;
  }
  return v;
}

[[NAT_FORCE_INLINE]] inline void ApplyNatRewrite(PacketView& pkt, std::uint32_t new_ip,
                                                   std::uint16_t new_port,
                                                   NatDirection dir) noexcept {
  const std::uint32_t old_ip_net = (dir == NatDirection::kIn2Out) ? pkt.ip->saddr : pkt.ip->daddr;
  const std::uint32_t new_ip_net = htonl(new_ip);
  const std::uint16_t old_port_net = htons(
      (dir == NatDirection::kIn2Out || pkt.proto == IPPROTO_ICMP) ? pkt.src_port : pkt.dst_port);
  const std::uint16_t new_port_net = htons(new_port);

  pkt.ip->check = IncrementalChecksumUpdate(pkt.ip->check, old_ip_net, new_ip_net);
  if (dir == NatDirection::kIn2Out)
    pkt.ip->saddr = new_ip_net;
  else
    pkt.ip->daddr = new_ip_net;

  if (pkt.proto == IPPROTO_ICMP) {
    auto* icmp = static_cast<struct icmphdr*>(pkt.l4);
    if (icmp->checksum != 0) {
      icmp->checksum = IncrementalChecksumUpdate16(icmp->checksum, old_port_net, new_port_net);
    }
    icmp->un.echo.id = new_port_net;
    return;
  }

  if (pkt.l4_csum && *pkt.l4_csum != 0) {
    *pkt.l4_csum = IncrementalChecksumUpdateCombined(*pkt.l4_csum, old_ip_net, new_ip_net,
                                                     old_port_net, new_port_net);
  }
  if (pkt.proto == IPPROTO_TCP) {
    auto* tcp = static_cast<struct tcphdr*>(pkt.l4);
    if (dir == NatDirection::kIn2Out)
      tcp->source = new_port_net;
    else
      tcp->dest = new_port_net;
  } else if (pkt.proto == IPPROTO_UDP) {
    auto* udp = static_cast<struct udphdr*>(pkt.l4);
    if (dir == NatDirection::kIn2Out)
      udp->source = new_port_net;
    else
      udp->dest = new_port_net;
  }
}

constexpr std::uint32_t kMaxTranslationBatch{256};

struct BatchEntry {
  PacketView pkt;
  nat::engine::FiveTuple key;
  nat::engine::LookupOutcome outcome;
  std::uint64_t hash{0};
  bool valid{false};
  bool supported{true};
};

[[NAT_FORCE_INLINE]] inline void RecordIpCheckFailure(
    nat::engine::ThreadSessionTable& table, IpCheck check) noexcept {
  auto& telemetry = table.MutableTelemetry();
  if (check == IpCheck::kNonFirstFragment) {
    ++telemetry.fragment_drops;
  } else {
    ++telemetry.malformed_drops;
  }
}

[[NAT_FORCE_INLINE]] inline bool PrepareIn2Out(
    nat::engine::ThreadSessionTable& table, void* ip4_hdr, void* l4_hdr, std::uint8_t proto,
    BatchEntry& entry) noexcept {
  entry.valid = false;
  entry.supported = true;
  const auto ip_check = CheckIpv4(static_cast<const struct iphdr*>(ip4_hdr));
  if (ip_check != IpCheck::kOk) {
    RecordIpCheckFailure(table, ip_check);
    return false;
  }
  auto pkt = ParsePacketHeaders(ip4_hdr, l4_hdr, proto);
  if (!pkt) {
    entry.supported = false;
    return false;
  }
  entry.pkt = *pkt;
  entry.key = nat::engine::FiveTuple{
      .src_ip{entry.pkt.src_ip},
      .dst_ip{entry.pkt.dst_ip},
      .src_port{entry.pkt.src_port},
      .dst_port{entry.pkt.dst_port},
      .protocol{proto},
  };
  entry.hash = table.HashKey(entry.key);
  entry.valid = true;
  return true;
}

[[NAT_FORCE_INLINE]] inline bool PrepareOut2In(
    nat::engine::ThreadSessionTable& table, void* ip4_hdr, void* l4_hdr, std::uint8_t proto,
    BatchEntry& entry) noexcept {
  entry.valid = false;
  entry.supported = true;
  const auto ip_check = CheckIpv4(static_cast<const struct iphdr*>(ip4_hdr));
  if (ip_check != IpCheck::kOk) {
    RecordIpCheckFailure(table, ip_check);
    return false;
  }
  auto pkt = ParsePacketHeaders(ip4_hdr, l4_hdr, proto);
  if (!pkt) {
    entry.supported = false;
    return false;
  }
  entry.pkt = *pkt;
  const bool is_icmp = (proto == IPPROTO_ICMP);
  entry.key = nat::engine::FiveTuple{
      .src_ip{entry.pkt.dst_ip},
      .dst_ip{entry.pkt.src_ip},
      .src_port{is_icmp ? entry.pkt.src_port : entry.pkt.dst_port},
      .dst_port{static_cast<std::uint16_t>(is_icmp ? 0 : entry.pkt.src_port)},
      .protocol{proto},
  };
  entry.hash = table.HashKey(entry.key);
  entry.valid = true;
  return true;
}

[[NAT_FORCE_INLINE]] inline bool NeedsTcpState(const PacketView& pkt) noexcept {
  if (pkt.proto != IPPROTO_TCP) return false;
  const auto* tcp = static_cast<const struct tcphdr*>(pkt.l4);
  return tcp->rst || tcp->fin || (tcp->ack && !tcp->syn);
}

[[NAT_FORCE_INLINE]] inline nat_action_t ApplyIn2OutFast(
    BatchEntry& entry, const nat::engine::LookupOutcome& outcome) noexcept {
  ApplyNatRewrite(entry.pkt, outcome.translated_ip, outcome.translated_port,
                  NatDirection::kIn2Out);
  return NAT_TRANSLATE_OK;
}

[[NAT_FORCE_INLINE]] inline nat_action_t ApplyIn2OutSession(
    nat::engine::ThreadSessionTable& table, BatchEntry& entry,
    nat::engine::NatSession* session, std::uint64_t now_ns) noexcept {
  ApplyNatRewrite(entry.pkt, session->outside.src_ip, session->outside.src_port,
                  NatDirection::kIn2Out);
  if (!table.LazyUpdates()) {
    session->last_active_ts = now_ns;
    session->total_packets++;
  }

  if (entry.key.protocol == IPPROTO_TCP) {
    auto* tcp = static_cast<struct tcphdr*>(entry.pkt.l4);
    if (tcp->rst) {
      table.DeleteSessionByIdx(table.SessionIndex(session));
    } else if (tcp->fin) {
      session->flags = static_cast<std::uint16_t>(
          session->flags & ~nat::engine::kSessionFlagTcpEstablished);
    } else if (tcp->ack && !tcp->syn) {
      session->flags = static_cast<std::uint16_t>(
          session->flags | nat::engine::kSessionFlagTcpEstablished);
    }
  }
  return NAT_TRANSLATE_OK;
}

[[NAT_FORCE_INLINE]] inline nat_action_t FinishOut2In(
    nat::engine::ThreadSessionTable& table, BatchEntry& entry, std::uint64_t now_ns,
    std::uint32_t now_sec) noexcept {
  auto* session = table.LookupOut2InHashed(entry.key, entry.hash, now_ns, now_sec);
  if (!session) [[unlikely]] {
    ++table.MutableTelemetry().out2in_misses;
    return NAT_TRANSLATE_DROP;
  }

  ApplyNatRewrite(entry.pkt, session->inside.src_ip, session->inside.src_port,
                  NatDirection::kOut2In);
  if (!table.LazyUpdates()) {
    session->last_active_ts = now_ns;
    session->total_packets++;
  }

  if (entry.key.protocol == IPPROTO_TCP) {
    auto* tcp = static_cast<struct tcphdr*>(entry.pkt.l4);
    if (tcp->rst) {
      table.DeleteSessionByIdx(table.SessionIndex(session));
    } else if (tcp->fin) {
      session->flags = static_cast<std::uint16_t>(
          session->flags & ~nat::engine::kSessionFlagTcpEstablished);
    } else if (tcp->ack && !tcp->syn) {
      session->flags = static_cast<std::uint16_t>(
          session->flags | nat::engine::kSessionFlagTcpEstablished);
    }
  }
  return NAT_TRANSLATE_OK;
}

[[nodiscard]] inline nat_engine_t* ValidateEngine(nat_engine_t* engine) noexcept {
  if (engine != nullptr && !engine->cpp_engine.IsValid()) {
    delete engine;
    return nullptr;
  }
  return engine;
}

}  // namespace

extern "C" {

nat_engine_t* nat_engine_create(uint32_t num_threads) noexcept {
#if defined(__cpp_exceptions)
  try {
    return ValidateEngine(new nat_engine(num_threads));
  } catch (...) {
    return nullptr;
  }
#else
  // The plugin is built with -fno-exceptions. nothrow new protects the outer
  // allocation; nested container allocations still terminate on OOM.
  return ValidateEngine(new (std::nothrow) nat_engine(num_threads));
#endif
}

nat_engine_t* nat_engine_create_with_capacity(uint32_t num_threads,
                                              uint32_t sessions_per_worker) noexcept {
  return nat_engine_create_with_capacity_and_active(num_threads, sessions_per_worker, 0);
}

nat_engine_t* nat_engine_create_with_capacity_and_active(
    uint32_t num_threads, uint32_t sessions_per_worker,
    uint32_t expected_active) noexcept {
#if defined(__cpp_exceptions)
  try {
    return ValidateEngine(
        new nat_engine(num_threads, sessions_per_worker, expected_active));
  } catch (...) {
    return nullptr;
  }
#else
  return ValidateEngine(new (std::nothrow)
                            nat_engine(num_threads, sessions_per_worker,
                                       expected_active));
#endif
}

void nat_engine_destroy(nat_engine_t* engine) noexcept {
  delete engine;
}

uint64_t nat_engine_get_total_sessions(const nat_engine_t* engine) noexcept {
  return engine ? engine->cpp_engine.GetTotalSessions() : 0;
}

uint64_t nat_engine_get_total_translations(const nat_engine_t* engine) noexcept {
  return engine ? engine->cpp_engine.GetTotalTranslations() : 0;
}

nat_telemetry_t nat_engine_get_telemetry(const nat_engine_t* engine) noexcept {
  if (!engine) return {};
  const auto tel = engine->cpp_engine.GetAggregatedTelemetry();
  return nat_telemetry_t{
      .packets_processed = tel.packets_processed,
      .new_sessions_created = tel.new_sessions_created,
      .create_failures = tel.create_failures,
      .existing_session_hits = tel.existing_session_hits,
      .fast_path_hits = tel.fast_path_hits,
      .hot_cache_hits = tel.hot_cache_hits,
      .lru_evictions = tel.lru_evictions,
      .expired_reclaims = tel.expired_reclaims,
      .active_sessions_high_watermark = tel.active_sessions_high_watermark,
      .pba_blocks_allocated = tel.pba_blocks_allocated,
      .pba_blocks_freed = tel.pba_blocks_freed,
      .pba_port_alloc_failures = tel.pba_port_alloc_failures,
      .bihash_insert_failures = tel.bihash_insert_failures,
      .fragment_drops = tel.fragment_drops,
      .malformed_drops = tel.malformed_drops,
      .out2in_misses = tel.out2in_misses,
      .slow_path_packets = tel.slow_path_packets,
  };
}

void nat_engine_process_tick(nat_engine_t* engine, uint32_t thread_index) noexcept {
  if (engine == nullptr || thread_index >= engine->cpp_engine.ThreadCount()) {
    return;
  }
  engine->cpp_engine.GetThreadTable(thread_index).ProcessTick(
      nat::engine::current_timestamp_ns());
}

void nat_engine_set_ha_enabled(nat_engine_t* engine, bool enabled) noexcept {
  if (engine) {
    engine->cpp_engine.SetHaEnabled(enabled);
  }
}

void nat_engine_set_lazy_updates(nat_engine_t* engine, bool enabled) noexcept {
  if (engine) {
    engine->cpp_engine.SetLazyUpdates(enabled);
  }
}

void nat_engine_set_inline_values(nat_engine_t* engine, bool enabled) noexcept {
  if (engine) {
    engine->cpp_engine.SetInlineValues(enabled);
  }
}

void nat_engine_set_hot_cache(nat_engine_t* engine, uint32_t entries) noexcept {
  if (engine) {
    engine->cpp_engine.SetHotCache(entries);
  }
}

void nat_engine_export_sessions(const nat_engine_t* engine, nat_session_export_cb_t cb,
                                void* ctx) noexcept {
  if (engine) {
    engine->cpp_engine.ExportSessions(cb, ctx);
  }
}

bool nat_engine_import_session(nat_engine_t* engine,
                               const nat_session_record_t* record) noexcept {
  return engine != nullptr && record != nullptr &&
         engine->cpp_engine.ImportSession(*record);
}

void nat_engine_flush_sessions(nat_engine_t* engine) noexcept {
  if (engine) {
    engine->cpp_engine.FlushSessions();
  }
}

void nat_engine_set_pool(nat_engine_t* engine, uint32_t start_ip, uint32_t end_ip) noexcept {
  if (engine) {
    engine->cpp_engine.SetPool(start_ip, end_ip);
  }
}

void nat_engine_set_block_size(nat_engine_t* engine, uint16_t block_size) noexcept {
  if (engine) {
    engine->cpp_engine.SetBlockSize(block_size);
  }
}

void nat_engine_set_max_blocks_per_user(nat_engine_t* engine, uint16_t max_blocks) noexcept {
  if (engine) {
    engine->cpp_engine.SetMaxBlocksPerUser(max_blocks);
  }
}

void nat_engine_set_vector_config(nat_engine_t* engine, uint16_t batch_size,
                                  uint8_t prefetch_distance) noexcept {
  if (engine) {
    engine->cpp_engine.SetVectorConfig(batch_size, prefetch_distance);
  }
}

bool nat_engine_set_sessions_per_worker(nat_engine_t* engine,
                                        uint32_t sessions_per_worker) noexcept {
  return engine != nullptr &&
         engine->cpp_engine.ReconfigureSessions(sessions_per_worker);
}

void nat_engine_set_max_sessions_per_worker(nat_engine_t* engine,
                                            uint32_t max_sessions) noexcept {
  if (engine) {
    engine->cpp_engine.SetMaxSessions(max_sessions);
  }
}

void nat_engine_set_force_eviction(nat_engine_t* engine, bool enabled) noexcept {
  if (engine) {
    engine->cpp_engine.SetForceEvictOnOverflow(enabled);
  }
}

void nat_engine_set_timeouts(nat_engine_t* engine, uint32_t udp_sec,
                             uint32_t tcp_est_sec, uint32_t tcp_trans_sec,
                             uint32_t icmp_sec) noexcept {
  if (!engine) return;
  engine->cpp_engine.SetTimeouts(nat::engine::SessionTimeouts{
      .udp_sec = udp_sec,
      .tcp_established_sec = tcp_est_sec,
      .tcp_transitory_sec = tcp_trans_sec,
      .icmp_sec = icmp_sec,
  });
}

[[gnu::hot]] nat_action_t nat_translate_in2out(nat_engine_t* engine, uint32_t thread_index,
                                               void* ip4_hdr, void* l4_hdr,
                                               uint8_t proto) noexcept {
  if (engine == nullptr || thread_index >= engine->cpp_engine.ThreadCount()) [[unlikely]] {
    return NAT_TRANSLATE_DROP;
  }

  auto& table = engine->cpp_engine.GetThreadTable(thread_index);
  table.RecordPacketProcessed();

  const auto ip_check = CheckIpv4(static_cast<const struct iphdr*>(ip4_hdr));
  if (ip_check != IpCheck::kOk) [[unlikely]] {
    auto& telemetry = table.MutableTelemetry();
    if (ip_check == IpCheck::kNonFirstFragment) {
      ++telemetry.fragment_drops;
    } else {
      ++telemetry.malformed_drops;
    }
    return NAT_TRANSLATE_DROP;
  }

  auto pkt = ParsePacketHeaders(ip4_hdr, l4_hdr, proto);
  if (!pkt)
    return NAT_TRANSLATE_OK;

  const nat::engine::FiveTuple in_key{
      .src_ip{pkt->src_ip},
      .dst_ip{pkt->dst_ip},
      .src_port{pkt->src_port},
      .dst_port{pkt->dst_port},
      .protocol{proto},
  };
  std::uint32_t now_sec = 0;
  const std::uint64_t now_ns = nat::engine::current_timestamp_ns(&now_sec);
  auto outcome = table.LookupIn2OutOutcome(in_key, now_ns, now_sec);
  auto* session = outcome.session;

  if (!session) [[unlikely]] {
    auto port = table.AllocatePortForUser(pkt->src_ip, table.PublicIp());
    if (!port) {
      table.RecordCreateFailure();
      ++table.MutableTelemetry().slow_path_packets;
      return NAT_TRANSLATE_SLOW_PATH;
    }
    session = table.CreateSessionWithOutcome(
        outcome, in_key, nat::engine::FiveTuple{
                             .src_ip{port->public_ip},
                             .dst_ip{pkt->dst_ip},
                             .src_port{port->port},
                             .dst_port{pkt->dst_port},
                             .protocol{proto},
                         });
    if (!session) [[unlikely]] {
      table.ReleasePortForUser(pkt->src_ip, port->public_ip, port->port);
      table.RecordCreateFailure();
      ++table.MutableTelemetry().slow_path_packets;
      return NAT_TRANSLATE_SLOW_PATH;
    }
  }

  ApplyNatRewrite(*pkt, session->outside.src_ip, session->outside.src_port, NatDirection::kIn2Out);
  if (!table.LazyUpdates()) {
    session->last_active_ts = now_ns;
    session->total_packets++;
  }

  if (proto == IPPROTO_TCP) {
    auto* tcp = static_cast<struct tcphdr*>(pkt->l4);
    if (tcp->rst) {
      table.DeleteSessionByIdx(table.SessionIndex(session));
    } else if (tcp->fin) {
      session->flags =
          static_cast<std::uint16_t>(session->flags & ~nat::engine::kSessionFlagTcpEstablished);
    } else if (tcp->ack && !tcp->syn) {
      session->flags =
          static_cast<std::uint16_t>(session->flags | nat::engine::kSessionFlagTcpEstablished);
    }
  }
  return NAT_TRANSLATE_OK;
}

[[gnu::hot]] nat_action_t nat_translate_out2in(nat_engine_t* engine, uint32_t thread_index,
                                               void* ip4_hdr, void* l4_hdr,
                                               uint8_t proto) noexcept {
  if (engine == nullptr || thread_index >= engine->cpp_engine.ThreadCount()) [[unlikely]] {
    return NAT_TRANSLATE_DROP;
  }

  auto& table = engine->cpp_engine.GetThreadTable(thread_index);
  table.RecordPacketProcessed();

  const auto ip_check = CheckIpv4(static_cast<const struct iphdr*>(ip4_hdr));
  if (ip_check != IpCheck::kOk) [[unlikely]] {
    auto& telemetry = table.MutableTelemetry();
    if (ip_check == IpCheck::kNonFirstFragment) {
      ++telemetry.fragment_drops;
    } else {
      ++telemetry.malformed_drops;
    }
    return NAT_TRANSLATE_DROP;
  }

  auto pkt = ParsePacketHeaders(ip4_hdr, l4_hdr, proto);
  if (!pkt)
    return NAT_TRANSLATE_OK;

  const bool is_icmp = (proto == IPPROTO_ICMP);
  const nat::engine::FiveTuple out_key{
      .src_ip{pkt->dst_ip},
      .dst_ip{pkt->src_ip},
      .src_port{is_icmp ? pkt->src_port : pkt->dst_port},
      .dst_port{static_cast<std::uint16_t>(is_icmp ? 0 : pkt->src_port)},
      .protocol{proto},
  };
  std::uint32_t now_sec = 0;
  const std::uint64_t now_ns = nat::engine::current_timestamp_ns(&now_sec);
  auto* session = table.LookupOut2In(out_key, now_ns, now_sec);
  if (!session) [[unlikely]] {
    ++table.MutableTelemetry().out2in_misses;
    return NAT_TRANSLATE_DROP;
  }

  ApplyNatRewrite(*pkt, session->inside.src_ip, session->inside.src_port, NatDirection::kOut2In);
  if (!table.LazyUpdates()) {
    session->last_active_ts = now_ns;
    session->total_packets++;
  }

  if (proto == IPPROTO_TCP) {
    auto* tcp = static_cast<struct tcphdr*>(pkt->l4);
    if (tcp->rst) {
      table.DeleteSessionByIdx(table.SessionIndex(session));
    } else if (tcp->fin) {
      session->flags =
          static_cast<std::uint16_t>(session->flags & ~nat::engine::kSessionFlagTcpEstablished);
    } else if (tcp->ack && !tcp->syn) {
      session->flags =
          static_cast<std::uint16_t>(session->flags | nat::engine::kSessionFlagTcpEstablished);
    }
  }
  return NAT_TRANSLATE_OK;
}

void nat_translate_in2out_batch(nat_engine_t* engine, uint32_t thread_index,
                                void** ip4_headers, void** l4_headers, uint8_t* protos,
                                nat_action_t* actions, uint32_t count) noexcept {
  if (engine == nullptr || count == 0 || count > kMaxTranslationBatch ||
      ip4_headers == nullptr || l4_headers == nullptr || protos == nullptr ||
      actions == nullptr || thread_index >= engine->cpp_engine.ThreadCount()) {
    if (actions != nullptr) {
      for (std::uint32_t i = 0; i < count; ++i) actions[i] = NAT_TRANSLATE_DROP;
    }
    return;
  }

  auto& table = engine->cpp_engine.GetThreadTable(thread_index);
  table.RecordPackets(count);
  std::uint32_t now_sec = 0;
  const std::uint64_t now_ns = nat::engine::current_timestamp_ns(&now_sec);

  BatchEntry entries[kMaxTranslationBatch];
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!PrepareIn2Out(table, ip4_headers[i], l4_headers[i], protos[i], entries[i])) {
      actions[i] = entries[i].supported ? NAT_TRANSLATE_DROP : NAT_TRANSLATE_OK;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (entries[i].valid) {
      table.PrefetchIn2OutHash(entries[i].hash);
      table.PrefetchIn2OutDataHash(entries[i].hash);
    }
  }

  table.LookupIn2OutBatch(entries, count, now_ns, now_sec);

  nat::engine::PendingCreate pending[kMaxTranslationBatch];
  std::size_t pending_count = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!entries[i].valid) continue;
    if (entries[i].outcome.session != nullptr) {
      actions[i] = ApplyIn2OutSession(table, entries[i], entries[i].outcome.session, now_ns);
    } else if (entries[i].outcome.fast_path) {
      if (NeedsTcpState(entries[i].pkt)) {
        const auto full = table.LookupIn2OutOutcomeHashed(
            entries[i].key, entries[i].hash, now_ns, now_sec);
        if (full.session != nullptr) {
          actions[i] = ApplyIn2OutSession(table, entries[i], full.session, now_ns);
        } else {
          actions[i] = NAT_TRANSLATE_DROP;
        }
      } else {
        actions[i] = ApplyIn2OutFast(entries[i], entries[i].outcome);
      }
    } else {
      actions[i] = NAT_TRANSLATE_DROP;
      pending[pending_count++] = nat::engine::PendingCreate{
          .outcome = entries[i].outcome,
          .in_key = entries[i].key,
          .entry_index = i,
      };
    }
  }

  if (pending_count != 0) {
    table.CreateSessionsBatch(pending, pending_count, now_ns);
    for (std::size_t i = 0; i < pending_count; ++i) {
      if (pending[i].session == nullptr) {
        actions[pending[i].entry_index] = NAT_TRANSLATE_SLOW_PATH;
        ++table.MutableTelemetry().slow_path_packets;
        continue;
      }
      actions[pending[i].entry_index] =
          ApplyIn2OutSession(table, entries[pending[i].entry_index], pending[i].session, now_ns);
    }
  }
}

void nat_translate_out2in_batch(nat_engine_t* engine, uint32_t thread_index,
                                void** ip4_headers, void** l4_headers, uint8_t* protos,
                                nat_action_t* actions, uint32_t count) noexcept {
  if (engine == nullptr || count == 0 || count > kMaxTranslationBatch ||
      ip4_headers == nullptr || l4_headers == nullptr || protos == nullptr ||
      actions == nullptr || thread_index >= engine->cpp_engine.ThreadCount()) {
    if (actions != nullptr) {
      for (std::uint32_t i = 0; i < count; ++i) actions[i] = NAT_TRANSLATE_DROP;
    }
    return;
  }

  auto& table = engine->cpp_engine.GetThreadTable(thread_index);
  table.RecordPackets(count);
  std::uint32_t now_sec = 0;
  const std::uint64_t now_ns = nat::engine::current_timestamp_ns(&now_sec);

  BatchEntry entries[kMaxTranslationBatch];
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!PrepareOut2In(table, ip4_headers[i], l4_headers[i], protos[i], entries[i])) {
      actions[i] = entries[i].supported ? NAT_TRANSLATE_DROP : NAT_TRANSLATE_OK;
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (entries[i].valid) {
      table.PrefetchOut2InHash(entries[i].hash);
      table.PrefetchOut2InDataHash(entries[i].hash);
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (entries[i].valid) {
      actions[i] = FinishOut2In(table, entries[i], now_ns, now_sec);
    }
  }
}

bool nat_engine_delete_session(nat_engine_t* engine, uint32_t thread_index, void* ip4_hdr,
                               void* l4_hdr, uint8_t proto) noexcept {
  if (!engine) return false;
  auto pkt = ParsePacketHeaders(ip4_hdr, l4_hdr, proto);
  if (!pkt) return false;
  const nat::engine::FiveTuple in_key{
      .src_ip{pkt->src_ip},
      .dst_ip{pkt->dst_ip},
      .src_port{pkt->src_port},
      .dst_port{pkt->dst_port},
      .protocol{proto},
  };
  return engine->cpp_engine.GetThreadTable(thread_index).DeleteSession(in_key);
}

}  // extern "C"
