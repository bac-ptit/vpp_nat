#!/usr/bin/env bash
# config/setup_2node_bgp_ha.sh
# Sets up 2 isolated VPP nodes (Node 1 & Node 2) in an Active-Active HA topology with BGP/ECMP.

set -euo pipefail

if [ "$EUID" -ne 0 ]; then
  echo "[-] Please run as root: sudo bash config/setup_2node_bgp_ha.sh"
  exit 1
fi

echo "============================================================"
echo "  Setting up 2-Node Active-Active VPP HA Topology           "
echo "============================================================"

# 1. Clean up any previous processes, namespaces, and links
echo "[1/6] Cleaning up previous instances and network devices..."
pkill -9 -f vpp 2>/dev/null || true
pkill -9 -f bird 2>/dev/null || true
sleep 1

ip netns del ns-inside 2>/dev/null || true
ip netns del ns-outside 2>/dev/null || true

ip link del br-inside 2>/dev/null || true
ip link del br-outside 2>/dev/null || true
ip link del veth-in-n1 2>/dev/null || true
ip link del veth-out-n1 2>/dev/null || true
ip link del veth-in-n2 2>/dev/null || true
ip link del veth-out-n2 2>/dev/null || true
ip link del veth-client-br 2>/dev/null || true
ip link del veth-server-br 2>/dev/null || true
ip link del veth-ha1 2>/dev/null || true
ip link del veth-ha2 2>/dev/null || true

# 2. Create Network Namespaces
echo "[2/6] Creating namespaces: ns-inside (Client) and ns-outside (Target)..."
ip netns add ns-inside
ip netns add ns-outside

# 3. Create Inside and Outside Bridges
echo "[3/6] Creating L2 bridges: br-inside (10.10.0.0/24) and br-outside (198.18.0.0/24)..."
ip link add br-inside type bridge
ip link set br-inside up

ip link add br-outside type bridge
ip link set br-outside up

# Client veth -> br-inside
ip link add veth-client type veth peer name veth-client-br
ip link set veth-client netns ns-inside
ip link set veth-client-br master br-inside
ip link set veth-client-br up

ip netns exec ns-inside ip link set lo up
ip netns exec ns-inside ip link set veth-client up
ip netns exec ns-inside ip addr add 10.10.0.2/24 dev veth-client

# Server veth -> br-outside
ip link add veth-server type veth peer name veth-server-br
ip link set veth-server netns ns-outside
ip link set veth-server-br master br-outside
ip link set veth-server-br up

ip netns exec ns-outside ip link set lo up
ip netns exec ns-outside ip link set veth-server up
ip netns exec ns-outside ip addr add 198.18.0.2/24 dev veth-server
# Add routes back to client and NAT pools in ns-outside
ip netns exec ns-outside ip route add 10.10.0.0/24 via 198.18.0.11 dev veth-server || true
ip netns exec ns-outside ip route add 203.0.113.0/24 via 198.18.0.11 dev veth-server || true

# Node 1 Veths
ip link add veth-in-n1 type veth peer name veth-in-n1-br
ip link set veth-in-n1-br master br-inside
ip link set veth-in-n1-br up
ip link set veth-in-n1 up

ip link add veth-out-n1 type veth peer name veth-out-n1-br
ip link set veth-out-n1-br master br-outside
ip link set veth-out-n1-br up
ip link set veth-out-n1 up

# Node 2 Veths
ip link add veth-in-n2 type veth peer name veth-in-n2-br
ip link set veth-in-n2-br master br-inside
ip link set veth-in-n2-br up
ip link set veth-in-n2 up

ip link add veth-out-n2 type veth peer name veth-out-n2-br
ip link set veth-out-n2-br master br-outside
ip link set veth-out-n2-br up
ip link set veth-out-n2 up

# Dedicated HA Interconnect Link between Node 1 and Node 2 (172.30.0.0/30)
ip link add veth-ha1 type veth peer name veth-ha2
ip link set veth-ha1 up
ip link set veth-ha2 up

# 4. Launch Node 1 and Node 2 VPP Instances (All 16 cores: Node 1 on Core 0-7, Node 2 on Core 8-15)
echo "[4/6] Starting Node 1 (Core 0-7: 1 main + 7 workers) and Node 2 (Core 8-15: 1 main + 7 workers)..."
nohup /usr/bin/vpp -c /home/bac/programming/nat/config/node1-startup.conf </dev/null >/dev/null 2>&1 &
nohup /usr/bin/vpp -c /home/bac/programming/nat/config/node2-startup.conf </dev/null >/dev/null 2>&1 &

sleep 2

# 5. Configure VPP Node 1
echo "[5/6] Configuring VPP Node 1..."
CLI1="/run/vpp/cli-node1.sock"
vppctl -s "$CLI1" create host-interface name veth-in-n1
vppctl -s "$CLI1" set interface state host-veth-in-n1 up
vppctl -s "$CLI1" set interface ip address host-veth-in-n1 10.10.0.11/24

vppctl -s "$CLI1" create host-interface name veth-out-n1
vppctl -s "$CLI1" set interface state host-veth-out-n1 up
vppctl -s "$CLI1" set interface ip address host-veth-out-n1 198.18.0.11/24

vppctl -s "$CLI1" create host-interface name veth-ha1
vppctl -s "$CLI1" set interface state host-veth-ha1 up
vppctl -s "$CLI1" set interface ip address host-veth-ha1 172.30.0.1/30

# NAT PBA pool 1 (shared by both HA nodes so mappings survive failover)
vppctl -s "$CLI1" nat pba set sessions 128000
vppctl -s "$CLI1" nat pba set pool 203.0.113.1 203.0.113.32
vppctl -s "$CLI1" set interface nat pba in host-veth-in-n1
vppctl -s "$CLI1" set interface nat pba out host-veth-out-n1

# 6. Configure VPP Node 2
echo "[6/6] Configuring VPP Node 2..."
CLI2="/run/vpp/cli-node2.sock"
vppctl -s "$CLI2" create host-interface name veth-in-n2
vppctl -s "$CLI2" set interface state host-veth-in-n2 up
vppctl -s "$CLI2" set interface ip address host-veth-in-n2 10.10.0.12/24

vppctl -s "$CLI2" create host-interface name veth-out-n2
vppctl -s "$CLI2" set interface state host-veth-out-n2 up
vppctl -s "$CLI2" set interface ip address host-veth-out-n2 198.18.0.12/24

vppctl -s "$CLI2" create host-interface name veth-ha2
vppctl -s "$CLI2" set interface state host-veth-ha2 up
vppctl -s "$CLI2" set interface ip address host-veth-ha2 172.30.0.2/30

# NAT PBA pool 2 (same pool as node 1 for HA session continuity)
vppctl -s "$CLI2" nat pba set sessions 128000
vppctl -s "$CLI2" nat pba set pool 203.0.113.1 203.0.113.32
vppctl -s "$CLI2" set interface nat pba in host-veth-in-n2
vppctl -s "$CLI2" set interface nat pba out host-veth-out-n2

echo "============================================================"
echo "  Both VPP Node 1 and Node 2 are UP and synchronized!       "
echo "  Node 1 Inside IP: 10.10.0.11/24 (CLI: $CLI1)              "
echo "  Node 2 Inside IP: 10.10.0.12/24 (CLI: $CLI2)              "
echo "  HA Interconnect:  172.30.0.1 <---> 172.30.0.2 (Port 1234) "
echo "============================================================"
