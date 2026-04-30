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
//
// Phase 1a 新布局（DRAFT §7.1）：
// Block = { strong: u32, weak: u32, payload: T }，payload 始于偏移 8
// Box 实例只持有一个 block 指针（handle）；payload 指针 = handle + 8
// 哨兵 strong == 0xFFFFFFFF 时所有 retain/release 操作 no-op（用于 .rodata 字面量；Phase 1c 启用）
// Phase 1a 仅维护 strong 计数；weak 字段已写入 layout 但仅初始化为 1，Phase 1d 接入弱引用协议时启用

// 获取 Box 内存分配函数
// 签名: ptr _box_alloc(i64 payload_size)
// 分配 8 字节 RC 头 + payload_size，初始化 strong=1, weak=1，返回 block 指针
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
// 签名: void _box_retain(ptr block)
// 哨兵跳过；否则 strong++
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
// 签名: void _box_release(ptr block)
// 哨兵跳过；否则 strong--；当 strong=0 时释放整个 block
// payload 析构由调用方在 IR 内联（Phase 1a 与现状一致；Phase 1d 加入 weak 协议）
llvm::Function* getBoxReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_box_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// ==================== Array<T> 动态数组支持（Phase 1b 新 ABI） ====================

// _array_alloc(i64 elemSize, i64 initCap, i64 initLen) -> Block*
// 分配 block + 可选 data 缓冲；strong=1, weak=1；调用方负责把元素 memcpy 进 data
llvm::Function* getArrayAllocFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_alloc";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(builder.getInt64Ty());  // elemSize
    paramTypes.push_back(builder.getInt64Ty());  // initCap
    paramTypes.push_back(builder.getInt64Ty());  // initLen

    auto fnType = llvm::FunctionType::get(
        llvm::PointerType::get(builder.getContext(), 0),
        paramTypes,
        false
    );
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// _array_grow(Block* handle, i64 elemSize, i64 newCap) -> void
// handle 必须非空；原地修改 block.cap、block.data
llvm::Function* getArrayGrowFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_grow";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));  // handle
    paramTypes.push_back(builder.getInt64Ty());  // elemSize
    paramTypes.push_back(builder.getInt64Ty());  // newCap

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// _array_release(Block* handle) -> void
// strong--；归零时 free(data) + free(block)；null/哨兵跳过
llvm::Function* getArrayReleaseFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_release";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// _array_retain(Block* handle) -> void
// strong++；null/哨兵跳过
llvm::Function* getArrayRetainFn(llvm::Module* module, llvm::IRBuilder<>& builder) {
    string fnName = "_array_retain";
    auto func = module->getFunction(fnName);
    if (func) return func;

    vector<llvm::Type*> paramTypes;
    paramTypes.push_back(llvm::PointerType::get(builder.getContext(), 0));

    auto fnType = llvm::FunctionType::get(builder.getVoidTy(), paramTypes, false);
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, module);
}

// ==================== Box 辅助函数实现 ====================

