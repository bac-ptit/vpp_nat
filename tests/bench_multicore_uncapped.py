#!/usr/bin/env python3
"""
================================================================================
NAT44 MULTI-CORE UNCAPPED STRESS BENCHMARK (15.0 SECONDS)
================================================================================
Spawns 8 parallel sender processes across all CPU cores in ns-inside to:
  1. Fully saturate multi-core pipeline so ALL cores are active in btop.
  2. Blast uncapped concurrent sessions (>1,000,000 target).
  3. Verify organic, fluctuating session distribution across all 15 VPP workers.
================================================================================
"""

import socket
import struct
import time
import subprocess
import multiprocessing
import os
import sys

TARGET_IP = "198.18.0.2"
DURATION_SEC = 15.0
NUM_PROCS = 8
BATCH_SIZE = 500

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


def sender_worker(proc_id: int, duration_sec: float, counter_val):
    """Parallel packet injector running on a dedicated CPU core."""
    raw_sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    raw_sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)

    dst_ip_bytes = socket.inet_aton(TARGET_IP)
    udp_payload = b"NAT_UNCAPPED_STRESS"
    udp_len = 8 + len(udp_payload)

    t_start = time.perf_counter()
    t_end = t_start + duration_sec
    local_sent = 0

    base_id = proc_id * 10_000_000
    flow_id = 0

    while time.perf_counter() < t_end:
        for _ in range(BATCH_SIZE):
            flow_id += 1
            curr_id = base_id + flow_id

            # Multiplicative hash for uniform worker handoff across VPP
            mixed = (curr_id * 2654435761) & 0xFFFFFFFF
            ip_b2 = (mixed >> 16) % 250
            ip_b3 = (mixed >> 8) % 250
            src_ip = socket.inet_aton(f"10.10.{ip_b2}.{ip_b3 + 1}")
            sport = 1024 + (mixed % 64000)
            dport = 80 + ((curr_id ^ (curr_id >> 4)) % 2000)

            udp_hdr = struct.pack("!HHHH", sport, dport, udp_len, 0)
            ip_hdr = struct.pack(
                "!BBHHHBBH4s4s",
                0x45, 0, 20 + udp_len, curr_id & 0xFFFF, 0, 64, 17, 0, src_ip, dst_ip_bytes
            )
            ip_chk = checksum(ip_hdr)
            ip_hdr = struct.pack(
                "!BBHHHBBH4s4s",
                0x45, 0, 20 + udp_len, curr_id & 0xFFFF, 0, 64, 17, ip_chk, src_ip, dst_ip_bytes
            )

            raw_sock.sendto(ip_hdr + udp_hdr + udp_payload, (TARGET_IP, 0))
            local_sent += 1

    raw_sock.close()
    with counter_val.get_lock():
        counter_val.value += local_sent


def run_benchmark():
    print(f"{COLOR_BOLD}{COLOR_CYAN}================================================================")
    print(f"   NAT44 MULTI-CORE FULL LOAD & UNCAPPED BENCHMARK (15.0s)      ")
    print(f"================================================================{COLOR_RESET}")
    print(f"[*] Parallel Senders: {NUM_PROCS} worker processes (all CPU cores active)")
    print(f"[*] Target Duration:  {DURATION_SEC} seconds")
    print(f"[*] Inside Subnet:    ns-inside (10.10.x.x) -> {TARGET_IP}")
    print(f"[*] Quota Ceiling:    UNCAPPED (sessions limit set to 300,000/thread)")

    # Initial VPP state
    try:
        vpp_init = subprocess.check_output(
            ["sudo", "vppctl", "-s", "/run/vpp/cli.sock", "show", "nat44", "summary"],
            text=True,
        )
        print(f"\n{COLOR_YELLOW}[*] Initial VPP NAT State:\n{vpp_init.strip()}{COLOR_RESET}\n")
    except Exception as e:
        print(f"[-] Warning: Failed to query initial VPP state: {e}")

    total_counter = multiprocessing.Value('q', 0)
    procs = []

    print(f"{COLOR_CYAN}[*] Launching {NUM_PROCS} parallel sender processes... WATCH BTOP NOW!{COLOR_RESET}")

    t_start = time.perf_counter()
    for i in range(NUM_PROCS):
        p = multiprocessing.Process(
            target=sender_worker,
            args=(i, DURATION_SEC, total_counter),
        )
        p.start()
        procs.append(p)

    for p in procs:
        p.join()

    t_actual = time.perf_counter() - t_start
    total_sent = total_counter.value
    cps = total_sent / t_actual

    print(f"\n{COLOR_BOLD}{COLOR_GREEN}================================================================")
    print(f"              MULTI-CORE STRESS TEST COMPLETED                  ")
    print(f"================================================================{COLOR_RESET}")
    print(f"  • Test Duration:              {t_actual:.2f} seconds")
    print(f"  • Unique 5-Tuple Flows Sent:  {total_sent:,} flows")
    print(f"  • Multi-Core Aggregate Rate:  {cps:,.1f} CPS (Connections/sec)")
    print(f"================================================================\n")

    print(f"{COLOR_YELLOW}[*] Querying VPP NAT44 final state table...{COLOR_RESET}")
    try:
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
    except Exception as e:
        print(f"[-] Error querying VPP sessions: {e}")


if __name__ == "__main__":
    run_benchmark()
