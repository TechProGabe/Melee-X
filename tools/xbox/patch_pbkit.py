#!/usr/bin/env python3
"""Patch nxdk's pbkit.c so a GPU fault can't take down the whole console.

Usage: patch_pbkit.py <nxdk>/lib/pbkit/pbkit.c <out>/pbkit.c

Stock pbkit handles a PGRAPH error inside its DPC by switching to the debug
screen and looping on Sleep() forever at DISPATCH_LEVEL; several of its
interrupt-time waits spin on GPU registers with no bound. Either one freezes
the machine with every thread stopped, so the watchdog can't write hang.log.
Here:
  - PGRAPH errors and DMA pusher errors are reported to
    ocx_pb_gpu_fault() (xbox/src/hw/nv2a.c records them; logged at passive level),
    acknowledged, and the GPU carries on (the faulting method is dropped,
    the way nouveau handles it);
  - interrupt-time register waits give up after OCX_SPIN_MAX polls;
  - the DPC loop runs at most 64 rounds, and once ocx_pb_irq_off is set
    (an interrupt storm, decided in xbox/src/hw/nv2a.c) it stops re-enabling the
    GPU interrupt so threads can run and the watchdog can report;
  - the depth format can be set before pb_init (pb_DepthFmt no longer static,
    Z16 sized and scaled): 720p pairs its 16-bit colour buffer with Z24S8
    since v53, Z16 with -DXGX_Z24_16BPP=0 (xbox/src/hw/nv2a.c); the Z16
    depth tile's flags can be set with -DOCX_Z16_TILE_FLAGS (default
    pbkit's 0x84000001);
  - ocx_pb_layout() reports the pushbuffer, framebuffers and depth buffer
    addresses for the layout line at boot;
  - ocx_pb_retarget_back_buffer() points rendering back at the current back
    buffer after an EFB copy drew elsewhere through another DMA object. DMA
    object 9 still describes the back buffer, so only the surface state is
    pushed; pb_target_back_buffer() rewrites object 9 through four
    GPU->CPU interrupts (PB_SETOUTER) every time. With
    ocx_pb_retarget_repitch set (nv2a.c) the pitch and offsets go again
    after the format.
Every replacement must match exactly once, or the build fails.
"""
import re
import sys

src_path, out_path = sys.argv[1], sys.argv[2]
src = open(src_path, encoding="utf-8").read()


def sub(pattern, repl, flags=0, count=1):
    global src
    found = re.findall(pattern, src, flags)
    if len(found) != count:
        sys.exit(f"patch_pbkit: expected {count} match(es) for {pattern!r}, found {len(found)}")
    src = re.sub(pattern, lambda m: repl, src, flags=flags)


# declarations, after the local includes
sub(r'(#include "nv20_shader\.h"[^\n]*\n)',
    '#include "nv20_shader.h" //(search "nouveau" on wiki)\n'
    "\n/* Melee-X (from OpenCrossing-Xbox): tools/xbox/patch_pbkit.py */\n"
    "void ocx_pb_gpu_fault(unsigned kind, unsigned a, unsigned b, unsigned c, unsigned d);\n"
    "extern volatile int ocx_pb_irq_off;\n"
    "#define OCX_SPIN_MAX 4000000u\n"
    "static int ocx_spin(unsigned *n, unsigned kind)\n"
    "{\n"
    "    if (++*n < OCX_SPIN_MAX) return 1;\n"
    "    ocx_pb_gpu_fault(kind, VIDEOREG(NV_PGRAPH_STATUS), VIDEOREG(NV_PMC_INTR_0), 0, 0);\n"
    "    return 0;\n"
    "}\n")

# vblank acknowledge loop
sub(r'do\s*\{\s*VIDEOREG\(PCRTC_INTR\)=PCRTC_INTR_VBLANK_RESET;\s*\}while\(VIDEOREG\(NV_PMC_INTR_0\)&NV_PMC_INTR_0_PCRTC_PENDING\);',
    "{ unsigned ocx_n = 0;\n"
    "    do\n    {\n        VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;\n"
    "    }while((VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING) && ocx_spin(&ocx_n, 10)); }")

# PGRAPH idle waits inside the interrupt handler
sub(r'while\(VIDEOREG\(NV_PGRAPH_STATUS\)\);',
    "{ unsigned ocx_n = 0; while(VIDEOREG(NV_PGRAPH_STATUS) && ocx_spin(&ocx_n, 11)); }")
sub(r'while ?\(VIDEOREG\(NV_PGRAPH_STATUS\)\) \{\};',
    "{ unsigned ocx_n = 0; while(VIDEOREG(NV_PGRAPH_STATUS) && ocx_spin(&ocx_n, 12)) {}; }", count=2)
sub(r'while\(VIDEOREG\(NV_PGRAPH_STATUS\)!=NV_PGRAPH_STATUS_NOT_BUSY\)',
    "unsigned ocx_n = 0;\n"
    "    while(VIDEOREG(NV_PGRAPH_STATUS)!=NV_PGRAPH_STATUS_NOT_BUSY && ocx_spin(&ocx_n, 13))")
sub(r'while\(VIDEOREG8\(NV_PFIFO_CACHES\)&NV_PFIFO_CACHES_DMA_SUSPEND_BUSY\);',
    "{ unsigned ocx_n = 0; while((VIDEOREG8(NV_PFIFO_CACHES)&NV_PFIFO_CACHES_DMA_SUSPEND_BUSY) && ocx_spin(&ocx_n, 14)); }")

