#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "nat/nat_plugin.h"
#include "nat_ha_sync.h"

namespace {

struct Packet {
  struct iphdr ip;
  struct tcphdr tcp;
};

void InitPacket(Packet& pkt, std::uint32_t src_ip, std::uint16_t src_port) {
  std::memset(&pkt, 0, sizeof(pkt));
  pkt.ip.version = 4;
  pkt.ip.ihl = 5;
  pkt.ip.tot_len = htons(sizeof(pkt));
  pkt.ip.ttl = 64;
  pkt.ip.protocol = IPPROTO_TCP;
  pkt.ip.saddr = htonl(src_ip);
  pkt.ip.daddr = htonl(0xC6120002);
  pkt.tcp.source = htons(src_port);
  pkt.tcp.dest = htons(80);
  pkt.tcp.doff = 5;
  pkt.tcp.syn = 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s active|standby <local-port> <peer-port>\n", argv[0]);
    return 2;
  }
  const std::string mode{argv[1]};
  const auto local_port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
  const auto peer_port = static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10));

  nat_engine_t* engine = nat_engine_create_with_capacity(1, 4096);
  if (engine == nullptr) {
    std::fprintf(stderr, "engine create failed\n");
    return 1;
  }

  if (mode == "active") {
    for (int i = 0; i < 100; ++i) {
      Packet pkt;
      InitPacket(pkt, 0x0A0A0000U + static_cast<std::uint32_t>(i),
                 static_cast<std::uint16_t>(20000 + i));
      const auto action =
          nat_translate_in2out(engine, 0, &pkt.ip, &pkt.tcp, IPPROTO_TCP);
      if (action != NAT_TRANSLATE_OK) {
        std::fprintf(stderr, "create failed at %d\n", i);
        return 1;
      }
    }
    nat_ha_sync_start(engine, 1, "127.0.0.1", local_port, "127.0.0.1", peer_port, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    std::printf("active_sessions=%llu\n",
                static_cast<unsigned long long>(nat_engine_get_total_sessions(engine)));
    nat_ha_sync_stop();
    nat_engine_destroy(engine);
    return 0;
  }

  if (mode == "standby") {
    nat_ha_sync_start(engine, 2, "127.0.0.1", local_port, "127.0.0.1", peer_port, 500);
    nat_ha_status_t status{};
    for (int i = 0; i < 60; ++i) {
      nat_ha_sync_get_status(&status);
      if (status.shadow_sessions >= 100) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("shadow_sessions=%llu\n",
                static_cast<unsigned long long>(status.shadow_sessions));
    const bool promoted = nat_ha_sync_promote(engine);
    nat_ha_sync_stop();
    std::printf("promoted=%d promoted_sessions=%llu\n", promoted ? 1 : 0,
                static_cast<unsigned long long>(nat_engine_get_total_sessions(engine)));
    nat_engine_destroy(engine);
    return 0;
  }

  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  nat_engine_destroy(engine);
  return 2;
}
