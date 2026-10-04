#!/usr/bin/env python3
"""tools/xbox/net_audit.py against hand-made captures: a clean one (two
consoles take link-local addresses by the book, beacon, ping each other;
a third gets a DHCP lease from a router) must pass, and for every rule of
docs/lan-plan.md D14 that net_audit checks, a copy with one violation of it
must fail naming that rule. Standard library only."""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
AUDIT = os.path.join(HERE, "net_audit.py")

A_MAC = bytes.fromhex("0050f2640d18")
B_MAC = bytes.fromhex("0250f2640d19")
C_MAC = bytes.fromhex("0050f2112233")
R_MAC = bytes.fromhex("a0b1c2d3e4f5")   # the router
BC = b"\xff" * 6
A_IP, B_IP = bytes([169, 254, 13, 24]), bytes([169, 254, 14, 25])
C_IP, R_IP = bytes([192, 168, 1, 50]), bytes([192, 168, 1, 1])


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(f"!{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def eth(dst, src, et, body):
    return dst + src + struct.pack("!H", et) + body


def arp(src_mac, spa, tpa, op=1, tha=b"\0" * 6):
    return eth(BC, src_mac, 0x0806, struct.pack("!HHBBH", 1, 0x0800, 6, 4, op) + src_mac + spa + tha + tpa)


IP_ID = [0]


def ipv4(src, dst, proto, payload, ttl=1, frag=0x4000, bad_csum=False):
    IP_ID[0] = (IP_ID[0] + 1) & 0xFFFF   # each datagram its own, as lwIP numbers them
    h = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), IP_ID[0], frag, ttl, proto, 0, src, dst)
    c = csum(h) ^ (1 if bad_csum else 0)
    return h[:10] + struct.pack("!H", c) + h[12:] + payload


def udp(src, dst, sport, dport, data, ttl=1, zero_csum=False, **kw):
    u = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
    c = 0 if zero_csum else (csum(src + dst + struct.pack("!BBH", 0, 17, len(u)) + u) or 0xFFFF)
    return ipv4(src, dst, 17, u[:6] + struct.pack("!H", c) + u[8:], ttl=ttl, **kw)


def dhcp_msg(mac, mtype, yiaddr=b"\0" * 4, op=1, opts=b"", chaddr=None):
    b = struct.pack("!BBBBIHH", op, 1, 6, 0, 0x1234, 0, 0x8000) + b"\0" * 4 + yiaddr + b"\0" * 8
    b += (chaddr or mac) + b"\0" * 10 + b"\0" * 192 + b"\x63\x82\x53\x63"
    return b + bytes([53, 1, mtype]) + opts + b"\xff"


def claim(frames, t, mac, ip):
    """RFC 5227 by the book: probes at t, t+1.5, t+3, announces at t+5, t+7. Returns when the address is usable."""
    for k in range(3):
        frames.append((t + 1.5 * k, arp(mac, b"\0" * 4, ip)))
    frames.append((t + 5, arp(mac, ip, ip)))
    frames.append((t + 7, arp(mac, ip, ip)))
    return t + 7.1


