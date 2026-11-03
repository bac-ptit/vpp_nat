#!/usr/bin/env bash
# ==============================================================================
# NAT44 PBA — HA failover scenario: disable node1, verify node2 promotion
#
# Usage:
#   sudo bash tests/ha_failover_test.sh [planned|kill|link]
#
#   planned : stop HA sync on node1 (graceful)
#   kill    : kill node1 nat-ctl and VPP (unplanned process failure)
#   link    : drop the HA link and stop node1 control plane
#
# Environment:
#   VPP1_SOCK, VPP2_SOCK   CLI sockets (default node1/node2)
#   PRELOAD_CMD            optional shell command that creates traffic/sessions
#   RESTART_CMD            optional command used to bring node1 back after test
# ==============================================================================

set -euo pipefail

MODE="${1:-kill}"
VPP1_SOCK="${VPP1_SOCK:-/run/vpp/cli-node1.sock}"
VPP2_SOCK="${VPP2_SOCK:-/run/vpp/cli-node2.sock}"

GREEN="\033[0;32m"; CYAN="\033[0;36m"; YELLOW="\033[1;33m"; RED="\033[0;31m"; RESET="\033[0m"

vpp1() { vppctl -s "${VPP1_SOCK}" "$@"; }
vpp2() { vppctl -s "${VPP2_SOCK}" "$@"; }

now_ms() { date +%s%3N; }

sessions_on() {
  local out
  out="$("$@" show nat pba 2>/dev/null || true)"
  echo "${out}" | awk '/Total Active Sessions:/ {print $4; exit}'
}

ha_line() {
  local out
  out="$("$@" show nat pba ha 2>/dev/null || true)"
  echo "${out}" | head -n 1
}

if [[ "${EUID}" -ne 0 ]]; then
  echo -e "${RED}[-] Run as root: sudo bash $0 ${MODE}${RESET}"
  exit 1
fi

if ! command -v vppctl >/dev/null 2>&1; then
  echo -e "${RED}[-] vppctl not found${RESET}"
  exit 1
fi

echo -e "${CYAN}================================================================${RESET}"
echo -e "${CYAN}  NAT44 PBA HA FAILOVER TEST (${MODE})${RESET}"
echo -e "${CYAN}================================================================${RESET}"

echo -e "\n${YELLOW}[1/6] Checking both VPP instances${RESET}"
vpp1 show version >/dev/null
vpp2 show version >/dev/null
echo -e "${GREEN}[+] node1 and node2 reachable${RESET}"

echo -e "\n${YELLOW}[2/6] Initial HA state${RESET}"
echo "  node1: $(ha_line vpp1)"
echo "  node2: $(ha_line vpp2)"

if [[ -n "${PRELOAD_CMD:-}" ]]; then
  echo -e "\n${YELLOW}[3/6] Preloading sessions${RESET}"
  eval "${PRELOAD_CMD}"
else
  echo -e "\n${YELLOW}[3/6] No PRELOAD_CMD set; using current sessions${RESET}"
fi

SESSIONS_BEFORE="$(sessions_on vpp1)"
echo "  node1 sessions before failover: ${SESSIONS_BEFORE:-unknown}"

echo -e "\n${YELLOW}[4/6] Disabling node1 (${MODE})${RESET}"
START_MS="$(now_ms)"

case "${MODE}" in
  planned)
    vpp1 nat pba ha stop || true
    pkill -9 -f "nat-ctl.*--node node1" 2>/dev/null || true
    ;;
  link)
    ip link set veth-ha1 down 2>/dev/null || true
    ip link set veth-ha2 down 2>/dev/null || true
    pkill -9 -f "nat-ctl.*--node node1" 2>/dev/null || true
    ;;
  kill)
    pkill -9 -f "nat-ctl.*--node node1" 2>/dev/null || true
    pkill -9 -f "vpp.*node1-startup" 2>/dev/null || true
    ;;
  *)
    echo -e "${RED}[-] Unknown mode: ${MODE}. Use planned|kill|link.${RESET}"
    exit 1
    ;;
esac

echo -e "\n${YELLOW}[5/6] Waiting for node2 promotion${RESET}"
PROMOTED=0
for _ in $(seq 1 300); do
  LINE="$(ha_line vpp2)"
  if echo "${LINE}" | grep -q "role=active"; then
    PROMOTED=1
    break
  fi
  sleep 0.1
done
END_MS="$(now_ms)"
FAILOVER_MS=$((END_MS - START_MS))

if [[ "${PROMOTED}" -ne 1 ]]; then
  echo -e "${RED}[-] node2 did not promote within 30 s${RESET}"
  echo "  last node2 status: $(ha_line vpp2)"
  exit 1
fi

SESSIONS_AFTER="$(sessions_on vpp2)"
echo -e "${GREEN}[+] node2 promoted${RESET}"
echo "  node2 HA: $(ha_line vpp2)"
echo "  node2 sessions after promotion: ${SESSIONS_AFTER:-unknown}"

echo -e "\n${YELLOW}[6/6] Result${RESET}"
printf "  failover time:     %d ms\n" "${FAILOVER_MS}"
if [[ -n "${SESSIONS_BEFORE}" && -n "${SESSIONS_AFTER}" && "${SESSIONS_BEFORE}" -gt 0 ]]; then
  SURVIVAL="$(awk -v a="${SESSIONS_AFTER}" -v b="${SESSIONS_BEFORE}" 'BEGIN {printf "%.2f", 100*a/b}')"
  printf "  session survival:  %s %% (%s / %s)\n" "${SURVIVAL}" "${SESSIONS_AFTER}" "${SESSIONS_BEFORE}"
else
  printf "  session survival:  n/a\n"
fi

if ip netns list 2>/dev/null | grep -q '^ns-inside'; then
  echo -n "  traffic probe:     "
  if ip netns exec ns-inside ping -c 2 -W 1 198.18.0.2 >/dev/null 2>&1; then
    echo -e "${GREEN}PASS${RESET}"
  else
    echo -e "${RED}FAIL (VIP/route may not have moved)${RESET}"
  fi
fi

if [[ -n "${RESTART_CMD:-}" ]]; then
  echo -e "\n${YELLOW}[*] Restarting node1 for failback test${RESET}"
  eval "${RESTART_CMD}"
fi

echo -e "${CYAN}================================================================${RESET}"
echo -e "${GREEN}  HA failover scenario finished${RESET}"
echo -e "${CYAN}================================================================${RESET}"
