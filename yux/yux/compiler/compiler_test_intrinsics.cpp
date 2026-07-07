// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 测试断言内建实现 (#Test 断言 API，spec §11.3.5)
//
// 为 sdk/yux/src/yux/core/assert.yux 中以 #Builtin 占位的 4 个断言函数
// 在调用点合成 IR：
// - assert_eq:<T>(actual, expected)  T ∈ i8..u64 / f32 / f64 / bool（其他类型 E6030）
// - assert_true(actual bool)
// - assert_false(actual bool)
// - fail(msg String)              v1 忽略 msg 内容
//
// 失败路径统一为：调 SDK `_yux_test_assert_failed()` → RaiseException(0xE0FA17ED)
// → 由 yux test 的 SEH wrapper 翻译为 ASSERT_FAILED 显示。
// v1 不在 IR 中打印断言种类 / 实参值 / msg 文本，待 String stringify 扩展同期补齐。

#include "compiler_test_intrinsics.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "compiler.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace {

// 获取或声明断言失败函数
// 始终调用 SDK 的 _yux_test_assert_failed() → RaiseException(0xE0FA17ED)
// 由 yux test (JIT) 或 yux-test-runner.exe (DLL) 的 SEH wrapper 捕获
llvm::Function* getAssertFailedFn(llvm::Module* module, FileNode* file) {
    string modName = "yux.core"; // 兜底
    if (file) {
        auto* fnSym = file->lookupFnSymbol("__yux_test_assert_failed");
        if (fnSym && !fnSym->moduleName.empty()) {
            modName = fnSym->moduleName;
        }
    }
    string name = Mangler::function(modName, "__yux_test_assert_failed", {}, true);
    auto fn = module->getFunction(name);
    if (fn) return fn;
    auto fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(module->getContext()), {}, false);
    return llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, name, module);
}

// 在 cond 为真（"失败"）时跳转到失败块：调断言失败函数 → unreachable
void emitAssertFailureBranch(llvm::IRBuilder<>& builder, llvm::Module* module, llvm::Value* failCond,
                             const string& siteName, FileNode* file) {
    auto& ctx = module->getContext();
    auto fnCtx = builder.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx, siteName + ".fail", fnCtx);
    auto contBB = llvm::BasicBlock::Create(ctx, siteName + ".cont", fnCtx);
    builder.CreateCondBr(failCond, failBB, contBB);

    builder.SetInsertPoint(failBB);
    auto failedFn = getAssertFailedFn(module, file);
    builder.CreateCall(failedFn, {});
    // RaiseException 抛 SEH，控制流不返回
    builder.CreateUnreachable();

    builder.SetInsertPoint(contBB);
}

} // namespace

// ==================== assert_eq:<T> ====================
//
// 整型 (i8..u64): ICmpEQ
// 浮点 (f32, f64): FCmpOEQ (ordered: NaN != NaN，与 IEEE 754 == 同义)
// bool: ICmpEQ on i1
// 其他: E6030

llvm::Value* Compiler::compileTestAssertEq(p<ExprCallNode> callNode, vector<llvm::Value*>& args,
                                           vector<TypeInfo>& argTypes, const TypeInfo& typeArg) {
    if (args.size() != 2) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6027, "assert_eq", 2);
    }

    TypeInfo actualTypeArg = typeArg;
    if (actualTypeArg.isRef() && actualTypeArg.refElementType()) actualTypeArg = *actualTypeArg.refElementType();
    // Auto-load T& arguments ([] returns T&, assert_eq needs T values)
    for (size_t i = 0; i < args.size(); ++i) {
        if (argTypes[i].isRef() && args[i]->getType()->isPointerTy()) {
            auto inner = *argTypes[i].refElementType();
            args[i] = _builder.CreateLoad(getLLVMType(inner), args[i], "assert.load");
            argTypes[i] = inner;
        }
    }
    const string& tname = actualTypeArg.name;
    bool isInt = (tname == "i8" || tname == "u8" || tname == "i16" || tname == "u16" || tname == "i32" ||
                  tname == "u32" || tname == "i64" || tname == "u64" || tname == "isize" || tname == "usize");
    bool isBool = (tname == "bool");
    bool isFloat = (tname == "f32" || tname == "f64");

    if (!(isInt || isBool || isFloat)) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6030, actualTypeArg.getFullName());
    }

    // 两个实参必须 LLVM 类型一致：unify 漏配 / 显式 turbofish 与字面量不匹配 / 等情况
    // 这里若不拦，LLVM 的 CreateICmpEQ / CreateFCmpOEQ 会触发 same-type 断言导致编译器崩溃
    // 典型触发：`assert_eq(arr.len(), 3)` —— len() 返 i64，字面量 3 默认 i32
    // 用 LLVM 类型比较（而非 TypeInfo），以便类型别名 / 同底层类型不同别名 仍视为相等
    if (args[0]->getType() != args[1]->getType()) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6031, argTypes[0].getFullName(),
                       argTypes[1].getFullName());
    }

    llvm::Value* eq;
    if (isFloat) {
        eq = _builder.CreateFCmpOEQ(args[0], args[1], "assert_eq.cmp");
    } else {
        eq = _builder.CreateICmpEQ(args[0], args[1], "assert_eq.cmp");
    }
    auto neq = _builder.CreateNot(eq, "assert_eq.neq");
    emitAssertFailureBranch(_builder, _module, neq, "assert_eq", _file);
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== assert_true(bool) ====================

