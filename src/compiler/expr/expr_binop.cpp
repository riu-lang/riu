// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 二元 / 算术 / 比较表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "analyzer/symbol_suggest.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <set>

// ==================== 自定义类型运算符方法调用 ====================

// 编译自定义类型的二元运算符方法调用
// 将运算符表达式转换为方法调用，如 a + b -> a.plus(b)
llvm::Value* Compiler::compileCustomTypeBinaryOp(p<ExprNode> leftExpr, p<ExprNode> rightExpr, const TypeInfo& leftType,
                                                 const string& methodName, int lineNum) {

    DEBUG_LOG_VAL("    Expr: CustomTypeBinaryOp", leftType.name << "." << methodName);

    // 获取左操作数的指针
    llvm::Value* leftPtr = nullptr;
    if (auto leftLiteral = dynamic_cast<ExprLiteralNode*>(leftExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(leftLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                leftPtr = it->second;
            }
        }
    }

    if (!leftPtr) {
        // v0.16: [] 返回 T&——leftVal 已是指针，直接用作 leftPtr，不再包一层 alloca
        if (leftType.isRef()) {
            leftPtr = compileExpr(leftExpr);
        } else {
            auto leftVal = compileExpr(leftExpr);
            auto structType = getLLVMType(leftType);
            if (!structType) {
                throw YuxError(lineNum, ErrorCode::E3096, leftType.name);
            }
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_lhs_tmp");
            _builder.CreateStore(leftVal, alloca);
            leftPtr = alloca;
        }
    }

    auto rightType = rightExpr->getType();
    // Phase 4b: 右操作数是 T& 字面变量时，从 _localVarPtrs 直接取裸 ptr，
    // 避免 compileExpr 对 ref 自动 load 出 struct 值后又 alloca 写回 —— 写回会
    // 把 struct 看成是 ptr 类型，触发 LLVM 签名校验失败。
    llvm::Value* rightVal = nullptr;
    if (rightType.isRef()) {
        if (auto rl = dynamic_cast<ExprLiteralNode*>(rightExpr)) {
            if (auto ol = dynamic_cast<LiteralObjNode*>(rl->literal())) {
                auto vn = ol->getValue().getText();
                auto rit = _localVarPtrs.find(vn);
                if (rit != _localVarPtrs.end()) rightVal = rit->second;
            }
        }
    }
    if (!rightVal) {
        rightVal = compileExpr(rightExpr);
    }

    // Phase 4b: 操作数本身是 T& 时（如 fn 形参 `actual String&`），剥掉一层 Ref
    // 与方法注册的 [Self, Self&] 对齐；不剥则 lookup 失败导致调用方编译期崩溃。
    // v0.6 Phase 2b: 透明类型别名解析，使 `A = i32` 这类别名走到运算符方法时
    // 仍能匹配到 `i32.plus` 等内置方法。
    TypeInfo effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;
    TypeInfo effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;
    effLeftType = applySubst(effLeftType);
    effRightType = applySubst(effRightType);
    string methodFullName = effLeftType.name + "." + methodName;

    // 查找方法（spec §7.2.3.3）：二元运算符方法形参可为任意类型，
    // 编译器遍历 leftType 的全部同名方法候选，按优先级匹配右操作数类型：
    //   1. 精确匹配：形参非 Ref，类型一致，无需自动取址
    //   2. 自动取址匹配：形参为 Ref<T>，T 与右操作数类型一致（§7.2.3.6）
    // 同一优先级内多个候选歧义 → E6014；无匹配 → E3073。
    vector<FnSymbolInfo*> candidates;
    _file->collectFnOverloads(methodFullName, candidates);
    if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
        _yux->sdkFile()->collectFnOverloads(methodFullName, candidates);
    }
    // 去重：_parentScope 递归 + SDK 直查可能返回同一 FnSymbolInfo*
    std::ranges::sort(candidates);
    auto [dupFirst, dupLast] = std::ranges::unique(candidates);
    candidates.erase(dupFirst, dupLast);

    FnSymbolInfo* methodSymbol = nullptr;
    vector<FnSymbolInfo*> exactMatches;
    vector<FnSymbolInfo*> refMatches;

    for (auto* cand : candidates) {
        // 二元运算符方法签名：params[0] = 接收者, params[1] = 右操作数
        if (cand->params.size() != 2) continue;
        const TypeInfo& candParam = cand->params[1];

        if (!candParam.isRef() && candParam == effRightType) {
            exactMatches.push_back(cand);
        } else if (candParam.isRef()) {
            auto refElem = candParam.refElementType();
            if (refElem && *refElem == effRightType) {
                refMatches.push_back(cand);
            }
        }
    }

    if (exactMatches.size() > 1) {
        string sigs;
        for (auto* m : exactMatches) {
            if (!sigs.empty()) sigs += " | ";
            sigs += effLeftType.name + "." + methodName + "(" + m->params[1].name + ")";
        }
        throw YuxError(lineNum, ErrorCode::E6014, methodFullName, effRightType.name, exactMatches.size(), sigs);
    }
    if (exactMatches.size() == 1) {
        methodSymbol = exactMatches[0];
    } else if (refMatches.size() > 1) {
        string sigs;
        for (auto* m : refMatches) {
            if (!sigs.empty()) sigs += " | ";
            sigs += effLeftType.name + "." + methodName + "(" + m->params[1].name + ")";
        }
        throw YuxError(lineNum, ErrorCode::E6014, methodFullName, effRightType.name, refMatches.size(), sigs);
    } else if (refMatches.size() == 1) {
        methodSymbol = refMatches[0];
    } else {
        // Bucket 6 (CURRENT-check.md): E3073 无匹配候选,
        // 这里保留作幂等防御性双跑 (sema 已抛, 正常情况到不了).
        FileNode* sdkFilePtr = (_yux && _yux->sdkFile()) ? _yux->sdkFile() : nullptr;
        sema::validateBinOpMethodResolution(_file, sdkFilePtr, effLeftType, effRightType, methodName, lineNum, 0);
        // unreachable: helper 内必抛
    }

    // 准备方法参数
    vector<llvm::Value*> methodArgs;
    methodArgs.push_back(leftPtr);

    // 确定右操作数的 ABI 传递方式（spec §7.2.3.3）：
    // 形参为 Ref<T> 或 structParamUsesPointer → 传指针；其余按值。
    // 与 getLLVMFunctionType / compileFn 的方法形参 ABI 保持一致。
    TypeInfo declaredRhsType = methodSymbol->params.size() >= 2 ? methodSymbol->params[1] : rightType;
    bool rhsByPtr = declaredRhsType.isRef() || structParamUsesPointer(declaredRhsType.name);

    if (rhsByPtr) {
        // 形参期望指针
        if (rightType.isRef()) {
            // 右操作数本身是引用，rightVal 已是指针，直接传
            methodArgs.push_back(rightVal);
        } else {
            // 右操作数是值，取址后传递
            auto structType = getLLVMType(effRightType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_rhs_tmp");
            _builder.CreateStore(rightVal, alloca);
            methodArgs.push_back(alloca);
        }
    } else {
        // 形参期望值（内置标量 / 平凡结构体）
        if (rightType.isRef()) {
            // 右操作数是引用，需要 load 后按值传递
            auto structType = getLLVMType(effRightType);
            auto loaded = _builder.CreateLoad(structType, rightVal, "op_rhs_val");
            methodArgs.push_back(loaded);
        } else {
            // 右操作数是值，直接传递
            methodArgs.push_back(rightVal);
        }
    }

    // 获取或创建方法函数
    // mangle 与 LLVM 签名都按 methodSymbol 实际声明的形参类型走
    // （可为任意类型，spec §7.2.3.3），与 SDK / 用户代码侧定义的方法符号一一对应。
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    vector<TypeInfo> argTypes;
    argTypes.push_back(declaredRhsType);
    string mangledName = Mangler::method(ownerMod, effLeftType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        if (rhsByPtr) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            paramTypes.push_back(getLLVMType(declaredRhsType));
        }
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }

    return _builder.CreateCall(fn, methodArgs);
}

