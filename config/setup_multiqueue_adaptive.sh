#!/usr/bin/env bash
# config/setup_multiqueue_adaptive.sh
# Sets up Linux network namespaces (ns-inside and ns-outside) and VPP Tap v2 interfaces
# with Multi-Queue (4 queues per interface) and Adaptive RX-Mode for zero idle CPU overhead.

set -euo pipefail

VPP_SOCK="${1:-/run/vpp/cli.sock}"

if [ "$EUID" -ne 0 ]; then
  echo "[-] Please run as root: sudo bash config/setup_multiqueue_adaptive.sh"
  exit 1
fi

echo "============================================================"
echo "  Setting up Multi-Queue Tap v2 with Adaptive RX-Mode"
echo "============================================================"

# 1. Clean up existing namespaces and interfaces
echo "[+] Cleaning up previous namespaces and interfaces..."
if [ -S "$VPP_SOCK" ]; then
  vppctl -s "$VPP_SOCK" delete tap tap1 2>/dev/null || true
  vppctl -s "$VPP_SOCK" delete tap tap2 2>/dev/null || true
  vppctl -s "$VPP_SOCK" delete host-interface name veth-in-host 2>/dev/null || true
  vppctl -s "$VPP_SOCK" delete host-interface name veth-out-host 2>/dev/null || true
fi
ip netns del ns-inside 2>/dev/null || true
ip netns del ns-outside 2>/dev/null || true

# 2. Re-create namespaces
ip netns add ns-inside
ip netns add ns-outside

# 3. Create Tap v2 inside interface (4 RX queues, adaptive mode)
echo "[+] Creating Tap v2 Inside interface (tap1, 4 RX queues)..."
vppctl -s "$VPP_SOCK" create tap id 1 host-if-name veth-in host-ns ns-inside host-ip4-addr 10.10.0.2/24 host-ip4-gw 10.10.0.1 num-rx-queues 4
vppctl -s "$VPP_SOCK" set interface state tap1 up
vppctl -s "$VPP_SOCK" set interface ip address tap1 10.10.0.1/24
vppctl -s "$VPP_SOCK" set interface rx-mode tap1 adaptive

# 4. Create Tap v2 outside interface (4 RX queues, adaptive mode)
echo "[+] Creating Tap v2 Outside interface (tap2, 4 RX queues)..."
vppctl -s "$VPP_SOCK" create tap id 2 host-if-name veth-out host-ns ns-outside host-ip4-addr 198.18.0.2/24 host-ip4-gw 198.18.0.1 num-rx-queues 4
vppctl -s "$VPP_SOCK" set interface state tap2 up
vppctl -s "$VPP_SOCK" set interface ip address tap2 198.18.0.1/24
vppctl -s "$VPP_SOCK" set interface rx-mode tap2 adaptive

# 5. Configure the NAT PBA plugin and IP pool
echo "[+] Enabling NAT PBA with Multi-Worker pool..."
vppctl -s "$VPP_SOCK" nat pba set sessions 64000
vppctl -s "$VPP_SOCK" nat pba set timeouts udp 30 tcp-established 300 tcp-transitory 10 icmp 5
vppctl -s "$VPP_SOCK" nat pba set pool 203.0.113.1 203.0.113.32
vppctl -s "$VPP_SOCK" set interface nat pba in tap1
vppctl -s "$VPP_SOCK" set interface nat pba out tap2

echo "============================================================"
echo "  Multi-Queue (4 queues/interface) in Adaptive Mode is UP!"
echo "  Check queue placement: vppctl show interface rx-placement"
echo "  Test ping: sudo ip netns exec ns-inside ping -c 3 198.18.0.2"
echo "============================================================"