llvm::Value* Compiler::compileTestAssertTrue(p<ExprCallNode> callNode, vector<llvm::Value*>& args,
                                             vector<TypeInfo>& argTypes) {
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6027, "assert_true", 1);
    }
    // Rc<T> auto-deref: 从 Rc struct { ptr handle } 提取 payload 内 T 值
    if (argTypes[0].isRc()) {
        auto inner = argTypes[0].rcElementType();
        if (inner && inner->name == "bool") {
            auto handle = _builder.CreateExtractValue(args[0], {0}, "assert.rc.handle");
            auto payload =
                _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "assert.rc.payload");
            args[0] = _builder.CreateLoad(_builder.getInt1Ty(), payload, "assert.rc.bool");
        }
    }
    if (args[0]->getType()->isPointerTy()) args[0] = _builder.CreateLoad(_builder.getInt1Ty(), args[0], "assert.load");
    auto failCond = _builder.CreateNot(args[0], "assert_true.neg");
    emitAssertFailureBranch(_builder, _module, failCond, "assert_true", _file);
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== assert_false(bool) ====================

llvm::Value* Compiler::compileTestAssertFalse(p<ExprCallNode> callNode, vector<llvm::Value*>& args,
                                              vector<TypeInfo>& argTypes) {
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6027, "assert_false", 1);
    }
    // Rc<T> auto-deref: 从 Rc struct { ptr handle } 提取 payload 内 T 值
    if (argTypes[0].isRc()) {
        auto inner = argTypes[0].rcElementType();
        if (inner && inner->name == "bool") {
            auto handle = _builder.CreateExtractValue(args[0], {0}, "assert.rc.handle");
            auto payload =
                _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "assert.rc.payload");
            args[0] = _builder.CreateLoad(_builder.getInt1Ty(), payload, "assert.rc.bool");
        }
    }
    if (args[0]->getType()->isPointerTy()) args[0] = _builder.CreateLoad(_builder.getInt1Ty(), args[0], "assert.load");
    emitAssertFailureBranch(_builder, _module, args[0], "assert_false", _file);
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== fail(String) ====================
//
// v1：忽略 msg 文本，直接进失败路径
// msg 实参在调用点已被 compileFunctionCall 计算并装入 args[0]，
// 这里仍要让其值在 IR 中被消费，避免悬空构造的 String 没有析构 / RC 维护。
// 简化处理：args[0] 是栈上结构体值，无显式释放需求；fail 触发 RaiseException 后
// 该测试函数被 SEH 拆栈，本就没有正常的析构机会（与崩溃测试一致）。

llvm::Value* Compiler::compileTestFail(p<ExprCallNode> callNode, vector<llvm::Value*>& args,
                                       vector<TypeInfo>& /*argTypes*/) {
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6027, "fail", 1);
    }
    // 直接走失败路径：恒真条件
    auto trueCond = llvm::ConstantInt::getTrue(_context);
    emitAssertFailureBranch(_builder, _module, trueCond, "fail", _file);
    return llvm::UndefValue::get(_builder.getVoidTy());
}
