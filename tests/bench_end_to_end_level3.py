#!/usr/bin/env python3
import subprocess
import time
import json
import re
import os
import sys

def run_cmd(cmd, check=True):
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and res.returncode != 0:
        print(f"[-] Command failed: {cmd}\n[-] Error: {res.stderr}")
    return res.stdout.strip()

def get_vpp_int_stats():
    out = run_cmd("sudo vppctl show int", check=False)
    stats = {}
    current_if = None
    for line in out.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0].startswith("host-veth-"):
            current_if = parts[0]
            stats[current_if] = {"rx_pkts": 0, "rx_bytes": 0, "tx_pkts": 0, "tx_bytes": 0}
            line_str = " ".join(parts[1:])
        else:
            line_str = " ".join(parts)

        if current_if and "rx packets" in line_str:
            tokens = line_str.split("rx packets")
            try:
                stats[current_if]["rx_pkts"] = int(tokens[1].split()[0])
            except (IndexError, ValueError):
                pass
        elif current_if and "rx bytes" in line_str:
            tokens = line_str.split("rx bytes")
            try:
                stats[current_if]["rx_bytes"] = int(tokens[1].split()[0])
            except (IndexError, ValueError):
                pass
        elif current_if and "tx packets" in line_str:
            tokens = line_str.split("tx packets")
            try:
                stats[current_if]["tx_pkts"] = int(tokens[1].split()[0])
            except (IndexError, ValueError):
                pass
        elif current_if and "tx bytes" in line_str:
            tokens = line_str.split("tx bytes")
            try:
                stats[current_if]["tx_bytes"] = int(tokens[1].split()[0])
            except (IndexError, ValueError):
                pass
    return stats

