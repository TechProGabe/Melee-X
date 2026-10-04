#!/usr/bin/env python3
"""Allow 480p, 720p and widescreen in an Xbox EEPROM image (for xemu).

Usage: eeprom_video.py <eeprom.bin>     # edits the file in place: use a copy

Sets the user section's video flags (offset 0x94: 0x00010000 widescreen,
0x00020000 720p, 0x00080000 480p) and its checksum (0x60, over 0x64-0xBF),
the layout on xboxdevwiki's EEPROM page. With xemu's default HDTV AV pack
the game then runs `env MX_VIDEO=720` at 1280x720 (docs/testing.md
"720p in xemu").
"""
import struct
import sys

VIDEO_FLAGS = 0x94
CHECKSUM = 0x60
WIDESCREEN, MODE_720P, MODE_480P = 0x00010000, 0x00020000, 0x00080000


def section_checksum(data):
    """The EEPROM's checksum: 32-bit words summed with the carries folded in, inverted."""
    high = low = 0
    for i in range(0, len(data), 4):
        v = struct.unpack_from('<I', data, i)[0]
        s = ((high << 32) | low) + v
        high = (s >> 32) & 0xFFFFFFFF
        low = (low + v) & 0xFFFFFFFF
    return ~(high + low) & 0xFFFFFFFF


def main():
    path = sys.argv[1]
    b = bytearray(open(path, 'rb').read())
    if len(b) != 256:
        sys.exit(f'{path}: {len(b)} bytes, not an EEPROM image')
    if section_checksum(b[CHECKSUM + 4:0xC0]) != struct.unpack_from('<I', b, CHECKSUM)[0]:
        sys.exit(f'{path}: user section checksum is wrong already; not touching it')
    old = struct.unpack_from('<I', b, VIDEO_FLAGS)[0]
    new = old | WIDESCREEN | MODE_720P | MODE_480P
    struct.pack_into('<I', b, VIDEO_FLAGS, new)
    struct.pack_into('<I', b, CHECKSUM, section_checksum(b[CHECKSUM + 4:0xC0]))
    open(path, 'wb').write(b)
    print(f'{path}: video flags {old:08x} -> {new:08x}')


if __name__ == '__main__':
    main()
