// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 运行时辅助函数实现
//
// - riurt C 符号声明（riurt_alloc / riu_rc_*）
// - Weak / Dyn / typed Rc release / Array / Heap / 启动 stub 的 IR
//
// Rc 的 alloc/retain/generic release 在 riurt.lib（ffi/riu_rc.c），不再 emit IR 体。

#include "compiler_runtime.h"
#include "ast/riu.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace runtime {

// ==================== Windows API 辅助 ====================

// 获取或创建 Windows API 函数声明
// 这些是外部函数，链接时由 Windows 系统提供
llvm::Function* getOrCreateWindowsAPI(llvm::Module* module, llvm::IRBuilder<>& builder, const string& name) {
    // 检查是否已存在
    auto func = module->getFunction(name);
    if (func) return func;

    // GetStdHandle: 取标准 IO 句柄
    // 签名: ptr GetStdHandle(i32 nStdHandle)
    if (name == "GetStdHandle") {
        auto fnType =
            llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), {builder.getInt32Ty()}, false);
        return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, module);
    }

    // WriteFile: 同步写文件 / 句柄
    // 签名: i32 WriteFile(ptr hFile, ptr lpBuffer, u32 nNumberOfBytesToWrite,
    //                    ptr lpNumberOfBytesWritten, ptr lpOverlapped)
    if (name == "WriteFile") {
        auto ptrTy = llvm::PointerType::get(builder.getContext(), 0);
        auto fnType =
            llvm::FunctionType::get(builder.getInt32Ty(), {ptrTy, ptrTy, builder.getInt32Ty(), ptrTy, ptrTy}, false);
        return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, module);
    }

    // ExitProcess: 终止进程
    // 签名: void ExitProcess(u32 uExitCode) #NoReturn
    if (name == "ExitProcess") {
        auto fnType = llvm::FunctionType::get(builder.getVoidTy(), {builder.getInt32Ty()}, false);
        auto fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, module);
        fn->addFnAttr(llvm::Attribute::NoReturn);
        return fn;
    }

    // SetConsoleOutputCP / SetConsoleCP: 设置控制台代码页
    // 签名: i1 SetConsoleOutputCP(i32 codePage)
    if (name == "SetConsoleOutputCP" || name == "SetConsoleCP") {
        auto fnType = llvm::FunctionType::get(builder.getInt1Ty(), {builder.getInt32Ty()}, false);
        return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, module);
    }

    return nullptr;
}

// ==================== riurt C 运行时函数声明 ====================
// 替代直接 emit Win32 HeapAlloc/HeapFree IR 的方案。
// riurt 是纯 C99 静态库（riurt.lib），由编译器链接到每个 riu 程序。

// riurt_alloc(size u64) -> ptr
llvm::Function* getRiurtAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riurt_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;
    auto fnType =
        llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), {builder.getInt64Ty()}, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riurt_realloc(ptr, new_size u64) -> ptr
llvm::Function* getRiurtReallocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riurt_realloc";
    auto func = module->getFunction(fnName);
    if (func) return func;
    auto fnType =
        llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0),
                                {llvm::PointerType::get(builder.getContext(), 0), builder.getInt64Ty()}, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riurt_free(ptr) -> void
llvm::Function* getRiurtFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riurt_free";
    auto func = module->getFunction(fnName);
    if (func) return func;
    auto fnType =
        llvm::FunctionType::get(builder.getVoidTy(), {llvm::PointerType::get(builder.getContext(), 0)}, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 SetConsoleOutputCP 函数
llvm::Function* getSetConsoleOutputCPFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "SetConsoleOutputCP");
}

// 获取 SetConsoleCP 函数
llvm::Function* getSetConsoleCPFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "SetConsoleCP");
}

// ==================== leak 检测（Phase 8a） ====================

// 获取（或新建 extern 声明）全局 _rc_block_count（i64）
// 定义在 riurt.lib（riu_rc.c）；IR 只声明
llvm::GlobalVariable* getRcBlockCountGlobal(llvm::Module* module, llvm::IRBuilder<>& builder) {
    const string name = "__riu_rc_block_count";
    if (auto g = module->getGlobalVariable(name)) return g;
    auto i64Ty = builder.getInt64Ty();
    return new llvm::GlobalVariable(*module, i64Ty,
                                    /*isConstant*/ false, llvm::GlobalValue::ExternalLinkage,
                                    /*init*/ nullptr, // extern 声明
                                    name);
}

