// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 运行时辅助函数声明
// 
// 本文件声明编译器生成的运行时辅助函数:
// - 内存分配/释放函数
// - Box<T> 智能指针支持函数
// - Array<T> 动态数组支持函数
// - 程序启动函数 (main -> yux_main)
// 
// 这些函数由编译器在编译 SDK (core.yux) 时生成，
// 并链接到每个 yux 程序中。

#ifndef YUX_LANG_COMPILER_RUNTIME_H
#define YUX_LANG_COMPILER_RUNTIME_H

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include "types.h"

class Compiler;

namespace runtime {

// ==================== Windows API 辅助 ====================

llvm::Function* getOrCreateWindowsAPI(
    llvm::Module* module,
    llvm::IRBuilder<>& builder,
    const string& name);

llvm::Function* getProcessHeapFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getHeapAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getHeapReAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getHeapFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getSetConsoleOutputCPFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getSetConsoleCPFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== Box<T> 智能指针支持 ====================

llvm::Function* getBoxAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getBoxRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder);
// _box_release(handle) -> void
// Phase 1d.1：strong--；归零时 weak--，weak 也归零时 free 整个 block；null/哨兵跳过
// payload 析构仍由调用方在 IR 内联（在 _box_release 之前），因此外部 Weak 在 strong=0 后
// upgrade 必须读 strong 来判活而非 payload；Weak 自身仅维护 block 存活
llvm::Function* getBoxReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _box_upgrade(handle) -> handle_or_null
// Phase 1d.2：Weak→Box 升级；null/strong==0 → null；哨兵 → handle；其他 strong++ 返回 handle
llvm::Function* getBoxUpgradeFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _weak_release(handle) -> void
// weak--；归零时 free 整个 block（前提：strong 已 0，否则 weak 不可能比 strong 先归零）
// null/哨兵跳过
llvm::Function* getWeakReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== Array<T> 动态数组支持（Phase 1b）====================
// Block 布局: { u32 strong, u32 weak, i64 len, i64 cap, ptr data }；data 是间接指针
// handle == null 表示空数组；强引用归零时 free(data) + free(block)；哨兵 0xFFFFFFFF 跳过 RC

// _array_alloc(elemSize, initCap, initLen) -> Block*
//   分配 block + (initCap > 0 ? data 缓冲)；strong=1, weak=1；len=initLen；data 由调用方填充
llvm::Function* getArrayAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
// _array_grow(handle, elemSize, newCap) -> void
//   原地修改 block.cap、block.data；外部 handle 不动
llvm::Function* getArrayGrowFn(llvm::Module* module, llvm::IRBuilder<>& builder);
// _array_release(handle) -> void
//   strong--；归零时 free(data) + free(block)；null/哨兵跳过
llvm::Function* getArrayReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);
// _array_retain(handle) -> void
//   strong++；null/哨兵跳过
llvm::Function* getArrayRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== 运行时辅助函数生成 ====================

void emitBoxHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitWeakHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitArrayHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitMainStartup(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitRuntimeHelpers(llvm::IRBuilder<>& builder, llvm::Module* module);

}

#endif //YUX_LANG_COMPILER_RUNTIME_H
