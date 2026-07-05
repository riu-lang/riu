#include "libm.h"

double __yuxrt_math_invalid(double x)
{
	return (x - x) / (x - x);
}
