// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 运行时辅助函数声明
//
// 本文件声明编译器生成的运行时辅助函数:
// - 内存分配/释放函数
// - Rc<T> 智能指针支持函数
// - Array<T> 动态数组支持函数
// - 程序启动函数 (main -> riu_main)
//
// 这些函数由编译器在编译 SDK (core.ut) 时生成，
// 并链接到每个 riu 程序中。

#ifndef RIU_LANG_COMPILER_RUNTIME_H
#define RIU_LANG_COMPILER_RUNTIME_H

#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>

#include "types.h"
#include <llvm/IR/Module.h>
#include <string>
#include <vector>

class Compiler;

namespace runtime {

// ==================== Windows API 辅助（I/O 相关，暂保留） ====================

llvm::Function* getOrCreateWindowsAPI(llvm::Module* module, llvm::IRBuilder<>& builder, const string& name);

llvm::Function* getSetConsoleOutputCPFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getSetConsoleCPFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== riurt C 运行时函数声明 ====================
// 替代直接 emit Win32 HeapAlloc/HeapFree IR 的方案。
// riurt 是纯 C99 静态库（riurt.lib），由编译器链接到每个 riu 程序。

llvm::Function* getRiurtAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getRiurtReallocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getRiurtFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// riurt ffi：Rc 计数协议（riu_rc_*）。alloc/retain/release 不再 emit IR 体。
llvm::Function* getRiuRcReleaseNeedDtorFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getRiuRcDropBlockFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// 全局 rc_block_count 增减（Phase 8a leak 检测）
// 定义在 riurt（riu_rc.c）；IR 只 extern。delta: +1（alloc）或 -1（free）
void emitRcBlockCountAdd(llvm::IRBuilder<>& builder, llvm::Module* module, int64_t delta);

// 生成 _box_release_<T> 的函数体（Phase B-2）
// func 必须为空（刚声明）；dtorFn 为 T 的析构函数（可为 null，表示 T 平凡）
void emitRcReleaseTypedFn(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                          llvm::Function* func, llvm::Function* dtorFn);

// B-2 inline-dtor: Rc<T> 其中 T 为 Rc/Weak/fn 等无独立 dtor 函数的内联析构类型。
// kind 决定 payload 布局；payloadReleaseFn 为 strong==0 时对内层 handle 调用的释放函数：
//   TypeKind::Rc   → payload[0] 内层 handle → 内层 Rc 的 typed release（可递归，Rc<Rc<Rc<T>>>）
//   TypeKind::Weak → payload[0] 内层 handle → _weak_release
//   TypeKind::Fn   → payload[8] captures ptr → _box_release_dtor（null / 栈嵌入 LSB 跳过）
// payloadReleaseFn 为 null 时跳过 payload 析构（安全退化：仅释放本层 RC block）。
void emitRcReleaseForInlineDtorFn(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                                  llvm::Function* func, TypeKind kind, llvm::Function* payloadReleaseFn);

// riu_rc_alloc / riu_rc_retain / riu_rc_release（riurt.lib；与旧 __riu_box_* 同协议）
llvm::Function* getRcAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);
llvm::Function* getRcRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder);
// riu_rc_release(handle) -> void
// strong--；归零时 weak--，weak 也归零时 free；null/哨兵跳过。不跑 payload 析构。
llvm::Function* getRcReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _dyn_release(data, vtable) -> void  (Phase 3e, DRAFT-dyn-draft §12.9)
// owned Dyn<D> 的释放路径：与 _box_release 同形，但 strong==0 时按
// vtable[0] 间接调用 U 的析构函数（fn(ptr) void，接 payload+8 即实例指针），
// 再走 weak-- + free。null / 哨兵跳过；vtable[0] = null（U 平凡）时仅释放 RC 块。
// 借用 Dyn<D&> 不调用本函数（借用不动 RC）。
llvm::Function* getDynReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _box_release_dtor(handle) -> void  (Phase 4a-2)
// 与 _box_release 相同，但 strong 归零时先按 payload[0..8] 处的 dtor fn ptr 调
// dtor(payload + 8)，再走 weak/free。专为 lambda captures 共享 Rc 设计：
// 多个 fat-ptr 副本共享同一 captures Rc 时，字段级析构必须只在 strong==0 一次性触发。
// payload 头 8 字节 = dtor fn ptr（null 跳过），其后才是各 capture 字段。
llvm::Function* getRcReleaseDtorFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _box_release_<T>(handle) -> void  (Phase B-2)
// 专为 Rc<T> 其中 T 需析构：与 _box_release 同形（null/哨兵跳过、strong--），
// 但 strong==0 时先对 payload（handle+8）调 T 的析构函数，再走 weak-- + free。
// 每个 (T, module) 生成独立一份，由 Compiler::getOrCreateRcTypedReleaseFn 按需生成 body。
// mangledName 由调用方构造（如 "_box_release_T_<sanitized>"），需保证模块内唯一。
llvm::Function* getRcReleaseTypedFn(llvm::Module* module, llvm::IRBuilder<>& builder, const string& mangledName);

