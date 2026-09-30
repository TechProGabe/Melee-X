/* nv2a_fog.c - GX fog (GXSetFog) on the NV2A.
 *
 * GXSetFog turns (type, start, end, near, far) into BP registers: A and C as
 * floats cut to 11 mantissa bits, and for perspective fog B as a 24-bit
 * magnitude b_mag and a shift b_shift (libogc GX_SetFog). Per pixel the
 * GameCube then takes the 24-bit depth Zs it writes to the EFB and computes
 * (Dolphin's PixelShaderGen WriteFog)
 *     perspective:  ze = A * 2^24 / (b_mag - (Zs >> b_shift))   eye depth / (end - start)
 *     orthographic: ze = A * Zs / 2^24
 *     x = clamp(ze - C, 0, 1)
 *     F = x (LIN), 1 - 2^(-8x) (EXP), 1 - 2^(-8x^2) (EXP2),
 *         2^(-8(1-x)) (REVEXP), 2^(-8(1-x)^2) (REVEXP2)
 * and after the last TEV stage mixes F of the fog colour into the colour,
 * not alpha: rgb = (rgb * (256 - F256) + fog * F256) >> 8, F256 = F * 256.
 *
 * The depth is a projective function of the view-space position P,
 * Zs / 2^24 = (a . P) / (w . P) with a and w the projection's depth and w
 * rows, so ze - C (1 - (ze - C) for the REV types) is a ratio of two linear
 * functions of P, which the vertex program evaluates exactly per vertex.
 * When the fog's near and far are the projection's (HSD passes the camera's)
 * the denominator is a constant and y is linear in P: the rasterizer's
 * perspective-correct interpolation then keeps LIN fog exact across a
 * triangle. The exponential curves are exact at the vertices and
 * interpolated between them, as the NV2A's own EXP modes are (it evaluates
 * fog per vertex). GXSetFogRangeAdj's horizontal correction is not applied. */
#include <string.h>

#include "nv2a_fog.h"

enum { FC_FOG = 3 };   /* final combiner input: FOG (rgb: fog colour, alpha: fog factor) */

/* a float as a GX fog register keeps it: sign, exponent, 11 mantissa bits */
static float gx_float11(float f) {
    union { float f; uint32_t u; } c;
    c.f = f;
    c.u &= 0xFFFFF000u;
    return c.f;
}

static double pow2(int e) {
    double r = 1.0;
    for (; e > 0; e--) r *= 2.0;
    for (; e < 0; e++) r *= 0.5;
    return r;
}

void fog_setup(uint32_t type, float startz, float endz, float nearz, float farz, const float zrow[4],
               const float wrow[4], float zmax, FogSetup* out) {
    int rev = (type & 7) >= 6, i;
    double s = rev ? -1.0 : 1.0, a[4], k;
    memset(out, 0, sizeof *out);
    out->kind = fog_kind(type);
    if (out->kind == VPF_OFF) return;
    out->curve[0] = -8.0f;                  /* F = a + b * 2^(-8 y^n) */
    out->curve[1] = rev ? 0.0f : 1.0f;
    out->curve[2] = rev ? 1.0f : -1.0f;
    /* Zs / 2^24 = (a . P) / (wrow . P): the depth row in depth-buffer units,
     * then GX's 24-bit depth over 2^24 */
    for (i = 0; i < 4; i++) a[i] = zrow[i] * (16777215.0 / 16777216.0) / zmax;
    if (type & 8) {
        /* orthographic: y = s (A Zs/2^24 - C) + rev */
        float A = 0.0f, C = 0.0f;
        if (farz != nearz && endz != startz) {
            float r = 1.0f / (endz - startz);
            A = (farz - nearz) * r;
            C = (startz - nearz) * r;
        }
        A = gx_float11(A);
        C = gx_float11(C);
        k = (rev ? 1.0 : 0.0) - s * C;
        for (i = 0; i < 4; i++) {
            out->num[i] = (float)(s * A * a[i] + k * wrow[i]);
            out->den[i] = wrow[i];
        }
    } else {
        /* perspective: y = s (A 2^24 / (b_mag - Zs / 2^b_shift) - C) + rev
         *              = s Ah / (D - Zs/2^24) + k,  Ah = A 2^b_shift, D = b_mag 2^(b_shift-24) */
        float A, B, C, B_mant;
        int b_shift = 1;
        uint32_t b_mag = 0;
        double Ah, D;
        if (farz == nearz || endz == startz) {
            A = 0.0f;
            B = 0.5f;
            C = 0.0f;
        } else {
            A = (farz * nearz) / ((farz - nearz) * (endz - startz));
            B = farz / (farz - nearz);
            C = startz / (endz - startz);
        }
        B_mant = B;
        while (B_mant > 1.0f) {
            B_mant /= 2.0f;
            b_shift++;
        }
        while (B_mant > 0.0f && B_mant < 0.5f) {
            B_mant *= 2.0f;
            b_shift--;
        }
        if (B_mant > 0.0f) b_mag = (uint32_t)(B_mant * 8388638.0f) & 0xFFFFFFu;
        A = gx_float11((float)(A / pow2(b_shift)));
        C = gx_float11(C);
        Ah = A * pow2(b_shift);
        D = b_mag * pow2(b_shift - 24);
        k = (rev ? 1.0 : 0.0) - s * C;
        if (Ah == 0.0) {
            /* ze = 0 everywhere (GXSetFog's odd case: near == far or start == end) */
            out->num[3] = (float)k;
            out->den[3] = 1.0f;
            return;
        }
        /* y = ((s Ah + k D) (w . P) - k (a . P)) / (D (w . P) - (a . P)) */
        for (i = 0; i < 4; i++) {
            out->num[i] = (float)((s * Ah + k * D) * wrow[i] - k * a[i]);
            out->den[i] = (float)(D * wrow[i] - a[i]);
        }
    }
}

uint32_t fog_final_cw0(uint32_t cw0) {
    /* rgb = A*B + (1-A)*C + D: A = FOG.a (the factor), B = FOG.rgb, C = PREV */
    return (uint32_t)(FC_FOG | 1 << 4) << 24 | (uint32_t)FC_FOG << 16 | (cw0 & 0xFF00u);
}
