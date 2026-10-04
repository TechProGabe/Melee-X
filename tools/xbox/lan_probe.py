#!/usr/bin/env python3
"""The network probe's peer on a PC (docs/testing.md "Network"): what
an Xbox running a test build with `env MX_NETPROBE=1` (xhw_netprobe.c) sees
as another console. Beacons once a second, answers pings, logs the beacons
it hears, and with --ping pings every console it has heard 60 times a
second for 60 s and prints the round trips as the PC sees them.

  # xemu with the NAT back end (xemu.toml forwards host UDP 41011 to the
  # guest's 41001, docs/testing.md): the defaults
  tools/xbox/lan_probe.py
  # console on the LAN: bind the PC's LAN address, beacon to the broadcast address
  tools/xbox/lan_probe.py --bind 192.168.1.20 --port 41001 --to 255.255.255.255:41001 --ping

It binds one address only (default 127.0.0.1), and only a loopback or
private (LAN) one: nothing here faces the internet. Stops after --secs
(default: run until Ctrl+C). Wire (text): "MXNP1 BEACON <mac>",
"MXNP1 PING <seq> <t>", "MXNP1 PONG <seq> <t>"."""
import argparse
import ipaddress
import select
import socket
import statistics
import sys
import time


def private_or_die(addr):
    ip = ipaddress.ip_address(addr)
    if not (ip.is_loopback or ip.is_private or ip.is_link_local):
        sys.exit(f"{addr} is not a loopback, LAN or link-local address: refusing to bind it")


def hostport(s):
    h, p = s.rsplit(":", 1)
    return h, int(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=41002)
    ap.add_argument("--to", action="append", help="beacon destination host:port (default 127.0.0.1:41011)")
    ap.add_argument("--ping", action="store_true", help="ping the consoles heard, 60 Hz for 60 s")
    ap.add_argument("--secs", type=float, default=0)
    a = ap.parse_args()
    private_or_die(a.bind)
    to = [hostport(t) for t in (a.to or ["127.0.0.1:41011"])]
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.bind((a.bind, a.port))
    if hasattr(socket, "SIO_UDP_CONNRESET"):   # Windows: don't report port unreachables as errors
        s.ioctl(socket.SIO_UDP_CONNRESET, False)
    t0 = time.monotonic()
    peers = {}           # (ip, port) -> first heard
    rtts = {}            # ip -> [us]
    sent = {}
    answered = {}
    next_beacon = t0
    ping_start = ping_end = None
    next_ping = 0.0
    seq = 0
    print(f"lan_probe: {a.bind}:{a.port}, beacons to {', '.join(f'{h}:{p}' for h, p in to)}", flush=True)
    while not a.secs or time.monotonic() - t0 < a.secs:
        now = time.monotonic()
        if now >= next_beacon:
            for h, p in to:
                try:
                    s.sendto(b"MXNP1 BEACON pc", (h, p))
                except OSError as e:
                    print(f"beacon to {h}:{p}: {e}", flush=True)
            next_beacon += 1.0
        if a.ping and peers and ping_start is None:
            ping_start, ping_end, next_ping = now, now + 60, now
            print(f"pinging {len(peers)} console(s) for 60 s", flush=True)
        if ping_start is not None and now >= next_ping and now < ping_end:
            for (ip, port) in peers:
                s.sendto(f"MXNP1 PING {seq} {int(time.monotonic() * 1e6)}".encode(), (ip, port))
                sent[ip] = sent.get(ip, 0) + 1
            seq += 1
            next_ping += 1 / 60
        if ping_start is not None and ping_end and now >= ping_end + 0.5:
            for ip, n in sent.items():
                r = sorted(rtts.get(ip, []))
                if r:
                    print(f"rtt {ip}: {n} sent, {len(r)} back, lost {n - len(r)}, min {r[0]} avg "
                          f"{int(statistics.mean(r))} p99 {r[min(len(r) - 1, len(r) * 99 // 100)]} max {r[-1]} us",
                          flush=True)
                else:
                    print(f"rtt {ip}: {n} sent, 0 back", flush=True)
            ping_end = 0
        due = min(next_beacon, next_ping if ping_start is not None and ping_end else next_beacon)
        r, _, _ = select.select([s], [], [], max(0.0, due - time.monotonic()))
        if not r:
            continue
        try:
            data, src = s.recvfrom(2048)
        except ConnectionResetError:   # Windows: an earlier datagram's port unreachable (xemu not up yet)
            continue
        t = time.monotonic()
        parts = data.decode("ascii", "replace").split()
        if len(parts) < 2 or parts[0] != "MXNP1":
            continue
        if parts[1] == "BEACON":
            if src not in peers:
                peers[src] = t
                print(f"[{t - t0:7.2f}] beacon from {src[0]}:{src[1]} ({' '.join(parts[2:])})", flush=True)
        elif parts[1] == "PING" and len(parts) >= 4:
            s.sendto(f"MXNP1 PONG {parts[2]} {parts[3]}".encode(), src)
            n = answered[src[0]] = answered.get(src[0], 0) + 1
            if n == 1 or n % 600 == 0:
                print(f"[{t - t0:7.2f}] {n} pings from {src[0]} answered", flush=True)
        elif parts[1] == "PONG" and len(parts) >= 4 and ping_start is not None:
            rtts.setdefault(src[0], []).append(int(t * 1e6) - int(parts[3]))


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