def clean():
    f = []
    ta = claim(f, 4.0, A_MAC, A_IP)
    tb = claim(f, 4.3, B_MAC, B_IP)
    # the third console: DHCP with the router, then its ACD check, then use
    f.append((1.0, eth(BC, C_MAC, 0x0800, udp(b"\0" * 4, b"\xff" * 4, 68, 67, dhcp_msg(C_MAC, 1), ttl=255))))
    f.append((1.1, eth(C_MAC, R_MAC, 0x0800, udp(R_IP, C_IP, 67, 68, dhcp_msg(C_MAC, 2, C_IP, op=2), ttl=64))))
    f.append((1.2, eth(BC, C_MAC, 0x0800, udp(b"\0" * 4, b"\xff" * 4, 68, 67, dhcp_msg(C_MAC, 3), ttl=255))))
    f.append((1.3, eth(C_MAC, R_MAC, 0x0800, udp(R_IP, C_IP, 67, 68,
                                                   dhcp_msg(C_MAC, 5, C_IP, op=2, opts=bytes([1, 4, 255, 255, 255, 0])), ttl=64))))
    tc = claim(f, 1.5, C_MAC, C_IP)
    # beacons once a second from each, pings and pongs between A and B at 60 Hz for 5 s
    for k in range(20):
        t = 9.0 + k
        f.append((t, eth(BC, A_MAC, 0x0800, udp(A_IP, b"\xff" * 4, 41001, 41001, b"MXNP1 BEACON 0050f2640d18"))))
        f.append((t + 0.3, eth(BC, B_MAC, 0x0800, udp(B_IP, b"\xff" * 4, 41001, 41001, b"MXNP1 BEACON 0250f2640d19"))))
        f.append((t + 0.6, eth(BC, C_MAC, 0x0800, udp(C_IP, b"\xff" * 4, 41001, 41001, b"MXNP1 BEACON 0050f2112233"))))
    for k in range(300):
        t = 12.0 + k / 60
        f.append((t, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"MXNP1 PING %d 1" % k))))
        f.append((t + 0.001, eth(A_MAC, B_MAC, 0x0800, udp(B_IP, A_IP, 41001, 41001, b"MXNP1 PONG %d 1" % k))))
    # an ICMP echo reply and a TCP reset are fine (rule 11)
    f.append((20.0, eth(R_MAC, C_MAC, 0x0800, ipv4(C_IP, R_IP, 1, icmp(0), ttl=255))))
    f.append((20.1, eth(R_MAC, C_MAC, 0x0800, ipv4(C_IP, R_IP, 6, tcp(0x14), ttl=255))))
    assert ta < 12 and tb < 12 and tc < 9
    return f


def icmp(typ):
    b = struct.pack("!BBHI", typ, 0, 0, 0) + b"ping"
    return b[:2] + struct.pack("!H", csum(b)) + b[4:]


def tcp(flags):
    return struct.pack("!HHIIBBHHH", 80, 1234, 0, 0, 0x50, flags, 0, 0, 0)


def write_pcap(path, frames):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for t, fr in sorted(frames, key=lambda x: x[0]):
            fh.write(struct.pack("<IIII", int(t), int(round((t % 1) * 1e6)) % 1000000, len(fr), len(fr)))
            fh.write(fr)


