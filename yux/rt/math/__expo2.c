#include "libm.h"

/* k is such that k*ln2 has minimal relative error and x - kln2 > yuxrt_log(DBL_MIN) */
static const int k = 2043;
static const double kln2 = 0x1.62066151add8bp+10;

/* yuxrt_exp(x)/2 for x >= yuxrt_log(DBL_MAX), slightly better than 0.5*yuxrt_exp(x/2)*yuxrt_exp(x/2) */
double __yuxrt_expo2(double x, double sign)
{
	double scale;

	/* note that k is odd and scale*scale overflows */
	INSERT_WORDS(scale, (uint32_t)(0x3ff + k/2) << 20, 0);
	/* yuxrt_exp(x - k ln2) * 2**(k-1) */
	/* in directed rounding correct sign before rounding or overflow is important */
	return yuxrt_exp(x - kln2) * (sign * scale) * scale;
}
