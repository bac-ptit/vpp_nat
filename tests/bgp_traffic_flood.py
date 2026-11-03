#!/usr/bin/env python3
"""
Parallel high-speed UDP flood injector for ns-inside in BGP failover test.
"""

import sys
import time
import socket
import struct
import multiprocessing

TARGET_IP = "198.18.0.2"


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    s = sum(struct.unpack(f"!{len(data)//2}H", data))
    s = (s >> 16) + (s & 0xFFFF)
    s += s >> 16
    return ~s & 0xFFFF


def worker(proc_id: int, duration_sec: float, counter):
    raw_sock = socket.socket(socket.AF_INET, socket.SOCK_RAW, socket.IPPROTO_RAW)
    raw_sock.setsockopt(socket.IPPROTO_IP, socket.IP_HDRINCL, 1)

    dst_ip_bytes = socket.inet_aton(TARGET_IP)
    udp_payload = b"LIVE_HA_STRESS"
    udp_len = 8 + len(udp_payload)

    t_end = time.perf_counter() + duration_sec
    local_sent = 0
    base_id = proc_id * 5_000_000
    flow_id = 0

    while time.perf_counter() < t_end:
        for _ in range(500):
            flow_id += 1
            curr_id = base_id + flow_id
            mixed = (curr_id * 2654435761) & 0xFFFFFFFF
            src_ip = socket.inet_aton(f"10.10.{(mixed >> 16) % 250}.{(mixed >> 8) % 250 + 1}")
            sport = 1024 + (mixed % 64000)
            dport = 80 + ((curr_id ^ (curr_id >> 4)) % 2000)

            udp_hdr = struct.pack("!HHHH", sport, dport, udp_len, 0)
            ip_hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_len, curr_id & 0xFFFF, 0, 64, 17, 0, src_ip, dst_ip_bytes)
            ip_chk = checksum(ip_hdr)
            ip_hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_len, curr_id & 0xFFFF, 0, 64, 17, ip_chk, src_ip, dst_ip_bytes)

            raw_sock.sendto(ip_hdr + udp_hdr + udp_payload, (TARGET_IP, 0))
            local_sent += 1

    raw_sock.close()
    with counter.get_lock():
        counter.value += local_sent


if __name__ == "__main__":
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    num_procs = int(sys.argv[2]) if len(sys.argv) > 2 else 6

    total = multiprocessing.Value('q', 0)
    procs = []
    t0 = time.perf_counter()
    for i in range(num_procs):
        p = multiprocessing.Process(target=worker, args=(i, duration, total))
        p.start()
        procs.append(p)

    for p in procs:
        p.join()
    t_actual = time.perf_counter() - t0
    print(f"SENT:{total.value}:TIME:{t_actual:.4f}")