// 在当前 IRBuilder 插入位置 emit `_rc_block_count += delta`（delta 为 +1 / -1 i64 常量）
// 计数器定义在 riurt；此处只 load/store 该 extern。
void emitRcBlockCountAdd(llvm::IRBuilder<>& builder, llvm::Module* module, int64_t delta) {
    auto g = getRcBlockCountGlobal(module, builder);
    auto i64Ty = builder.getInt64Ty();
    auto cur = builder.CreateLoad(i64Ty, g, "rc_blk_cur");
    auto next = builder.CreateAdd(cur, llvm::ConstantInt::get(i64Ty, delta), "rc_blk_next");
    builder.CreateStore(next, g);
}

// ==================== Rc<T> 智能指针支持 ====================
//
// Phase 1a 新布局（DRAFT §7.1）：
// Block = { strong: u32, weak: u32, payload: T }，payload 始于偏移 8
// Rc 实例只持有一个 block 指针（handle）；payload 指针 = handle + 8
// 哨兵 strong == 0xFFFFFFFF 时所有 retain/release 操作 no-op（用于 .rodata 字面量；Phase 1c 启用）
// Phase 1a 仅维护 strong 计数；weak 字段已写入 layout 但仅初始化为 1，Phase 1d 接入弱引用协议时启用

// riu_rc_alloc(payload_size i64) -> ptr  （riurt.lib）
llvm::Function* getRcAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riu_rc_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(builder.getInt64Ty());

    auto fnType = llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riu_rc_retain(ptr block) -> void
llvm::Function* getRcRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riu_rc_retain";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riu_rc_release(ptr block) -> void（无 payload 析构）
llvm::Function* getRcReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riu_rc_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riu_rc_release_need_dtor(ptr) -> i32
llvm::Function* getRiuRcReleaseNeedDtorFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riu_rc_release_need_dtor";
    auto func = module->getFunction(fnName);
    if (func) return func;
    auto fnType =
        llvm::FunctionType::get(builder.getInt32Ty(), {llvm::PointerType::get(builder.getContext(), 0)}, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// riu_rc_drop_block(ptr) -> void
llvm::Function* getRiuRcDropBlockFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "riu_rc_drop_block";
    auto func = module->getFunction(fnName);
    if (func) return func;
    auto fnType =
        llvm::FunctionType::get(builder.getVoidTy(), {llvm::PointerType::get(builder.getContext(), 0)}, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取带 payload 析构 dispatch 的 Rc 释放函数（Phase 4a-2）
// 签名: void _box_release_dtor(ptr block)
// 见头文件说明。runtime 实现见 emitRcHelpers。
llvm::Function* getRcReleaseDtorFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_box_release_dtor";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取按 T 特化的 Rc 释放函数声明（Phase B-2）
// 签名: void _box_release_<T>(ptr block)
// 函数体由 Compiler::getOrCreateRcTypedReleaseFn 按需生成。
llvm::Function* getRcReleaseTypedFn(llvm::Module* module, llvm::IRBuilder<>& builder, const string& mangledName) {
    auto func = module->getFunction(mangledName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    // B-4: LinkOnceODRLinkage 避免多模块各自生成同签名 typed release 函数时符号冲突
    auto fn = llvm::Function::Create(fnType, llvm::Function::LinkOnceODRLinkage, mangledName, module);
    // COFF 平台下 linkonce_odr 必须显式 COMDAT，否则仍按强符号 emit
    fn->setComdat(module->getOrInsertComdat(mangledName));
    return fn;
}

// 获取 owned Dyn<D> 的释放函数（Phase 3e）
// 签名: void _dyn_release(ptr data, ptr vtable)
// 见头文件说明。runtime 实现见 emitRcHelpers。
llvm::Function* getDynReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_dyn_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0)); // data
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0)); // vtable

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Rc 升级（Weak→Rc）函数（Phase 1d.2）
// 签名: ptr _box_upgrade(ptr block)
// null/strong==0 → 返回 null；哨兵 → 直接返回 block；否则 strong++ 并返回 block
llvm::Function* getRcUpgradeFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_box_upgrade";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Weak 引用增加函数（Phase 3a）
// 签名: void _weak_retain(ptr block)
// null/哨兵跳过；否则 weak++
llvm::Function* getWeakRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_weak_retain";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Weak 引用减少函数
// 签名: void _weak_release(ptr block)
// 哨兵 / null 跳过；否则 weak--；weak==0 时 free 整个 block
llvm::Function* getWeakReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_weak_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// ==================== Array<T> 动态数组支持（Phase 1b 新 ABI） ====================

