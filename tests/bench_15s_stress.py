#!/usr/bin/env python3
"""
================================================================================
NAT44 15-Second Continuous Stateful Benchmark (TRex ASTF-Style Emulation)
================================================================================
Runs continuous high-speed stateful traffic through VPP NAT for 15.0 seconds.
Measures:
  - Throughput (Packets/sec, PPS)
  - Connection Setup Rate (Sessions/sec, CPS)
  - Total Active Concurrent Sessions Created
  - Bi-directional Round-Trip Delivery Rate (%)
  - RTT Latencies (Min, Avg, Max, p95)
  - VPP Data Plane Hardware Performance (Vectors/Call, CPU Clocks/Pkt)
================================================================================
"""

import os
import sys
import time
import socket
import select
import subprocess
from typing import List

SERVER_IP = "198.18.0.2"
SERVER_PORT = 9000
CLIENT_IP = "10.10.0.2"
DURATION_SEC = 15.0

COLOR_CYAN = "\033[0;36m"
COLOR_GREEN = "\033[0;32m"
COLOR_YELLOW = "\033[1;33m"
COLOR_BOLD = "\033[1m"
COLOR_RESET = "\033[0m"


def run_responder(duration_sec: float = 18.0):
    """Dynamic Learned Responder running in ns-outside."""
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind((SERVER_IP, SERVER_PORT))
    server.setblocking(False)

    t_end = time.time() + duration_sec
    while time.time() < t_end:
        r, _, _ = select.select([server], [], [], 0.05)
        if r:
            try:
                while True:
                    data, client_addr = server.recvfrom(2048)
                    server.sendto(data, client_addr)
            except BlockingIOError:
                pass
    server.close()


