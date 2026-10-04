#!/usr/bin/env python3
"""A copy of an Xbox EEPROM image with another MAC address, for a second
xemu instance (tools/xbox/xemu_pair.sh): two instances with one MAC break
ARP, AutoIP and the lobby's host election.

  tools/xbox/eeprom_mac.py C:/xemu/eeprom.bin C:/xemu/b/eeprom-b.bin   # locally administered, last byte + 1
  tools/xbox/eeprom_mac.py in.bin out.bin --mac 02:50:f2:64:0d:42 # any unicast MAC
  tools/xbox/eeprom_mac.py in.bin                                       # show MAC, serial, checksums

The factory section (0x30-0x5F) is plain: its checksum at 0x30 covers
0x34-0x5F (serial at 0x34, MAC at 0x40) and is recomputed; nothing else
changes (the HMAC at 0x00 covers only the encrypted section 0x14-0x2F). The
input is checked first: a file whose factory checksum is already wrong is
refused. Never commit an EEPROM image."""
import argparse
import struct
import sys


def checksum(b):
    """The kernel's EEPROM section checksum (xemu's xbox_eeprom_crc)."""
    hi = lo = 0
    for i in range(len(b) // 4):
        v = struct.unpack_from("<I", b, i * 4)[0]
        s = ((hi << 32) | lo) + v
        hi = (s >> 32) & 0xFFFFFFFF
        lo = (lo + v) & 0xFFFFFFFF
    return (~(hi + lo)) & 0xFFFFFFFF


def show(d, what):
    print(f"{what}: MAC {d[0x40:0x46].hex(':')}, serial {d[0x34:0x40].decode('ascii', 'replace')}, "
          f"factory checksum {struct.unpack_from('<I', d, 0x30)[0]:08x} (computed {checksum(d[0x34:0x60]):08x})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--mac", help="aa:bb:cc:dd:ee:ff (default: the source's, locally administered, last byte + 1)")
    a = ap.parse_args()
    d = bytearray(open(a.src, "rb").read())
    if len(d) != 256:
        sys.exit(f"{a.src}: {len(d)} bytes, an EEPROM image has 256")
    show(d, a.src)
    if struct.unpack_from("<I", d, 0x30)[0] != checksum(d[0x34:0x60]):
        sys.exit("the factory checksum is wrong already: not this tool's format, refusing")
    if not a.dst:
        return
    if a.mac:
        mac = bytes(int(x, 16) for x in a.mac.replace("-", ":").split(":"))
        if len(mac) != 6:
            sys.exit("--mac wants six bytes")
    else:
        # D14 rule 2: a locally administered unicast address (IEEE 802: U/L bit set, I/G
        # clear), never another vendor's; the last byte differs too, so lwIP's AutoIP
        # (seeded by the last two bytes) picks another address
        mac = bytes([(d[0x40] | 0x02) & 0xFE]) + bytes(d[0x41:0x45]) + bytes([(d[0x45] + 1) & 0xFF])
    if mac[0] & 1:
        sys.exit("a multicast MAC (low bit of the first byte set) is no NIC's address")
    d[0x40:0x46] = mac
    struct.pack_into("<I", d, 0x30, checksum(d[0x34:0x60]))
    open(a.dst, "wb").write(d)
    show(d, a.dst)


if __name__ == "__main__":
    main()
