#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../src/plugin/nat_engine.hpp"

namespace {

constexpr std::uint32_t kOutsideIp = 0xC6120002;  // 198.18.0.2

struct ExportContext {
  std::vector<nat_session_record_t> records;
};

void Collect(void* ctx, const nat_session_record_t* record) {
  static_cast<ExportContext*>(ctx)->records.push_back(*record);
}

nat::engine::FiveTuple MakeInside(std::uint32_t index) {
  return nat::engine::FiveTuple{
      .src_ip{0x0A0A0000U + index},
      .dst_ip{kOutsideIp},
      .src_port{static_cast<std::uint16_t>(20000 + index)},
      .dst_port{80},
      .protocol{6},
  };
}

}  // namespace

int main() {
  std::printf("========================================\n");
  std::printf("  Running Unit Test: HA session sync    \n");
  std::printf("========================================\n");

  nat::engine::NatEngine active(1, 4096);
  auto& active_table = active.GetThreadTable(0);

  constexpr std::size_t kSessions = 1000;
  std::vector<nat::engine::FiveTuple> inside_keys;
  inside_keys.reserve(kSessions);

  for (std::size_t i = 0; i < kSessions; ++i) {
    const auto inside = MakeInside(static_cast<std::uint32_t>(i));
    const auto public_port = active_table.AllocatePortForUser(inside.src_ip, 0xCB007101);
    assert(public_port.has_value());
    const nat::engine::FiveTuple outside{
        .src_ip{0xCB007101},
        .dst_ip{kOutsideIp},
        .src_port{public_port->port},
        .dst_port{80},
        .protocol{6},
    };
    auto* session = active_table.CreateSession(inside, outside);
    assert(session != nullptr);
    inside_keys.push_back(inside);
  }
  assert(active_table.SessionCount() == kSessions);

  ExportContext exported;
  exported.records.reserve(kSessions);
  active.ExportSessions(Collect, &exported);
  assert(exported.records.size() == kSessions);
  for (const auto& record : exported.records) {
    assert(record.thread_index == 0);
    assert(record.protocol == 6);
    assert(record.outside_src_port >= 1024);
    assert(record.block_base_port <= record.outside_src_port);
  }
  std::printf("[+] exported %zu sessions\n", exported.records.size());

  nat::engine::NatEngine standby(1, 4096);
  std::size_t imported = 0;
  for (const auto& record : exported.records) {
    if (standby.ImportSession(record)) ++imported;
  }
  assert(imported == kSessions);
  assert(standby.GetThreadTable(0).SessionCount() == kSessions);
  std::printf("[+] imported %zu sessions\n", imported);

  for (const auto& key : inside_keys) {
    assert(standby.GetThreadTable(0).LookupIn2Out(key) != nullptr);
  }
  std::printf("[+] all in2out lookups hit after import\n");

  for (const auto& record : exported.records) {
    const nat::engine::FiveTuple outside_key{
        .src_ip{record.outside_src_ip},
        .dst_ip{record.outside_dst_ip},
        .src_port{record.outside_src_port},
        .dst_port{record.outside_dst_port},
        .protocol{record.protocol},
    };
    assert(standby.GetThreadTable(0).LookupOut2In(outside_key) != nullptr);
  }
  std::printf("[+] all out2in lookups hit after import\n");

  // Importing the same record twice must update, not duplicate.
  assert(standby.ImportSession(exported.records.front()));
  assert(standby.GetThreadTable(0).SessionCount() == kSessions);
  std::printf("[+] duplicate import is idempotent\n");

  // Invalid records are rejected.
  auto invalid = exported.records.front();
  invalid.protocol = 99;
  assert(!standby.ImportSession(invalid));
  invalid = exported.records.front();
  invalid.thread_index = 7;
  assert(!standby.ImportSession(invalid));
  std::printf("[+] invalid records rejected\n");

  standby.FlushSessions();
  assert(standby.GetThreadTable(0).SessionCount() == 0);
  std::printf("[+] flush cleared all sessions\n");

  std::printf("All HA sync tests passed.\n");
  return 0;
}