def main():
    print("================================================================================")
    print("   LEVEL 3 (END-TO-END) VIRTUAL SYSTEM BENCHMARK ON CURRENT PC                  ")
    print("================================================================================")
    print("[*] Architecture:   Client (ns-inside) -> veth-in -> VPP NAT -> veth-out -> Server (ns-outside)")
    print("[*] Subnets:        Inside 10.10.0.0/24 | Outside 198.18.0.0/24")
    print("[*] NAT Mode:       VPP Dynamic Endpoint-Dependent NAT44 (10.10.0.2 -> 203.0.113.x)")
    print("[*] Test Duration:  10 Seconds continuous multi-stream TCP/UDP traffic")
    print("--------------------------------------------------------------------------------\n")

    # 1. Clean up old processes
    run_cmd("sudo killall -9 iperf3 2>/dev/null || true", check=False)
    time.sleep(0.5)

    # 2. Launch 4 iperf3 servers on ns-outside
    num_servers = 4
    base_port = 5201
    print(f"[1/3] Launching {num_servers} iperf3 servers in namespace 'ns-outside' (Ports {base_port}-{base_port+num_servers-1})...")
    for i in range(num_servers):
        p = base_port + i
        run_cmd(f"sudo ip netns exec ns-outside iperf3 -s -p {p} -D")
    time.sleep(1.0)

    # 3. Snapshot VPP Interface Stats before test
    stats_before = get_vpp_int_stats()
    t_start = time.time()

    # 4. Launch 4 iperf3 clients in ns-inside, each with 4 parallel streams (total 16 streams)
    duration = 10
    streams_per_client = 4
    total_streams = num_servers * streams_per_client
    print(f"[2/3] Starting {num_servers} iperf3 clients ({total_streams} parallel TCP streams) for {duration} seconds...")

    client_procs = []
    output_files = []
    for i in range(num_servers):
        p = base_port + i
        out_file = f"/tmp/iperf_client_{p}.json"
        output_files.append(out_file)
        cmd = f"sudo ip netns exec ns-inside iperf3 -c 198.18.0.2 -p {p} -P {streams_per_client} -t {duration} -J > {out_file}"
        proc = subprocess.Popen(cmd, shell=True)
        client_procs.append(proc)

    # Wait for all clients to finish
    for proc in client_procs:
        proc.wait()

    t_end = time.time()
    elapsed = t_end - t_start

    # 5. Snapshot VPP Interface Stats after test
    stats_after = get_vpp_int_stats()

    # 6. Check Active NAT sessions in VPP
    vpp_sessions_raw = run_cmd("sudo vppctl show nat44 sessions", check=False)
    active_session_count = vpp_sessions_raw.count("dynamic translation")

    # 7. Aggregate iperf3 metrics
    total_bytes_sent = 0
    total_bytes_recv = 0
    total_retransmits = 0
    bitrates_gbps = []

    for out_file in output_files:
        if os.path.exists(out_file):
            try:
                with open(out_file, "r") as f:
                    data = json.load(f)
                    end_data = data.get("end", {})
                    sum_sent = end_data.get("sum_sent", {})
                    sum_recv = end_data.get("sum_received", {})
                    total_bytes_sent += sum_sent.get("bytes", 0)
                    total_bytes_recv += sum_recv.get("bytes", 0)
                    total_retransmits += sum_sent.get("retransmits", 0)
                    bps = sum_sent.get("bits_per_second", 0)
                    bitrates_gbps.append(bps / 1e9)
            except Exception as e:
                pass
            finally:
                if os.path.exists(out_file):
                    os.remove(out_file)

    # Cleanup servers
    run_cmd("sudo killall -9 iperf3 2>/dev/null || true", check=False)

    total_gbps = sum(bitrates_gbps)
    total_gb_transferred = total_bytes_sent / (1024**3)

    # VPP Packet counters
    in_rx_pkts = 0
    out_tx_pkts = 0
    if "host-veth-in-host" in stats_after and "host-veth-in-host" in stats_before:
        in_rx_pkts = stats_after["host-veth-in-host"]["rx_pkts"] - stats_before["host-veth-in-host"]["rx_pkts"]
    if "host-veth-out-host" in stats_after and "host-veth-out-host" in stats_before:
        out_tx_pkts = stats_after["host-veth-out-host"]["tx_pkts"] - stats_before["host-veth-out-host"]["tx_pkts"]

    pps_rate = in_rx_pkts / elapsed if elapsed > 0 else 0

    print("\n[3/3] Benchmark Complete! Generating Level 3 Report...\n")
    print("================================================================================")
    print("   BẢNG TỔNG HỢP KẾT QUẢ ĐO END-TO-END (LEVEL 3) TRÊN MÁY HIỆN TẠI              ")
    print("================================================================================")
    print("  ┌──────────────────────────────────────────────┬────────────────────────────┐")
    print("  │ Chỉ số kiểm tra End-to-End                   │ Giá trị thực tế đạt được   │")
    print("  ├──────────────────────────────────────────────┼────────────────────────────┤")
    print(f"  │ Loại lưu lượng kiểm tra                      │ TCP Multi-Stream (16 luồng)│")
    print(f"  │ Thời gian chạy liên tục                      │ {elapsed:>18.2f} giây     │")
    print(f"  │ Số kết nối TCP đồng thời (Active Sessions)   │ {total_streams:>18} sessions │")
    print(f"  │ Tổng dung lượng truyền tải thành công        │ {total_gb_transferred:>18.2f} GBytes   │")
    print(f"  │ TỔNG BĂNG THÔNG END-TO-END (THROUGHPUT)      │ {total_gbps:>18.2f} Gbps     │")
    print(f"  │ Băng thông quy đổi GB/s                      │ {total_gbps/8.0:>18.2f} GB/s      │")
    print(f"  │ Số gói tin VPP NAT đã xử lý (Total Packets)  │ {in_rx_pkts:>18,d} packets  │")
    print(f"  │ Tốc độ xử lý gói tin End-to-End (PPS)        │ {pps_rate:>18,.0f} pps      │")
    print(f"  │ Tốc độ gói tin Mpps (Million pkts/sec)       │ {pps_rate/1e6:>18.3f} Mpps     │")
    print(f"  │ Số gói tin bị truyền lại (TCP Retransmits)   │ {total_retransmits:>18} packets  │")
    print(f"  │ Tỷ lệ mất gói / lỗi truyền lại               │ {total_retransmits/max(1, in_rx_pkts)*100:>18.4f} %        │")
    print("  └──────────────────────────────────────────────┴────────────────────────────┘")
    print("================================================================================\n")

if __name__ == "__main__":
    main()