llvm::Value* Compiler::compileAddSubExpr(p<ExprAddSubNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    // v0.6 Phase 2b: 透明别名解析，使 `A = i32` 后 `A + A` 仍走内置算子路径
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref，使内置类型检查落在标量名上
    auto effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;

    string opStr = (node->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
    DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type.name);

    // v0.6 Phase 2c：`+` 表达式整体类型为 String 时，
    // 走 StringBuilder 整链 lower（多段连续 `+` 合并为单次 builder）。
    if (node->op() == ExprAddSubNode::Op::Add && type.name == "String") {
        return compileStringPlusChain(node);
    }

    // 检查是否为自定义类型（用剥 Ref 后的标量名）
    if (!isBuiltinType(effLeftType.name)) {
        string methodName = (node->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = effLeftType.startsWith('f');

    // v0.16: 操作数若是 T&（如 arr[i]）则 load 出值
    if (leftType.isRef()) {
        left = _builder.CreateLoad(getLLVMType(effLeftType), left, "add_lhs");
    }
    auto rightType = applySubst(node->right()->getType());
    if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effLeftType), right, "add_rhs");
    }

    if (node->op() == ExprAddSubNode::Op::Add) {
        if (isFloat) {
            return _builder.CreateFAdd(left, right);
        }
        return _builder.CreateAdd(left, right);
    } else {
        if (isFloat) {
            return _builder.CreateFSub(left, right);
        }
        return _builder.CreateSub(left, right);
    }
}