# PGRAPH error: record + acknowledge instead of "System halted" (Sleep at DPC level)
sub(r'pb_show_debug_screen\(\);\s*debugPrint\("\\n"\);\s*if \(nsource&NV_PGRAPH_NSOURCE_DATA_ERROR_PENDING\).*?while\(1\) \{\s*Sleep\(2000\);\s*\};',
    "ocx_pb_gpu_fault(1, nsource, GrClass, trapped_address, DataLow);\n"
    "                            VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_NOTIFY_RESET|\n"
    "                                            NV_PGRAPH_INTR_ERROR_RESET|\n"
    "                                            NV_PGRAPH_INTR_SINGLE_STEP_RESET|\n"
    "                                            NV_PGRAPH_INTR_MORE_RESET;",
    flags=re.S)

# DMA pusher error: record instead of switching the screen to the debug console for good
sub(r'pb_show_debug_screen\(\);\s*debugPrint\("Software Put=%08lx\\n",\(DWORD\)pb_Put\);.*?debugPrint\("Dma push buffer engine encountered invalid data at these addresses\.\\n"\);',
    "ocx_pb_gpu_fault(2, (DWORD)pb_Put, VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT), VIDEOREG(NV_PFIFO_CACHE1_DMA_GET), 0);",
    flags=re.S)

# DPC: bounded rounds; leave the interrupt masked after a storm
sub(r'DWORD           status;\n\n    do\n    \{\n        more=0;',
    "DWORD           status;\n    unsigned        ocx_rounds=0;\n\n    do\n    {\n        more=0;")
sub(r'\}while\(more\);\n\n    VIDEOREG\(NV_PMC_INTR_EN_0\)=NV_PMC_INTR_EN_0_INTA_HARDWARE;',
    "}while(more && ++ocx_rounds < 64);\n"
    "\n    if (more) ocx_pb_gpu_fault(3, VIDEOREG(NV_PMC_INTR_0), VIDEOREG(NV_PGRAPH_INTR), VIDEOREG(NV_PFIFO_INTR_0), 0);\n"
    "    if (!ocx_pb_irq_off) VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_HARDWARE;")

# settable depth format (Z16 for the 16-bit 720p mode)
sub(r'static unsigned int pb_DepthFmt = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;',
    "unsigned int pb_DepthFmt = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;")
sub(r'int DepthBpp = 32;\n    assert\(pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8\);\n    pb_ZScale = \(float\)0xFFFFFF;',
    "int DepthBpp = pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? 16 : 32;\n"
    "    assert(pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 || pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16);\n"
    "    pb_ZScale = pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? (float)0xFFFF : (float)0xFFFFFF;")

# The depth buffer's tile (1) always gets 0x84000001: compression tags on
# (bit 31) with the 32-bit flag (bit 26), also for Z16 at 720p. Behind a
# switch until a console A/B says which is right: -DOCX_Z16_TILE_FLAGS=
# 0x80000001 (tags, no 32-bit flag) or 0x00000001 (uncompressed) for Z16.
sub(r'\nvoid pb_assign_tile\(',
    "\n#ifndef OCX_Z16_TILE_FLAGS\n#define OCX_Z16_TILE_FLAGS 0x84000001u   /* Melee-X: Z16 depth tile, stock */\n#endif\n\n"
    "void pb_assign_tile(")
sub(r'0x84000001          //DWORD tile_flags \(0x04000000 for 32 bits\)',
    "(pb_DepthFmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16 ? OCX_Z16_TILE_FLAGS : 0x84000001) "
    "//DWORD tile_flags (0x04000000 for 32 bits)")

src += """

/* Melee-X (tools/xbox/patch_pbkit.py): back to the current back buffer
 * without rewriting DMA object 9, which still points at it. */
extern int ocx_pb_retarget_repitch;
void ocx_pb_retarget_back_buffer(void)
{
    uint32_t *p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT3,9);
    p=pb_push3(p,NV20_TCL_PRIMITIVE_3D_BUFFER_PITCH,(pb_DepthStencilPitch<<16)|(pb_FrameBuffersPitch&0xFFFF),0,0);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_HORIZ,pb_FrameBuffersWidth<<16,pb_FrameBuffersHeight<<16);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BUFFER_FORMAT,pb_GPUFrameBuffersFormat|pb_FBVFlag);
    /* the pitch above went in while the copy's swizzled surface was still
     * set: send it again now the format is linear (nv2a.c, XGX_COPY_FIX) */
    if (ocx_pb_retarget_repitch)
        p=pb_push3(p,NV20_TCL_PRIMITIVE_3D_BUFFER_PITCH,(pb_DepthStencilPitch<<16)|(pb_FrameBuffersPitch&0xFFFF),0,0);
    pb_end(p);
}
"""

src += """

/* Melee-X (tools/xbox/patch_pbkit.py): where pbkit's buffers are, for the
 * [NV2A] layout line (a GPU write past a buffer lands in its neighbour). */
void ocx_pb_layout(unsigned *out)
{
    out[0]=(unsigned)pb_Head;
    out[1]=pb_Size;
    out[2]=pb_FBAddr[0];
    out[3]=pb_FBAddr[1];
    out[4]=pb_FBAddr[2];
    out[5]=pb_FBSize;
    out[6]=pb_DSAddr;
    out[7]=pb_DSSize;
}
"""

open(out_path, "w", encoding="utf-8").write(src)
