#!/usr/bin/env python3
"""Receive the log lines a test build sends with `env MX_LOG_UDP=<ip>:<port>`
(xhw_net.c, one line a datagram) and write one file per source address,
each line with the PC's time of arrival: two consoles' logs on one clock.

  tools/xbox/lan_logd.py                                   # xemu NAT: the guest sends to 10.0.2.2:41050
  tools/xbox/lan_logd.py --bind 192.168.1.20 --out logs    # consoles: MX_LOG_UDP=192.168.1.20:41050

Files: <out>/<source ip>.log, appended to, lines "HH:MM:SS.mmm <line>".
Binds one loopback or private (LAN) address only, default 127.0.0.1:41050.
Stops after --secs (default: Ctrl+C)."""
import argparse
import ipaddress
import os
import socket
import sys
import time


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=41050)
    ap.add_argument("--out", default=".")
    ap.add_argument("--secs", type=float, default=0)
    a = ap.parse_args()
    ip = ipaddress.ip_address(a.bind)
    if not (ip.is_loopback or ip.is_private or ip.is_link_local):
        sys.exit(f"{a.bind} is not a loopback, LAN or link-local address: refusing to bind it")
    os.makedirs(a.out, exist_ok=True)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((a.bind, a.port))
    s.settimeout(0.5)
    files, counts = {}, {}
    t0 = time.monotonic()
    print(f"lan_logd: {a.bind}:{a.port} -> {a.out}/<source>.log", flush=True)
    try:
        while not a.secs or time.monotonic() - t0 < a.secs:
            try:
                data, src = s.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            now = time.time()
            stamp = time.strftime("%H:%M:%S", time.localtime(now)) + f".{int(now * 1000) % 1000:03d}"
            f = files.get(src[0])
            if f is None:
                f = files[src[0]] = open(os.path.join(a.out, f"{src[0]}.log"), "a", encoding="utf-8")
                print(f"new source {src[0]}", flush=True)
            f.write(f"{stamp} {data.decode('utf-8', 'replace').rstrip()}\n")
            f.flush()
            counts[src[0]] = counts.get(src[0], 0) + 1
    except KeyboardInterrupt:
        pass
    for k, n in counts.items():
        print(f"{k}: {n} lines")


if __name__ == "__main__":
    main()
