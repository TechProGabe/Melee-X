#!/usr/bin/env python3
"""Check a packet capture against the network conduct rules
(docs/lan-plan.md D14) and print every violation with its frame number and
time; exit 1 if there is one. Standard library only.

  tools/xbox/net_audit.py pair.pcap                      # xemu_tap.py's capture of the xemu pair
  tools/xbox/net_audit.py lan.pcapng                     # pktmon etl2pcap's, from the PC on the console's switch
  tools/xbox/net_audit.py off.pcap --silent 00:50:f2:64:0d:18   # rule 1: an offline boot sends nothing
  tools/xbox/net_audit.py run.pcap --normal               # also: the governor never reached

Reads pcap (microsecond or nanosecond) and pcapng (Ethernet link type).
The consoles are the source MACs given with --console, or else every
source MAC whose vendor part is 00:50:f2 (Microsoft, the Xbox NIC) with or
without the locally administered bit (xemu's second EEPROM, eeprom_mac.py).
Only the consoles' frames are judged; what the PC or a router sends is
context (addresses heard, DHCP masks). pktmon sees a packet at several
places in the PC's stack: a frame identical to one less than 5 ms before is
dropped as a copy (--no-dedup keeps them).

What is checked, by D14 rule:
  1  no frame at all from a --silent MAC
  2  the source MAC is unicast
  3  ARP: an address is probed (RFC 5227 §2.1.1: 3 probes, 1-2 s apart),
     then announced (2 s after the last probe, then 2 s apart) before any
     IP datagram comes from it; at most 10 addresses tried a minute
  4  DHCP from a console is well formed: BOOTREQUEST, Ethernet, its own MAC
     in chaddr, the magic cookie, a message type, an end option, broadcast
     while it has no address
  5  a link-local address lies in 169.254.1.0-169.254.254.255
  6  no IPv6 frame, no IGMP, no multicast destination (MAC or IP)
  7  IPv4 header and UDP checksums valid (a UDP checksum of 0 counts as
     missing), no fragment, at most 1200 bytes of UDP payload, TTL 1, the
     same port for source and destination, and only 41000 or 41001 (41050:
     a test build's log stream, MX_LOG_UDP)
  8  broadcast only to 255.255.255.255:41001, under 200 bytes, at most two
     in any second; never a directed broadcast
  9  unicast only to an on-link address that has been heard from, and not
     to one silent for longer than --peer-timeout (default 6.5 s)
 10  per console and source port, in every one-second window: at most
     250 + 32 datagrams and 256 KB + 38400 bytes (the governor's ceiling);
     with --normal, at most 250 and 256 KB (the governor never reached)
 11  from a console only ICMP echo replies and destination unreachables,
     and only TCP resets
Windows are slightly shorter than a second (--slack, 50 ms) for the
capture's own timing. Not checked here (the engine's, phases 1-3): the
announce cadence while in a lobby, no broadcast in a match, answers no
larger than the request before the handshake, silence after a BYE."""
import argparse
import collections
import struct
import sys

CONSOLE_OUI = bytes.fromhex("0050f2")
GAME_PORTS = {41000, 41001}
TEST_PORTS = {41050}
DISCOVERY = 41001
GOV_RATE, GOV_BYTES, GOV_BURST, MAX_SEND = 250, 262144, 32, 1200
PROBE_MIN, PROBE_MAX, PROBE_NUM, ANNOUNCE_WAIT, ANNOUNCE_INTERVAL = 1.0, 2.0, 3, 2.0, 2.0
TIMING_TOL = 0.15   # seconds: capture timing against lwIP's 100 ms ACD timer