// B-3: _array_free_data(ptr data) → void — null 安全 HeapFree
llvm::Function* getArrayFreeDataFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_array_free_data";
    auto func = module->getFunction(fnName);
    if (func) return func;
    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));
    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    func = llvm::Function::Create(fnType, llvm::Function::LinkOnceODRLinkage, fnName, module);
    // COFF 平台下 linkonce_odr 必须显式 COMDAT，否则仍按强符号 emit
    func->setComdat(module->getOrInsertComdat(fnName));

    // B-3: lazy emit body — 按需生成，使非 SDK 模块也能调用
    auto& ctx = builder.getContext();
    auto entry = llvm::BasicBlock::Create(ctx, "entry", func);
    auto freeBB = llvm::BasicBlock::Create(ctx, "do_free", func);
    auto doneBB = llvm::BasicBlock::Create(ctx, "done", func);
    auto savedBB = builder.GetInsertBlock();
    auto savedIP = savedBB ? builder.GetInsertPoint() : llvm::BasicBlock::iterator();
    builder.SetInsertPoint(entry);

    auto ptrTy = llvm::PointerType::get(ctx, 0);
    llvm::Value* data = &*func->arg_begin();
    auto isNull = builder.CreateICmpEQ(data, llvm::ConstantPointerNull::get(ptrTy), "is_null");
    builder.CreateCondBr(isNull, doneBB, freeBB);

    builder.SetInsertPoint(freeBB);
    auto freeFn = getRiurtFreeFn(module, builder);
    builder.CreateCall(freeFn, {data});
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();

    if (savedBB) builder.SetInsertPoint(savedBB, savedIP);
    return func;
}

// ==================== Heap<T> 堆作用域句柄支持（DRAFT-heap-types §8.3a） ====================
//
// 与 Rc 的区别：
//   - 无 RC 头（无 8 字节 strong/weak 前缀），layout = 裸 payload
//   - 单所有权 + 作用域绑定，alloc/free 一一对应
//   - 不参与 leak 计数（rc_block_count），不影响现有 _rc_block_count 协议

