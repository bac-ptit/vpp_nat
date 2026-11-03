#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "nat/nat_plugin.h"

namespace {

constexpr std::uint32_t kInsideIp = 0x0A0A0105;   // 10.10.1.5
constexpr std::uint32_t kOutsideIp = 0xC6120002;  // 198.18.0.2
constexpr std::uint32_t kPublicIp = 0xCB007101;   // 203.0.113.1

std::uint32_t SumWords(const void* data, std::size_t len, std::uint32_t sum = 0) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  while (len > 1) {
    std::uint16_t word{0};
    std::memcpy(&word, p, sizeof(word));
    sum += word;
    p += 2;
    len -= 2;
  }
  if (len != 0) {
    sum += *p;
  }
  return sum;
}

std::uint16_t Fold(std::uint32_t sum) {
  while ((sum >> 16) != 0) {
    sum = (sum & 0xFFFFU) + (sum >> 16);
  }
  const auto result = static_cast<std::uint16_t>(~sum & 0xFFFFU);
  return result == 0 ? 0xFFFF : result;
}

std::uint16_t IpChecksum(const struct iphdr& ip) {
  struct iphdr copy = ip;
  copy.check = 0;
  return Fold(SumWords(&copy, sizeof(copy)));
}

std::uint16_t IcmpChecksum(const struct icmphdr& icmp) {
  struct icmphdr copy = icmp;
  copy.checksum = 0;
  return Fold(SumWords(&copy, sizeof(copy)));
}

struct IcmpPacket {
  struct iphdr ip;
  struct icmphdr icmp;
};

void InitIcmp(IcmpPacket& pkt, std::uint32_t src_ip, std::uint32_t dst_ip, std::uint16_t id,
              std::uint8_t type) {
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.ip.version = 4;
  pkt.ip.ihl = 5;
  pkt.ip.tot_len = htons(static_cast<std::uint16_t>(sizeof(pkt)));
  pkt.ip.ttl = 64;
  pkt.ip.protocol = IPPROTO_ICMP;
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(dst_ip);
  pkt.ip.check = IpChecksum(pkt.ip);

  pkt.icmp.type = type;
  pkt.icmp.code = 0;
  pkt.icmp.un.echo.id = htons(id);
  pkt.icmp.un.echo.sequence = htons(1);
  pkt.icmp.checksum = IcmpChecksum(pkt.icmp);
}

struct TcpPacket {
  struct iphdr ip;
  struct tcphdr tcp;
};

void InitTcp(TcpPacket& pkt, std::uint32_t src_ip, std::uint32_t dst_ip, std::uint16_t src_port,
             std::uint16_t dst_port, bool syn, bool ack, bool fin, bool rst) {
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.ip.version = 4;
  pkt.ip.ihl = 5;
  pkt.ip.tot_len = htons(static_cast<std::uint16_t>(sizeof(pkt)));
  pkt.ip.ttl = 64;
  pkt.ip.protocol = IPPROTO_TCP;
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(dst_ip);
  pkt.ip.check = IpChecksum(pkt.ip);

  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(dst_port);
  pkt.tcp.doff = 5;
  pkt.tcp.syn = syn ? 1 : 0;
  pkt.tcp.ack = ack ? 1 : 0;
  pkt.tcp.fin = fin ? 1 : 0;
  pkt.tcp.rst = rst ? 1 : 0;
  pkt.tcp.check = 0;  // bridge skips the incremental L4 update when zero
}

}  // namespace

