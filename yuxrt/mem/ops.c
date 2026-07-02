// yuxrt — 内存块操作实现（自包含，不调 CRT）
#include "mem.h"

void* yuxrt_memcpy(void* dst, const void* src, size_t size) {
    // 逐字节复制。编译器会对齐优化为 SIMD / rep movsb。
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    size_t i = 0;
    for (; i < size; ++i) {
        d[i] = s[i];
    }
    return dst;
}

void* yuxrt_memset(void* dst, uint8_t val, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    size_t i = 0;
    for (; i < size; ++i) {
        d[i] = val;
    }
    return dst;
}

int yuxrt_memcmp(const void* a, const void* b, size_t size) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    size_t i = 0;
    for (; i < size; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

void* yuxrt_memmove(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        // 正向复制（无重叠风险时与 memcpy 等价）
        size_t i = 0;
        for (; i < size; ++i) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // 反向复制（src 在 dst 之前，从尾向头避免覆盖）
        size_t i = size;
        while (i > 0) {
            --i;
            d[i] = s[i];
        }
    }
    // d == s：无需操作
    return dst;
}