// __riu_heap_alloc(i64 payloadSize) -> ptr
llvm::Function* getHeapHandleAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_heap_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(builder.getInt64Ty());

    auto fnType = llvm::FunctionType::get(llvm::PointerType::get(builder.getContext(), 0), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// __riu_heap_free(ptr) -> void
llvm::Function* getHeapHandleFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "__riu_heap_free";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

void emitHeapHandleHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Heap<T> helper functions");

    auto allocFn = runtime::getRiurtAllocFn(module, builder);
    auto freeFn = runtime::getRiurtFreeFn(module, builder);
    auto ptrTy = llvm::PointerType::get(context, 0);

    // __riu_heap_alloc: 直接调 riurt_alloc(payloadSize)，返回裸 payload 指针
    {
        DEBUG_LOG("  Emitting __riu_heap_alloc");
        auto fn = getHeapHandleAllocFn(module, builder);
        if (fn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", fn);
            builder.SetInsertPoint(entry);

            llvm::Value* payloadSize = &*fn->arg_begin();
            auto buf = builder.CreateCall(allocFn, {payloadSize}, "heap_buf");
            builder.CreateRet(buf);
        }
    }

    // __riu_heap_free: null 跳过；非 null 调 riurt_free(ptr)
    {
        DEBUG_LOG("  Emitting __riu_heap_free");
        auto fn = getHeapHandleFreeFn(module, builder);
        if (fn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", fn);
            auto freeBB = llvm::BasicBlock::Create(context, "free", fn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", fn);
            builder.SetInsertPoint(entry);

            llvm::Value* ptr = &*fn->arg_begin();
            auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            auto isNull = builder.CreateICmpEQ(ptr, nullPtr, "is_null");
            builder.CreateCondBr(isNull, doneBB, freeBB);

            builder.SetInsertPoint(freeBB);
            builder.CreateCall(freeFn, {ptr});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }
}

// ==================== Rc 辅助函数实现 ====================
// alloc/retain/generic release 在 riurt（riu_rc_*）。typed / dtor / dyn 只在 strong 归零时跑 payload 析构，再 drop。

template <typename F>
static void emitReleaseViaC(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                            llvm::Function* func, F&& onStrongZero) {
    if (!func || !func->empty()) return;
    auto needFn = getRiuRcReleaseNeedDtorFn(module, builder);
    auto dropFn = getRiuRcDropBlockFn(module, builder);
    auto i32Ty = builder.getInt32Ty();

    auto entry = llvm::BasicBlock::Create(context, "entry", func);
    auto dtorBB = llvm::BasicBlock::Create(context, "dtor", func);
    auto doneBB = llvm::BasicBlock::Create(context, "done", func);

    builder.SetInsertPoint(entry);
    llvm::Value* block = &*func->arg_begin();
    auto need = builder.CreateCall(needFn, {block}, "need_dtor");
    auto isNeed = builder.CreateICmpNE(need, llvm::ConstantInt::get(i32Ty, 0), "is_need");
    builder.CreateCondBr(isNeed, dtorBB, doneBB);

    builder.SetInsertPoint(dtorBB);
    onStrongZero(block);
    builder.CreateCall(dropFn, {block});
    builder.CreateBr(doneBB);

    builder.SetInsertPoint(doneBB);
    builder.CreateRetVoid();
}

void emitRcHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Rc helper functions");

    auto ptrTy = llvm::PointerType::get(context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    // _box_release_dtor: 计数走 C；strong 归零时先 dtor(payload+8)
    {
        DEBUG_LOG("  Emitting _box_release_dtor");
        emitReleaseViaC(context, builder, module, getRcReleaseDtorFn(module, builder), [&](llvm::Value* block) {
            auto dtorSlot = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(8)}, "dtor_slot");
            auto dtorPtr = builder.CreateLoad(ptrTy, dtorSlot, "dtor_ptr");
            auto* fn = builder.GetInsertBlock()->getParent();
            auto* dtorCallBB = llvm::BasicBlock::Create(context, "dtor_call", fn);
            auto* afterDtorBB = llvm::BasicBlock::Create(context, "after_dtor", fn);
            auto dtorIsNull = builder.CreateICmpEQ(dtorPtr, nullPtr, "dtor_is_null");
            builder.CreateCondBr(dtorIsNull, afterDtorBB, dtorCallBB);

            builder.SetInsertPoint(dtorCallBB);
            auto fieldsBase = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(16)}, "fields_base");
            auto dtorFnTy = llvm::FunctionType::get(builder.getVoidTy(), {ptrTy}, false);
            builder.CreateCall(dtorFnTy, dtorPtr, {fieldsBase});
            builder.CreateBr(afterDtorBB);

            builder.SetInsertPoint(afterDtorBB);
        });
    }

    // _dyn_release: 计数走 C；strong 归零时按 vtable[0] dispatch
    {
        DEBUG_LOG("  Emitting _dyn_release");
        auto releaseFn = getDynReleaseFn(module, builder);
        if (releaseFn->empty()) {
            auto needFn = getRiuRcReleaseNeedDtorFn(module, builder);
            auto dropFn = getRiuRcDropBlockFn(module, builder);
            auto i32Ty = builder.getInt32Ty();

            auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
            auto dtorBB = llvm::BasicBlock::Create(context, "dtor", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);
            builder.SetInsertPoint(entry);

            auto argIt = releaseFn->arg_begin();
            llvm::Value* data = &*argIt++;
            llvm::Value* vtable = &*argIt;
            auto need = builder.CreateCall(needFn, {data}, "need_dtor");
            auto isNeed = builder.CreateICmpNE(need, llvm::ConstantInt::get(i32Ty, 0), "is_need");
            builder.CreateCondBr(isNeed, dtorBB, doneBB);

            builder.SetInsertPoint(dtorBB);
            auto* skipDtorBB = llvm::BasicBlock::Create(context, "skip_dtor", releaseFn);
            auto* loadDtorBB = llvm::BasicBlock::Create(context, "load_dtor", releaseFn);
            auto* dtorCallBB = llvm::BasicBlock::Create(context, "dtor_call", releaseFn);
            auto* afterDtorBB = llvm::BasicBlock::Create(context, "after_dtor", releaseFn);
            auto vtableIsNull = builder.CreateICmpEQ(vtable, nullPtr, "vtable_is_null");
            builder.CreateCondBr(vtableIsNull, skipDtorBB, loadDtorBB);

            builder.SetInsertPoint(loadDtorBB);
            auto dtorPtr = builder.CreateLoad(ptrTy, vtable, "dtor_ptr");
            auto dtorIsNull = builder.CreateICmpEQ(dtorPtr, nullPtr, "dtor_is_null");
            builder.CreateCondBr(dtorIsNull, afterDtorBB, dtorCallBB);

            builder.SetInsertPoint(dtorCallBB);
            auto payload = builder.CreateGEP(builder.getInt8Ty(), data, {builder.getInt64(8)}, "payload");
            auto dtorFnTy = llvm::FunctionType::get(builder.getVoidTy(), {ptrTy}, false);
            builder.CreateCall(dtorFnTy, dtorPtr, {payload});
            builder.CreateBr(afterDtorBB);

            builder.SetInsertPoint(skipDtorBB);
            builder.CreateBr(afterDtorBB);

            builder.SetInsertPoint(afterDtorBB);
            builder.CreateCall(dropFn, {data});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }
}

