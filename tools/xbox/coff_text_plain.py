#!/usr/bin/env python3
"""Rename every `.text$<function>` section of COFF objects to plain `.text`,
in place (docs/fps-plan.md A2).

  tools/xbox/coff_text_plain.py a.obj b.obj @list.txt ...

With -ffunction-sections the mingw triple (the game and sdk objects) names
each function's COMDAT section `.text$<function>`, and lld-link applies
/order only among input sections of the same name: every function would be
its own group, laid out by name. nxdk's own triple names them all `.text`.
Renamed, they are COMDATs in one `.text` group like the platform code's,
and xbox/order.txt lays them out. Only the 8-byte name field of the section
header changes (a long name's string-table entry is left unused). Running
it again does nothing."""
import struct
import sys


def plain_text(path):
    with open(path, 'r+b') as f:
        data = bytearray(f.read())
        machine, nsec, _, symtab, nsyms, opt, _ = struct.unpack_from('<HHIIIHH', data, 0)
        if machine != 0x14C:   # not an i386 COFF object (LLVM bitcode under XBOX_LTO)
            return 0
        strtab = symtab + nsyms * 18
        changed = 0
        for i in range(nsec):
            at = 20 + opt + 40 * i
            raw = bytes(data[at:at + 8])
            if raw.startswith(b'/'):   # long name: /<decimal offset into the string table>
                off = strtab + int(raw[1:].rstrip(b'\0'))
                name = bytes(data[off:data.index(b'\0', off)])
            else:
                name = raw.rstrip(b'\0')
            if name.startswith(b'.text$'):
                data[at:at + 8] = b'.text\0\0\0'
                changed += 1
        if changed:
            f.seek(0)
            f.write(data)
    return changed


def main():
    paths = []
    for a in sys.argv[1:]:
        if a.startswith('@'):
            with open(a[1:]) as f:
                paths += [line.strip() for line in f if line.strip()]
        else:
            paths.append(a)
    total = sum(plain_text(p) for p in paths)
    print(f'coff_text_plain: {len(paths)} objects, {total} sections renamed to .text')


if __name__ == '__main__':
    main()