def violations():
    """rule -> (what, extra frames, extra net_audit arguments)"""
    v = {}
    v[1] = ("an offline console sends", [(30.0, arp(A_MAC, A_IP, A_IP))], ["--silent", "00:50:f2:64:0d:18"])
    v[2] = ("multicast source MAC", [(30.0, eth(BC, bytes.fromhex("0150f2640d18"), 0x0800,
                                                 udp(A_IP, b"\xff" * 4, 41001, 41001, b"x")))], [])
    v[3] = ("an address used without ACD", [(30.0, eth(B_MAC, A_MAC, 0x0800,
                                                         udp(bytes([169, 254, 99, 9]), B_IP, 41001, 41001, b"x")))], [])
    v[4] = ("DHCP with another MAC in chaddr", [(30.0, eth(BC, C_MAC, 0x0800, udp(b"\0" * 4, b"\xff" * 4, 68, 67,
                                                       dhcp_msg(C_MAC, 1, chaddr=bytes(6)), ttl=255)))], [])
    v[5] = ("a link-local address in 169.254.0.0/24", [(30.0, arp(A_MAC, b"\0" * 4, bytes([169, 254, 0, 7])))], [])
    v[6] = ("an IPv6 frame", [(30.0, eth(bytes.fromhex("333300000002"), A_MAC, 0x86DD, b"\x60" + b"\0" * 39))], [])
    v[7] = ("TTL 64 on a game datagram", [(30.0, eth(B_MAC, A_MAC, 0x0800,
                                                       udp(A_IP, B_IP, 41001, 41001, b"x", ttl=64)))], [])
    v[8] = ("a directed broadcast", [(30.0, eth(BC, C_MAC, 0x0800,
                                                  udp(C_IP, bytes([192, 168, 1, 255]), 41001, 41001, b"x")))], [])
    v[9] = ("unicast off the link", [(30.0, eth(R_MAC, C_MAC, 0x0800,
                                                  udp(C_IP, bytes([8, 8, 8, 8]), 41000, 41000, b"x")))], [])
    v[10] = ("400 datagrams in half a second", [(30.0 + k / 800, eth(B_MAC, A_MAC, 0x0800,
                                                                       udp(A_IP, B_IP, 41000, 41000, b"x" * 20)))
                                                 for k in range(400)], [])
    v[11] = ("a TCP SYN", [(30.0, eth(R_MAC, C_MAC, 0x0800, ipv4(C_IP, R_IP, 6, tcp(0x02), ttl=255)))], [])
    # more of each rule's checks, all to be caught
    extra = [
        (3, "probes 0.3 s apart", [(40.0, arp(A_MAC, b"\0" * 4, bytes([169, 254, 50, 1]))),
                                   (40.3, arp(A_MAC, b"\0" * 4, bytes([169, 254, 50, 1])))], []),
        (6, "IGMP", [(30.0, eth(bytes.fromhex("01005e000001"), A_MAC, 0x0800, ipv4(A_IP, bytes([224, 0, 0, 1]), 2, b"\x16\0\0\0" + b"\0" * 4)))], []),
        (7, "a fragment", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"x", frag=0x2000)))], []),
        (7, "a bad UDP checksum", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"x")[:-1] + b"y"))], []),
        (7, "no UDP checksum", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"x", zero_csum=True)))], []),
        (7, "1300 bytes", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41000, 41000, b"x" * 1300)))], []),
        (7, "port 5000", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 5000, 5000, b"x")))], []),
        (7, "a bad IP header checksum", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"x", bad_csum=True)))], []),
        (8, "a broadcast to port 41000", [(30.0, eth(BC, A_MAC, 0x0800, udp(A_IP, b"\xff" * 4, 41000, 41000, b"x")))], []),
        (8, "a 300-byte broadcast", [(30.5, eth(BC, A_MAC, 0x0800, udp(A_IP, b"\xff" * 4, 41001, 41001, b"x" * 300)))], []),
        (8, "three broadcasts in a second", [(30.0 + k * 0.2, eth(BC, A_MAC, 0x0800, udp(A_IP, b"\xff" * 4, 41001, 41001, b"x")))
                                              for k in range(3)], []),
        (9, "to an address never heard from", [(30.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, bytes([169, 254, 77, 7]), 41001, 41001, b"x")))], []),
        (9, "to a peer silent for 10 s", [(40.0, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41001, 41001, b"x")))], []),
        (10, "the governor reached (--normal)", [(30.0 + k / 600, eth(B_MAC, A_MAC, 0x0800, udp(A_IP, B_IP, 41000, 41000, b"x")))
                                                 for k in range(270)], ["--normal"]),
        (11, "an ICMP echo request", [(30.0, eth(R_MAC, C_MAC, 0x0800, ipv4(C_IP, R_IP, 1, icmp(8), ttl=255)))], []),
    ]
    return v, extra


def run(frames, args, tmp, name):
    p = os.path.join(tmp, name + ".pcap")
    write_pcap(p, frames)
    r = subprocess.run([sys.executable, AUDIT, p] + args, capture_output=True, text=True)
    return r.returncode, r.stdout


def main():
    base = clean()
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        code, out = run(base, [], tmp, "clean")
        if code != 0:
            print("the clean capture fails:\n" + out)
            return 1
        print("clean capture: passes")
        code, out = run(base, ["--normal"], tmp, "clean-normal")
        if code != 0:
            print("the clean capture fails with --normal:\n" + out)
            return 1
        v, extra = violations()
        cases = [(r, w, fr, a) for r, (w, fr, a) in sorted(v.items())] + extra
        for i, (rule, what, frames, args) in enumerate(cases):
            # the silent-peer case needs B quiet after 20 s: drop B's frames after that
            fr = base + frames
            if "silent" in what:
                fr = [x for x in fr if not (x[1][6:12] == B_MAC and x[0] > 25)]
            code, out = run(fr, args, tmp, f"case{i}")
            ok = code == 1 and f"rule {rule}:" in out
            print(f"rule {rule:2}: {what}: {'caught' if ok else 'MISSED'}")
            if not ok:
                print(out)
                fails += 1
    if len({r for r, *_ in cases}) < 11:
        print("not every rule has a case")
        fails += 1
    print("net audit ok" if not fails else f"{fails} cases missed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