llvm::Value* Compiler::compileMulDivModExpr(p<ExprMulDivModNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref
    auto effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;

    string opStr;
    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul:
        opStr = "*";
        break;
    case ExprMulDivModNode::Op::Div:
        opStr = "/";
        break;
    case ExprMulDivModNode::Op::Mod:
        opStr = "%";
        break;
    }
    DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type.name);

    // 检查是否为自定义类型（用剥 Ref 后的标量名）
    if (!isBuiltinType(effLeftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul:
            methodName = "mul";
            break;
        case ExprMulDivModNode::Op::Div:
            methodName = "div";
            break;
        case ExprMulDivModNode::Op::Mod:
            methodName = "mod";
            break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = effLeftType.startsWith('f');
    bool isUnsigned = effLeftType.startsWith('u');

    // v0.16: 操作数若是 T& 则 load 出值
    if (leftType.isRef()) {
        left = _builder.CreateLoad(getLLVMType(effLeftType), left, "mul_lhs");
    }
    auto rightType = applySubst(node->right()->getType());
    if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effLeftType), right, "mul_rhs");
    }

    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul:
        if (isFloat) {
            return _builder.CreateFMul(left, right);
        }
        return _builder.CreateMul(left, right);
    case ExprMulDivModNode::Op::Div:
        if (isFloat) {
            return _builder.CreateFDiv(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateUDiv(left, right);
        }
        return _builder.CreateSDiv(left, right);
    case ExprMulDivModNode::Op::Mod:
        if (isFloat) {
            return _builder.CreateFRem(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateURem(left, right);
        }
        return _builder.CreateSRem(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3077);
}

llvm::Value* Compiler::compileBinOpExpr(p<ExprBinOpNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref
    auto effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;

    string opStr;
    switch (node->op()) {
    case ExprBinOpNode::Op::And:
        opStr = "&";
        break;
    case ExprBinOpNode::Op::Or:
        opStr = "|";
        break;
    case ExprBinOpNode::Op::Xor:
        opStr = "^";
        break;
    case ExprBinOpNode::Op::Shl:
        opStr = "<<";
        break;
    case ExprBinOpNode::Op::Shr:
        opStr = ">>";
        break;
    }
    DEBUG_LOG_VAL("    Expr: BinOp", opStr << " : " << type.name);

    // 检查是否为自定义类型（用剥 Ref 后的标量名）
    if (!isBuiltinType(effLeftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprBinOpNode::Op::And:
            methodName = "and";
            break;
        case ExprBinOpNode::Op::Or:
            methodName = "or";
            break;
        case ExprBinOpNode::Op::Xor:
            methodName = "xor";
            break;
        case ExprBinOpNode::Op::Shl:
            methodName = "shl";
            break;
        case ExprBinOpNode::Op::Shr:
            methodName = "shr";
            break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());

    // v0.16: 操作数若是 T& 则 load 出值
    if (leftType.isRef()) {
        left = _builder.CreateLoad(getLLVMType(effLeftType), left, "binop_lhs");
    }
    auto rightType = applySubst(node->right()->getType());
    if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effLeftType), right, "binop_rhs");
    }

    switch (node->op()) {
    case ExprBinOpNode::Op::And:
        return _builder.CreateAnd(left, right);
    case ExprBinOpNode::Op::Or:
        return _builder.CreateOr(left, right);
    case ExprBinOpNode::Op::Xor:
        return _builder.CreateXor(left, right);
    case ExprBinOpNode::Op::Shl:
        return _builder.CreateShl(left, right);
    case ExprBinOpNode::Op::Shr:
        if (type.startsWith('u')) {
            return _builder.CreateLShr(left, right);
        }
        return _builder.CreateAShr(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3075);
}