// ==================== Phase B-2: 按 T 特化的 Rc 释放函数体生成 ====================
// 计数走 C；strong 归零时先对 payload（handle+8）调 T::~()，再 drop_block。
void emitRcReleaseTypedFn(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                          llvm::Function* func, llvm::Function* dtorFn) {
    emitReleaseViaC(context, builder, module, func, [&](llvm::Value* block) {
        if (dtorFn) {
            auto payloadPtr = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(8)}, "payload_ptr");
            builder.CreateCall(dtorFn, {payloadPtr});
        }
    });
}

// ==================== B-2 inline-dtor: Rc<inline-type> typed release ====================
// Rc<T> 其中 T 为 Rc/Weak/fn 等无独立 dtor 函数的内联析构类型。
void emitRcReleaseForInlineDtorFn(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                                  llvm::Function* func, TypeKind kind, llvm::Function* payloadReleaseFn) {
    emitReleaseViaC(context, builder, module, func, [&](llvm::Value* block) {
        if (!payloadReleaseFn) return;
        auto ptrTy = llvm::PointerType::get(context, 0);
        if (kind == TypeKind::Rc) {
            auto payloadPtr = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(8)}, "nested_rc_payload");
            auto handleAddr =
                builder.CreateBitCast(payloadPtr, llvm::PointerType::get(context, 0), "nested_rc_handle_addr");
            auto handle = builder.CreateLoad(ptrTy, handleAddr, "nested_rc_handle");
            builder.CreateCall(payloadReleaseFn, {handle});
        } else if (kind == TypeKind::Weak) {
            auto payloadPtr =
                builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(8)}, "nested_weak_payload");
            auto handleAddr =
                builder.CreateBitCast(payloadPtr, llvm::PointerType::get(context, 0), "nested_weak_handle_addr");
            auto handle = builder.CreateLoad(ptrTy, handleAddr, "nested_weak_handle");
            builder.CreateCall(payloadReleaseFn, {handle});
        } else if (kind == TypeKind::Fn) {
            auto capturesAddr =
                builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(16)}, "fn_captures_addr");
            auto capturesPtr =
                builder.CreateBitCast(capturesAddr, llvm::PointerType::get(context, 0), "fn_captures_ptr");
            auto captures = builder.CreateLoad(ptrTy, capturesPtr, "fn_captures");
            auto i64Ty = builder.getInt64Ty();
            auto capInt = builder.CreatePtrToInt(captures, i64Ty, "fn.cap.asint");
            auto isStack = builder.CreateICmpNE(builder.CreateAnd(capInt, builder.getInt64(1)), builder.getInt64(0),
                                                "fn.cap.isstack");
            auto isNull = builder.CreateICmpEQ(captures, llvm::ConstantPointerNull::get(ptrTy), "fn.cap.isnull");
            auto skip = builder.CreateOr(isStack, isNull, "fn.cap.skip");
            auto* pf = builder.GetInsertBlock()->getParent();
            auto* relBB = llvm::BasicBlock::Create(context, "fn.cap.rel", pf);
            auto* contBB = llvm::BasicBlock::Create(context, "fn.cap.cont", pf);
            builder.CreateCondBr(skip, contBB, relBB);
            builder.SetInsertPoint(relBB);
            builder.CreateCall(payloadReleaseFn, {captures});
            builder.CreateBr(contBB);
            builder.SetInsertPoint(contBB);
        }
    });
}