// _box_upgrade(handle) -> handle_or_null
// Phase 1d.2：Weak→Rc 升级；null/strong==0 → null；哨兵 → handle；其他 strong++ 返回 handle
llvm::Function* getRcUpgradeFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _weak_release(handle) -> void
// weak--；归零时 free 整个 block（前提：strong 已 0，否则 weak 不可能比 strong 先归零）
// null/哨兵跳过
llvm::Function* getWeakReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// _weak_retain(handle) -> void（Phase 3a）
// weak++；null/哨兵跳过
llvm::Function* getWeakRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== Array<T> 动态数组支持（B-3）====================
// B-3: Array 去 Builtin/去 Block，layout = { ptr _data, u64 _len, u64 _cap }。
// _data 是直接 HeapAlloc 的数据缓冲指针；元素析构由 Compiler 按具体 T 发射。

// _array_free_data(data Ptr) → void
//   null 安全：data == null 跳过；否则 HeapFree(GetProcessHeap(), 0, data)
llvm::Function* getArrayFreeDataFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== Heap<T> 堆作用域句柄支持（DRAFT-heap-types §8.3a）====================
// 单所有权、作用域绑定、无 RC 头，layout = 裸 T*
// 与 Rc<T> 不同：调用方不维护引用计数；alloc/free 一一对应；析构由编译器在作用域尾内联调用 dtor 后再调 free

// __riu_heap_alloc(payloadSize) -> ptr
//   分配 payloadSize 字节裸 buffer（无 RC 头），返回指向 payload 的指针
//   ctor 由编译器在 IR 内联调用；null 不可能（runtime 失败时调用 HeapAlloc 行为）
llvm::Function* getHeapHandleAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// __riu_heap_free(ptr) -> void
//   释放 buffer；null 跳过
//   payload 析构由调用方在 IR 内联（在 __riu_heap_free 之前），与 _box_release 一致
llvm::Function* getHeapHandleFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== leak 检测（Phase 8a） ====================
// 全局 i64 `__riu_rc_block_count`：定义在 riurt；IR 仅 extern
llvm::GlobalVariable* getRcBlockCountGlobal(llvm::Module* module, llvm::IRBuilder<>& builder);

// ==================== 运行时辅助函数生成 ====================

void emitRcHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitWeakHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitArrayHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitHeapHandleHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module);
void emitMainStartup(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                     const std::vector<std::string>& initModuleNames = {});
// 测试 DLL 模式：生成 riu_test_init（dllexport），调全局 init + _riu_register_tests
// riu-test-runner.exe 加载 DLL 后通过 GetProcAddress 调用
void emitTestDllInit(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                     const std::vector<std::string>& initModuleNames = {});
void emitRuntimeHelpers(llvm::IRBuilder<>& builder, llvm::Module* module);

} // namespace runtime

#endif // RIU_LANG_COMPILER_RUNTIME_H
