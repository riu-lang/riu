// riurt — Windows x64 平台层实现
// 使用 HeapAlloc / HeapFree（kernel32），不依赖 CRT malloc/free。
// 编译时不得链接 msvcrt（riu 程序走自身运行时）。
#include "port.h"
#include "../mem/mem.h"

#if defined(_WIN32) || defined(_WIN64)

// kernel32.dll — 进程堆操作
#define WIN32_LEAN_AND_MEAN
#define NOGDICAPMASKS
#define NOVIRTUALKEYCODES
#define NOWINMESSAGES
#define NOWINSTYLES
#define NOSYSMETRICS
#define NOMENUS
#define NOICONS
#define NOKEYSTATES
#define NOSYSCOMMANDS
#define NORASTEROPS
#define NOSHOWWINDOW
#define OEMRESOURCE
#define NOATOM
#define NOCLIPBOARD
#define NOCOLOR
#define NOCTLMGR
#define NODRAWTEXT
#define NOGDI
#define NOKERNEL
// #define NOUSER            // 注释掉——某些 SDK 版本下缺失该宏会导致编译错误
#define NONLS
#define NOMB
#define NOMEMMGR
#define NOMETAFILE
#define NOMINMAX
// #define NOMSG             // 同上，部分 SDK 版本不认
#define NOOPENFILE
#define NOSCROLL
#define NOSERVICE
#define NOSOUND
#define NOTEXTMETRIC
#define NOWH
#define NOWINOFFSETS
#define NOCOMM
#define NOKANJI
#define NOHELP
#define NOPROFILER
#define NODEFERWINDOWPOS
#define NOMCX

#include <windows.h>

// HEAP_ZERO_MEMORY = 0x00000008
#ifndef HEAP_ZERO_MEMORY
#define HEAP_ZERO_MEMORY 0x00000008
#endif

void riurt_abort(void) {
    // 不依赖 CRT abort。#Test SEH 接住后本函数不会回到调用方；
    // 无 handler 时 OS 终止。若被 continue 执行，再 ExitProcess(3)（传统 abort 退出码）。
    RaiseException((DWORD)RIURT_ABORT_CODE, 0, 0, NULL);
    ExitProcess(3);
}

void* riurt_plat_alloc(size_t size) {
    HANDLE heap = GetProcessHeap();
    void* ptr = HeapAlloc(heap, 0, (SIZE_T)size);
    if (!ptr) {
        riurt_abort();
    }
    return ptr;
}

void* riurt_plat_alloc_zeroed(size_t size) {
    HANDLE heap = GetProcessHeap();
    void* ptr = HeapAlloc(heap, HEAP_ZERO_MEMORY, (SIZE_T)size);
    if (!ptr) {
        riurt_abort();
    }
    return ptr;
}

void* riurt_plat_realloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return riurt_plat_alloc(new_size);
    }
    HANDLE heap = GetProcessHeap();
    void* new_ptr = HeapReAlloc(heap, 0, ptr, (SIZE_T)new_size);
    if (!new_ptr) {
        riurt_abort();
    }
    return new_ptr;
}

void riurt_plat_free(void* ptr) {
    if (ptr) {
        HANDLE heap = GetProcessHeap();
        HeapFree(heap, 0, ptr);
    }
}

#else
// 非 Windows 平台：暂未实现
#error "riurt: unsupported platform (only Windows x64 is currently supported)"
#endif