// ==================== Weak 辅助函数实现（Phase 1d.1） ====================
// Block 与 Rc 共享同一布局：{ u32 strong @0, u32 weak @4, payload }
// _weak_release 仅维护 block 存活；payload 已在 strong 归零时被调用方析构

void emitWeakHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Weak helper functions");

    auto freeFn = runtime::getRiurtFreeFn(module, builder);

    auto ptrTy = llvm::PointerType::get(context, 0);
    auto i32Ty = builder.getInt32Ty();
    auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    // _box_upgrade(handle) -> handle_or_null
    // Phase 1d.2：null → null；哨兵 → handle；strong==0 → null；否则 strong++ 返回 handle
    DEBUG_LOG("  Emitting _box_upgrade");
    auto upgradeFn = getRcUpgradeFn(module, builder);
    if (upgradeFn->empty()) {
        auto entry = llvm::BasicBlock::Create(context, "entry", upgradeFn);
        auto checkBB = llvm::BasicBlock::Create(context, "check", upgradeFn);
        auto retBlockBB = llvm::BasicBlock::Create(context, "ret_block", upgradeFn);
        auto strongCheckBB = llvm::BasicBlock::Create(context, "strong_check", upgradeFn);
        auto incBB = llvm::BasicBlock::Create(context, "inc", upgradeFn);
        auto retNullBB = llvm::BasicBlock::Create(context, "ret_null", upgradeFn);

        builder.SetInsertPoint(entry);
        llvm::Value* block = &*upgradeFn->arg_begin();
        auto isNull = builder.CreateICmpEQ(block, nullPtr, "is_null");
        builder.CreateCondBr(isNull, retNullBB, checkBB);

        builder.SetInsertPoint(checkBB);
        auto strong = builder.CreateLoad(i32Ty, block, "strong");
        auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
        builder.CreateCondBr(isSentinel, retBlockBB, strongCheckBB);

        builder.SetInsertPoint(strongCheckBB);
        auto isZero = builder.CreateICmpEQ(strong, llvm::ConstantInt::get(i32Ty, 0), "is_zero");
        builder.CreateCondBr(isZero, retNullBB, incBB);

        builder.SetInsertPoint(incBB);
        auto newStrong = builder.CreateAdd(strong, llvm::ConstantInt::get(i32Ty, 1), "new_strong");
        builder.CreateStore(newStrong, block);
        builder.CreateBr(retBlockBB);

        builder.SetInsertPoint(retBlockBB);
        builder.CreateRet(block);

        builder.SetInsertPoint(retNullBB);
        builder.CreateRet(nullPtr);
    }

    // _weak_retain(handle): null/哨兵跳过；weak++（Phase 3a）
    DEBUG_LOG("  Emitting _weak_retain");
    auto retainFn = getWeakRetainFn(module, builder);
    if (retainFn->empty()) {
        auto entry = llvm::BasicBlock::Create(context, "entry", retainFn);
        auto checkBB = llvm::BasicBlock::Create(context, "check", retainFn);
        auto incBB = llvm::BasicBlock::Create(context, "inc", retainFn);
        auto doneBB = llvm::BasicBlock::Create(context, "done", retainFn);

        builder.SetInsertPoint(entry);
        llvm::Value* block = &*retainFn->arg_begin();
        auto isNull = builder.CreateICmpEQ(block, nullPtr, "is_null");
        builder.CreateCondBr(isNull, doneBB, checkBB);

        builder.SetInsertPoint(checkBB);
        auto strong = builder.CreateLoad(i32Ty, block, "strong");
        auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
        builder.CreateCondBr(isSentinel, doneBB, incBB);

        builder.SetInsertPoint(incBB);
        auto weakPtr = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(4)}, "weak_ptr");
        auto weak = builder.CreateLoad(i32Ty, weakPtr, "weak");
        auto newWeak = builder.CreateAdd(weak, llvm::ConstantInt::get(i32Ty, 1), "new_weak");
        builder.CreateStore(newWeak, weakPtr);
        builder.CreateBr(doneBB);

        builder.SetInsertPoint(doneBB);
        builder.CreateRetVoid();
    }

    // _weak_release(handle): null/哨兵跳过；weak--；weak==0 free
    DEBUG_LOG("  Emitting _weak_release");
    auto releaseFn = getWeakReleaseFn(module, builder);
    if (releaseFn->empty()) {
        auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
        auto checkBB = llvm::BasicBlock::Create(context, "check", releaseFn);
        auto decBB = llvm::BasicBlock::Create(context, "dec", releaseFn);
        auto freeBB = llvm::BasicBlock::Create(context, "free", releaseFn);
        auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);
        builder.SetInsertPoint(entry);

        llvm::Value* block = &*releaseFn->arg_begin();
        auto isNull = builder.CreateICmpEQ(block, nullPtr, "is_null");
        builder.CreateCondBr(isNull, doneBB, checkBB);

        builder.SetInsertPoint(checkBB);
        auto strongPtr = block;
        auto strong = builder.CreateLoad(i32Ty, strongPtr, "strong");
        auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
        builder.CreateCondBr(isSentinel, doneBB, decBB);

        builder.SetInsertPoint(decBB);
        auto weakPtr = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(4)}, "weak_ptr");
        auto weak = builder.CreateLoad(i32Ty, weakPtr, "weak");
        auto newWeak = builder.CreateSub(weak, llvm::ConstantInt::get(i32Ty, 1), "new_weak");
        builder.CreateStore(newWeak, weakPtr);
        auto isZero = builder.CreateICmpEQ(newWeak, llvm::ConstantInt::get(i32Ty, 0), "is_zero");
        builder.CreateCondBr(isZero, freeBB, doneBB);

        builder.SetInsertPoint(freeBB);
        builder.CreateCall(freeFn, {block});
        emitRcBlockCountAdd(builder, module, -1);
        builder.CreateBr(doneBB);

        builder.SetInsertPoint(doneBB);
        builder.CreateRetVoid();
    }
}

