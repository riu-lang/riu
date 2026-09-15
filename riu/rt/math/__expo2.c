#include "libm.h"

/* k is such that k*ln2 has minimal relative error and x - kln2 > riurt_log(DBL_MIN) */
static const int k = 2043;
static const double kln2 = 0x1.62066151add8bp+10;

/* riurt_exp(x)/2 for x >= riurt_log(DBL_MAX), slightly better than 0.5*riurt_exp(x/2)*riurt_exp(x/2) */
double __riurt_expo2(double x, double sign)
{
	double scale;

	/* note that k is odd and scale*scale overflows */
	INSERT_WORDS(scale, (uint32_t)(0x3ff + k/2) << 20, 0);
	/* riurt_exp(x - k ln2) * 2**(k-1) */
	/* in directed rounding correct sign before rounding or overflow is important */
	return riurt_exp(x - kln2) * (sign * scale) * scale;
}
