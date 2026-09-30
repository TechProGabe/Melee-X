/* nv2a_fog.h - GX fog (GXSetFog) on the NV2A (nv2a_fog.c). No pbkit here:
 * tools/xbox/test_fog.py builds nv2a_fog.c on the host. */
#ifndef NV2A_FOG_H
#define NV2A_FOG_H
#include <stdint.h>

#include "nv2a_vp.h"

/* What the back end sends for one GXSetFog state. The vertex program
 * (VpKey.fog = kind, rows VPC_FOG..VPC_FOG+2) computes GX's fog amount F per
 * vertex from P, the view-space position (w = 1):
 *     y = (num . P) / (den . P)                 GX's clamp(ze - C) before the clamp
 *     VPF_LIN:  F = y
 *     VPF_EXP:  F = a + b * 2^(-8 y),   y clamped to 0..1
 *     VPF_EXP2: F = a + b * 2^(-8 y^2), y clamped to 0..1
 * with (-8, a, b, 0) in curve. The NV2A's fog unit runs in LINEAR mode with
 * FOG_PARAMS (1, 1, 0), which makes the fog factor oFog.x itself, clamped to
 * 0..1 per pixel, and the final combiner mixes F of the fog colour into the
 * colour (fog_final_cw0). */
typedef struct {
    uint8_t kind;        /* VPF_*; VPF_OFF: no fog */
    float num[4], den[4];
    float curve[4];
} FogSetup;

/* GXFogType -> VPF_*: bit 3 is orthographic, the low 3 bits the curve (0 off,
 * 2 LIN, 4 EXP, 5 EXP2, 6 REVEXP, 7 REVEXP2; 1 and 3 are linear, as in Dolphin) */
static inline uint8_t fog_kind(uint32_t type) {
    switch (type & 7) {
        case 0: return VPF_OFF;
        case 4: case 6: return VPF_EXP;
        case 5: case 7: return VPF_EXP2;
        default: return VPF_LIN;
    }
}

/* type..farz: GXSetFog's arguments. zrow, wrow: the projection rows the
 * vertex program's depth and w come from (nv2a.c VPC_PROJ + 2 and + 3:
 * depth-buffer units, the viewport's depth range folded in); zmax: the
 * depth buffer's maximum (2^24-1 or 65535). */
void fog_setup(uint32_t type, float startz, float endz, float nearz, float farz, const float zrow[4],
               const float wrow[4], float zmax, FogSetup* out);

/* The final combiner's CW0 with fog: rgb = F * fog colour + (1 - F) * PREV,
 * PREV being where cw0 (A = B = D = 0, C = PREV) read it. Alpha (CW1's G) is
 * not touched. */
uint32_t fog_final_cw0(uint32_t cw0);

/* NV097_SET_FOG_COLOR: ABGR, red in the low byte. Alpha stays 0: the FOG
 * register's alpha is the fog factor, and GX's fog colour has no alpha. */
static inline uint32_t fog_color_abgr(const uint8_t rgba[4]) {
    return (uint32_t)rgba[0] | (uint32_t)rgba[1] << 8 | (uint32_t)rgba[2] << 16;
}

#endif
