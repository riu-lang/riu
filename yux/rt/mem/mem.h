// yuxrt — 内存操作模块声明
#ifndef YUXRT_MEM_H
#define YUXRT_MEM_H

#include "../port/port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 分配 / 释放 ----

void* yuxrt_alloc(size_t size);
void* yuxrt_alloc_zeroed(size_t size);
void* yuxrt_realloc(void* ptr, size_t new_size);
void yuxrt_free(void* ptr);

// ---- 内存块操作 ----

// 从 src 复制 size 字节到 dst。源和目标不得重叠。
void* yuxrt_memcpy(void* dst, const void* src, size_t size);

// 将 dst 的前 size 字节设为 val。
void* yuxrt_memset(void* dst, int val, size_t size);

// 比较 a 和 b 的前 size 字节。
// 返回 0 表示相等，<0 表示 a<b，>0 表示 a>b。
int yuxrt_memcmp(const void* a, const void* b, size_t size);

// 从 src 复制 size 字节到 dst。正确处理重叠（src<dst 时从尾向头拷贝）。
void* yuxrt_memmove(void* dst, const void* src, size_t size);

#ifdef __cplusplus
}
#endif

#endif // YUXRT_MEM_H
