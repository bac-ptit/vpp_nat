#!/usr/bin/env python3
import os
import re
import subprocess
import sys

SCALES = [
    (100_000, "100k"),
    (200_000, "200k"),
    (500_000, "500k"),
    (1_000_000, "1M"),
]

WORKER_POINTS = [1, 2, 4, 8, 12, 16]

def run_point(sessions_per_worker, workers, duration=5):
    cmd = [
        "sudo", "env",
        "NAT_BENCH_CHURN=0",
        f"NAT_BENCH_POOL={sessions_per_worker}",
        f"NAT_BENCH_MAX_SESSIONS={sessions_per_worker}",
        "NAT_BENCH_PUBLIC_COUNT=512",
        "NAT_BENCH_BATCH=256",
        "NAT_BENCH_LAZY=1",
        "./build/tests/bench_real_churn_10s",
        "64-up", str(duration), str(workers)
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    
    mpps_match = re.search(r"Packet forwarding successful:\s+([\d\.]+)\s+Mpps", out)
    cycles_match = re.search(r"Cycles/packet \(HW\):\s+([\d\.]+)", out)
    instr_match = re.search(r"Instructions/packet \(HW\):\s+([\d\.]+)", out)
    miss_match = re.search(r"Cache misses/packet:\s+([\d\.]+)\s+\(([\d\.]+)%", out)
    total_sess_match = re.search(r"Peak sessions \(sum of per-worker HWM\):\s+(\d+)", out)
    
    mpps = float(mpps_match.group(1)) if mpps_match else 0.0
    cycles = float(cycles_match.group(1)) if cycles_match else 0.0
    instr = float(instr_match.group(1)) if instr_match else 0.0
    miss_pct = float(miss_match.group(2)) if miss_match else 0.0
    total_sess = int(total_sess_match.group(1)) if total_sess_match else (sessions_per_worker * workers)
    
    return {
        "workers": workers,
        "per_worker": sessions_per_worker,
        "total_sess": total_sess,
        "mpps": mpps,
        "cycles": cycles,
        "instr": instr,
        "miss_pct": miss_pct,
    }

def main():
    print("=" * 80)
    print("STARTING FULL SCALING & WORKING SET SWEEP (5s per test, 0% churn, 64B Uplink)")
    print("Workers: 1, 2, 4, 8, 12, 16")
    print("Sessions / worker: 100k, 200k, 500k, 1M, plus 20M on 16W")
    print("=" * 80)
    
    results = {}
    
    for sess, label in SCALES:
        print(f"\n>>> Running scale: {label}/worker...")
        scale_results = []
        base_mpps = 1.0
        for w in WORKER_POINTS:
            print(f"  - Testing {w:2d} worker(s) at {label}/worker (Total: {sess * w:10,d} sessions)...", end="", flush=True)
            res = run_point(sess, w, duration=5)
            if w == 1:
                base_mpps = max(res["mpps"], 0.001)
            res["scale"] = res["mpps"] / base_mpps
            scale_results.append(res)
            print(f" done -> {res['mpps']:6.2f} Mpps, {res['cycles']:5.1f} cyc/pkt, Miss {res['miss_pct']:4.1f}%, Scale {res['scale']:5.2f}x")
        results[label] = scale_results
        
    print("\n>>> Testing 20,000,000 Total Sessions (1.25M/worker on 16W)...", end="", flush=True)
    res_20m = run_point(1_250_000, 16, duration=5)
    print(f" done -> {res_20m['mpps']:6.2f} Mpps, {res_20m['cycles']:5.1f} cyc/pkt, Miss {res_20m['miss_pct']:4.1f}%")
    results["20M"] = res_20m
    
    # Print formatted markdown table
    print("\n\n" + "=" * 80)
    print("### CONSOLIDATED BENCHMARK RESULTS")
    print("=" * 80 + "\n")
    
    for sess, label in SCALES:
        print(f"#### Working Set Scale: {label} per Worker")
        print("| Workers | Total Sessions | Est. Working Set | Forwarding Rate (Mpps) | Cycles / Packet | Cache Miss Rate | Scaling vs 1W |")
        print("|:---:|:---:|:---:|:---:|:---:|:---:|:---:|")
        for r in results[label]:
            ws_mb = (r["total_sess"] * 64 + r["total_sess"] * 32) / (1024 * 1024)
            ws_str = f"{ws_mb:.1f} MB" if ws_mb < 1024 else f"{ws_mb/1024:.2f} GB"
            print(f"| **{r['workers']}T** | {r['total_sess']:,} | {ws_str} | **{r['mpps']:.2f} Mpps** | {r['cycles']:.1f} | {r['miss_pct']:.1f}% | **{r['scale']:.2f}×** |")
        print()

    print("#### 20 Million Active Sessions Test (16 Workers @ 1.25M/worker)")
    r = results["20M"]
    ws_gb = (20_000_000 * 96) / (1024 * 1024 * 1024)
    print(f"- **Total Active Sessions:** 20,000,000 (20M)")
    print(f"- **Worker Cores:** 16 threads (100% SMT utilization)")
    print(f"- **Estimated Working Set:** ~{ws_gb:.2f} GB (Exceeds L3 by >110×)")
    print(f"- **Throughput:** **`{r['mpps']:.2f} Mpps`**")
    print(f"- **Cycles per Packet:** `{r['cycles']:.1f}` cycles")
    print(f"- **Cache Miss Rate:** `{r['miss_pct']:.1f}%`")

if __name__ == "__main__":
    main()
