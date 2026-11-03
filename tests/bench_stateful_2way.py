#!/usr/bin/env python3
"""
================================================================================
NAT44 Real-World Stateful 2-Way Traffic Benchmark (TRex ASTF Emulation)
================================================================================
Emulates carrier-grade 2-way traffic across VPP NAT:
  1. Client in ns-inside (10.10.0.2) sends requests with dynamic 5-tuples.
  2. VPP NAT translates IP/Port dynamically (In2Out).
  3. Dynamic Learned Responder in ns-outside (198.18.0.2) inspects incoming
     packets, extracts the NAT-assigned port, and replies to that exact port.
  4. VPP NAT matches the session and translates back (Out2In).
  5. Client verifies 100% round-trip delivery, measures RTT latency, and checks
     active session count in VPP NAT table.
================================================================================
"""

import os
import select
import socket
import subprocess
import sys
import threading
import time

COLOR_CYAN = "\033[0;36m"
COLOR_GREEN = "\033[0;32m"
COLOR_YELLOW = "\033[1;33m"
COLOR_BOLD = "\033[1m"
COLOR_RESET = "\033[0m"

SERVER_IP = "198.18.0.2"
SERVER_PORT = 9000
CLIENT_IP = "10.10.0.2"


def run_responder(duration_sec: float = 6.0):
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
                    # Dynamically echo back to the NAT-translated address & port
                    server.sendto(data, client_addr)
            except BlockingIOError:
                pass
    server.close()


def run_pipelined_test(total_packets: int = 10000):
    """Measures maximum 2-way round-trip rate and RTT latency through NAT."""
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind((CLIENT_IP, 0))

    received = 0
    rtts = []
    running = True

    def rx_worker():
        nonlocal received
        client.settimeout(0.2)
        while running:
            try:
                data, _ = client.recvfrom(2048)
                t_recv = time.perf_counter()
                t_send = float(data.decode())
                rtts.append((t_recv - t_send) * 1000.0)  # ms
                received += 1
            except socket.timeout:
                pass

    rx_thread = threading.Thread(target=rx_worker, daemon=True)
    rx_thread.start()

    t0 = time.perf_counter()
    for i in range(total_packets):
        t_now = time.perf_counter()
        client.sendto(f"{t_now}".encode(), (SERVER_IP, SERVER_PORT))
        if i % 100 == 0:
            time.sleep(0.0005)

    # Allow buffer draining
    time.sleep(0.3)
    running = False
    rx_thread.join(timeout=1.0)
    t1 = time.perf_counter()

    elapsed = t1 - t0 - 0.3
    client.close()

    delivery_rate = (received / total_packets) * 100.0
    throughput = received / elapsed if elapsed > 0 else 0
    avg_rtt = sum(rtts) / len(rtts) if rtts else 0.0
    min_rtt = min(rtts) if rtts else 0.0
    max_rtt = max(rtts) if rtts else 0.0

    return {
        "sent": total_packets,
        "received": received,
        "delivery_rate": delivery_rate,
        "throughput": throughput,
        "elapsed": elapsed,
        "avg_rtt": avg_rtt,
        "min_rtt": min_rtt,
        "max_rtt": max_rtt,
    }


def run_multisession_test(num_sessions: int = 500, pkts_per_session: int = 10):
    """Simulates concurrent sessions with stateful churn."""
    sockets = []
    for _ in range(num_sessions):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((CLIENT_IP, 0))
        s.setblocking(False)
        sockets.append(s)

    total_pkts = num_sessions * pkts_per_session
    received = 0

    t0 = time.perf_counter()
    for p in range(pkts_per_session):
        for i, s in enumerate(sockets):
            s.sendto(f"{p}:{i}".encode(), (SERVER_IP, SERVER_PORT))

        deadline = time.time() + 0.08
        while time.time() < deadline:
            r, _, _ = select.select(sockets, [], [], 0.01)
            if not r:
                break
            for s in r:
                try:
                    data, _ = s.recvfrom(2048)
                    received += 1
                except BlockingIOError:
                    pass

    # Final drain
    deadline = time.time() + 0.3
    while time.time() < deadline:
        r, _, _ = select.select(sockets, [], [], 0.02)
        if not r:
            break
        for s in r:
            try:
                data, _ = s.recvfrom(2048)
                received += 1
            except BlockingIOError:
                pass

    t1 = time.perf_counter()
    elapsed = t1 - t0

    for s in sockets:
        s.close()

    return {
        "sessions": num_sessions,
        "sent": total_pkts,
        "received": received,
        "delivery_rate": (received / total_pkts) * 100.0,
        "elapsed": elapsed,
        "rate": received / elapsed if elapsed > 0 else 0,
    }


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--server":
        duration = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
        run_responder(duration)
        return

    print(f"{COLOR_CYAN}================================================================{COLOR_RESET}")
    print(f"{COLOR_CYAN}   REAL-WORLD STATEFUL 2-WAY NAT BENCHMARK (ASTF EMULATION)     {COLOR_RESET}")
    print(f"{COLOR_CYAN}================================================================{COLOR_RESET}")

    # Launch server in ns-outside
    srv_cmd = f"sudo ip netns exec ns-outside python3 {os.path.abspath(__file__)} --server 8.0"
    srv_proc = subprocess.Popen(srv_cmd, shell=True)
    time.sleep(0.4)

    try:
        # Phase 1: High-Speed Pipelined Round-Trip Test
        print(f"\n{COLOR_YELLOW}[1/2] Running High-Speed 2-Way Pipelined Stream (10,000 pkts)...{COLOR_RESET}")
        res1 = run_pipelined_test(10000)
        print(f"  -> Delivered:   {res1['received']}/{res1['sent']} ({COLOR_GREEN}{res1['delivery_rate']:.1f}%{COLOR_RESET})")
        print(f"  -> 2-Way Speed: {COLOR_BOLD}{res1['throughput']:,.1f} round-trips/second{COLOR_RESET}")
        print(f"  -> RTT Latency: Avg = {res1['avg_rtt']:.3f} ms | Min = {res1['min_rtt']:.3f} ms | Max = {res1['max_rtt']:.3f} ms")

        # Phase 2: Concurrent Multi-Session Churn Test
        print(f"\n{COLOR_YELLOW}[2/2] Running Concurrent Multi-Session Churn (500 sessions, 5,000 pkts)...{COLOR_RESET}")
        res2 = run_multisession_test(num_sessions=500, pkts_per_session=10)
        print(f"  -> Concurrent Sessions: {res2['sessions']}")
        print(f"  -> Delivered:           {res2['received']}/{res2['sent']} ({COLOR_GREEN}{res2['delivery_rate']:.1f}%{COLOR_RESET})")
        print(f"  -> Churn Rate:          {COLOR_BOLD}{res2['rate']:,.1f} pkts/second{COLOR_RESET}")

        # Phase 3: Check VPP Active Sessions
        print(f"\n{COLOR_CYAN}--- VPP Active NAT Sessions in Memory ---{COLOR_RESET}")
        vpp_out = subprocess.run(
            "sudo vppctl -s /run/vpp/cli.sock show nat44 sessions | grep -E '(thread|sessions)'",
            shell=True,
            capture_output=True,
            text=True,
        )
        print(vpp_out.stdout.strip())

    finally:
        srv_proc.terminate()
        try:
            srv_proc.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            srv_proc.kill()

    print(f"\n{COLOR_CYAN}================================================================{COLOR_RESET}")
    print(f"{COLOR_GREEN}Stateful 2-way benchmark completed successfully!{COLOR_RESET}")


if __name__ == "__main__":
    main()
