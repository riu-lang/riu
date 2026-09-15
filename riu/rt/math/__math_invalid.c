#include "libm.h"

double __riurt_math_invalid(double x)
{
	return (x - x) / (x - x);
}
