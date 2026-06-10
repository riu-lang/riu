// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 一元 / 取引用 / 括号表达式编译：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
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

// 编译自定义类型的一元运算符方法调用
// 将运算符表达式转换为方法调用，如 -a -> a.neg()
llvm::Value* Compiler::compileCustomTypeUnaryOp(p<ExprNode> expr, const TypeInfo& type, const string& methodName,
                                                int lineNum) {

    // v0.16: [] 返回 T&——剥 Ref 用于方法名查找
    auto effType = type.isRef() ? *type.refElementType() : type;

    DEBUG_LOG_VAL("    Expr: CustomTypeUnaryOp", effType.name << "." << methodName);

    // 获取操作数的指针
    llvm::Value* ptr = nullptr;
    if (type.isRef()) {
        // v0.16: [] 返回 T&——compileExpr 已返回指针，直接用作 self ptr
        ptr = compileExpr(expr);
    } else {
        if (auto literal = dynamic_cast<ExprLiteralNode*>(expr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal->literal())) {
                auto varName = objLiteral->getValue().getText();
                auto it = _localVarPtrs.find(varName);
                if (it != _localVarPtrs.end()) {
                    ptr = it->second;
                }
            }
        }

        if (!ptr) {
            auto val = compileExpr(expr);
            auto structType = getLLVMType(effType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_tmp");
            _builder.CreateStore(val, alloca);
            ptr = alloca;
        }
    }

    // 查找方法
    string methodFullName = effType.name + "." + methodName;
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(effType);

    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }

    if (!methodSymbol) {
        throw YuxError(lineNum, ErrorCode::E3074, effType.name,
                       methodName == "neg"   ? "-"
                       : methodName == "inv" ? "~"
                       : methodName == "not" ? "!"
                                             : methodName,
                       methodName);
    }

    // 获取或创建方法函数
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    vector<TypeInfo> argTypes;
    string mangledName = Mangler::method(ownerMod, effType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }

    return _builder.CreateCall(fn, {ptr});
}

llvm::Value* Compiler::compileParenExpr(p<ExprParenNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    DEBUG_LOG("    Expr: Paren");
    return compileExpr(node->expr());
}

llvm::Value* Compiler::compileGetRefExpr(p<ExprGetRefNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto objName = node->obj().getText();
    auto& subs = node->subs();

    DEBUG_LOG_VAL("    Expr: GetRef", objName);

    auto it = _localVarPtrs.find(objName);
    if (it == _localVarPtrs.end()) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode, node->getLineNumber(), node->getColumn(), ErrorCode::E3031,
                                           objName);
    }

    llvm::Value* currentPtr = it->second;
    auto sym = _currentFnNode->lookupSymbol(objName);
    if (!sym) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode, node->getLineNumber(), node->getColumn(), ErrorCode::E3030,
                                           objName);
    }

    TypeInfo currentType = sym->type;
    // Phase 4a: 若源是 T&（参数 / 局部 ref），currentPtr 已经是底层 T 的地址；剥到 T
    if (currentType.isRef()) {
        if (auto inner = currentType.refElementType()) currentType = *inner;
    }

    for (auto& sub : subs) {
        auto memberName = sub.getText();

        // Phase 4c: Rc<T>.field —— 自动 deref：load handle，payload = handle + 8
        if (currentType.isRc()) {
            auto rcStructType = getLLVMType(currentType);
            auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(rcStructType, currentPtr, {zero32, zero32}, "rc.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "rc.handle");
            currentPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "rc.payload");
            if (auto inner = currentType.rcElementType()) currentType = *inner;
        }

        auto structDecl = _file->getStructDecl(currentType.name);
        if (!structDecl && _yux && _yux->sdkFile()) {
            structDecl = _yux->sdkFile()->getStructDecl(currentType.name);
        }
        if (!structDecl) {
            // Phase 3.4.d.1: SemaPass.visitExpr ExprGetRefNode 顶部
            // setResolvedType(getType) 已通过 ExprGetRefNode::getType 抛 E3041
            // (kMigratedCodes 命中, 自动重抛), 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3041, currentType.name);
        }

        int fieldIndex = structDecl->fieldIndex(memberName);
        if (fieldIndex < 0) {
            // Phase 3.4.d.1: 同上, getType 已抛 E3040, 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3040, currentType.name, memberName);
        }

        // Phase 3.4.d.2: E3042 私有字段可见性 整体抠到 sema::validatePrivateFieldAccess.
        // SemaPass.visitExpr ExprGetRefNode 分支调用 validateGetRefPrivacy 已沿同链路抢先抛;
        // 这里保留作幂等防御性双跑.
        sema::validatePrivateFieldAccess(structDecl, memberName, currentType.name, _currentStructName,
                                         node->getLineNumber(), node->getColumn());

        auto field = structDecl->fields()[fieldIndex];
        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        std::array<llvm::Value*, 2> indices{zero, idx};

        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        TypeInfo fieldType = field->getType();
        if (currentType.isGeneric() && structDecl->isGeneric() &&
            currentType.genericArgs.size() == structDecl->typeParams().size()) {
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    currentType.genericArgs[i] ? *currentType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        currentType = fieldType;
    }

    return currentPtr;
}

llvm::Value* Compiler::compileUnaryExpr(p<ExprUnaryNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto type = node->getType();
    auto rightType = node->right()->getType();
    // v0.16: [] 返回 T&——标量操作符自动剥 Ref
    auto effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;

    string opStr;
    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        opStr = "-";
        break;
    case ExprUnaryNode::Op::Rev:
        opStr = "~";
        break;
    case ExprUnaryNode::Op::Not:
        opStr = "!";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);

    // 检查是否为自定义类型（用剥 Ref 后的标量名）
    if (!isBuiltinType(effRightType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprUnaryNode::Op::Neg:
            methodName = "neg";
            break;
        case ExprUnaryNode::Op::Rev:
            methodName = "inv";
            break;
        case ExprUnaryNode::Op::Not:
            methodName = "not";
            break;
        }
        return compileCustomTypeUnaryOp(node->right(), rightType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isBool = type.name == "bool";

    // v0.16: 操作数若是 T& 则 load 出值
    if (rightType.isRef()) {
        right = _builder.CreateLoad(getLLVMType(effRightType), right, "unary_op");
    }

    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        if (isFloat) {
            return _builder.CreateFNeg(right, "neg");
        }
        return _builder.CreateNeg(right, "neg");
    case ExprUnaryNode::Op::Rev:
        if (isFloat) {
            // Phase 3.4.h: ExprUnaryNode::getType 已抛 E3070 (kMigratedCodes 命中,
            // SemaPass 自动重抛), 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3070, type.name);
        }
        return _builder.CreateNot(right, "not");
    case ExprUnaryNode::Op::Not:
        if (!isBool) {
            // Phase 3.4.h: 同上, getType 已抛 E3071, 此处不可达; 保留作幂等防御性双跑。
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3071, type.name);
        }
        return _builder.CreateNot(right, "lnot");
    }

    // Phase 3.4.h: switch default unreachable, 上面三个 case 已覆盖全部 Op; 兜底。
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3072);
}
