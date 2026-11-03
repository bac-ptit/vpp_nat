#!/usr/bin/env bash
# ==============================================================================
# NAT44 PBA — strong-scaling, 15% churn, fixed 3,3 GHz
#
# Chuẩn đo:
#   - Tổng session nền: 48.000 (chia đều cho số worker).
#   - Tổng active cap: 72.000 (chia đều; 1W=72k, 16W=4,5k/worker).
#   - 15% churn, batch 64, 64B uplink.
#   - Pin toàn bộ CPU ở 3,3 GHz, chạy 1/2/4/8/16 worker, lấy median.
#
# Đây là strong scaling: tổng working set giữ cố định 72k session, nên đo
# được speedup thật của engine thay vì tăng working set theo số worker.
#
# Usage: sudo DURATION=10 REPEATS=3 bash tests/bench_strong_churn15.sh
# ==============================================================================

set -euo pipefail

DURATION="${DURATION:-30}"
REPEATS="${REPEATS:-3}"
BATCH="${BATCH:-128}"
TOTAL_SESSIONS="${TOTAL_SESSIONS:-48000}"
TOTAL_CAP="${TOTAL_CAP:-72000}"
FREQ_KHZ="${FREQ_KHZ:-3300000}"
BIN="${BIN:-./build/tests/bench_real_churn_10s}"
OUT_DIR="${OUT_DIR:-build/bench_strong_churn15}"
read -ra WORKERS <<<"${WORKERS:-1 2 4 8 16}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "[-] Cần root để pin tần số và đọc PMU: sudo bash $0"
  exit 1
fi
if [[ ! -x "${BIN}" ]]; then
  echo "[-] Không thấy ${BIN}; build trước: cmake --build build -j\$(nproc)"
  exit 1
fi

mkdir -p "${OUT_DIR}"
PREV_MIN="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)"
PREV_MAX="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq)"
PREV_GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"

restore_freq() {
  for c in /sys/devices/system/cpu/cpu*/cpufreq; do
    echo "${PREV_MAX}" >"$c/scaling_max_freq" 2>/dev/null || true
    echo "${PREV_MIN}" >"$c/scaling_min_freq" 2>/dev/null || true
    echo "${PREV_GOV}" >"$c/scaling_governor" 2>/dev/null || true
  done
}
trap restore_freq EXIT

for c in /sys/devices/system/cpu/cpu*/cpufreq; do
  echo performance >"$c/scaling_governor"
  echo "${FREQ_KHZ}" >"$c/scaling_max_freq"
  echo "${FREQ_KHZ}" >"$c/scaling_min_freq"
done

echo "[*] Requested ${FREQ_KHZ} kHz; cpu0 actual $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) kHz"
echo "[*] strong: total sessions ${TOTAL_SESSIONS}, total active cap ${TOTAL_CAP}, batch ${BATCH}"
echo "[*] duration ${DURATION}s x ${REPEATS}; output ${OUT_DIR}"

median() {
  sort -n | awk '{a[NR]=$1} END {if (NR==0) print "0"; else print a[int((NR+1)/2)]}'
}

printf '%-8s %-12s %-9s %-14s %-12s %-12s %-12s\n' \
  "Workers" "Mpps(med)" "Scaling" "Sessions/s(med)" "Cyc/pkt(med)" "Peak(med)" "ChurnOK(med)"
printf '%s\n' "-----------------------------------------------------------------------------------------------"

BASE=""
for n in "${WORKERS[@]}"; do
  cap_worker=$((TOTAL_CAP / n))
  mpps_values=()
  session_values=()
  cyc_values=()
  peak_values=()
  churn_values=()

  for r in $(seq 1 "${REPEATS}"); do
    log="${OUT_DIR}/strong_${n}w_r${r}.log"
    env NAT_BENCH_CHURN=15 NAT_BENCH_TOTAL_SESSIONS="${TOTAL_SESSIONS}" \
      NAT_BENCH_MAX_SESSIONS="${cap_worker}" NAT_BENCH_BATCH="${BATCH}" \
      "${BIN}" 64-up "${DURATION}" "${n}" >"${log}" 2>&1

    mpps="$(sed -n 's/.*Packet forwarding successful: \([0-9.]*\).*/\1/p' "${log}" | head -n1)"
    sessions="$(sed -n 's/.*Total Sessions Handled \/ sec: \([0-9,]*\) session\/s.*/\1/p' "${log}" | head -n1 | tr -d ',')"
    cyc="$(sed -n 's/.*Cycles\/packet (HW): \([0-9.]*\).*/\1/p' "${log}" | head -n1)"
    peak="$(sed -n 's/.*Peak sessions (sum of per-worker HWM): \([0-9]*\).*/\1/p' "${log}" | head -n1)"
    churn="$(sed -n 's/.*Churn success: \([0-9.]*\)%.*/\1/p' "${log}" | head -n1)"

    if [[ -z "${mpps}" ]]; then
      echo "[-] ${n} worker run ${r}: không đọc được Mpps, xem ${log}"
      exit 1
    fi
    mpps_values+=("${mpps}")
    session_values+=("${sessions:-0}")
    cyc_values+=("${cyc:-0}")
    peak_values+=("${peak:-0}")
    churn_values+=("${churn:-0}")
  done

  mpps_med="$(printf '%s\n' "${mpps_values[@]}" | median)"
  session_med="$(printf '%s\n' "${session_values[@]}" | median)"
  cyc_med="$(printf '%s\n' "${cyc_values[@]}" | median)"
  peak_med="$(printf '%s\n' "${peak_values[@]}" | median)"
  churn_med="$(printf '%s\n' "${churn_values[@]}" | median)"
  if [[ -z "${BASE}" ]]; then BASE="${mpps_med}"; fi
  scaling="$(awk -v a="${mpps_med}" -v b="${BASE}" 'BEGIN { printf "%.2f", a / b }')"

  printf '%-8s %-12s %-9s %-14s %-12s %-12s %-12s\n' \
    "${n}" "${mpps_med}" "${scaling}x" "${session_med}" "${cyc_med}" "${peak_med}" "${churn_med}%"
done

printf '%s\n' "-----------------------------------------------------------------------------------------------"
printf 'Logs: %s\n' "${OUT_DIR}"
printf 'Chuẩn: strong scaling, 15%% churn, batch %s, 3,3 GHz, churn success kỳ vọng ≈15%%.\n' "${BATCH}"
