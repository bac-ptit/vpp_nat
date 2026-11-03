#!/usr/bin/env python3
"""
================================================================================
NAT44 100,000+ SESSIONS SCALE BENCHMARK (15.0 SECONDS)
================================================================================
Generates 100,000+ distinct stateful 5-tuple flows into VPP NAT data-plane
without Linux OS socket descriptor limits by using kernel-bypass raw frame injection.
Measures:
  - Connections Per Second (CPS)
  - Total Active Concurrent Sessions Created in VPP Table
  - Memory & Translation Table Health
================================================================================
"""

import socket
import struct
import time
import subprocess
import sys

TARGET_IP = "198.18.0.2"
DURATION_SEC = 15.0

COLOR_CYAN = "\033[0;36m"
COLOR_GREEN = "\033[0;32m"
COLOR_YELLOW = "\033[1;33m"
COLOR_BOLD = "\033[1m"
COLOR_RESET = "\033[0m"


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(f"!{len(data)//2}H", data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF


def run_100k_benchmark():
    print(f"{COLOR_BOLD}{COLOR_CYAN}================================================================")
    print(f"      NAT44 100,000+ SESSIONS HIGH-SCALE BENCHMARK (15s)        ")
    print(f"================================================================{COLOR_RESET}")
    print(f"[*] Target Scale:    100,000+ Concurrent Stateful Sessions")
    print(f"[*] Target Duration: {DURATION_SEC} seconds")
    print(f"[*] Inside Traffic:  ns-inside (10.10.x.x) -> {TARGET_IP}")

    # Check initial VPP sessions
    vpp_init = subprocess.check_output(
        ["sudo", "vppctl", "-s", "/run/vpp/cli.sock", "show", "nat44", "summary"],
        text=True,
    )
    print(f"\n{COLOR_YELLOW}[*] Initial VPP NAT State:\n{vpp_init.strip()}{COLOR_RESET}\n")

    raw_sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    raw_sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)

    dst_ip_bytes = socket.inet_aton(TARGET_IP)
    udp_payload = b"NAT_SCALE_TEST"
    udp_len = 8 + len(udp_payload)

    total_sessions_target = 150000
    batch_size = 1000

    t_start = time.perf_counter()
    t_end = t_start + DURATION_SEC
    last_report = t_start
    total_sent = 0

    print(f"{COLOR_CYAN}[*] Starting 15s session burst generator...{COLOR_RESET}")

    flow_id = 0
    while time.perf_counter() < t_end:
        # Send a batch of distinct 5-tuples
        for _ in range(batch_size):
            flow_id += 1
            # Scramble 5-tuple bits to uniformly distribute across all 15 VPP workers
            mixed = (flow_id * 2654435761) & 0xFFFFFFFF
            ip_b2 = (mixed >> 16) % 250
            ip_b3 = (mixed >> 8) % 250
            src_ip = socket.inet_aton(f"10.10.{ip_b2}.{ip_b3 + 1}")
            sport = 1024 + (mixed % 64000)
            dport = 80 + ((flow_id ^ (flow_id >> 4)) % 1000)

            # UDP Header
            udp_hdr = struct.pack("!HHHH", sport, dport, udp_len, 0)

            # IP Header (20 bytes)
            ip_hdr = struct.pack(
                "!BBHHHBBH4s4s",
                0x45, 0, 20 + udp_len, flow_id & 0xFFFF, 0, 64, 17, 0, src_ip, dst_ip_bytes
            )
            ip_chk = checksum(ip_hdr)
            ip_hdr = struct.pack(
                "!BBHHHBBH4s4s",
                0x45, 0, 20 + udp_len, flow_id & 0xFFFF, 0, 64, 17, ip_chk, src_ip, dst_ip_bytes
            )

            raw_sock.sendto(ip_hdr + udp_hdr + udp_payload, (TARGET_IP, 0))
            total_sent += 1

        now = time.perf_counter()
        if now - last_report >= 3.0:
            elapsed = now - t_start
            cps = total_sent / elapsed
            print(f"  [T+{elapsed:4.1f}s] Injected: {total_sent:7,d} unique flows | Rate: {cps:8,.1f} CPS")
            last_report = now

        # Slight yield to space out across 15 seconds
        time.sleep(0.005)

    t_actual = time.perf_counter() - t_start
    raw_sock.close()

    cps = total_sent / t_actual

    print(f"\n{COLOR_BOLD}{COLOR_GREEN}================================================================")
    print(f"              SCALE BENCHMARK RESULTS (15.0 SECONDS)            ")
    print(f"================================================================{COLOR_RESET}")
    print(f"  • Test Duration:              {t_actual:.2f} seconds")
    print(f"  • Unique 5-Tuple Flows Sent:  {total_sent:,} flows")
    print(f"  • Flow Generation Rate:       {cps:,.1f} CPS (Connections/sec)")
    print(f"================================================================\n")

    print(f"{COLOR_YELLOW}[*] Querying VPP NAT44 final state table...{COLOR_RESET}")
    vpp_final = subprocess.check_output(
        ["sudo", "vppctl", "-s", "/run/vpp/cli.sock", "show", "nat44", "summary"],
        text=True,
    )
    print(vpp_final.strip())

    vpp_sess = subprocess.check_output(
        "sudo vppctl -s /run/vpp/cli.sock show nat44 sessions | grep -E '(thread|sessions)'",
        shell=True,
        text=True,
    )
    print(f"\n{COLOR_GREEN}{vpp_sess.strip()}{COLOR_RESET}")


if __name__ == "__main__":
    run_100k_benchmark()