llvm::Value* Compiler::compileCompareExpr(p<ExprCompareNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    (void)node->getType();
    auto leftType = applySubst(node->left()->getType());
    auto rightType = applySubst(node->right()->getType());
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref 用于类型匹配
    auto effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;
    auto effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;

    if (effLeftType != effRightType) {
        // spec §7.2.3.3: 非内置类型允许跨类型比较，类型匹配由方法解析完成；
        // 内置类型跨类型时 getType 已抛 E3004 (kMigratedCodes)，此处不可达。
        if (isBuiltinType(effLeftType.name)) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3004, effLeftType.name, effRightType.name);
        }
    }

    // Bucket 6 (CURRENT-check.md): E3078 (Weak ==/!=) + E3073 (Ptr ordering) 形态校验
    // 抠到 sema::validateCompareOpForm; SemaPass 已接管实际抛出点, 此处幂等防御性双跑.
    sema::validateCompareOpForm(effLeftType, node->op(), node->getLineNumber(), node->getColumn());

    // Ptr：内置 == / !=（用于 `p == null` 等场景）
    if (effLeftType.isPtr()) {
        if (node->op() == ExprCompareNode::Op::Eq || node->op() == ExprCompareNode::Op::Ne) {
            auto left = compileExpr(node->left());
            auto right = compileExpr(node->right());
            // v0.16: Ptr& 需 load 后比较
            if (leftType.isRef()) {
                left = _builder.CreateLoad(getLLVMType(effLeftType), left, "ptr_cmp_lhs");
            }
            if (rightType.isRef()) {
                right = _builder.CreateLoad(getLLVMType(effRightType), right, "ptr_cmp_rhs");
            }
            if (node->op() == ExprCompareNode::Op::Eq) {
                return _builder.CreateICmpEQ(left, right);
            }
            return _builder.CreateICmpNE(left, right);
        }
    }

    string opStr;
    switch (node->op()) {
    case ExprCompareNode::Op::Eq:
        opStr = "==";
        break;
    case ExprCompareNode::Op::Ne:
        opStr = "!=";
        break;
    case ExprCompareNode::Op::Lt:
        opStr = "<";
        break;
    case ExprCompareNode::Op::Le:
        opStr = "<=";
        break;
    case ExprCompareNode::Op::Gt:
        opStr = ">";
        break;
    case ExprCompareNode::Op::Ge:
        opStr = ">=";
        break;
    case ExprCompareNode::Op::AndAnd:
        opStr = "&&";
        break;
    case ExprCompareNode::Op::OrOr:
        opStr = "||";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << effLeftType.name);

    // && 和 || 是逻辑运算符，不转换为方法调用
    if (node->op() == ExprCompareNode::Op::AndAnd || node->op() == ExprCompareNode::Op::OrOr) {
        auto left = compileExpr(node->left());
        auto right = compileExpr(node->right());
        // v0.16: bool& 自动 load
        if (leftType.isRef()) {
            left = _builder.CreateLoad(getLLVMType(effLeftType), left, "logical_lhs");
        }
        if (rightType.isRef()) {
            right = _builder.CreateLoad(getLLVMType(effRightType), right, "logical_rhs");
        }
        if (node->op() == ExprCompareNode::Op::AndAnd) {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.rhs");
            return _builder.CreateAnd(leftBool, rightBool, "and");
        } else {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.rhs");
            return _builder.CreateOr(leftBool, rightBool, "or");
        }
    }

    // 检查是否为自定义类型（用剥 Ref 后的标量名）
    if (!isBuiltinType(effLeftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprCompareNode::Op::Eq:
            methodName = "eq";
            break;
        case ExprCompareNode::Op::Ne:
            methodName = "ne";
            break;
        case ExprCompareNode::Op::Lt:
            methodName = "lt";
            break;
        case ExprCompareNode::Op::Le:
            methodName = "le";
            break;
        case ExprCompareNode::Op::Gt:
            methodName = "gt";
            break;
        case ExprCompareNode::Op::Ge:
            methodName = "ge";
            break;
        default:
            break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = effLeftType.startsWith('f');
    bool isUnsigned = effLeftType.startsWith('u');

    // v0.16: 操作数若是 T&（如 arr[i]）则 load 出值
    if (leftType.isRef()) {
        left = _builder.CreateLoad(getLLVMType(effLeftType), left, "cmp_lhs");
    }
    if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effRightType), right, "cmp_rhs");
    }

    switch (node->op()) {
    case ExprCompareNode::Op::Eq:
        if (isFloat) {
            return _builder.CreateFCmpOEQ(left, right);
        }
        return _builder.CreateICmpEQ(left, right);
    case ExprCompareNode::Op::Ne:
        if (isFloat) {
            return _builder.CreateFCmpONE(left, right);
        }
        return _builder.CreateICmpNE(left, right);
    case ExprCompareNode::Op::Lt:
        if (isFloat) {
            return _builder.CreateFCmpOLT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULT(left, right);
        }
        return _builder.CreateICmpSLT(left, right);
    case ExprCompareNode::Op::Le:
        if (isFloat) {
            return _builder.CreateFCmpOLE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULE(left, right);
        }
        return _builder.CreateICmpSLE(left, right);
    case ExprCompareNode::Op::Gt:
        if (isFloat) {
            return _builder.CreateFCmpOGT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGT(left, right);
        }
        return _builder.CreateICmpSGT(left, right);
    case ExprCompareNode::Op::Ge:
        if (isFloat) {
            return _builder.CreateFCmpOGE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGE(left, right);
        }
        return _builder.CreateICmpSGE(left, right);
    default:
        break;
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3076);
}
