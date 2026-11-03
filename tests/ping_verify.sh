#!/usr/bin/env bash
# tests/ping_verify.sh
# Functional connectivity, NAT address pool inspection, and HA sync verification.

set -euo pipefail

NODE1_SOCK="/run/vpp/cli-node1.sock"
if [ ! -S "$NODE1_SOCK" ] && [ -S "/run/vpp/cli.sock" ]; then
  NODE1_SOCK="/run/vpp/cli.sock"
fi
NODE2_SOCK="/run/vpp/cli-node2.sock"

echo "===================================================="
echo "  NAT44 Data Plane & HA Verification Run            "
echo "===================================================="

echo "==> [1/4] Verifying VPP socket responsiveness..."
if [ -S "$NODE1_SOCK" ]; then
  echo "[+] Node 1 (Active) VPP Status:"
  sudo vppctl -s "$NODE1_SOCK" show version
else
  echo "[-] Node 1 socket ($NODE1_SOCK) not present."
fi

if [ -S "$NODE2_SOCK" ]; then
  echo "[+] Node 2 (Standby) VPP Status:"
  sudo vppctl -s "$NODE2_SOCK" show version
else
  echo "[-] Node 2 socket ($NODE2_SOCK) not present."
fi

echo "==> [2/4] Checking NAT44 IP Pool and PBA status..."
if [ -S "$NODE1_SOCK" ]; then
  sudo vppctl -s "$NODE1_SOCK" show nat44 addresses || true
  sudo vppctl -s "$NODE1_SOCK" show nat pba || true
fi

echo "==> [3/5] Testing end-to-end ping through NAT..."
if ip netns list | grep -q "ns-inside"; then
  echo "[+] Sending ping from ns-inside (10.10.0.2) -> ns-outside (198.18.0.2)..."
  sudo ip netns exec ns-inside ping -c 3 198.18.0.2 || true
  if [ -S "$NODE1_SOCK" ]; then
    echo "[+] Active NAT44 sessions:"
    sudo vppctl -s "$NODE1_SOCK" show nat44 sessions || true
  fi
else
  echo "[*] Namespaces not found. Run 'pixi run setup-subnets' first."
fi

echo "==> [4/5] Inspecting HA Synchronization Counters..."
if [ -S "$NODE1_SOCK" ]; then
  echo "[+] Node 1 HA status:"
  sudo vppctl -s "$NODE1_SOCK" show nat44 ei ha || true
fi
if [ -S "$NODE2_SOCK" ]; then
  echo "[+] Node 2 HA status:"
  sudo vppctl -s "$NODE2_SOCK" show nat44 ei ha || true
fi

echo "==> [5/5] Inspecting VRRP VIP forwarding state..."
if [ -S "$NODE1_SOCK" ]; then
  sudo vppctl -s "$NODE1_SOCK" show vrrp vr || true
fi

echo "===================================================="
echo "  Verification run completed.                       "
echo "===================================================="
