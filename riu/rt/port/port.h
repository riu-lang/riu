// riurt — 平台抽象层接口
// 每个平台的实现（win32.c / posix.c / ...）提供以下函数的具体实现。
// 当前仅 Windows x64（win32.c）。
#ifndef RIURT_PORT_H
#define RIURT_PORT_H

#include <stddef.h> // size_t
#include <stdint.h> // uint8_t, uint64_t

#ifdef __cplusplus
extern "C" {
#endif

// ---- 内存分配（平台层实现）----

// 分配 size 字节未初始化内存。失败时 abort。
void* riurt_plat_alloc(size_t size);

// 分配 size 字节零填充内存。失败时 abort。
void* riurt_plat_alloc_zeroed(size_t size);

// 重新分配内存块。失败时 abort。
void* riurt_plat_realloc(void* ptr, size_t new_size);

// 释放内存块。ptr 为 NULL 时无操作。
void riurt_plat_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif // RIURT_PORT_H
