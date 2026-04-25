// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 运行时辅助函数实现
// 
// 本文件实现编译器生成的运行时辅助函数:
// - Windows API 声明 (GetProcessHeap, HeapAlloc 等)
// - Box<T> 智能指针的内存管理函数
// - Array<T> 动态数组的内存管理函数
// - 程序启动函数 (设置控制台编码、调用 yux_main)
// 
// 注意: 这些函数在编译 SDK (core.yux) 时生成，
// 并链接到每个 yux 程序中。

#include "compiler_runtime.h"
#include "yux.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace runtime {

// ==================== Windows API 辅助 ====================

// 获取或创建 Windows API 函数声明
// 这些是外部函数，链接时由 Windows 系统提供
llvm::Function* getOrCreateWindowsAPI(
    llvm::Module* module,
    llvm::IRBuilder<>& builder,
    const string& name)
{
    // 检查是否已存在
    auto func = module->getFunction(name);
    if (func) return func;

    // GetProcessHeap: 获取进程默认堆
    // 签名: ptr GetProcessHeap()
    if (name == "GetProcessHeap") {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(builder.getContext(), 0),
            {},
            false
        );
        return llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
    }

    // HeapAlloc: 从堆分配内存
    // 签名: ptr HeapAlloc(ptr heap, i64 flags, i64 size)
    if (name == "HeapAlloc") {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(builder.getContext(), 0),
            {llvm::PointerType::get(builder.getContext(), 0), builder.getInt64Ty(), builder.getInt64Ty()},
            false
        );
        return llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
    }

    // HeapReAlloc: 重新分配堆内存
    // 签名: ptr HeapReAlloc(ptr heap, i64 flags, ptr mem, i64 size)
    if (name == "HeapReAlloc") {
        auto fnType = llvm::FunctionType::get(
            llvm::PointerType::get(builder.getContext(), 0),
            {
                llvm::PointerType::get(builder.getContext(), 0), builder.getInt64Ty(),
                llvm::PointerType::get(builder.getContext(), 0), builder.getInt64Ty()
            },
            false
        );
        return llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
    }

    // HeapFree: 释放堆内存
    // 签名: i32 HeapFree(ptr heap, i64 flags, ptr mem)
    if (name == "HeapFree") {
        auto fnType = llvm::FunctionType::get(
            builder.getInt32Ty(),
            {
                llvm::PointerType::get(builder.getContext(), 0), builder.getInt64Ty(),
                llvm::PointerType::get(builder.getContext(), 0)
            },
            false
        );
        return llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
    }

    // SetConsoleOutputCP / SetConsoleCP: 设置控制台代码页
    // 签名: i1 SetConsoleOutputCP(i32 codePage)
    if (name == "SetConsoleOutputCP" || name == "SetConsoleCP") {
        auto fnType = llvm::FunctionType::get(
            builder.getInt1Ty(),
            {builder.getInt32Ty()},
            false
        );
        return llvm::Function::Create(
            fnType,
            llvm::Function::ExternalLinkage,
            name,
            module
        );
    }

    return nullptr;
}

// 获取 GetProcessHeap 函数
llvm::Function* getProcessHeapFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "GetProcessHeap");
}

// 获取 HeapAlloc 函数
llvm::Function* getHeapAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "HeapAlloc");
}

// 获取 HeapReAlloc 函数
llvm::Function* getHeapReAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "HeapReAlloc");
}

// 获取 HeapFree 函数
llvm::Function* getHeapFreeFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "HeapFree");
}

// 获取 SetConsoleOutputCP 函数
llvm::Function* getSetConsoleOutputCPFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "SetConsoleOutputCP");
}

// 获取 SetConsoleCP 函数
llvm::Function* getSetConsoleCPFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    return getOrCreateWindowsAPI(module, builder, "SetConsoleCP");
}

// ==================== Box<T> 智能指针支持 ====================

