#include <cassert>

#include "../src/plugin/nat_engine.hpp"
#include "helpers/format_helpers.hpp"
#include "helpers/print.hpp"

int main() {
  nat::io::println("========================================");
  nat::io::println("  Running Unit Test: C++ NAT Engine     ");
  nat::io::println("========================================");

  nat::engine::NatEngine engine(4);
  auto& table{engine.GetThreadTable(0)};

  const nat::engine::FiveTuple in_flow{
      .src_ip{0x0A0A0105},  // 10.10.1.5
      .dst_ip{0xC6120101},  // 198.18.1.1
      .src_port{54321},
      .dst_port{80},
      .protocol{6},  // TCP
  };

  const nat::engine::FiveTuple out_flow{
      .src_ip{0xCB007101},  // 203.0.113.1
      .dst_ip{0xC6120101},  // 198.18.1.1
      .src_port{20000},
      .dst_port{80},
      .protocol{6},  // TCP
  };

  // Initially lookup should fail
  assert(table.LookupIn2Out(in_flow) == nullptr);
  assert(table.LookupOut2In(out_flow) == nullptr);

  // Create session
  auto* session{table.CreateSession(in_flow, out_flow)};
  assert(session != nullptr);
  assert(session->inside == in_flow);
  assert(session->outside == out_flow);
  assert(session->total_packets == 1);

  // Test custom std::formatter
  const std::string formatted_tuple{std::format("{}", in_flow)};
  nat::io::println("[+] Formatted FiveTuple: {}", formatted_tuple);
  assert(!formatted_tuple.empty());

  const std::string formatted_session{std::format("{}", *session)};
  nat::io::println("[+] Formatted NatSession: {}", formatted_session);
  assert(!formatted_session.empty());

  // Bi-directional lookup must succeed
  auto* found_in{table.LookupIn2Out(in_flow)};
  assert(found_in != nullptr);
  assert(found_in->outside.src_port == 20000);

  auto* found_out{table.LookupOut2In(out_flow)};
  assert(found_out != nullptr);
  assert(found_out->inside.src_ip == 0x0A0A0105);

  // Test PBA allocation for user
  auto port1{table.AllocatePortForUser(0x0A0A0105, 0xCB007101)};
  assert(port1.has_value());
  auto port2{table.AllocatePortForUser(0x0A0A0105, 0xCB007101)};
  assert(port2.has_value());
  assert(*port1 != *port2);

  table.ReleasePortForUser(0x0A0A0105, *port1);

  // Delete session
  assert(table.DeleteSession(in_flow));
  assert(table.LookupIn2Out(in_flow) == nullptr);
  assert(table.LookupOut2In(out_flow) == nullptr);

  // -------------------------------------------------------------
  // Test P0: Multi-block PBA allocation without port leak (>64 ports)
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Multi-block PBA allocation (80 ports)...");
  std::vector<std::uint16_t> user_ports;
  user_ports.reserve(80);
  for (std::size_t i = 0; i < 80; ++i) {
    auto p = table.AllocatePortForUser(0x0A0A0202, 0xCB007101);
    assert(p.has_value());
    user_ports.push_back(*p);
  }
  assert(user_ports.size() == 80);
  // Verify all 80 ports are unique
  std::sort(user_ports.begin(), user_ports.end());
  assert(std::unique(user_ports.begin(), user_ports.end()) == user_ports.end());
  // Release all 80 ports and verify no crash or leak
  for (auto p : user_ports) {
    table.ReleasePortForUser(0x0A0A0202, p);
  }

  // -------------------------------------------------------------
  // Test P0: Subscriber Ownership & Collision Check
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Subscriber Collision Verification...");
  const std::uint32_t user1 = 0x0A010005;
  const std::uint32_t user2 = 0x0A020005; // Collides on same lower 14 bits (5)
  auto p_user1 = table.AllocatePortForUser(user1, 0xCB007101);
  auto p_user2 = table.AllocatePortForUser(user2, 0xCB007101);
  assert(p_user1.has_value());
  assert(p_user2.has_value());
  table.ReleasePortForUser(user1, *p_user1);
  table.ReleasePortForUser(user2, *p_user2);

  // -------------------------------------------------------------
  // Test P0: Deletion must not break probe chains (tombstones)
  // C and B collide; A lies between them in the probe sequence.
  // -------------------------------------------------------------
  nat::io::println("[*] Testing PBA tombstone deletion...");
  const auto tel_before = table.GetTelemetry();
  const std::uint32_t user_c = 0x0A000009;
  const std::uint32_t user_a = 0x0A00000A;
  const std::uint32_t user_b = 0x0A010009;
  auto p_c = table.AllocatePortForUser(user_c, 0xCB007101);
  auto p_a = table.AllocatePortForUser(user_a, 0xCB007101);
  auto p_b = table.AllocatePortForUser(user_b, 0xCB007101);
  assert(p_c.has_value());
  assert(p_a.has_value());
  assert(p_b.has_value());
  table.ReleasePortForUser(user_a, *p_a);
  table.ReleasePortForUser(user_b, *p_b);
  table.ReleasePortForUser(user_c, *p_c);
  const auto tel_after = table.GetTelemetry();
  assert(tel_after.pba_blocks_allocated - tel_before.pba_blocks_allocated == 3);
  assert(tel_after.pba_blocks_freed - tel_before.pba_blocks_freed == 3);

  // -------------------------------------------------------------
  // Test P0: Protocol-Specific Timeouts & Lazy Expiry
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Protocol-Specific Timeouts & Lazy Expiry...");
  table.SetTimeouts(nat::engine::SessionTimeouts{
      .udp_sec = 120,
      .tcp_established_sec = 3600,
      .tcp_transitory_sec = 60,
      .icmp_sec = 30,
  });

  auto* exp_session = table.CreateSession(in_flow, out_flow);
  assert(exp_session != nullptr);
  const std::uint64_t now_sim = nat::engine::current_timestamp_ns();

  // TCP transitory: age by 40s (timeout is 60s) -> NOT expired yet
  exp_session->last_active_ts = now_sim - 40'000'000'000ULL;
  assert(table.LookupIn2Out(in_flow, now_sim) != nullptr);

  // Age by 70s (>60s) -> Expired!
  exp_session->last_active_ts = now_sim - 70'000'000'000ULL;
  auto* lookup_res = table.LookupIn2Out(in_flow, now_sim);
  assert(lookup_res == nullptr); // Expired and retired!

  // -------------------------------------------------------------
  // Test P0: Expired Session Reclamation Does NOT Delete Replacement Mapping
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Replacement Mapping Protection during Reclaim...");
  // in_flow was retired above and queued in retire_queue_.
  // Now create a replacement session with the same in_flow key:
  const nat::engine::FiveTuple out_flow_new{
      .src_ip{0xCB007101},
      .dst_ip{0xC6120101},
      .src_port{20001},
      .dst_port{80},
      .protocol{6},
  };
  auto* replacement = table.CreateSession(in_flow, out_flow_new);
  assert(replacement != nullptr);
  assert(replacement->outside.src_port == 20001);

  // Now trigger reclaim batch, which processes the retired old session
  table.ReclaimBatch(64);

  // The replacement session MUST still be alive in bihash!
  auto* after_reclaim = table.LookupIn2Out(in_flow, now_sim);
  assert(after_reclaim != nullptr);
  assert(after_reclaim->outside.src_port == 20001);
  assert(table.DeleteSession(in_flow));

  // -------------------------------------------------------------
  // Test P0: PBA Block Tracking & Reuse without Collisions
  // -------------------------------------------------------------
  nat::io::println("[*] Testing PBA Block Tracking & Reclaim...");
  const std::uint32_t test_user = 0x0A0A0999;
  auto port_a = table.AllocatePortForUser(test_user, 0xCB007101);
  assert(port_a.has_value());
  // Free the port, which should free the block back to the block bitmap
  table.ReleasePortForUser(test_user, *port_a);

  // Allocate again - should succeed cleanly
  auto port_b = table.AllocatePortForUser(test_user, 0xCB007101);
  assert(port_b.has_value());
  table.ReleasePortForUser(test_user, *port_b);

  // -------------------------------------------------------------
  // Test P0: Bihash Prefetch Functions
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Bihash Prefetch Calls...");
  table.PrefetchIn2Out(in_flow);
  table.PrefetchOut2In(out_flow);

  // -------------------------------------------------------------
  // Test P0: Strict Timeout Reclaim (No Preemptive Eviction of Active Sessions)
  // -------------------------------------------------------------
  nat::io::println("[*] Testing Strict Timeout Reclaim when Table is Full...");
  auto& full_table = engine.GetThreadTable(1);
  full_table.SetMaxSessions(2);
  full_table.SetForceEvictOnOverflow(false); // Strict: only evict expired sessions!
  full_table.SetTimeouts(nat::engine::SessionTimeouts{
      .udp_sec = 60,
      .tcp_established_sec = 60,
      .tcp_transitory_sec = 60,
      .icmp_sec = 60,
  });

  const nat::engine::FiveTuple s1_in{0x0A000001, 0x08080808, 1001, 80, 6};
  const nat::engine::FiveTuple s1_out{0xCB007101, 0x08080808, 20001, 80, 6};
  const nat::engine::FiveTuple s2_in{0x0A000002, 0x08080808, 1002, 80, 6};
  const nat::engine::FiveTuple s2_out{0xCB007101, 0x08080808, 20002, 80, 6};
  const nat::engine::FiveTuple s3_in{0x0A000003, 0x08080808, 1003, 80, 6};
  const nat::engine::FiveTuple s3_out{0xCB007101, 0x08080808, 20003, 80, 6};

  auto* s1 = full_table.CreateSession(s1_in, s1_out);
  auto* s2 = full_table.CreateSession(s2_in, s2_out);
  assert(s1 != nullptr && s2 != nullptr);
  assert(full_table.SessionCount() == 2);

  // Table is full (2/2). Both s1 and s2 are fresh (active, not expired).
  // Creating s3 MUST FAIL and return nullptr!
  auto* s3_fail = full_table.CreateSession(s3_in, s3_out);
  assert(s3_fail == nullptr);
  // Both s1 and s2 MUST still be alive!
  assert(full_table.LookupIn2Out(s1_in) != nullptr);
  assert(full_table.LookupIn2Out(s2_in) != nullptr);
  assert(full_table.SessionCount() == 2);
  nat::io::println("[+] Active sessions protected from eviction when table is full!");

  // Now age s1 past timeout (>60s)
  const std::uint64_t now_ts = nat::engine::current_timestamp_ns();
  s1->last_active_ts = now_ts - 100'000'000'000ULL; // 100s ago -> Expired!
  s2->last_active_ts = now_ts; // Still fresh!

  // Now try creating s3 again: s1 expired, so it is reclaimed, and s3 succeeds!
  auto* s3_success = full_table.CreateSession(s3_in, s3_out);
  assert(s3_success != nullptr);
  assert(full_table.LookupIn2Out(s3_in) != nullptr);
  assert(full_table.LookupIn2Out(s2_in) != nullptr); // s2 is still alive!
  assert(full_table.LookupIn2Out(s1_in) == nullptr); // s1 was reclaimed!
  assert(full_table.SessionCount() == 2);
  nat::io::println("[+] Expired session reclaimed to admit new session!");

  nat::io::println("[+] All C++ NAT Engine and Formatter tests passed!");
  return 0;
}