// 生成 Box 相关的辅助函数实现（Phase 1a 新布局）
// Block = { u32 strong, u32 weak, payload... }
// 哨兵：strong == 0xFFFFFFFF 表示 .rodata 字面量，所有 retain/release 跳过
void emitBoxHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Box helper functions");

    auto getProcessHeapFn = runtime::getProcessHeapFn(module, builder);
    auto heapAllocFn = runtime::getHeapAllocFn(module, builder);
    auto heapFreeFn = runtime::getHeapFreeFn(module, builder);

    auto ptrTy = llvm::PointerType::get(context, 0);
    auto i32Ty = builder.getInt32Ty();
    auto i64Ty = builder.getInt64Ty();
    auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);

    // _box_alloc: 分配 8 字节 RC 头 + payload_size，初始化 strong=1, weak=1
    {
        DEBUG_LOG("  Emitting _box_alloc");
        auto allocFn = getBoxAllocFn(module, builder);
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", allocFn);
            builder.SetInsertPoint(entry);

            llvm::Value* payloadSize = &*allocFn->arg_begin();

            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");

            // 总大小 = 8（RC 头）+ payload_size
            auto headerSize = builder.getInt64(8);
            auto totalSize = builder.CreateAdd(payloadSize, headerSize, "total_size");

            auto block = builder.CreateCall(heapAllocFn, {heap, builder.getInt64(0), totalSize}, "block");

            // 写 strong=1（offset 0）
            auto strongPtr = block;
            builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), strongPtr);
            // 写 weak=1（offset 4）
            auto weakPtr = builder.CreateGEP(builder.getInt8Ty(), block, {builder.getInt64(4)}, "weak_ptr");
            builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), weakPtr);

            builder.CreateRet(block);
        }
    }

    // _box_retain: 哨兵跳过；否则 strong++
    {
        DEBUG_LOG("  Emitting _box_retain");
        auto retainFn = getBoxRetainFn(module, builder);
        if (retainFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", retainFn);
            auto incBB = llvm::BasicBlock::Create(context, "inc", retainFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", retainFn);
            builder.SetInsertPoint(entry);

            llvm::Value* block = &*retainFn->arg_begin();

            // 哨兵检测：strong == 0xFFFFFFFF
            auto strongPtr = block;
            auto strong = builder.CreateLoad(i32Ty, strongPtr, "strong");
            auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
            builder.CreateCondBr(isSentinel, doneBB, incBB);

            builder.SetInsertPoint(incBB);
            auto newStrong = builder.CreateAdd(strong, llvm::ConstantInt::get(i32Ty, 1), "new_strong");
            builder.CreateStore(newStrong, strongPtr);
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }

    // _box_release: 哨兵跳过；否则 strong--；strong=0 时 free 整个 block
    // Phase 1a：单纯按 strong 计数管理生命周期（Phase 1d 接入 weak 协议时改造）
    {
        DEBUG_LOG("  Emitting _box_release");
        auto releaseFn = getBoxReleaseFn(module, builder);
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
            auto decBB = llvm::BasicBlock::Create(context, "dec", releaseFn);
            auto freeBB = llvm::BasicBlock::Create(context, "free", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);
            builder.SetInsertPoint(entry);

            llvm::Value* block = &*releaseFn->arg_begin();

            auto strongPtr = block;
            auto strong = builder.CreateLoad(i32Ty, strongPtr, "strong");
            auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
            builder.CreateCondBr(isSentinel, doneBB, decBB);

            builder.SetInsertPoint(decBB);
            auto newStrong = builder.CreateSub(strong, llvm::ConstantInt::get(i32Ty, 1), "new_strong");
            builder.CreateStore(newStrong, strongPtr);
            auto isZero = builder.CreateICmpEQ(newStrong, llvm::ConstantInt::get(i32Ty, 0), "is_zero");
            builder.CreateCondBr(isZero, freeBB, doneBB);

            builder.SetInsertPoint(freeBB);
            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            builder.CreateCall(heapFreeFn, {heap, builder.getInt64(0), block});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }

    (void)ptrTy;
    (void)i64Ty;
}

// ==================== Array 辅助函数实现（Phase 1b 新 ABI） ====================
// Block 字节布局: { u32 strong @0, u32 weak @4, i64 len @8, i64 cap @16, ptr data @24 }
// Block 总头部 = 32 字节；data 是间接指针指向独立 heap 缓冲

void emitArrayHelpers(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* module) {
    DEBUG_LOG("Emitting Array helper functions");

    auto getProcessHeapFn = runtime::getProcessHeapFn(module, builder);
    auto heapAllocFn = runtime::getHeapAllocFn(module, builder);
    auto heapReAllocFn = runtime::getHeapReAllocFn(module, builder);
    auto heapFreeFn = runtime::getHeapFreeFn(module, builder);

    auto ptrTy = llvm::PointerType::get(context, 0);
    auto i32Ty = builder.getInt32Ty();
    auto i64Ty = builder.getInt64Ty();
    auto i8Ty = builder.getInt8Ty();
    auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    auto i64C = [&](int64_t v) { return builder.getInt64(v); };

    // _array_alloc(elemSize, initCap, initLen) -> Block*
    // 分配 32 字节 Block；如 initCap > 0 则额外分配 initCap*elemSize 数据缓冲，否则 data=null
    {
        DEBUG_LOG("  Emitting _array_alloc");
        auto allocFn = getArrayAllocFn(module, builder);
        if (allocFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", allocFn);
            auto allocDataBB = llvm::BasicBlock::Create(context, "alloc_data", allocFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", allocFn);
            builder.SetInsertPoint(entry);

            auto argIt = allocFn->args().begin();
            llvm::Value* elemSize = argIt; elemSize->setName("elem_size"); ++argIt;
            llvm::Value* initCap = argIt; initCap->setName("init_cap"); ++argIt;
            llvm::Value* initLen = argIt; initLen->setName("init_len");

            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            auto block = builder.CreateCall(heapAllocFn, {heap, i64C(0), i64C(32)}, "block");

            builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), block);                              // strong @0
            auto weakPtr = builder.CreateGEP(i8Ty, block, {i64C(4)}, "weak_ptr");
            builder.CreateStore(llvm::ConstantInt::get(i32Ty, 1), weakPtr);                            // weak @4
            auto lenPtr = builder.CreateGEP(i8Ty, block, {i64C(8)}, "len_ptr");
            builder.CreateStore(initLen, lenPtr);                                                       // len @8
            auto capPtr = builder.CreateGEP(i8Ty, block, {i64C(16)}, "cap_ptr");
            builder.CreateStore(initCap, capPtr);                                                       // cap @16
            auto dataFieldPtr = builder.CreateGEP(i8Ty, block, {i64C(24)}, "data_field_ptr");
            builder.CreateStore(nullPtr, dataFieldPtr);                                                 // data @24 = null（默认）

            auto needData = builder.CreateICmpSGT(initCap, i64C(0), "need_data");
            builder.CreateCondBr(needData, allocDataBB, doneBB);

            builder.SetInsertPoint(allocDataBB);
            auto byteSize = builder.CreateMul(initCap, elemSize, "byte_size");
            auto dataMem = builder.CreateCall(heapAllocFn, {heap, i64C(0), byteSize}, "data_mem");
            builder.CreateStore(dataMem, dataFieldPtr);
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRet(block);
        }
    }

    // _array_grow(handle, elemSize, newCap) -> void
    // handle 必须非空；realloc data 缓冲并更新 block.cap、block.data
    {
        DEBUG_LOG("  Emitting _array_grow");
        auto growFn = getArrayGrowFn(module, builder);
        if (growFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", growFn);
            auto allocBB = llvm::BasicBlock::Create(context, "alloc", growFn);
            auto reallocBB = llvm::BasicBlock::Create(context, "realloc", growFn);
            auto storeBB = llvm::BasicBlock::Create(context, "store", growFn);
            builder.SetInsertPoint(entry);

            auto argIt = growFn->args().begin();
            llvm::Value* handle = argIt; handle->setName("handle"); ++argIt;
            llvm::Value* elemSize = argIt; elemSize->setName("elem_size"); ++argIt;
            llvm::Value* newCap = argIt; newCap->setName("new_cap");

            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");

            auto capPtr = builder.CreateGEP(i8Ty, handle, {i64C(16)}, "cap_ptr");
            auto dataFieldPtr = builder.CreateGEP(i8Ty, handle, {i64C(24)}, "data_field_ptr");
            auto oldData = builder.CreateLoad(ptrTy, dataFieldPtr, "old_data");
            auto newByteSize = builder.CreateMul(newCap, elemSize, "new_byte_size");

            auto isNull = builder.CreateICmpEQ(oldData, nullPtr, "data_is_null");
            builder.CreateCondBr(isNull, allocBB, reallocBB);

            builder.SetInsertPoint(allocBB);
            auto allocedData = builder.CreateCall(heapAllocFn, {heap, i64C(0), newByteSize}, "alloced_data");
            builder.CreateBr(storeBB);

            builder.SetInsertPoint(reallocBB);
            auto realloced = builder.CreateCall(heapReAllocFn, {heap, i64C(0), oldData, newByteSize}, "realloced");
            builder.CreateBr(storeBB);

            builder.SetInsertPoint(storeBB);
            auto phi = builder.CreatePHI(ptrTy, 2, "new_data");
            phi->addIncoming(allocedData, allocBB);
            phi->addIncoming(realloced, reallocBB);
            builder.CreateStore(phi, dataFieldPtr);
            builder.CreateStore(newCap, capPtr);
            builder.CreateRetVoid();
        }
    }

    // _array_retain(handle) -> void
    // null/哨兵跳过；否则 strong++
    {
        DEBUG_LOG("  Emitting _array_retain");
        auto retainFn = getArrayRetainFn(module, builder);
        if (retainFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", retainFn);
            auto checkBB = llvm::BasicBlock::Create(context, "check", retainFn);
            auto incBB = llvm::BasicBlock::Create(context, "inc", retainFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", retainFn);
            builder.SetInsertPoint(entry);

            llvm::Value* handle = &*retainFn->arg_begin();
            auto isNull = builder.CreateICmpEQ(handle, nullPtr, "is_null");
            builder.CreateCondBr(isNull, doneBB, checkBB);

            builder.SetInsertPoint(checkBB);
            auto strong = builder.CreateLoad(i32Ty, handle, "strong");
            auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
            builder.CreateCondBr(isSentinel, doneBB, incBB);

            builder.SetInsertPoint(incBB);
            auto newStrong = builder.CreateAdd(strong, llvm::ConstantInt::get(i32Ty, 1), "new_strong");
            builder.CreateStore(newStrong, handle);
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }

    // _array_release(handle) -> void
    // null/哨兵跳过；strong--；归零时 free(data) + free(block)
    {
        DEBUG_LOG("  Emitting _array_release");
        auto releaseFn = getArrayReleaseFn(module, builder);
        if (releaseFn->empty()) {
            auto entry = llvm::BasicBlock::Create(context, "entry", releaseFn);
            auto checkBB = llvm::BasicBlock::Create(context, "check", releaseFn);
            auto decBB = llvm::BasicBlock::Create(context, "dec", releaseFn);
            auto freeBB = llvm::BasicBlock::Create(context, "free", releaseFn);
            auto freeDataBB = llvm::BasicBlock::Create(context, "free_data", releaseFn);
            auto freeBlockBB = llvm::BasicBlock::Create(context, "free_block", releaseFn);
            auto doneBB = llvm::BasicBlock::Create(context, "done", releaseFn);
            builder.SetInsertPoint(entry);

            llvm::Value* handle = &*releaseFn->arg_begin();
            auto isNull = builder.CreateICmpEQ(handle, nullPtr, "is_null");
            builder.CreateCondBr(isNull, doneBB, checkBB);

            builder.SetInsertPoint(checkBB);
            auto strong = builder.CreateLoad(i32Ty, handle, "strong");
            auto isSentinel = builder.CreateICmpEQ(strong, sentinel, "is_sentinel");
            builder.CreateCondBr(isSentinel, doneBB, decBB);

            builder.SetInsertPoint(decBB);
            auto newStrong = builder.CreateSub(strong, llvm::ConstantInt::get(i32Ty, 1), "new_strong");
            builder.CreateStore(newStrong, handle);
            auto isZero = builder.CreateICmpEQ(newStrong, llvm::ConstantInt::get(i32Ty, 0), "is_zero");
            builder.CreateCondBr(isZero, freeBB, doneBB);

            builder.SetInsertPoint(freeBB);
            // free(data) if data != null
            auto dataFieldPtr = builder.CreateGEP(i8Ty, handle, {i64C(24)}, "data_field_ptr");
            auto data = builder.CreateLoad(ptrTy, dataFieldPtr, "data");
            auto dataIsNull = builder.CreateICmpEQ(data, nullPtr, "data_is_null");
            builder.CreateCondBr(dataIsNull, freeBlockBB, freeDataBB);

            builder.SetInsertPoint(freeDataBB);
            auto heap = builder.CreateCall(getProcessHeapFn, {}, "heap");
            builder.CreateCall(heapFreeFn, {heap, i64C(0), data});
            builder.CreateBr(freeBlockBB);

            builder.SetInsertPoint(freeBlockBB);
            auto heap2 = builder.CreateCall(getProcessHeapFn, {}, "heap");
            builder.CreateCall(heapFreeFn, {heap2, i64C(0), handle});
            builder.CreateBr(doneBB);

            builder.SetInsertPoint(doneBB);
            builder.CreateRetVoid();
        }
    }

    (void)ptrTy;
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