// ==================== Array 辅助函数实现（Phase 1b 新 ABI） ====================
// Block 字节布局: { u32 strong @0, u32 weak @4, i64 len @8, i64 cap @16, ptr data @24 }
// Block 总头部 = 32 字节；data 是间接指针指向独立 heap 缓冲

void emitArrayHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Array helper: _array_free_data");
    // B-3: _array_free_data body 由 getArrayFreeDataFn 按需 lazy emit（LinkOnceODR），
    // 此处仅确保 SDK 模块内也有定义（用于 JIT 模式下首个加载模块可直接 resolve）
    getArrayFreeDataFn(module, builder);
    (void)context;
}

// ==================== 程序启动 ====================

// 生成 main 启动函数
// 设置控制台编码为 UTF-8，然后按拓扑序调用各模块 _riu_global_init_<Mod>()
// 最后调用 riu_main
void emitMainStartup(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                     const std::vector<std::string>& initModuleNames) {
    DEBUG_LOG("Emitting main startup function");

    auto setConsoleOutputCP = getSetConsoleOutputCPFn(module, builder);
    auto setConsoleCP = getSetConsoleCPFn(module, builder);

    auto fnType = llvm::FunctionType::get(builder.getInt32Ty(), {}, false);
    auto mainStartup = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, "mainStartup", module);
    DEBUG_LOG("  Created mainStartup function");

    auto entry = llvm::BasicBlock::Create(context, "entry", mainStartup);
    builder.SetInsertPoint(entry);

    // 设置控制台代码页为 UTF-8 (65001)
    auto cpUtf8 = llvm::ConstantInt::get(builder.getInt32Ty(), 65001);
    builder.CreateCall(setConsoleOutputCP, {cpUtf8});
    builder.CreateCall(setConsoleCP, {cpUtf8});
    DEBUG_LOG("  Set console code page to UTF-8");

    // DRAFT-static-vars Phase 6: 按模块拓扑序调用 __riu_global_init.<Mod>()
    // initModuleNames 由 Compiler 端传入（Riu::loadOrder），已按导入依赖的拓扑序排列。
    auto voidFnType = llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    if (!initModuleNames.empty()) {
        for (auto& modName : initModuleNames) {
            string fnName = "__riu_global_init." + modName;
            auto callee = module->getOrInsertFunction(fnName, voidFnType);
            builder.CreateCall(callee, {});
            DEBUG_LOG_VAL("  Called global init (topo order)", fnName);
        }
    } else {
        // 兼容旧路径（单文件模式 / 无 Riu 驱动）：遍历当前 Module 内所有 init 函数
        for (auto& func : module->getFunctionList()) {
            auto funcName = func.getName();
            if (funcName.starts_with("__riu_global_init.")) {
                builder.CreateCall(&func, {});
                DEBUG_LOG_VAL("  Called global init (legacy)", funcName.str());
            }
        }
    }

    // 调用 riu_main
    auto riuMain = module->getFunction("riu_main");
    if (riuMain) {
        builder.CreateCall(riuMain, {});
        DEBUG_LOG("  Called riu_main");
    } else {
        DEBUG_LOG("  riu_main not found");
    }
    builder.CreateRet(builder.getInt32(0));
}

