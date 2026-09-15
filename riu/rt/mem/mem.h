// riurt — 内存操作模块声明
#ifndef RIURT_MEM_H
#define RIURT_MEM_H

#include "../port/port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- 分配 / 释放 ----

void* riurt_alloc(size_t size);
void* riurt_alloc_zeroed(size_t size);
void* riurt_realloc(void* ptr, size_t new_size);
void riurt_free(void* ptr);

// ---- 内存块操作 ----

// 从 src 复制 size 字节到 dst。源和目标不得重叠。
void* riurt_memcpy(void* dst, const void* src, size_t size);

// 将 dst 的前 size 字节设为 val。
void* riurt_memset(void* dst, int val, size_t size);

// 比较 a 和 b 的前 size 字节。
// 返回 0 表示相等，<0 表示 a<b，>0 表示 a>b。
int riurt_memcmp(const void* a, const void* b, size_t size);

// 从 src 复制 size 字节到 dst。正确处理重叠（src<dst 时从尾向头拷贝）。
void* riurt_memmove(void* dst, const void* src, size_t size);

#ifdef __cplusplus
}
#endif

#endif // RIURT_MEM_H
