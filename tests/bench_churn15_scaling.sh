#!/usr/bin/env bash
# ==============================================================================
# NAT44 PBA — 15% churn scaling test (1/2/4/8/16 workers)
#
# Chuẩn đo:
#   - 30.000 flow/worker, 15% new session, 85% established.
#   - max active = 0 (unlimited; engine chạm trần port PBA 64k/worker).
#   - 64B, uplink only, duration mặc định 5s mỗi mốc.
#   - In kèm peak active và churn success để phát hiện workload bị lệch.
#
# Usage: sudo bash tests/bench_churn15_scaling.sh [duration_sec]
# ==============================================================================

set -euo pipefail

DURATION="${1:-5}"
BIN="./build/tests/bench_real_churn_10s"
OUT_DIR="build/bench_churn15"
WORKERS=(1 2 4 8 16)

if [[ "${EUID}" -ne 0 ]]; then
  echo "[-] Chạy bằng root để có PMU và pinning: sudo bash $0 ${DURATION}"
  exit 1
fi

if [[ ! -x "${BIN}" ]]; then
  echo "[-] Không thấy ${BIN}; build trước: cmake --build build -j\$(nproc)"
  exit 1
fi

mkdir -p "${OUT_DIR}"

printf '%-8s %-10s %-9s %-11s %-8s %-12s %-8s\n' \
  "Workers" "Mpps" "Scaling" "Cyc/packet" "Instr/pkt" "Peak active" "ChurnOK"
printf '%s\n' "--------------------------------------------------------------------------------"

BASE=""
for n in "${WORKERS[@]}"; do
  log="${OUT_DIR}/churn15_${n}w.log"
  env NAT_BENCH_CHURN=15 NAT_BENCH_MAX_SESSIONS=0 \
    "${BIN}" 64-up "${DURATION}" "${n}" >"${log}" 2>&1

  mpps="$(sed -n 's/.*Packet forwarding successful: \([0-9.]*\).*/\1/p' "${log}" | head -n1)"
  cyc="$(sed -n 's/.*Cycles\/packet (HW): \([0-9.]*\).*/\1/p' "${log}" | head -n1)"
  instr="$(sed -n 's/.*Instructions\/packet (HW): \([0-9.]*\).*/\1/p' "${log}" | head -n1)"
  peak="$(sed -n 's/.*Peak sessions (sum of per-worker HWM): \([0-9]*\).*/\1/p' "${log}" | head -n1)"
  churn="$(sed -n 's/.*Churn success: \([0-9.]*\)%.*/\1/p' "${log}" | head -n1)"

  if [[ -z "${mpps}" ]]; then
    echo "[-] ${n} worker: không đọc được Mpps, xem ${log}"
    exit 1
  fi
  if [[ -z "${BASE}" ]]; then BASE="${mpps}"; fi
  scaling="$(awk -v a="${mpps}" -v b="${BASE}" 'BEGIN { printf "%.2f", a / b }')"

  printf '%-8s %-10s %-9s %-11s %-8s %-12s %-8s\n' \
    "${n}" "${mpps}" "${scaling}x" "${cyc}" "${instr}" "${peak:-n/a}" "${churn:-n/a}%"
done

printf '%s\n' "--------------------------------------------------------------------------------"
printf 'Logs: %s\n' "${OUT_DIR}"
printf 'Ghi chú: max active = 0 (unlimited), 15%% churn; peak tăng theo worker vì engine\n'
printf 'chạm trần port PBA 64k/worker. Chỉ so scaling khi churn success ≈ 15%%.\n'
