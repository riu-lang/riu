// yuxrt — 数学函数模块声明
// 所有函数以 yuxrt_ 为前缀，避免与 CRT math.h 冲突。
// 移植自 musl-1.2.6 libm。
#ifndef YUXRT_MATH_H
#define YUXRT_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

// ---- 三角函数 ----
double yuxrt_sin(double x);
double yuxrt_cos(double x);
double yuxrt_tan(double x);

// ---- 反三角函数 ----
double yuxrt_asin(double x);
double yuxrt_acos(double x);
double yuxrt_atan(double x);
double yuxrt_atan2(double y, double x);

// ---- 双曲函数 ----
double yuxrt_sinh(double x);
double yuxrt_cosh(double x);
double yuxrt_tanh(double x);

// ---- 指数/对数 ----
double yuxrt_exp(double x);
double yuxrt_expm1(double x);
double yuxrt_log(double x);
double yuxrt_log10(double x);
double yuxrt_log2(double x);
double yuxrt_log1p(double x);
double yuxrt_pow(double x, double y);

// ---- 幂/根 ----
double yuxrt_sqrt(double x);
double yuxrt_cbrt(double x);
double yuxrt_hypot(double x, double y);

// ---- 舍入 ----
double yuxrt_fabs(double x);
double yuxrt_floor(double x);
double yuxrt_ceil(double x);
double yuxrt_trunc(double x);
double yuxrt_round(double x);

// ---- 余数 ----
double yuxrt_fmod(double x, double y);

// ---- 位操作 ----
double yuxrt_scalbn(double x, int n);

#ifdef __cplusplus
}
#endif

#endif // YUXRT_MATH_H
