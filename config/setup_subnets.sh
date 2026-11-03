#!/usr/bin/env bash
# config/setup_subnets.sh
# Sets up Linux network namespaces (ns-inside and ns-outside) and veth pairs
# connected to VPP host interfaces for functional manual testing of the 2 subnets.

set -euo pipefail

VPP_SOCK="${1:-/run/vpp/cli-node1.sock}"

if [ "$EUID" -ne 0 ]; then
  echo "[-] Please run as root (or use sudo): sudo bash config/setup_subnets.sh"
  exit 1
fi

echo "============================================================"
echo "  Setting up 2 Subnets (Inside: 10.10.0.0/24, Outside: 198.18.0.0/24)"
echo "============================================================"

# 1. Clean up any previous namespaces, veth links, and VPP host-interfaces
echo "[+] Cleaning up previous namespaces and interfaces..."
if [ ! -S "$VPP_SOCK" ] && [ -S "/run/vpp/cli.sock" ]; then
  VPP_SOCK="/run/vpp/cli.sock"
fi
if [ -S "$VPP_SOCK" ]; then
  vppctl -s "$VPP_SOCK" delete host-interface name veth-in-host 2>/dev/null || true
  vppctl -s "$VPP_SOCK" delete host-interface name veth-out-host 2>/dev/null || true
fi
ip netns del ns-inside 2>/dev/null || true
ip netns del ns-outside 2>/dev/null || true
ip link del veth-in-host 2>/dev/null || true
ip link del veth-out-host 2>/dev/null || true

# 2. Create Network Namespaces
echo "[+] Creating namespaces: ns-inside and ns-outside..."
ip netns add ns-inside
ip netns add ns-outside

# 3. Create Inside Subnet veth pair (10.10.0.0/24)
echo "[+] Setting up Inside Subnet (10.10.0.0/24)..."
ip link add veth-in-host numtxqueues 16 numrxqueues 16 type veth peer name veth-in-ns numtxqueues 16 numrxqueues 16
ip link set veth-in-ns netns ns-inside

# Configure ns-inside client IP and default route pointing to VPP
ip netns exec ns-inside ip link set lo up
ip netns exec ns-inside ip link set veth-in-ns up
ip netns exec ns-inside ip addr add 10.10.0.2/24 dev veth-in-ns
ip netns exec ns-inside ip route add default via 10.10.0.1 dev veth-in-ns
ip link set veth-in-host up

# 4. Create Outside Subnet veth pair (198.18.0.0/24)
echo "[+] Setting up Outside Subnet (198.18.0.0/24)..."
ip link add veth-out-host numtxqueues 16 numrxqueues 16 type veth peer name veth-out-ns numtxqueues 16 numrxqueues 16
ip link set veth-out-ns netns ns-outside

# Configure ns-outside target IP and route
ip netns exec ns-outside ip link set lo up
ip netns exec ns-outside ip link set veth-out-ns up
ip netns exec ns-outside ip addr add 198.18.0.2/24 dev veth-out-ns
ip netns exec ns-outside ip route add default via 198.18.0.1 dev veth-out-ns
ip link set veth-out-host up

echo "[+] Linux namespaces and veth pairs created successfully:"
echo "    • ns-inside:  IP=10.10.0.2/24,    Gateway=10.10.0.1 (VPP inside)"
echo "    • ns-outside: IP=198.18.0.2/24,   Gateway=198.18.0.1 (VPP outside)"

# 5. If VPP CLI socket is active, automatically connect host-interfaces into VPP
if [ ! -S "$VPP_SOCK" ] && [ -S "/run/vpp/cli.sock" ]; then
  VPP_SOCK="/run/vpp/cli.sock"
fi

if [ -S "$VPP_SOCK" ]; then
  echo "[+] Found running VPP instance at $VPP_SOCK. Binding interfaces..."
  vppctl -s "$VPP_SOCK" create host-interface name veth-in-host || true
  vppctl -s "$VPP_SOCK" set interface state host-veth-in-host up || true
  vppctl -s "$VPP_SOCK" set interface ip address host-veth-in-host 10.10.0.1/16 || true
  vppctl -s "$VPP_SOCK" set interface rx-mode host-veth-in-host polling || true

  vppctl -s "$VPP_SOCK" create host-interface name veth-out-host || true
  vppctl -s "$VPP_SOCK" set interface state host-veth-out-host up || true
  vppctl -s "$VPP_SOCK" set interface ip address host-veth-out-host 198.18.0.1/24 || true
  vppctl -s "$VPP_SOCK" set interface rx-mode host-veth-out-host polling || true

  # Configure the in-tree NAT PBA plugin (per-worker session pool)
  vppctl -s "$VPP_SOCK" nat pba set sessions 300000 || true
  vppctl -s "$VPP_SOCK" nat pba set timeouts udp 120 tcp-established 300 tcp-transitory 10 icmp 5 || true
  vppctl -s "$VPP_SOCK" nat pba set pool 203.0.113.1 203.0.113.64 || true

  # Enable the NAT PBA feature on inside/outside interfaces
  vppctl -s "$VPP_SOCK" set interface nat pba in host-veth-in-host || true
  vppctl -s "$VPP_SOCK" set interface nat pba out host-veth-out-host || true
  echo "[+] VPP interfaces (Polling Mode) and NAT44 configured and set UP."
else
  echo "[*] Note: VPP socket ($VPP_SOCK) is not running yet."
  echo "    When VPP is started, run this command to bind the host interfaces in VPP:"
  echo "    sudo vppctl create host-interface name veth-in-host"
  echo "    sudo vppctl set int state host-veth-in-host up"
  echo "    sudo vppctl set int ip address host-veth-in-host 10.10.0.1/24"
  echo "    sudo vppctl create host-interface name veth-out-host"
  echo "    sudo vppctl set int state host-veth-out-host up"
  echo "    sudo vppctl set int ip address host-veth-out-host 198.18.0.1/24"
fi

echo "============================================================"
echo "  Done! You can test manually with:"
echo "  sudo ip netns exec ns-inside ping 10.10.0.1"
echo "  sudo ip netns exec ns-inside ping 198.18.0.2"
echo "============================================================"
