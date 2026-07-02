#include "libm.h"

/* yuxrt_sinh(x) = (yuxrt_exp(x) - 1/yuxrt_exp(x))/2
 *         = (yuxrt_exp(x)-1 + (yuxrt_exp(x)-1)/yuxrt_exp(x))/2
 *         = x + x^3/6 + o(x^5)
 */
double yuxrt_sinh(double x)
{
	union {double f; uint64_t i;} u = {.f = x};
	uint32_t w;
	double t, h, absx;

	h = 0.5;
	if (u.i >> 63)
		h = -h;
	/* |x| */
	u.i &= (uint64_t)-1/2;
	absx = u.f;
	w = u.i >> 32;

	/* |x| < yuxrt_log(DBL_MAX) */
	if (w < 0x40862e42) {
		t = yuxrt_expm1(absx);
		if (w < 0x3ff00000) {
			if (w < 0x3ff00000 - (26<<20))
				/* note: inexact and underflow are raised by expm1 */
				/* note: this branch avoids spurious underflow */
				return x;
			return h*(2*t - t*t/(t+1));
		}
		/* note: |x|>yuxrt_log(0x1p26)+eps could be just h*yuxrt_exp(x) */
		return h*(t + t/(t+1));
	}

	/* |x| > yuxrt_log(DBL_MAX) or nan */
	/* note: the result is stored to handle overflow */
	t = __yuxrt_expo2(absx, 2*h);
	return t;
}
