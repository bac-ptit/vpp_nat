#!/usr/bin/env bash
# ==============================================================================
# NAT44 Data Plane Performance Benchmark Suite
# Tests:
#   1. TCP Bandwidth & Throughput (Gbps)
#   2. UDP Packet Rate (PPS) with 64-byte small frames
#   3. Session Creation Rate (CPS - Connections Per Second) with 10k flows
# ==============================================================================

set -euo pipefail

COLOR_GREEN="\033[0;32m"
COLOR_CYAN="\033[0;36m"
COLOR_YELLOW="\033[1;33m"
COLOR_RESET="\033[0m"

echo -e "${COLOR_CYAN}======================================================${COLOR_RESET}"
echo -e "${COLOR_CYAN}   NAT44 DATA PLANE PERFORMANCE BENCHMARK SUITE       ${COLOR_RESET}"
echo -e "${COLOR_CYAN}======================================================${COLOR_RESET}"

# 1. Check prerequisite network namespaces
if ! ip netns list | grep -q "ns-inside" || ! ip netns list | grep -q "ns-outside"; then
    echo "Namespaces not found. Setting up subnets first..."
    sudo bash config/setup_subnets.sh
fi

# Ensure previous iperf3 daemons are stopped
sudo pkill -f "iperf3 -s" 2>/dev/null || true
sleep 0.5

# Start iperf3 server in ns-outside
sudo ip netns exec ns-outside iperf3 -s -D
sleep 0.5

echo -e "\n${COLOR_YELLOW}[1/3] Benchmarking TCP Throughput (Bandwidth)...${COLOR_RESET}"
sudo ip netns exec ns-inside iperf3 -c 198.18.0.2 -t 5 --format g

echo -e "\n${COLOR_YELLOW}[2/3] Benchmarking UDP Packet Rate (64-byte Datagrams)...${COLOR_RESET}"
sudo ip netns exec ns-inside iperf3 -c 198.18.0.2 -u -b 0 -l 64 -t 4

# Stop iperf3 server
sudo pkill -f "iperf3 -s" 2>/dev/null || true

echo -e "\n${COLOR_YELLOW}[3/3] Benchmarking Connections Per Second (CPS) & Session Scale...${COLOR_RESET}"
sudo ip netns exec ns-inside python3 -c '
import socket
import time

TARGET_IP = "198.18.0.2"
FLOW_COUNT = 20000

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
print(f"Generating {FLOW_COUNT} distinct 5-tuple flows through NAT...")

t0 = time.perf_counter()
for i in range(FLOW_COUNT):
    port = 10000 + (i % 55000)
    s.sendto(b"NAT-BENCHMARK-PAYLOAD", (TARGET_IP, port))
t1 = time.perf_counter()

elapsed = t1 - t0
cps = FLOW_COUNT / elapsed
print(f"-> Created {FLOW_COUNT} sessions in {elapsed:.3f}s: {cps:,.1f} CPS")
'

echo -e "\n${COLOR_GREEN}Current Active Sessions in VPP NAT Table:${COLOR_RESET}"
sudo vppctl -s /run/vpp/cli.sock show nat44 sessions | grep -E "(thread|sessions)" || true

echo -e "\n${COLOR_CYAN}======================================================${COLOR_RESET}"
echo -e "${COLOR_GREEN}Benchmark complete!${COLOR_RESET}"
