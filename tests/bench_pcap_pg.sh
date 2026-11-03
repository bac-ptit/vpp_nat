#!/usr/bin/env bash
# ==============================================================================
# NAT44 PBA Micro-Benchmark: VPP Packet Generator (PG) with Raw-IP PCAP
# Replays synthetic PCAP files directly in VPP Hugepages memory to measure:
#   1. Session Creation Rate (CPS - Connections Per Second)
#   2. PBA 64-bit Port Block Allocation throughput
#   3. CPU Clocks per Packet and Vector Processing Batching (vectors/call)
# Zero-Kernel Overhead: Bypasses Linux kernel socket stack, veth, and softirq.
# ==============================================================================

set -euo pipefail

COLOR_GREEN="\033[0;32m"
COLOR_CYAN="\033[0;36m"
COLOR_YELLOW="\033[1;33m"
COLOR_BOLD="\033[1m"
COLOR_RESET="\033[0m"

FLOW_COUNT="${1:-50000}"
PCAP_FILE="/tmp/nat_pba_bench_${FLOW_COUNT}.pcap"
STREAM_NAME="pba_pg_stream"

echo -e "${COLOR_CYAN}================================================================${COLOR_RESET}"
echo -e "${COLOR_CYAN}   VPP NATIVE PACKET GENERATOR (.PCAP) NAT BENCHMARK            ${COLOR_RESET}"
echo -e "${COLOR_CYAN}================================================================${COLOR_RESET}"

# 1. Generate Synthetic Raw-IP PCAP File
echo -e "\n${COLOR_YELLOW}[1/4] Generating synthetic Raw-IP PCAP (${FLOW_COUNT} unique flows)...${COLOR_RESET}"
python3 -c "
import struct, socket, time

t0 = time.perf_counter()
pcap_hdr = struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 12)  # LinkType = 12 (Raw IP)

with open('${PCAP_FILE}', 'wb') as f:
    f.write(pcap_hdr)
    for i in range(${FLOW_COUNT}):
        src_ip = socket.inet_aton(f'10.10.{(i // 250) % 250}.{(i % 250) + 1}')
        dst_ip = socket.inet_aton('198.18.0.2')
        # IPv4 Header: 20 bytes
        ip_hdr = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 44, i & 0xFFFF, 0, 64, 17, 0, src_ip, dst_ip)
        # UDP Header: 8 bytes
        udp_hdr = struct.pack('!HHHH', 1024 + (i % 60000), 80, 24, 0)
        payload = b'PBA-BENCH-STREAM'
        pkt = ip_hdr + udp_hdr + payload
        pkt_hdr = struct.pack('<IIII', 0, 0, len(pkt), len(pkt))
        f.write(pkt_hdr + pkt)

t1 = time.perf_counter()
print(f'-> PCAP generated: {${FLOW_COUNT}:,} packets written to ${PCAP_FILE} in {t1 - t0:.3f}s')
"

# 2. Configure VPP Packet Generator
echo -e "\n${COLOR_YELLOW}[2/4] Registering stream with VPP Packet Generator in Hugepages memory...${COLOR_RESET}"
# Cleanup existing stream if present
sudo vppctl "packet-generator delete ${STREAM_NAME}" 2>/dev/null || true

# Register new stream directly to nat-pba-in2out node
sudo vppctl "packet-generator new name ${STREAM_NAME} node nat-pba-in2out pcap ${PCAP_FILE} limit ${FLOW_COUNT} rate 1e7"

# 3. Execute Benchmark Run
echo -e "\n${COLOR_YELLOW}[3/4] Injecting packets directly into VPP Graph Nodes (Zero-Copy)...${COLOR_RESET}"
sudo vppctl clear runtime

T_START=$(python3 -c "import time; print(time.perf_counter())")
sudo vppctl "packet-generator enable-stream ${STREAM_NAME}"

# Wait until stream completes
while true; do
    COUNT=$(sudo vppctl show packet-generator | grep "${STREAM_NAME}" | awk '{print $3}' || echo "0")
    if [[ "${COUNT}" -ge "${FLOW_COUNT}" ]]; then
        break
    fi
    sleep 0.05
done
T_END=$(python3 -c "import time; print(time.perf_counter())")

ELAPSED=$(python3 -c "print(${T_END} - ${T_START})")
CPS=$(python3 -c "print(${FLOW_COUNT} / ${ELAPSED})")

# 4. Display Results
echo -e "\n${COLOR_YELLOW}[4/4] Benchmark Results & Metrics:${COLOR_RESET}"
echo -e "${COLOR_GREEN}Execution Time:   ${ELAPSED} seconds${COLOR_RESET}"
echo -e "${COLOR_GREEN}Packets Injected: ${FLOW_COUNT}${COLOR_RESET}"
echo -e "${COLOR_BOLD}Effective CPS:    ${CPS:0:10} Sessions/second${COLOR_RESET}"

echo -e "\n${COLOR_CYAN}--- VPP Runtime Graph Node Statistics ---${COLOR_RESET}"
sudo vppctl show runtime | grep -E "(Name|nat-pba|pg-input)" || true

echo -e "\n${COLOR_CYAN}--- NAT PBA Engine Status ---${COLOR_RESET}"
sudo vppctl show nat pba

# Cleanup
sudo vppctl "packet-generator delete ${STREAM_NAME}" 2>/dev/null || true
rm -f "${PCAP_FILE}"

echo -e "\n${COLOR_CYAN}================================================================${COLOR_RESET}"
echo -e "${COLOR_GREEN}VPP PG PCAP Benchmark completed successfully!${COLOR_RESET}"
