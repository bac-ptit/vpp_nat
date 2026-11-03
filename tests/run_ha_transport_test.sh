#!/usr/bin/env bash
set -euo pipefail

BIN="${1:?usage: run_ha_transport_test.sh <test_ha_transport>}"
STANDBY_LOG="$(mktemp)"
ACTIVE_LOG="$(mktemp)"

cleanup() {
  rm -f "${STANDBY_LOG}" "${ACTIVE_LOG}"
}
trap cleanup EXIT

"${BIN}" standby 3435 3434 >"${STANDBY_LOG}" 2>&1 &
STANDBY_PID=$!

sleep 0.3
"${BIN}" active 3434 3435 >"${ACTIVE_LOG}" 2>&1
wait "${STANDBY_PID}"

echo "--- active ---"
cat "${ACTIVE_LOG}"
echo "--- standby ---"
cat "${STANDBY_LOG}"

grep -q "active_sessions=100" "${ACTIVE_LOG}"
grep -q "shadow_sessions=100" "${STANDBY_LOG}"
grep -q "promoted=1 promoted_sessions=100" "${STANDBY_LOG}"
