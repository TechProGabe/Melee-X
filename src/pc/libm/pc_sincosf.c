/* PORT: Xbox. Not from musl: pc_sinf and pc_cosf (pc_sinf.c, pc_cosf.c) of
 * one argument in one call, for HSD_MtxSRT (three angles, each wanting both).
 * Every branch below is the matching branch of pc_sinf and of pc_cosf, with
 * the same double arguments to the same out-of-line kernels, so both results
 * are bit-identical to the separate calls whatever the x87 precision; only
 * the classification, the float -> double conversion and (for |x| > 9pi/4)
 * pc_rem_pio2f are shared. tests/xbox/test_anim_mtx.c compares it with
 * pc_sinf/pc_cosf. */

#include "pc_libm.h"

/* Small multiples of pi/2 rounded to double precision (pc_sinf.c, pc_cosf.c). */
static const double
s1pio2 = 1*M_PI_2, /* 0x3FF921FB, 0x54442D18 */
s2pio2 = 2*M_PI_2, /* 0x400921FB, 0x54442D18 */
s3pio2 = 3*M_PI_2, /* 0x4012D97C, 0x7F3321D2 */
s4pio2 = 4*M_PI_2; /* 0x401921FB, 0x54442D18 */

void pc_sincosf(float x, float* sinp, float* cosp)
{
	double y;
	uint32_t ix;
	unsigned n, sign;

	GET_FLOAT_WORD(ix, x);
	sign = ix >> 31;
	ix &= 0x7fffffff;

	if (ix <= 0x3f490fda) {  /* |x| ~<= pi/4 */
		if (ix < 0x39800000) {  /* |x| < 2**-12 */
			/* raise inexact if x!=0 and underflow if subnormal */
			FORCE_EVAL(ix < 0x00800000 ? x/0x1p120f : x+0x1p120f);
			*sinp = x;
			*cosp = 1.0f;
			return;
		}
		*sinp = pc_sindf(x);
		*cosp = pc_cosdf(x);
		return;
	}
	if (ix <= 0x407b53d1) {  /* |x| ~<= 5*pi/4 */
		if (ix <= 0x4016cbe3) {  /* |x| ~<= 3pi/4 */
			if (sign) {
				*sinp = -pc_cosdf(x + s1pio2);
				*cosp = pc_sindf(x + s1pio2);
			} else {
				*sinp = pc_cosdf(x - s1pio2);
				*cosp = pc_sindf(s1pio2 - x);
			}
			return;
		}
		*sinp = pc_sindf(sign ? -(x + s2pio2) : -(x - s2pio2));
		*cosp = -pc_cosdf(sign ? x+s2pio2 : x-s2pio2);
		return;
	}
	if (ix <= 0x40e231d5) {  /* |x| ~<= 9*pi/4 */
		if (ix <= 0x40afeddf) {  /* |x| ~<= 7*pi/4 */
			if (sign) {
				*sinp = pc_cosdf(x + s3pio2);
				*cosp = pc_sindf(-x - s3pio2);
			} else {
				*sinp = -pc_cosdf(x - s3pio2);
				*cosp = pc_sindf(x - s3pio2);
			}
			return;
		}
		*sinp = pc_sindf(sign ? x + s4pio2 : x - s4pio2);
		*cosp = pc_cosdf(sign ? x+s4pio2 : x-s4pio2);
		return;
	}

	/* sin and cos of Inf or NaN are NaN */
	if (ix >= 0x7f800000) {
		*sinp = x - x;
		*cosp = x - x;
		return;
	}

	/* general argument reduction needed */
	n = pc_rem_pio2f(x, &y);
	switch (n&3) {
	case 0:
		*sinp = pc_sindf(y);
		*cosp = pc_cosdf(y);
		return;
	case 1:
		*sinp = pc_cosdf(y);
		*cosp = pc_sindf(-y);
		return;
	case 2:
		*sinp = pc_sindf(-y);
		*cosp = -pc_cosdf(y);
		return;
	default:
		*sinp = -pc_cosdf(y);
		*cosp = pc_sindf(y);
		return;
	}
}
