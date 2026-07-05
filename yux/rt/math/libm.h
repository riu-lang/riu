// yuxrt — 适配版 musl libm.h（内部数学库头文件）
// 无 CRT 依赖：不包含 <math.h> / <endian.h> / <features.h>。
// 所有内部符号以 __yuxrt_ 为前缀。
#ifndef YUXRT_LIBM_H
#define YUXRT_LIBM_H

#include <float.h>
#include <stdint.h>

#include "math.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 类型定义 ----
typedef double double_t;
typedef float float_t;

// ---- 数学常量（float.h 可能已定义，仅补缺） ----
#ifndef INFINITY
#define INFINITY (__builtin_inf())
#endif
#ifndef NAN
#define NAN (__builtin_nan(""))
#endif
#ifndef DBL_EPSILON
#define DBL_EPSILON 2.2204460492503131e-16
#endif
// Windows x64: long double == double
#ifndef LDBL_EPSILON
#define LDBL_EPSILON DBL_EPSILON
#endif

// ---- 浮点求值方式 (x64 SSE: float→float, double→double) ----
#ifndef FLT_EVAL_METHOD
#define FLT_EVAL_METHOD 0
#endif

// ---- 字节序 (假定 x86/x64 小端) ----
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN

// ---- ELF visibility (Windows 不支持) ----
#define hidden

// ---- 分支预测 ----
#ifdef __GNUC__
#define predict_true(x) __builtin_expect(!!(x), 1)
#define predict_false(x) __builtin_expect(x, 0)
#else
#define predict_true(x) (x)
#define predict_false(x) (x)
#endif

// ---- isnan ----
static inline int __yuxrt_isnan(double x) {
    return x != x;
}
#define isnan(x) __yuxrt_isnan(x)

// ---- 浮点求值辅助（防止 excess precision） ----
static inline float eval_as_float(float x) {
    float y = x;
    return y;
}

static inline double eval_as_double(double x) {
    double y = x;
    return y;
}

// ---- 浮点屏障（阻止编译器优化重排） ----
static inline float fp_barrierf(float x) {
    volatile float y = x;
    return y;
}

static inline double fp_barrier(double x) {
    volatile double y = x;
    return y;
}

static inline void fp_force_evalf(float x) {
    volatile float y;
    y = x;
}

static inline void fp_force_eval(double x) {
    volatile double y;
    y = x;
}

#define FORCE_EVAL(x)                                                                                                  \
    do {                                                                                                               \
        if (sizeof(x) == sizeof(float)) {                                                                              \
            fp_force_evalf(x);                                                                                         \
        } else {                                                                                                       \
            fp_force_eval(x);                                                                                          \
        }                                                                                                              \
    } while (0)

// ---- 类型双关（C99 union 合法） ----
#define asuint(f)                                                                                                      \
    ((union {                                                                                                          \
        float _f;                                                                                                      \
        uint32_t _i;                                                                                                   \
    }){f})                                                                                                             \
        ._i
#define asfloat(i)                                                                                                     \
    ((union {                                                                                                          \
        uint32_t _i;                                                                                                   \
        float _f;                                                                                                      \
    }){i})                                                                                                             \
        ._f
#define asuint64(f)                                                                                                    \
    ((union {                                                                                                          \
        double _f;                                                                                                     \
        uint64_t _i;                                                                                                   \
    }){f})                                                                                                             \
        ._i
#define asdouble(i)                                                                                                    \
    ((union {                                                                                                          \
        uint64_t _i;                                                                                                   \
        double _f;                                                                                                     \
    }){i})                                                                                                             \
        ._f

// ---- 双精度字提取/插入 ----
#define EXTRACT_WORDS(hi, lo, d)                                                                                       \
    do {                                                                                                               \
        uint64_t __u = asuint64(d);                                                                                    \
        (hi) = __u >> 32;                                                                                              \
        (lo) = (uint32_t)__u;                                                                                          \
    } while (0)

#define GET_HIGH_WORD(hi, d)                                                                                           \
    do {                                                                                                               \
        (hi) = asuint64(d) >> 32;                                                                                      \
    } while (0)

#define GET_LOW_WORD(lo, d)                                                                                            \
    do {                                                                                                               \
        (lo) = (uint32_t)asuint64(d);                                                                                  \
    } while (0)

#define INSERT_WORDS(d, hi, lo)                                                                                        \
    do {                                                                                                               \
        (d) = asdouble(((uint64_t)(hi) << 32) | (uint32_t)(lo));                                                       \
    } while (0)

#define SET_HIGH_WORD(d, hi) INSERT_WORDS(d, hi, (uint32_t)asuint64(d))

#define SET_LOW_WORD(d, lo) INSERT_WORDS(d, asuint64(d) >> 32, lo)

#define GET_FLOAT_WORD(w, d)                                                                                           \
    do {                                                                                                               \
        (w) = asuint(d);                                                                                               \
    } while (0)

#define SET_FLOAT_WORD(d, w)                                                                                           \
    do {                                                                                                               \
        (d) = asfloat(w);                                                                                              \
    } while (0)

// ---- 配置 ----
#define WANT_ROUNDING 1
#define WANT_SNAN 0
#define TOINT_INTRINSICS 0
#define EXP_USE_TOINT_NARROW 0
#define issignalingf_inline(x) 0
#define issignaling_inline(x) 0

// ============================================================
// 内部函数声明（均以 __yuxrt_ 为前缀）
// ============================================================

// 三角函数内核
int __yuxrt_rem_pio2_large(double*, double*, int, int, int);
int __yuxrt_rem_pio2(double, double*);
double __yuxrt_sin(double, double, int);
double __yuxrt_cos(double, double);
double __yuxrt_tan(double, double, int);
double __yuxrt_expo2(double, double);

// 错误处理
float __yuxrt_math_xflowf(uint32_t, float);
float __yuxrt_math_uflowf(uint32_t);
float __yuxrt_math_oflowf(uint32_t);
float __yuxrt_math_divzerof(uint32_t);
float __yuxrt_math_invalidf(float);
double __yuxrt_math_xflow(uint32_t, double);
double __yuxrt_math_uflow(uint32_t);
double __yuxrt_math_oflow(uint32_t);
double __yuxrt_math_divzero(uint32_t);
double __yuxrt_math_invalid(double);

// 数据表常量（结构体定义在 *_data.h 中）
#define EXP_TABLE_BITS 7
#define EXP_POLY_ORDER 5
#define EXP2_POLY_ORDER 5
#define LOG_TABLE_BITS 7
#define LOG_POLY_ORDER 6
#define LOG_POLY1_ORDER 12
#define POW_LOG_TABLE_BITS 7
#define POW_LOG_POLY_ORDER 8

// 数据表 extern 声明（定义在 *_data.c 中）
extern const struct yuxrt_exp_data_t __yuxrt_exp_data;
extern const struct yuxrt_log_data_t __yuxrt_log_data;
extern const struct yuxrt_pow_log_data_t __yuxrt_pow_log_data;
extern const uint16_t __yuxrt_rsqrt_tab[128];

#ifdef __cplusplus
}
#endif

#endif // YUXRT_LIBM_H