// 测试 DLL 模式的 riu_test_init：由 riu-test-runner.exe 通过 GetProcAddress 调用
// 调用各模块全局初始化函数。测试注册表在编译期由 emitTestRegistrations 生成为全局数组，
// 无需运行时注册。
void emitTestDllInit(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module,
                     const std::vector<std::string>& initModuleNames) {
    DEBUG_LOG("Emitting test DLL init function");

    auto voidFnType = llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    auto initFn = llvm::Function::Create(voidFnType, llvm::Function::ExternalLinkage, "riu_test_init", module);
    // DLL 导出：riu-test-runner.exe 通过 GetProcAddress 查找
    initFn->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
    DEBUG_LOG("  Created riu_test_init (dllexport)");

    auto entry = llvm::BasicBlock::Create(context, "entry", initFn);
    builder.SetInsertPoint(entry);

    // 按模块拓扑序调用 __riu_global_init.<Mod>()
    auto voidInitFnType = llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    if (!initModuleNames.empty()) {
        for (auto& modName : initModuleNames) {
            string fnName = "__riu_global_init." + modName;
            auto callee = module->getOrInsertFunction(fnName, voidInitFnType);
            builder.CreateCall(callee, {});
            DEBUG_LOG_VAL("  Called global init (topo order)", fnName);
        }
    } else {
        // 兼容：遍历当前 Module 内所有 init 函数
        for (auto& func : module->getFunctionList()) {
            auto funcName = func.getName();
            if (funcName.starts_with("__riu_global_init.")) {
                builder.CreateCall(&func, {});
                DEBUG_LOG_VAL("  Called global init (legacy)", funcName.str());
            }
        }
    }

    builder.CreateRetVoid();
}

// ==================== 通用运行时辅助 ====================

// 生成通用运行时辅助函数
void emitRuntimeHelpers(llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting runtime helpers (SDK)");

    // __chkstk: 栈检查函数 (Windows 要求)
    // 这是一个空实现，实际栈检查由链接器提供
    auto chkstkFnType = llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    auto chkstk = llvm::Function::Create(chkstkFnType, llvm::Function::ExternalLinkage, "__chkstk", module);
    auto chkstkEntry = llvm::BasicBlock::Create(builder.getContext(), "entry", chkstk);
    builder.SetInsertPoint(chkstkEntry);
    builder.CreateRetVoid();
    DEBUG_LOG("  Emitted __chkstk");

    // _fltused: 浮点数使用标志 (Windows 要求)
    // 指示程序使用了浮点运算
    new llvm::GlobalVariable(*module, builder.getInt32Ty(), true, llvm::GlobalValue::ExternalLinkage,
                             builder.getInt32(0), "_fltused");
    DEBUG_LOG("  Created _fltused global");
}

} // namespace runtime