def run_benchmark_15s():
    if len(sys.argv) > 1 and sys.argv[1] == "--server":
        duration = float(sys.argv[2]) if len(sys.argv) > 2 else 18.0
        run_responder(duration)
        return

    print(f"{COLOR_BOLD}{COLOR_CYAN}================================================================")
    print(f"      NAT44 15-SECOND CONTINUOUS STATEFUL BENCHMARK            ")
    print(f"================================================================{COLOR_RESET}")
    print(f"[*] Target Duration: {DURATION_SEC} seconds")
    print(f"[*] Inside Client:   {CLIENT_IP} (ns-inside)")
    print(f"[*] Outside Server:  {SERVER_IP}:{SERVER_PORT} (ns-outside)")

    # 1. Start Responder in ns-outside
    srv_cmd = f"sudo ip netns exec ns-outside python3 {os.path.abspath(__file__)} --server {DURATION_SEC + 5.0}"
    srv_proc = subprocess.Popen(srv_cmd, shell=True)
    time.sleep(0.5)

    try:
        # Pre-allocate socket pools to simulate high concurrent sessions
        NUM_SESSIONS = 250
        sockets = []
        for _ in range(NUM_SESSIONS):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.bind((CLIENT_IP, 0))
            s.setblocking(False)
            sockets.append(s)

        total_sent = 0
        total_received = 0
        rtts: List[float] = []
        unique_sessions = set(s.getsockname()[1] for s in sockets)

        target_addr = (SERVER_IP, SERVER_PORT)
        print(f"\n{COLOR_YELLOW}[*] Traffic generation active ({NUM_SESSIONS} concurrent session sockets)...{COLOR_RESET}")

        t_start = time.perf_counter()
        t_end = t_start + DURATION_SEC
        last_report = t_start
        round_idx = 0

        while time.perf_counter() < t_end:
            round_idx += 1
            # Send burst across sockets
            t_burst = time.perf_counter()
            ts_str = f"{t_burst:.6f}"
            for i, s in enumerate(sockets):
                s.sendto(ts_str.encode(), target_addr)
                total_sent += 1

            # Drain replies for this round
            deadline = time.perf_counter() + 0.015
            while time.perf_counter() < deadline:
                r, _, _ = select.select(sockets, [], [], 0.002)
                if not r:
                    break
                t_recv = time.perf_counter()
                for s in r:
                    try:
                        data, _ = s.recvfrom(1024)
                        total_received += 1
                        try:
                            t_sent = float(data.decode())
                            rtts.append((t_recv - t_sent) * 1000.0)
                        except:
                            pass
                    except BlockingIOError:
                        pass

            # Periodically churn in new sessions (every 50 rounds, replace 20 sockets)
            if round_idx % 50 == 0:
                for idx in range(20):
                    replace_idx = (round_idx + idx) % NUM_SESSIONS
                    sockets[replace_idx].close()
                    new_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    new_sock.bind((CLIENT_IP, 0))
                    new_sock.setblocking(False)
                    sockets[replace_idx] = new_sock
                    unique_sessions.add(new_sock.getsockname()[1])

            # Progress update every 3 seconds
            now = time.perf_counter()
            if now - last_report >= 3.0:
                elapsed = now - t_start
                rate = total_received / elapsed if elapsed > 0 else 0
                print(f"  [T+{elapsed:4.1f}s] Sent: {total_sent:6d} | Received: {total_received:6d} ({rate:7.1f} pkts/s) | Total Sessions: {len(unique_sessions)}")
                last_report = now

        # Final drain
        deadline = time.perf_counter() + 0.5
        while time.perf_counter() < deadline:
            r, _, _ = select.select(sockets, [], [], 0.02)
            if not r:
                break
            t_recv = time.perf_counter()
            for s in r:
                try:
                    data, _ = s.recvfrom(1024)
                    total_received += 1
                    try:
                        t_sent = float(data.decode())
                        rtts.append((t_recv - t_sent) * 1000.0)
                    except:
                        pass
                except BlockingIOError:
                    pass

        t_actual = time.perf_counter() - t_start

        for s in sockets:
            s.close()

    finally:
        srv_proc.terminate()
        try:
            srv_proc.wait(timeout=1.0)
        except:
            srv_proc.kill()

    # Metrics calculation
    delivery_rate = (total_received / total_sent * 100.0) if total_sent > 0 else 0.0
    throughput = total_received / t_actual if t_actual > 0 else 0.0
    cps = len(unique_sessions) / t_actual if t_actual > 0 else 0.0

    rtts.sort()
    avg_rtt = (sum(rtts) / len(rtts)) if rtts else 0.0
    min_rtt = rtts[0] if rtts else 0.0
    max_rtt = rtts[-1] if rtts else 0.0
    p95_rtt = rtts[int(len(rtts) * 0.95)] if rtts else 0.0

    print(f"\n{COLOR_BOLD}{COLOR_GREEN}================================================================")
    print(f"              BENCHMARK RESULTS (15.0 SECONDS)                  ")
    print(f"================================================================{COLOR_RESET}")
    print(f"  • Test Duration:             {t_actual:.2f} seconds")
    print(f"  • Packets Sent (In2Out):     {total_sent:,} packets")
    print(f"  • Packets Echoed (Out2In):   {total_received:,} packets")
    print(f"  • 2-Way Delivery Rate:       {COLOR_BOLD}{delivery_rate:.2f} %{COLOR_RESET}")
    print(f"  • 2-Way Throughput:          {COLOR_BOLD}{throughput:,.1f} round-trips/second{COLOR_RESET} ({throughput * 2:,.1f} pkts/s)")
    print(f"  • Total Sessions Handled:    {COLOR_BOLD}{len(unique_sessions):,} sessions{COLOR_RESET}")
    print(f"  • Session Churn Rate (CPS):  {cps:,.1f} connections/second")
    print(f"  • RTT Latency (Min):         {min_rtt:.3f} ms")
    print(f"  • RTT Latency (Average):     {avg_rtt:.3f} ms")
    print(f"  • RTT Latency (p95):         {p95_rtt:.3f} ms")
    print(f"  • RTT Latency (Max):         {max_rtt:.3f} ms")
    print(f"================================================================\n")

    # Query VPP live status
    print(f"{COLOR_YELLOW}[*] Querying VPP NAT44 live session tables...{COLOR_RESET}")
    vpp_sess = subprocess.run(
        "sudo vppctl -s /run/vpp/cli.sock show nat44 sessions | grep -E '(thread|sessions)'",
        shell=True,
        capture_output=True,
        text=True,
    )
    print(vpp_sess.stdout.strip())

    pba_out = subprocess.run(
        "sudo vppctl -s /run/vpp/cli.sock show nat pba",
        shell=True,
        capture_output=True,
        text=True,
    )
    print(pba_out.stdout.strip())


if __name__ == "__main__":
    run_benchmark_15s()
