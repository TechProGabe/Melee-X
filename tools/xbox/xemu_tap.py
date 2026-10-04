#!/usr/bin/env python3
"""A relay between two xemu instances' UDP network back ends that writes
every Ethernet frame to a pcap (docs/testing.md "Two xemu instances"): the
virtual cable with a capture on it, no driver needed.
tools/xbox/xemu_pair.sh starts it between the pair; net_audit.py reads the
capture.

  tools/xbox/xemu_tap.py --pcap pair.pcap               # A: 127.0.0.1:9368 <-> tap :9370 | tap :9371 <-> B: 127.0.0.1:9369
  tools/xbox/xemu_tap.py --pcap pair.pcap --secs 600

xemu's UDP back end sends each frame as one datagram (bind_addr, remote_addr
in [net.udp]): A's remote_addr is the tap's A side (127.0.0.1:9370), B's
the tap's B side (9371). A frame from A goes out to B (B's bind_addr) from
the B side, and the other way round, so each instance hears its frames from
the address it sends to. Loopback only. Each frame is flushed to the pcap
as it passes, so a capture cut short by a kill is still readable."""
import argparse
import select
import socket
import struct
import sys
import time


def hostport(s):
    h, p = s.rsplit(":", 1)
    return h, int(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--a", default="127.0.0.1:9368", help="instance A's bind_addr")
    ap.add_argument("--b", default="127.0.0.1:9369", help="instance B's bind_addr")
    ap.add_argument("--tap-a", default="127.0.0.1:9370", help="where A sends (A's remote_addr)")
    ap.add_argument("--tap-b", default="127.0.0.1:9371", help="where B sends (B's remote_addr)")
    ap.add_argument("--pcap", required=True)
    ap.add_argument("--secs", type=float, default=0)
    a = ap.parse_args()
    for addr in (a.tap_a, a.tap_b):
        if hostport(addr)[0] != "127.0.0.1":
            sys.exit("the tap binds loopback only")
    sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sa.bind(hostport(a.tap_a))
    sb.bind(hostport(a.tap_b))
    for s in (sa, sb):
        if hasattr(socket, "SIO_UDP_CONNRESET"):   # Windows: an instance not up yet is no error
            s.ioctl(socket.SIO_UDP_CONNRESET, False)
    to_a, to_b = hostport(a.a), hostport(a.b)
    out = open(a.pcap, "wb")
    out.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
    out.flush()
    t0 = time.monotonic()
    wall0 = time.time()
    n = {"a": 0, "b": 0}
    print(f"xemu_tap: A {a.a} <-> {a.tap_a} | {a.tap_b} <-> B {a.b}, pcap {a.pcap}", flush=True)
    try:
        while not a.secs or time.monotonic() - t0 < a.secs:
            r, _, _ = select.select([sa, sb], [], [], 0.5)
            for s in r:
                try:
                    data, src = s.recvfrom(65535)
                except OSError:   # Windows: the other end isn't listening yet (ICMP port unreachable)
                    continue
                t = wall0 + (time.monotonic() - t0)
                out.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6), len(data), len(data)) + data)
                out.flush()
                if s is sa:
                    n["a"] += 1
                    try:
                        sb.sendto(data, to_b)
                    except OSError:
                        pass
                else:
                    n["b"] += 1
                    try:
                        sa.sendto(data, to_a)
                    except OSError:
                        pass
    except KeyboardInterrupt:
        pass
    print(f"xemu_tap: {n['a']} frames from A, {n['b']} from B", flush=True)


if __name__ == "__main__":
    main()