// 获取 Box 内存分配函数
// 签名: ptr _box_alloc(i64 size)
// 分配 size 字节内存 + 8 字节引用计数
llvm::Function* getBoxAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_box_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(builder.getInt64Ty());

    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(builder.getContext(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Box 引用计数增加函数
// 签名: void _box_retain(ptr refCountPtr)
llvm::Function* getBoxRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_box_retain";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Box 引用计数减少函数
// 签名: void _box_release(ptr refCountPtr, ptr dataPtr)
// 当引用计数降为 0 时释放内存
llvm::Function* getBoxReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_box_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// ==================== Array<T> 动态数组支持 ====================

// 获取 Array 内存分配函数
// 签名: ptr _array_alloc(i64 size)
llvm::Function* getArrayAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(builder.getInt64Ty());

    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(builder.getContext(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Array 扩容函数
// 签名: ptr _array_grow(ptr oldPtr, i64 newSize)
llvm::Function* getArrayGrowFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_grow";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));
    paramTypes.push_back(builder.getInt64Ty());

    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(builder.getContext(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// 获取 Array 释放函数
// 签名: void _array_release(ptr dataPtr)
llvm::Function* getArrayReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// ==================== Box 辅助函数实现 ====================

// 生成 Box 相关的辅助函数实现
void emitBoxHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Box helper functions");

    auto getProcessHeapFn = runtime::getProcessHeapFn(module, builder);
    auto heapAllocFn = runtime::getHeapAllocFn(module, builder);
    auto heapFreeFn = runtime::getHeapFreeFn(module, builder);

    // _box_alloc: 分配内存并初始化引用计数为 1
    {
        DEBUG_LOG("  Emitting _box_alloc");
        auto allocFn = getBoxAllocFn(module, builder);
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", allocFn);
            builder.SetInsertPoint(entry);

            auto args = allocFn->args();
            auto argIt = args.begin();
            llvm::Value* sizeVal = argIt;

            // 获取进程堆
            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");

            // 计算总大小 = 数据大小 + 引用计数大小(8字节)
            auto refCountSize = builder.getInt64(8);
            auto totalSize = builder.CreateAdd(sizeVal, refCountSize, "total_size");

            // 分配内存
            auto mem = builder.CreateCall(heapAllocFn, {heap, builder.getInt64(0), totalSize}, "mem");

            // 初始化引用计数为 1
            auto refCountPtr = builder.CreateBitCast(
                mem, llvm::PointerType::get(context, 0), "ref_count_ptr");
            builder.CreateStore(builder.getInt64(1), refCountPtr);

            // 返回数据指针 (跳过引用计数)
            auto dataPtr = builder.CreateGEP(builder.getInt8Ty(), mem, {refCountSize}, "data_ptr");

            builder.CreateRet(dataPtr);
        }
    }

    // _box_retain: 增加引用计数
    {
        DEBUG_LOG("  Emitting _box_retain");
        auto retainFn = getBoxRetainFn(module, builder);
        if (retainFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", retainFn);
            builder.SetInsertPoint(entry);

            auto args = retainFn->args();
            auto argIt = args.begin();
            llvm::Value* refCountPtr = argIt;

            // 引用计数 + 1
            auto currentCount = builder.CreateLoad(builder.getInt64Ty(), refCountPtr, "current_count");
            auto newCount = builder.CreateAdd(currentCount, builder.getInt64(1), "new_count");
            builder.CreateStore(newCount, refCountPtr);

            builder.CreateRetVoid();
        }
    }

    // _box_release: 减少引用计数，如果为 0 则释放内存
    {
        DEBUG_LOG("  Emitting _box_release");
        auto releaseFn = getBoxReleaseFn(module, builder);
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
            builder.SetInsertPoint(entry);

            auto args = releaseFn->args();
            auto argIt = args.begin();
            llvm::Value* refCountPtr = argIt;
            ++argIt;
            llvm::Value* dataPtr = argIt;

            // 引用计数 - 1
            auto currentCount = builder.CreateLoad(builder.getInt64Ty(), refCountPtr, "current_count");
            auto newCount = builder.CreateSub(currentCount, builder.getInt64(1), "new_count");
            builder.CreateStore(newCount, refCountPtr);

            // 检查是否为 0
            auto isZero = builder.CreateICmpEQ(newCount, builder.getInt64(0), "is_zero");

            auto freeBB = llvm::BasicBlock::Create(context, "free", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);

            builder.CreateCondBr(isZero, freeBB, doneBB);

            // 释放内存
            builder.SetInsertPoint(freeBB);
            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            auto refCountSize = builder.getInt64(8);
            // 计算原始内存指针 (数据指针 - 8)
            auto memPtr = builder.CreateGEP(
                builder.getInt8Ty(), dataPtr, {builder.CreateNeg(refCountSize)}, "mem_ptr");
            builder.CreateCall(heapFreeFn, {heap, builder.getInt64(0), memPtr});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }
}

// ==================== Array 辅助函数实现 ====================

// 生成 Array 相关的辅助函数实现
void emitArrayHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Array helper functions");

    auto getProcessHeapFn = runtime::getProcessHeapFn(module, builder);
    auto heapAllocFn = runtime::getHeapAllocFn(module, builder);
    auto heapReAllocFn = runtime::getHeapReAllocFn(module, builder);
    auto heapFreeFn = runtime::getHeapFreeFn(module, builder);

    // _array_alloc: 分配数组内存
    {
        DEBUG_LOG("  Emitting _array_alloc");
        auto allocFn = getArrayAllocFn(module, builder);
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", allocFn);
            builder.SetInsertPoint(entry);

            auto args = allocFn->args();
            auto argIt = args.begin();
            llvm::Value* sizeVal = argIt;

            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            auto mem = builder.CreateCall(heapAllocFn, {heap, builder.getInt64(0), sizeVal}, "mem");

            builder.CreateRet(mem);
        }
    }

    // _array_grow: 扩容数组 (支持从 null 分配或重新分配)
    {
        DEBUG_LOG("  Emitting _array_grow");
        auto growFn = getArrayGrowFn(module, builder);
        if (growFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", growFn);
            builder.SetInsertPoint(entry);

            auto args = growFn->args();
            auto argIt = args.begin();
            llvm::Value* oldPtr = argIt;
            ++argIt;
            llvm::Value* newSize = argIt;

            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");

            // 检查旧指针是否为 null
            auto isNull = builder.CreateICmpEQ(
                oldPtr, llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0)), "is_null");

            auto allocBB = llvm::BasicBlock::Create(context, "alloc", growFn);
            auto reallocBB = llvm::BasicBlock::Create(context, "realloc", growFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", growFn);

            builder.CreateCondBr(isNull, allocBB, reallocBB);

            // 新分配
            builder.SetInsertPoint(allocBB);
            auto newMemAlloc = builder.CreateCall(heapAllocFn, {heap, builder.getInt64(0), newSize}, "new_mem");
            builder.CreateBr(doneBB);

            // 重新分配
            builder.SetInsertPoint(reallocBB);
            auto newMemRealloc = builder.CreateCall(
                heapReAllocFn, {heap, builder.getInt64(0), oldPtr, newSize}, "new_mem");
            builder.CreateBr(doneBB);

            // 返回结果
            builder.SetInsertPoint(doneBB);
            auto phi = builder.CreatePHI(llvm::PointerType::get(context, 0), 2, "result");
            phi->addIncoming(newMemAlloc, allocBB);
            phi->addIncoming(newMemRealloc, reallocBB);

            builder.CreateRet(phi);
        }
    }

    // _array_release: 释放数组内存
    {
        DEBUG_LOG("  Emitting _array_release");
        auto releaseFn = getArrayReleaseFn(module, builder);
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
            builder.SetInsertPoint(entry);

            auto args = releaseFn->args();
            auto argIt = args.begin();
            llvm::Value* dataPtr = argIt;

            // 检查是否为 null
            auto isNull = builder.CreateICmpEQ(
                dataPtr, llvm::ConstantPointerNull::get(llvm::PointerType::get(context, 0)), "is_null");

            auto freeBB = llvm::BasicBlock::Create(context, "free", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);

            builder.CreateCondBr(isNull, doneBB, freeBB);

            // 释放内存
            builder.SetInsertPoint(freeBB);
            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            builder.CreateCall(heapFreeFn, {heap, builder.getInt64(0), dataPtr});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }
}

// ==================== 程序启动 ====================

// 生成 main 启动函数
// 设置控制台编码为 UTF-8，然后调用 yux_main
void emitMainStartup(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting main startup function");

    auto setConsoleOutputCP = getSetConsoleOutputCPFn(module, builder);
    auto setConsoleCP = getSetConsoleCPFn(module, builder);

    auto fnType = llvm::FunctionType::get(builder.getInt32Ty(), {}, false);
    auto mainStartup = llvm::Function::Create(
        fnType,
        llvm::Function::ExternalLinkage,
        "mainStartup",
        module
    );
    DEBUG_LOG("  Created mainStartup function");

    auto entry = llvm::BasicBlock::Create(context, "entry", mainStartup);
    builder.SetInsertPoint(entry);

    // 设置控制台代码页为 UTF-8 (65001)
    auto cpUtf8 = llvm::ConstantInt::get(builder.getInt32Ty(), 65001);
    builder.CreateCall(setConsoleOutputCP, {cpUtf8});
    builder.CreateCall(setConsoleCP, {cpUtf8});
    DEBUG_LOG("  Set console code page to UTF-8");

    // 调用 yux_main
    auto yuxMain = module->getFunction("yux_main");
    if (yuxMain) {
        builder.CreateCall(yuxMain, {});
        DEBUG_LOG("  Called yux_main");
    } else {
        DEBUG_LOG("  yux_main not found");
    }
    builder.CreateRet(builder.getInt32(0));
}

// ==================== 通用运行时辅助 ====================

// 生成通用运行时辅助函数
void emitRuntimeHelpers(llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting runtime helpers (SDK)");

    // __chkstk: 栈检查函数 (Windows 要求)
    // 这是一个空实现，实际栈检查由链接器提供
    auto chkstkFnType = llvm::FunctionType::get(builder.getVoidTy(), {}, false);
    auto chkstk = llvm::Function::Create(
        chkstkFnType,
        llvm::Function::ExternalLinkage,
        "__chkstk",
        module
    );
    auto chkstkEntry = llvm::BasicBlock::Create(builder.getContext(), "entry", chkstk);
    builder.SetInsertPoint(chkstkEntry);
    builder.CreateRetVoid();
    DEBUG_LOG("  Emitted __chkstk");

    // _fltused: 浮点数使用标志 (Windows 要求)
    // 指示程序使用了浮点运算
    new llvm::GlobalVariable(
        *module,
        builder.getInt32Ty(),
        true,
        llvm::GlobalValue::ExternalLinkage,
        builder.getInt32(0),
        "_fltused"
    );
    DEBUG_LOG("  Created _fltused global");
}

}
