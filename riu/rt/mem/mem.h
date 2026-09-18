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

// 不可恢复终止。SEH 码 0xE0AB0710（客户位 0xE + ABORT 谐音），
// 与 assert 0xE0FA17ED / panic 0xE0FA1750 区分。#Test 可接住；普通进程无 handler 则 OS 终止。
// 分配失败 / 越界等内部路径走这里，不走 ExitProcess(1)。
#define RIURT_ABORT_CODE 0xE0AB0710u
void riurt_abort(void);

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
