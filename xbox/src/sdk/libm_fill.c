/* libm_fill.c - libm functions nxdk's pdclib only stubs out.
 *
 * nxdk implements these as `assert(0); // Not implemented`, and its assert
 * is `cli; hlt`: the first call freezes the console with a black screen and
 * nothing in the log. Defining them here keeps the linker from pulling the
 * stub members (fma.obj, lround.obj, scalbn.obj) out of libpdclib.
 *
 * Callers: aurora's C_VECSquareMag (fmaf, to round like Gekko's fmadds),
 * melee-pc's widescreen (lroundf), its large-argument trig reduction and
 * rank_exp (scalbn). */
#include <math.h>

/* x*y is exact in double (24 + 24 bits); the sum is rounded to double, then
 * to float. That differs from a truly fused result by one ulp only when the
 * double sum lands exactly halfway between two floats, which is rare enough
 * for gameplay (netplay, which would need bit-exact Gekko rounding, is off). */
float fmaf(float x, float y, float z) { return (float)((double)x * (double)y + (double)z); }

double fma(double x, double y, double z) { return x * y + z; }

/* round half away from zero; x - trunc(x) is exact */
long lroundf(float x) {
    float t = truncf(x);
    if (fabsf(x - t) >= 0.5f) t += x < 0 ? -1.0f : 1.0f;
    return (long)t;
}

long lround(double x) {
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x < 0 ? -1.0 : 1.0;
    return (long)t;
}

double scalbn(double x, int n) { return ldexp(x, n); }
float scalbnf(float x, int n) { return ldexpf(x, n); }
