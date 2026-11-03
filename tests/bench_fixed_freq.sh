#!/usr/bin/env bash
# ==============================================================================
# NAT44 PBA — fixed-frequency 15% churn scaling baseline
#
# Chạy 1/2/4/8/16 worker ở cùng một mức xung (mặc định 3,3 GHz), 15% churn,
# max_sessions=0, 30 s x 3 lần, lấy median. Dùng cho Phase 0 và mọi phase sau.
#
# Usage:
#   sudo FREQ=3.3GHz DURATION=30 REPEATS=3 bash tests/bench_fixed_freq.sh
#
# Lưu ý: script khôi phục governor cũ khi thoát. Nếu máy không hỗ trợ userspace
# governor, script vẫn chạy nhưng ghi lại tần số thực tế để tránh so sánh sai.
# ==============================================================================

set -euo pipefail

FREQ="${FREQ:-3.3GHz}"
DURATION="${DURATION:-30}"
REPEATS="${REPEATS:-3}"
BIN="${BIN:-./build/tests/bench_real_churn_10s}"
OUT_DIR="${OUT_DIR:-build/bench_3p3}"
WORKERS=(1 2 4 8 16)

if [[ "${EUID}" -ne 0 ]]; then
  echo "[-] Cần root để đổi governor và đọc PMU: sudo bash $0"
  exit 1
fi
if [[ ! -x "${BIN}" ]]; then
  echo "[-] Không thấy ${BIN}; build trước: cmake --build build -j\$(nproc)"
  exit 1
fi

mkdir -p "${OUT_DIR}"
PREV_GOV=""
if command -v cpupower >/dev/null 2>&1; then
  PREV_GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)"
  cleanup() {
    if [[ -n "${PREV_GOV}" ]]; then
      cpupower frequency-set -g "${PREV_GOV}" >/dev/null 2>&1 || true
    fi
  }
  trap cleanup EXIT
  cpupower frequency-set -g userspace -f "${FREQ}" >/dev/null 2>&1 || {
    echo "[!] Không đặt được userspace ${FREQ}; dùng governor hiện tại."
    PREV_GOV=""
  }
else
  echo "[!] Không có cpupower; không thể pin xung. Sẽ ghi lại tần số thực tế."
fi

actual_freq_khz="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo 0)"
echo "[*] Frequency request: ${FREQ}; cpu0 actual: ${actual_freq_khz} kHz"
echo "[*] Duration per run: ${DURATION}s x ${REPEATS}; output: ${OUT_DIR}"

median() {
  sort -n | awk '{a[NR]=$1} END {if (NR==0) print "0"; else print a[int((NR+1)/2)]}'
}

printf '%-8s %-12s %-10s %-14s %-12s %-10s %-10s\n' \
  "Workers" "Mpps(med)" "Scaling" "Sessions/s(med)" "Cyc/pkt(med)" "Peak" "ChurnOK"
printf '%s\n' "-----------------------------------------------------------------------------------------------"

BASE=""
for n in "${WORKERS[@]}"; do
  mpps_values=()
  session_values=()
  cyc_values=()
  peak_values=()
  churn_values=()

  for r in $(seq 1 "${REPEATS}"); do
    log="${OUT_DIR}/fixed_${n}w_r${r}.log"
    env NAT_BENCH_CHURN=15 NAT_BENCH_MAX_SESSIONS=0 NAT_BENCH_POOL=30000 \
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

  printf '%-8s %-12s %-10s %-14s %-12s %-10s %-10s\n' \
    "${n}" "${mpps_med}" "${scaling}x" "${session_med}" "${cyc_med}" "${peak_med}" "${churn_med}%"
done

printf '%s\n' "-----------------------------------------------------------------------------------------------"
printf 'Logs: %s\n' "${OUT_DIR}"
printf 'Chuẩn: 30k flow/worker, 15%% churn, max_sessions=0, 64B, cùng tần số cho 1W và 16W.\n'
