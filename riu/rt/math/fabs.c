#include "libm.h"
#include <stdint.h>

double riurt_fabs(double x)
{
	union {double f; uint64_t i;} u = {x};
	u.i &= -1ULL/2;
	return u.f;
}
