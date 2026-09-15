#include "libm.h"

double __riurt_math_oflow(uint32_t sign)
{
	return __riurt_math_xflow(sign, 0x1p769);
}
