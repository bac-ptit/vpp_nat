#!/usr/bin/env bash
# ==============================================================================
# NAT44 Multi-Thread Scaling Performance Benchmark (64B Uplink & Downlink)
# Evaluates linear core scaling across 1, 2, 4, 8, and 16 worker threads
# Duration: 30 seconds per test (default) or user-specified duration
# ==============================================================================

set -euo pipefail

DURATION="${1:-30}"
BINARY="./build/tests/bench_real_churn_10s"

if [[ ! -x "${BINARY}" ]]; then
    echo "[-] Binary ${BINARY} not found. Building release binary with CMake..."
    cmake --build build -j$(nproc)
fi

echo "================================================================================"
echo "   NAT44 MULTI-THREAD SCALING BENCHMARK (64B UPLINK & DOWNLINK, 15% CHURN)"
echo "   Testing 1 Worker Thread, 8 Worker Threads, 16 Worker Threads (${DURATION}s each)"
echo "================================================================================"

exec "${BINARY}" scaling "${DURATION}"

