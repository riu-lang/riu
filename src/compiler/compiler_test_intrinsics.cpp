// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 测试断言内建实现 (#Test 断言 API，spec §11.3.5)
//
// 为 sdk/yux/src/yux/core/assert.yux 中以 #CompilerInner 占位的 4 个断言函数
// 在调用点合成 IR：
// - assert_eq:<T>(actual, expected)  T ∈ i8..u64 / f32 / f64 / bool（其他类型 E6030）
// - assert_true(actual bool)
// - assert_false(actual bool)
// - fail(msg String)              v1 忽略 msg 内容
//
// 失败路径统一为：调 SDK `_yux_test_assert_failed()` → RaiseException(0xE0FA17ED)
// → 由 yux test 的 SEH wrapper 翻译为 ASSERT_FAILED 显示。
// v1 不在 IR 中打印断言种类 / 实参值 / msg 文本，待 String stringify 扩展同期补齐。

#include "compiler.h"
#include "compiler_test_intrinsics.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

namespace {

// 获取或声明 SDK 侧 `_yux_test_assert_failed()` 函数
// 在用户测试模块编译时声明为 external；链接时由 sdk yux.lib / JIT 时由 sdk core.obj 提供
llvm::Function* getAssertFailedFn(llvm::Module* module) {
    // 私有 fn (`_` 前缀): mangler 模块前缀 "yux.core_" + 源名 "_yux_test_assert_failed" + "()"
    // 形成 "yux.core__yux_test_assert_failed()"
    string name = Mangler::function("yux.core", "_yux_test_assert_failed", {}, true);
    auto fn = module->getFunction(name);
    if (fn) return fn;
    auto fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(module->getContext()), {}, false);
    return llvm::Function::Create(
        fnTy, llvm::Function::ExternalLinkage, name, module);
}

// 在 cond 为真（"失败"）时跳转到失败块：调 _yux_test_assert_failed() → unreachable
// cond 为假时继续 fallthrough 到 contBB
// fnCtx 为当前正在编译的 LLVM 函数（用于附加新 BasicBlock）
void emitAssertFailureBranch(
    llvm::IRBuilder<>& builder, llvm::Module* module,
    llvm::Value* failCond, const string& siteName)
{
    auto& ctx = module->getContext();
    auto fnCtx = builder.GetInsertBlock()->getParent();
    auto failBB = llvm::BasicBlock::Create(ctx, siteName + ".fail", fnCtx);
    auto contBB = llvm::BasicBlock::Create(ctx, siteName + ".cont", fnCtx);
    builder.CreateCondBr(failCond, failBB, contBB);

    builder.SetInsertPoint(failBB);
    auto failedFn = getAssertFailedFn(module);
    builder.CreateCall(failedFn, {});
    // _yux_test_assert_failed 通过 RaiseException 抛 SEH 异常，正常控制流不返回；
    // unreachable 让 LLVM 优化掉后续路径
    builder.CreateUnreachable();

    builder.SetInsertPoint(contBB);
}

}

// ==================== assert_eq:<T> ====================
//
// 整型 (i8..u64): ICmpEQ
// 浮点 (f32, f64): FCmpOEQ (ordered: NaN != NaN，与 IEEE 754 == 同义)
// bool: ICmpEQ on i1
// 其他: E6030

llvm::Value* Compiler::compileTestAssertEq(
    p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
    const TypeInfo& typeArg)
{
    if (args.size() != 2) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6027, "assert_eq", 2);
    }

    const string& tname = typeArg.name;
    bool isInt = (tname == "i8" || tname == "u8" || tname == "i16" || tname == "u16" ||
                  tname == "i32" || tname == "u32" || tname == "i64" || tname == "u64");
    bool isBool = (tname == "bool");
    bool isFloat = (tname == "f32" || tname == "f64");

    if (!(isInt || isBool || isFloat)) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6030, typeArg.getFullName());
    }

    // 两个实参必须 LLVM 类型一致：unify 漏配 / 显式 turbofish 与字面量不匹配 / 等情况
    // 这里若不拦，LLVM 的 CreateICmpEQ / CreateFCmpOEQ 会触发 same-type 断言导致编译器崩溃
    // 典型触发：`assert_eq(arr.len(), 3)` —— len() 返 i64，字面量 3 默认 i32
    // 用 LLVM 类型比较（而非 TypeInfo），以便类型别名 / 同底层类型不同别名 仍视为相等
    if (args[0]->getType() != args[1]->getType()) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6031, argTypes[0].getFullName(), argTypes[1].getFullName());
    }

    llvm::Value* eq;
    if (isFloat) {
        eq = _builder.CreateFCmpOEQ(args[0], args[1], "assert_eq.cmp");
    } else {
        eq = _builder.CreateICmpEQ(args[0], args[1], "assert_eq.cmp");
    }
    auto neq = _builder.CreateNot(eq, "assert_eq.neq");
    emitAssertFailureBranch(_builder, _module, neq, "assert_eq");
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== assert_true(bool) ====================

llvm::Value* Compiler::compileTestAssertTrue(
    p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& /*argTypes*/)
{
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6027, "assert_true", 1);
    }
    auto failCond = _builder.CreateNot(args[0], "assert_true.neg");
    emitAssertFailureBranch(_builder, _module, failCond, "assert_true");
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== assert_false(bool) ====================

llvm::Value* Compiler::compileTestAssertFalse(
    p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& /*argTypes*/)
{
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6027, "assert_false", 1);
    }
    // 失败 = actual 为真 → 直接用 args[0] 作 cond
    emitAssertFailureBranch(_builder, _module, args[0], "assert_false");
    return llvm::UndefValue::get(_builder.getVoidTy());
}

// ==================== fail(String) ====================
//
// v1：忽略 msg 文本，直接进失败路径
// msg 实参在调用点已被 compileFunctionCall 计算并装入 args[0]，
// 这里仍要让其值在 IR 中被消费，避免悬空构造的 String 没有析构 / RC 维护。
// 简化处理：args[0] 是栈上结构体值，无显式释放需求；fail 触发 RaiseException 后
// 该测试函数被 SEH 拆栈，本就没有正常的析构机会（与崩溃测试一致）。

llvm::Value* Compiler::compileTestFail(
    p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& /*argTypes*/)
{
    if (args.size() != 1) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6027, "fail", 1);
    }
    // 直接走失败路径：恒真条件
    auto trueCond = llvm::ConstantInt::getTrue(_context);
    emitAssertFailureBranch(_builder, _module, trueCond, "fail");
    return llvm::UndefValue::get(_builder.getVoidTy());
}
