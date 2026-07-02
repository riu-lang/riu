#include "libm.h"

double __yuxrt_math_uflow(uint32_t sign)
{
	return __yuxrt_math_xflow(sign, 0x1p-767);
}
