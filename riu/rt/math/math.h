// riurt — 数学函数模块声明
// 所有函数以 riurt_ 为前缀，避免与 CRT math.h 冲突。
// 移植自 musl-1.2.6 libm。
#ifndef RIURT_MATH_H
#define RIURT_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

// ---- 三角函数 ----
double riurt_sin(double x);
double riurt_cos(double x);
double riurt_tan(double x);

// ---- 反三角函数 ----
double riurt_asin(double x);
double riurt_acos(double x);
double riurt_atan(double x);
double riurt_atan2(double y, double x);

// ---- 双曲函数 ----
double riurt_sinh(double x);
double riurt_cosh(double x);
double riurt_tanh(double x);

// ---- 指数/对数 ----
double riurt_exp(double x);
double riurt_expm1(double x);
double riurt_log(double x);
double riurt_log10(double x);
double riurt_log2(double x);
double riurt_log1p(double x);
double riurt_pow(double x, double y);

// ---- 幂/根 ----
double riurt_sqrt(double x);
double riurt_cbrt(double x);
double riurt_hypot(double x, double y);

// ---- 舍入 ----
double riurt_fabs(double x);
double riurt_floor(double x);
double riurt_ceil(double x);
double riurt_trunc(double x);
double riurt_round(double x);

// ---- 余数 ----
double riurt_fmod(double x, double y);

// ---- 位操作 ----
double riurt_scalbn(double x, int n);

#ifdef __cplusplus
}
#endif

#endif // RIURT_MATH_H