# ---- reading captures ----
def read_capture(path):
    """[(time, frame bytes)] of the Ethernet frames, in file order."""
    data = open(path, "rb").read()
    if len(data) < 24:
        sys.exit(f"{path}: too short for a capture")
    magic = data[:4]
    out = []
    if magic in (b"\xd4\xc3\xb2\xa1", b"\xa1\xb2\xc3\xd4", b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d"):
        le = magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1")
        ns = magic in (b"\x4d\x3c\xb2\xa1", b"\xa1\xb2\x3c\x4d")
        e = "<" if le else ">"
        link = struct.unpack_from(e + "I", data, 20)[0]
        if link != 1:
            sys.exit(f"{path}: link type {link}, want Ethernet (1)")
        off = 24
        while off + 16 <= len(data):
            sec, frac, incl, _ = struct.unpack_from(e + "IIII", data, off)
            off += 16
            out.append((sec + frac / (1e9 if ns else 1e6), data[off:off + incl]))
            off += incl
        return out
    if magic == b"\x0a\x0d\x0d\x0a":
        off, e, ifaces = 0, "<", []
        while off + 12 <= len(data):
            if data[off:off + 4] == b"\x0a\x0d\x0d\x0a":
                e = "<" if data[off + 8:off + 12] == b"\x4d\x3c\x2b\x1a" else ">"
                ifaces = []
            btype, blen = struct.unpack_from(e + "II", data, off)
            if blen < 12:
                break
            body = data[off + 8:off + blen - 4]
            if btype == 1:   # interface description: link type, options (if_tsresol)
                link = struct.unpack_from(e + "H", body, 0)[0]
                res, o = 1e-6, 8
                while o + 4 <= len(body):
                    code, ln = struct.unpack_from(e + "HH", body, o)
                    if code == 0:
                        break
                    if code == 9 and ln >= 1:
                        v = body[o + 4]
                        res = 2.0 ** -(v & 0x7F) if v & 0x80 else 10.0 ** -v
                    o += 4 + ((ln + 3) & ~3)
                ifaces.append((link, res))
            elif btype == 6:   # enhanced packet
                iface, hi, lo, cap, _ = struct.unpack_from(e + "IIIII", body, 0)
                link, res = ifaces[iface] if iface < len(ifaces) else (1, 1e-6)
                if link == 1:
                    out.append((((hi << 32) | lo) * res, body[20:20 + cap]))
            elif btype == 3:   # simple packet: no time
                ln = struct.unpack_from(e + "I", body, 0)[0]
                out.append((out[-1][0] if out else 0.0, body[4:4 + ln]))
            off += blen
        return out
    sys.exit(f"{path}: not a pcap or pcapng file")


# ---- helpers ----
def mac_s(b):
    return ":".join(f"{x:02x}" for x in b)


def ip_s(b):
    return ".".join(str(x) for x in b)


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(f"!{len(b) // 2}H", b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return s


def is_console_mac(m, given):
    if given:
        return m in given
    return bytes([m[0] & ~0x03]) + m[1:3] == CONSOLE_OUI   # with the U/L bit (and a broken I/G one, rule 2)


def link_local(ip):
    return ip[0] == 169 and ip[1] == 254


class Audit:
    def __init__(self, a):
        self.a = a
        self.given = {bytes.fromhex(x.replace(":", "").replace("-", "")) for x in (a.console or [])}
        self.silent = {bytes.fromhex(x.replace(":", "").replace("-", "")) for x in (a.silent or [])}
        self.violations = []
        self.by_rule = collections.Counter()
        self.consoles = set()
        self.heard = {}                                   # ip bytes -> last time a frame came from it
        self.mask = {}                                    # console ip -> mask (DHCP ACK option 1)
        self.claims = collections.defaultdict(dict)       # mac -> ip -> {probes: [], announces: []}
        self.tried = collections.defaultdict(list)        # mac -> [(time, ip)] first probe of each address
        self.bcast = collections.defaultdict(collections.deque)
        self.rate = collections.defaultdict(collections.deque)   # (mac, sport) -> deque[(t, bytes)]
        self.rate_bytes = collections.Counter()
        self.frames = 0

    def bad(self, rule, n, t, msg):
        self.by_rule[rule] += 1
        if self.by_rule[rule] <= self.a.max_per_rule:
            self.violations.append(f"frame {n} t={t:.3f}s rule {rule}: {msg}")

    # ---- per frame ----
    def frame(self, n, t, f):
        if len(f) < 14:
            return
        dst, src, et = f[0:6], f[6:12], struct.unpack_from("!H", f, 12)[0]
        body = f[14:]
        if et == 0x8100 and len(f) >= 18:   # an 802.1Q tag
            et, body = struct.unpack_from("!H", f, 16)[0], f[18:]
        if et == 0x0800 and len(body) >= 20:
            self.heard[bytes(body[12:16])] = t
        elif et == 0x0806 and len(body) >= 28:
            if body[14:18] != b"\0\0\0\0":
                self.heard[bytes(body[14:18])] = t
        if src in self.silent:
            self.bad(1, n, t, f"frame from {mac_s(src)}, which must stay silent (an offline boot)")
            return
        if not is_console_mac(src, self.given):
            if et == 0x0800:
                self.watch_dhcp_ack(body)
            return
        self.consoles.add(src)
        if src[0] & 1:
            self.bad(2, n, t, f"multicast source MAC {mac_s(src)}")
        if et == 0x86DD:
            self.bad(6, n, t, "an IPv6 frame")
            return
        if dst[0] & 1 and dst != b"\xff" * 6:
            self.bad(6, n, t, f"to the multicast MAC {mac_s(dst)}")
        if et == 0x0806:
            self.arp(n, t, src, body)
        elif et == 0x0800:
            self.ipv4(n, t, src, dst, body)
        else:
            self.bad(6, n, t, f"EtherType {et:04x}: a console sends IPv4 and ARP only")

    def watch_dhcp_ack(self, ip):
        """A server's DHCP answer: the mask that goes with the address it gives."""
        ihl = (ip[0] & 15) * 4
        if ip[9] != 17 or len(ip) < ihl + 8 + 240:
            return
        sport, dport = struct.unpack_from("!HH", ip, ihl)
        if sport != 67 or dport != 68:
            return
        d = ip[ihl + 8:]
        yiaddr, o, mask, mtype = bytes(d[16:20]), 240, None, None
        while o < len(d) and d[o] != 255:
            if d[o] == 0:
                o += 1
                continue
            if o + 1 >= len(d):
                break
            code, ln = d[o], d[o + 1]
            if code == 1 and ln == 4:
                mask = bytes(d[o + 2:o + 6])
            if code == 53 and ln == 1:
                mtype = d[o + 2]
            o += 2 + ln
        if mtype == 5 and mask:
            self.mask[yiaddr] = mask

    # ---- ARP and ACD (rules 3, 5) ----
    def arp(self, n, t, src, a):
        if len(a) < 28:
            self.bad(3, n, t, "a short ARP packet")
            return
        htype, ptype, hlen, plen, op = struct.unpack_from("!HHBBH", a, 0)
        sha, spa, tpa = bytes(a[8:14]), bytes(a[14:18]), bytes(a[24:28])
        if (htype, ptype, hlen, plen) != (1, 0x0800, 6, 4) or op not in (1, 2):
            self.bad(3, n, t, "a malformed ARP packet")
            return
        if sha != src:
            self.bad(3, n, t, f"ARP sender {mac_s(sha)} is not the frame's source {mac_s(src)}")
        if spa == b"\0\0\0\0":   # a probe for tpa
            c = self.claims[src].setdefault(tpa, {"probes": [], "announces": []})
            if c["probes"] and t - c["probes"][-1] > 10:   # a new attempt at the same address
                c["probes"], c["announces"] = [], []
            if not c["probes"]:
                self.tried[src].append((t, tpa))
                recent = [x for x in self.tried[src] if t - x[0] <= 60]
                if len({x[1] for x in recent}) > 11:
                    self.bad(3, n, t, "more than 10 addresses probed in a minute (RFC 5227 rate limit)")
            if c["probes"]:
                gap = t - c["probes"][-1]
                if not PROBE_MIN - TIMING_TOL <= gap <= PROBE_MAX + TIMING_TOL:
                    self.bad(3, n, t, f"ARP probes for {ip_s(tpa)} {gap:.2f} s apart (RFC 5227: 1-2 s)")
            if c["announces"]:
                self.bad(3, n, t, f"a probe for {ip_s(tpa)} after announcing it")
            c["probes"].append(t)
            if link_local(tpa) and not (1 <= tpa[2] <= 254):
                self.bad(5, n, t, f"link-local {ip_s(tpa)} outside 169.254.1.0-169.254.254.255")
            return
        if spa == tpa:   # an announcement (or a gratuitous ARP) of spa
            c = self.claims[src].setdefault(spa, {"probes": [], "announces": []})
            if link_local(spa) and not (1 <= spa[2] <= 254):
                self.bad(5, n, t, f"link-local {ip_s(spa)} outside 169.254.1.0-169.254.254.255")
            if not c["announces"]:
                if len(c["probes"]) < PROBE_NUM:
                    self.bad(3, n, t, f"{ip_s(spa)} announced after {len(c['probes'])} probes (RFC 5227: {PROBE_NUM})")
                elif t - c["probes"][-1] < ANNOUNCE_WAIT - TIMING_TOL:
                    self.bad(3, n, t, f"{ip_s(spa)} announced {t - c['probes'][-1]:.2f} s after the last probe (2 s)")
            elif len(c["announces"]) == 1 and t - c["announces"][0] < ANNOUNCE_INTERVAL - TIMING_TOL:
                self.bad(3, n, t, f"{ip_s(spa)}'s announcements {t - c['announces'][0]:.2f} s apart (2 s)")
            c["announces"].append(t)
            return
        # an ordinary request or reply: its sender address must be one we announced
        self.check_claimed(n, t, src, spa, "ARP")

    def check_claimed(self, n, t, src, ip, what):
        c = self.claims[src].get(ip)
        if not c or not c["announces"]:
            self.bad(3, n, t, f"{what} from {ip_s(ip)} before it was probed and announced (ACD)")

    # ---- IPv4 (rules 4, 6-11) ----
    def ipv4(self, n, t, src_mac, dst_mac, ip):
        if len(ip) < 20 or ip[0] >> 4 != 4:
            self.bad(7, n, t, "not an IPv4 header")
            return
        ihl = (ip[0] & 15) * 4
        total = struct.unpack_from("!H", ip, 2)[0]
        if ihl < 20 or total < ihl or total > len(ip):
            self.bad(7, n, t, "a malformed IPv4 header")
            return
        ip = ip[:total]
        if csum(bytes(ip[:ihl])) != 0xFFFF:
            self.bad(7, n, t, "IPv4 header checksum wrong")
        flags_frag = struct.unpack_from("!H", ip, 6)[0]
        if flags_frag & 0x3FFF:
            self.bad(7, n, t, "an IP fragment")
        ttl, proto, s, d = ip[8], ip[9], bytes(ip[12:16]), bytes(ip[16:20])
        if d[0] >= 224 and d != b"\xff" * 4:
            self.bad(6, n, t, f"to the multicast address {ip_s(d)}")
        if proto == 2:
            self.bad(6, n, t, "IGMP")
            return
        if s != b"\0\0\0\0":
            self.check_claimed(n, t, src_mac, s, "IP")
            if link_local(s) and not (1 <= s[2] <= 254):
                self.bad(5, n, t, f"link-local source {ip_s(s)} outside 169.254.1.0-169.254.254.255")
        l4 = ip[ihl:]
        if proto == 17:
            self.udp(n, t, src_mac, dst_mac, ip, s, d, ttl, l4)
        elif proto == 1:
            if csum(bytes(l4)) != 0xFFFF:
                self.bad(7, n, t, "ICMP checksum wrong")
            if not l4 or l4[0] not in (0, 3):
                self.bad(11, n, t, f"ICMP type {l4[0] if l4 else '?'} (only echo replies and unreachables)")
        elif proto == 6:
            if len(l4) < 14 or not l4[13] & 0x04:
                self.bad(11, n, t, "a TCP segment that is not a reset (no listener is ever opened)")
        else:
            self.bad(11, n, t, f"IP protocol {proto}")

    def udp(self, n, t, src_mac, dst_mac, ip, s, d, ttl, u):
        if len(u) < 8:
            self.bad(7, n, t, "a short UDP header")
            return
        sport, dport, ulen, uc = struct.unpack_from("!HHHH", u, 0)
        if ulen < 8 or ulen > len(u):
            self.bad(7, n, t, "UDP length wrong")
            return
        payload = ulen - 8
        pseudo = s + d + struct.pack("!BBH", 0, 17, ulen)
        if uc == 0:
            self.bad(7, n, t, "no UDP checksum")
        elif csum(pseudo + bytes(u[:ulen])) != 0xFFFF:
            self.bad(7, n, t, "UDP checksum wrong")
        if sport == 68 and dport == 67:
            self.dhcp(n, t, src_mac, s, d, u[8:ulen])
            return
        bcast = d == b"\xff" * 4
        if dst_mac == b"\xff" * 6 and not bcast:
            self.bad(8, n, t, f"a directed broadcast to {ip_s(d)}")
        if ttl != 1:
            self.bad(7, n, t, f"TTL {ttl} (1)")
        if payload > MAX_SEND:
            self.bad(7, n, t, f"{payload} bytes of UDP payload (1200 at most)")
        if sport != dport:
            self.bad(7, n, t, f"source port {sport} and destination port {dport} differ")
        if sport not in GAME_PORTS | TEST_PORTS:
            self.bad(7, n, t, f"port {sport}: 41000 and 41001 only")
        if bcast:
            if dport != DISCOVERY:
                self.bad(8, n, t, f"a broadcast to port {dport} ({DISCOVERY} only)")
            if payload >= 200:
                self.bad(8, n, t, f"a broadcast of {payload} bytes (under 200)")
            q = self.bcast[src_mac]
            q.append(t)
            while q and q[0] <= t - (1 - self.a.slack):
                q.popleft()
            if len(q) > 2:
                self.bad(8, n, t, f"{len(q)} broadcasts within a second (2 at most)")
        elif dst_mac != b"\xff" * 6:
            mask = self.mask.get(s, b"\xff\xff\0\0" if link_local(s) else b"\xff\xff\xff\0")
            if any((x & m) != (y & m) for x, y, m in zip(d, s, mask)) or (link_local(s) != link_local(d)):
                self.bad(9, n, t, f"to {ip_s(d)}, off the link of {ip_s(s)}/{ip_s(mask)}")
            if dport not in TEST_PORTS:
                last = self.heard.get(d)
                if last is None:
                    self.bad(9, n, t, f"to {ip_s(d)}, which was never heard from")
                elif t - last > self.a.peer_timeout:
                    self.bad(9, n, t, f"to {ip_s(d)}, silent for {t - last:.1f} s")
        q = self.rate[(src_mac, sport)]
        q.append((t, payload))
        key = (src_mac, sport)
        self.rate_bytes[key] += payload
        while q and q[0][0] <= t - (1 - self.a.slack):
            self.rate_bytes[key] -= q.popleft()[1]
        if len(q) > GOV_RATE + GOV_BURST or self.rate_bytes[key] > GOV_BYTES + GOV_BURST * MAX_SEND:
            self.bad(10, n, t, f"port {sport}: {len(q)} datagrams, {self.rate_bytes[key]} bytes in a second "
                               f"(the ceiling is {GOV_RATE + GOV_BURST}, {GOV_BYTES + GOV_BURST * MAX_SEND})")
        elif self.a.normal and (len(q) > GOV_RATE or self.rate_bytes[key] > GOV_BYTES):
            self.bad(10, n, t, f"port {sport}: {len(q)} datagrams, {self.rate_bytes[key]} bytes in a second: "
                               "the governor was reached")

    def dhcp(self, n, t, src_mac, s, d, b):
        if len(b) < 240:
            self.bad(4, n, t, "a short DHCP message")
            return
        op, htype, hlen = b[0], b[1], b[2]
        if (op, htype, hlen) != (1, 1, 6):
            self.bad(4, n, t, f"DHCP op/htype/hlen {op}/{htype}/{hlen} (1/1/6)")
        if bytes(b[28:34]) != src_mac:
            self.bad(4, n, t, f"DHCP chaddr {mac_s(b[28:34])} is not the sender {mac_s(src_mac)}")
        if bytes(b[236:240]) != b"\x63\x82\x53\x63":
            self.bad(4, n, t, "no DHCP magic cookie")
            return
        o, mtype, end = 240, None, False
        while o < len(b):
            if b[o] == 0:
                o += 1
                continue
            if b[o] == 255:
                end = True
                break
            if o + 1 >= len(b) or o + 2 + b[o + 1] > len(b):
                break
            if b[o] == 53 and b[o + 1] == 1:
                mtype = b[o + 2]
            o += 2 + b[o + 1]
        if mtype not in (1, 3, 4, 7, 8):
            self.bad(4, n, t, f"DHCP message type {mtype} from a client")
        if not end:
            self.bad(4, n, t, "DHCP options without an end option")
        if s == b"\0\0\0\0" and d != b"\xff" * 4:
            self.bad(4, n, t, f"DHCP from 0.0.0.0 to {ip_s(d)} (broadcast while it has no address)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture")
    ap.add_argument("--console", action="append", help="a console's MAC (default: every 00:50:f2 source)")
    ap.add_argument("--silent", action="append", help="a MAC that must send nothing (rule 1)")
    ap.add_argument("--normal", action="store_true", help="the governor must never be reached")
    ap.add_argument("--peer-timeout", type=float, default=6.5)
    ap.add_argument("--slack", type=float, default=0.05)
    ap.add_argument("--no-dedup", action="store_true")
    ap.add_argument("--max-per-rule", type=int, default=20)
    a = ap.parse_args()
    frames = read_capture(a.capture)
    au = Audit(a)
    recent = collections.deque()
    dups = 0
    for i, (t, f) in enumerate(frames, 1):
        if not a.no_dedup:
            while recent and recent[0][0] < t - 0.005:
                recent.popleft()
            if any(f == g for _, g in recent):
                dups += 1
                continue
            recent.append((t, f))
        au.frames += 1
        au.frame(i, t - frames[0][0], f)
    for v in au.violations:
        print(v)
    for rule, k in sorted(au.by_rule.items()):
        if k > a.max_per_rule:
            print(f"rule {rule}: {k - a.max_per_rule} more")
    print(f"{a.capture}: {au.frames} frames ({dups} copies dropped), consoles "
          f"{', '.join(mac_s(m) for m in sorted(au.consoles)) or 'none'}: "
          f"{sum(au.by_rule.values())} violations" + (" (" + ", ".join(f"rule {r}: {k}" for r, k in sorted(au.by_rule.items())) + ")" if au.by_rule else ""))
    return 1 if au.by_rule else 0


if __name__ == "__main__":
    sys.exit(main())
