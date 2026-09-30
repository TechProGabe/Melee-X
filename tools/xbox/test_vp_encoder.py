#!/usr/bin/env python3
"""Check xbox/src/hw/nv2a_vp.c's instruction encoder against nv2a-vsh.

The vertex programs are generated at runtime on the Xbox, where nothing can
compare them to a reference. Here, nv2a_vp.c is built for the host with
VP_HOST_TEST, which emits a fixed program covering every opcode, operand
slot, relative addressing, negation, swizzles and output kinds; nv2a-vsh
(pip install nv2a-vsh) assembles the same program from source, and the
machine words must match exactly.
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

SOURCE = """
arl a0, v1.x
dp4 r0.x, v0, c[A0+96]
dp4 oPos, r3, c4
mov r5.xyz, -r2.zyxw
mul r1.xyz, r1, r3.x
add r4.xyz, c70, -r0
mad r6, r5.z, c72, r6
rsq r5.y, r5.x
rcp r3.x, r2.w
max r5.z, r5, c4.x
min oD0, r6, c4.y
sge r5.w, r5.z, c4.x
dp3 oT1.x, v10, c111
mov oT2.zw, c4.xxxy
rcp oFog.x, r11.y
expp r3.z, r3.y
mad oFog.x, r3.z, c136.z, c136.y
"""

HARNESS = r"""
#include <stdio.h>
#include "nv2a_vp.h"
void vp_test_program(VpProgram* p);
int main(void) {
    static VpProgram p;
    unsigned i;
    vp_test_program(&p);
    p.words[(p.n - 1) * 4 + 3] |= 1u;
    for (i = 0; i < p.n * 4; i++) printf("0x%08x\n", p.words[i]);
    return 0;
}
"""


def main():
    from nv2a_vsh.assemble import assemble
    expected, errors = assemble(SOURCE)
    if errors:
        raise SystemExit(f'nv2a-vsh rejected the reference source: {errors}')
    expected = [w for insn in expected for w in insn]
    with tempfile.TemporaryDirectory() as tmp:
        exe = pathlib.Path(tmp) / 'vp_test'
        harness = pathlib.Path(tmp) / 'harness.c'
        harness.write_text(HARNESS)
        subprocess.run(['cc', '-O1', '-DVP_HOST_TEST', f'-I{ROOT}/xbox/src/hw', str(harness),
                        str(ROOT / 'xbox/src/hw/nv2a_vp.c'), '-o', str(exe)], check=True)
        actual = [int(line, 16) for line in subprocess.run([str(exe)], check=True, capture_output=True,
                                                            text=True).stdout.split()]
    if actual == expected:
        print(f'{len(actual) // 4} instructions match nv2a-vsh')
        return
    lines = SOURCE.strip().splitlines()
    for i in range(0, max(len(actual), len(expected)), 4):
        a, e = actual[i:i + 4], expected[i:i + 4]
        if a != e:
            print(f'{lines[i // 4] if i // 4 < len(lines) else "?"}\n  ours {[hex(x) for x in a]}\n  ref  {[hex(x) for x in e]}')
    sys.exit(1)


if __name__ == '__main__':
    main()