int main() {
  std::printf("========================================\n");
  std::printf("  Running Unit Test: NAT bridge paths   \n");
  std::printf("========================================\n");

  nat_engine_t* engine = nat_engine_create_with_capacity(1, 4096);
  assert(engine != nullptr);

  // --- ICMP echo: in2out rewrites source and echo id, checksums stay valid ---
  IcmpPacket echo;
  InitIcmp(echo, kInsideIp, kOutsideIp, 1234, ICMP_ECHO);
  assert(nat_translate_in2out(engine, 0, &echo.ip, &echo.icmp, IPPROTO_ICMP) ==
         NAT_TRANSLATE_OK);
  assert(ntohl(echo.ip.saddr) == kPublicIp);
  const std::uint16_t public_id = ntohs(echo.icmp.un.echo.id);
  assert(public_id != 1234);
  assert(echo.ip.check == IpChecksum(echo.ip));
  assert(echo.icmp.checksum == IcmpChecksum(echo.icmp));
  std::printf("[+] ICMP in2out: id %u -> %u, checksums valid\n", 1234, public_id);

  // --- ICMP echo reply: out2in restores destination and id ---
  IcmpPacket reply;
  InitIcmp(reply, kOutsideIp, kPublicIp, public_id, ICMP_ECHOREPLY);
  assert(nat_translate_out2in(engine, 0, &reply.ip, &reply.icmp, IPPROTO_ICMP) ==
         NAT_TRANSLATE_OK);
  assert(ntohl(reply.ip.daddr) == kInsideIp);
  assert(ntohs(reply.icmp.un.echo.id) == 1234);
  assert(reply.ip.check == IpChecksum(reply.ip));
  assert(reply.icmp.checksum == IcmpChecksum(reply.icmp));
  std::printf("[+] ICMP out2in: id restored, checksums valid\n");

  // --- Non-first fragment is dropped, not parsed as L4 ---
  const auto before_frag = nat_engine_get_telemetry(engine).fragment_drops;
  IcmpPacket fragment;
  InitIcmp(fragment, kInsideIp, kOutsideIp, 1234, ICMP_ECHO);
  fragment.ip.frag_off = htons(0x0001);
  fragment.ip.check = IpChecksum(fragment.ip);
  assert(nat_translate_in2out(engine, 0, &fragment.ip, &fragment.icmp, IPPROTO_ICMP) ==
         NAT_TRANSLATE_DROP);
  assert(nat_engine_get_telemetry(engine).fragment_drops == before_frag + 1);
  std::printf("[+] non-first fragment dropped and counted\n");

  // --- Malformed IHL is dropped ---
  const auto before_malformed = nat_engine_get_telemetry(engine).malformed_drops;
  IcmpPacket malformed;
  InitIcmp(malformed, kInsideIp, kOutsideIp, 1234, ICMP_ECHO);
  malformed.ip.ihl = 4;
  malformed.ip.check = IpChecksum(malformed.ip);
  assert(nat_translate_in2out(engine, 0, &malformed.ip, &malformed.icmp, IPPROTO_ICMP) ==
         NAT_TRANSLATE_DROP);
  assert(nat_engine_get_telemetry(engine).malformed_drops == before_malformed + 1);
  std::printf("[+] malformed IHL dropped and counted\n");

  // --- TCP: SYN creates, ACK marks established, RST deletes ---
  const std::uint64_t base_sessions = nat_engine_get_total_sessions(engine);

  TcpPacket syn;
  InitTcp(syn, kInsideIp, kOutsideIp, 40000, 80, true, false, false, false);
  assert(nat_translate_in2out(engine, 0, &syn.ip, &syn.tcp, IPPROTO_TCP) == NAT_TRANSLATE_OK);
  assert(nat_engine_get_total_sessions(engine) == base_sessions + 1);

  TcpPacket ack;
  InitTcp(ack, kInsideIp, kOutsideIp, 40000, 80, false, true, false, false);
  assert(nat_translate_in2out(engine, 0, &ack.ip, &ack.tcp, IPPROTO_TCP) == NAT_TRANSLATE_OK);
  assert(nat_engine_get_total_sessions(engine) == base_sessions + 1);

  TcpPacket rst;
  InitTcp(rst, kInsideIp, kOutsideIp, 40000, 80, false, false, false, true);
  assert(nat_translate_in2out(engine, 0, &rst.ip, &rst.tcp, IPPROTO_TCP) == NAT_TRANSLATE_OK);
  assert(nat_engine_get_total_sessions(engine) == base_sessions);
  std::printf("[+] TCP SYN/ACK/RST state transitions correct\n");

  // --- Out2in without a session is dropped ---
  const std::uint16_t orphan_id = static_cast<std::uint16_t>(public_id + 1000U);
  assert(orphan_id != public_id);
  IcmpPacket orphan;
  InitIcmp(orphan, kOutsideIp, kPublicIp, orphan_id, ICMP_ECHOREPLY);
  assert(nat_translate_out2in(engine, 0, &orphan.ip, &orphan.icmp, IPPROTO_ICMP) ==
         NAT_TRANSLATE_DROP);
  std::printf("[+] out2in orphan dropped\n");

  // --- Runtime config: a pool change applies to newly created sessions ---
  nat_engine_set_pool(engine, 0xCB00710A, 0xCB00710C);
  TcpPacket configured;
  InitTcp(configured, 0x0A0A0201, kOutsideIp, 41000, 80, true, false, false, false);
  assert(nat_translate_in2out(engine, 0, &configured.ip, &configured.tcp, IPPROTO_TCP) ==
         NAT_TRANSLATE_OK);
  assert(ntohl(configured.ip.saddr) == 0xCB00710A);
  std::printf("[+] runtime pool config applied to new sessions\n");

  nat_engine_set_block_size(engine, 32);
  nat_engine_set_max_blocks_per_user(engine, 1);
  nat_engine_set_vector_config(engine, 128, 8);
  std::printf("[+] runtime block/vector config accepted\n");

  // --- Batched translation: prefetch pipeline must produce the same result ---
  IcmpPacket batch_a;
  IcmpPacket batch_b;
  InitIcmp(batch_a, 0x0A0A0301, kOutsideIp, 3001, ICMP_ECHO);
  InitIcmp(batch_b, 0x0A0A0302, kOutsideIp, 3002, ICMP_ECHO);

  void* batch_ips[2] = {&batch_a.ip, &batch_b.ip};
  void* batch_l4s[2] = {&batch_a.icmp, &batch_b.icmp};
  std::uint8_t batch_protos[2] = {IPPROTO_ICMP, IPPROTO_ICMP};
  nat_action_t batch_actions[2] = {NAT_TRANSLATE_DROP, NAT_TRANSLATE_DROP};
  nat_translate_in2out_batch(engine, 0, batch_ips, batch_l4s, batch_protos, batch_actions, 2);
  assert(batch_actions[0] == NAT_TRANSLATE_OK);
  assert(batch_actions[1] == NAT_TRANSLATE_OK);
  const std::uint16_t public_a = ntohs(batch_a.icmp.un.echo.id);
  const std::uint16_t public_b = ntohs(batch_b.icmp.un.echo.id);
  assert(public_a != 3001);
  assert(public_b != 3002);

  IcmpPacket reply_a;
  IcmpPacket reply_b;
  InitIcmp(reply_a, kOutsideIp, ntohl(batch_a.ip.saddr), public_a, ICMP_ECHOREPLY);
  InitIcmp(reply_b, kOutsideIp, ntohl(batch_b.ip.saddr), public_b, ICMP_ECHOREPLY);
  void* reply_ips[2] = {&reply_a.ip, &reply_b.ip};
  void* reply_l4s[2] = {&reply_a.icmp, &reply_b.icmp};
  nat_action_t reply_actions[2] = {NAT_TRANSLATE_DROP, NAT_TRANSLATE_DROP};
  nat_translate_out2in_batch(engine, 0, reply_ips, reply_l4s, batch_protos, reply_actions, 2);
  assert(reply_actions[0] == NAT_TRANSLATE_OK);
  assert(reply_actions[1] == NAT_TRANSLATE_OK);
  assert(ntohs(reply_a.icmp.un.echo.id) == 3001);
  assert(ntohs(reply_b.icmp.un.echo.id) == 3002);
  std::printf("[+] batched in2out/out2in pipeline correct\n");

  // --- Slow path test when max_sessions is reached ---
  const auto sessions_now = nat_engine_get_total_sessions(engine);
  nat_engine_set_max_sessions_per_worker(engine, static_cast<uint32_t>(sessions_now));
  nat_engine_set_force_eviction(engine, false);

  TcpPacket overflow_pkt;
  InitTcp(overflow_pkt, 0x0A0A0999, kOutsideIp, 49999, 80, true, false, false, false);
  nat_action_t act = nat_translate_in2out(engine, 0, &overflow_pkt.ip, &overflow_pkt.tcp, IPPROTO_TCP);
  assert(act == NAT_TRANSLATE_SLOW_PATH);
  assert(nat_engine_get_telemetry(engine).slow_path_packets >= 1);
  std::printf("[+] table full redirected to NAT_TRANSLATE_SLOW_PATH\n");

  nat_engine_destroy(engine);
  std::printf("All NAT bridge tests passed.\n");
  return 0;
}
