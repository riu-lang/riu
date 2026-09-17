// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 形态校验入口：tryValidate*（运算符 / 索引 / 成员链 / match / ?. / if / to_string）
// 以及带靶向类型的数组字面量 / 填充检查。

#include "builtin_methods.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"
#include "sema/sema_pass.h"
#include "sema/sema_pass_detail.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"
#include "ast/riu.h"
#include "tools/diagnostic.h"
#include "types.h"

using namespace sema::pass;

void SemaPass::tryValidateCompareForm(ExprCompareNode* n) {
    // 与 validateCompareOpForm 对齐：Weak ==/!= → E3078，Ptr 排序 / Function == → E3073。
    // && / ||：镜像 ExprCompareNode::getType / compileCompareExpr 的 E3001。
    // 模板形参等实例化后再查；Weak<T> / Ptr 形态与内层 T 无关，模板期也报。
    if (!n || !n->left() || !n->right()) return;
    try {
        TypeInfo leftType = applyInstSubst(n->left()->getType()).peelAutoDeref();
        if (isCurrentTypeParam(leftType)) return;
        sema::validateCompareOpForm(leftType, n->op(), n->getLineNumber(), n->getColumn());
        if (n->op() != ExprCompareNode::Op::AndAnd && n->op() != ExprCompareNode::Op::OrOr) return;
        TypeInfo rightType = applyInstSubst(n->right()->getType()).peelAutoDeref();
        if (isCurrentTypeParam(rightType)) return;
        if (leftType == rightType) return;
        if (isIntTypeName(leftType.name) && isFlexibleIntExpr(n->right())) {
            tryInferIntType(n->right(), leftType);
            return;
        }
        if (isIntTypeName(rightType.name) && isFlexibleIntExpr(n->left())) {
            tryInferIntType(n->left(), rightType);
            return;
        }
        if (!isBuiltinType(leftType.name)) return;
        throw RiuError(n->getLineNumber(), n->getColumn(), ErrorCode::E3001, "comparison", leftType.name,
                       rightType.name);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateBinOpMethod(ExprNode* leftExpr, ExprNode* rightExpr, const string& methodName, int line,
                                      int col) {
    // gate 与 Compiler::compileAddSubExpr / MulDivMod / BinOp / Compare 内
    // `!isBuiltinType(leftType.name) → compileCustomTypeBinaryOp` 一致, 但
    // 进一步把容器类排除 (容器走专属 codegen / sema 路径, 不该走到 method 解析):
    //   * Ref / Rc / Array / Heap / Weak / Nullable / Ptr / Tuple
    // 泛型 struct 实例仍跳过（方法解析要完整 subst）；模板形参 T 等实例化后再查。
    // leftType / rightType getType 抛错 (lambda 形参等) 跳过, 留 Compiler 兜底.
    if (methodName.empty()) return;
    try {
        TypeInfo leftType = applyInstSubst(leftExpr->getType()).peelAutoDeref();
        TypeInfo rightType = applyInstSubst(rightExpr->getType()).peelAutoDeref();
        if (isCurrentTypeParam(leftType) || isCurrentTypeParam(rightType)) return;
        // String + 任意：getType 整链结果即 String，不走 E3001。
        if (methodName == "plus" && (leftType.isString() || rightType.isString())) return;
        // 实例化后内置类型：镜像 ExprAddSubNode::getType 的 E3001。
        if (isBuiltinType(leftType.name)) {
            if (leftType != rightType) {
                if (isIntTypeName(leftType.name) && isFlexibleIntExpr(rightExpr)) {
                    tryInferIntType(rightExpr, leftType);
                    return;
                }
                if (isIntTypeName(rightType.name) && isFlexibleIntExpr(leftExpr)) {
                    tryInferIntType(leftExpr, rightType);
                    return;
                }
                throw RiuError(line, col, ErrorCode::E3001, binOpE3001Kind(methodName), leftType.name, rightType.name);
            }
            return;
        }
        if (leftType.name.empty()) return;
        // String 走 StringBuilder 特殊 lowering / 其它 builtin-handled 路径,
        // 没有用户可见的 plus/eq/... 方法签名, 不能走 customBinaryOp 解析.
        if (leftType.isString()) return;
        if (leftType.isRef() || leftType.isArrayGeneric() || leftType.isWeak() || leftType.isNullable() ||
            leftType.isPtr() || leftType.isTuple())
            return;
        // Heap<T> → T / Rc<T> → T：运算符穿透 wrapper，在内部类型上验证方法
        TypeInfo resolvedLeftType = leftType;
        if (leftType.isHeap()) {
            auto heapInner = leftType.heapElementType();
            if (!heapInner) return;
            resolvedLeftType = *heapInner;
        }
        if (leftType.isRc()) {
            auto rcInner = leftType.rcElementType();
            if (!rcInner) return;
            resolvedLeftType = *rcInner;
        }
        StructDeclNode* decl = _names.lookupStruct(resolvedLeftType.name);
        if (!decl || decl->isGeneric()) return;
        TypeInfo effRightType = rightType.peelAutoDeref();
        sema::validateBinOpMethodResolution(_file, _sdkFile, resolvedLeftType, effRightType, methodName, line, col);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateUnaryOpMethod(ExprNode* rightExpr, const string& methodName, int line, int col) {
    // gate 与 Compiler::compileUnaryExpr 内 `!isBuiltinType → compileCustomTypeUnaryOp` 一致,
    // 并排除容器（与 tryValidateBinOpMethod 同款）。模板形参等实例化后再查。
    if (methodName.empty()) return;
    try {
        TypeInfo rightType = applyInstSubst(rightExpr->getType()).peelAutoDeref();
        if (isCurrentTypeParam(rightType)) return;
        if (isBuiltinType(rightType.name)) {
            if (methodName == "inv" && rightType.isFloat()) {
                throw RiuError(line, col, ErrorCode::E3070, rightType.name);
            }
            if (methodName == "not" && rightType.name != "bool") {
                throw RiuError(line, col, ErrorCode::E3071, rightType.name);
            }
            return;
        }
        if (rightType.name.empty()) return;
        if (rightType.isString()) return;
        if (rightType.isRef() || rightType.isArrayGeneric() || rightType.isWeak() || rightType.isNullable() ||
            rightType.isPtr() || rightType.isTuple())
            return;
        TypeInfo resolved = rightType;
        if (rightType.isHeap()) {
            auto heapInner = rightType.heapElementType();
            if (!heapInner) return;
            resolved = *heapInner;
        }
        if (rightType.isRc()) {
            auto rcInner = rightType.rcElementType();
            if (!rcInner) return;
            resolved = *rcInner;
        }
        StructDeclNode* decl = _names.lookupStruct(resolved.name);
        if (!decl || decl->isGeneric()) return;
        sema::validateUnaryOpMethodResolution(_file, _sdkFile, resolved, methodName, line, col);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::checkEmptyArrayLiteral(ExprArrayNode* n, const TypeInfo* expected) {
    // 与 compileArrayLiteralExpr 对齐：空 `[]` 仅 Array<T> 靶向合法；固定数组 /
    // 无注解 / 非数组靶向 → E3063。模板形参等实例化后再查（`let a T = []`）。
    if (!n || !n->elements().empty()) return;
    if (expected) {
        TypeInfo want = applyInstSubst(expected->peelRef());
        if (want.isArrayGeneric()) return;
        if (isCurrentTypeParam(want)) return;
    }
    throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3063);
}

void SemaPass::tryValidateIndexBase(ExprNode* arrayExpr, int line, int col) {
    // 与 ExprGetNode::getType / compileArraySetStatement 同款：只剥 Ref。
    // 模板形参等实例化后再查。
    if (!arrayExpr) return;
    try {
        TypeInfo at = arrayExpr->hasResolvedType() ? arrayExpr->resolvedType() : arrayExpr->getType();
        at = applyInstSubst(at).peelRef();
        if (isCurrentTypeParam(at)) return;
        if (at.isArrayGeneric() || at.isArray()) return;
        if (at.name.empty()) return;
        throw RiuError(line, col, ErrorCode::E3062, at.name);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateFieldChain(const TypeInfo& start, const vector<string>& members, int line, int col) {
    // 与 compileAssignStatement 成员赋值 / ExprGetRefNode::getType / 读路径 Dot 同款。
    // 模板形参等实例化后再查；已知 struct 的缺字段不依赖 T，模板期也报。
    if (members.empty()) return;
    auto peel = [this](TypeInfo t) {
        t = substSelfType(applyInstSubst(t), _currentStructName).peelAutoDeref();
        try {
            t = sema::resolveAlias(t, _file, _sdkFile).peelAutoDeref();
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        return t;
    };
    TypeInfo cur = peel(start);
    auto isPureDigits = [](const string& s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
    };
    for (const auto& mem : members) {
        if (isCurrentTypeParam(cur)) return;
        // DRAFT-spec-reflect §6: Field.value 写/取由 codegen 改写，不按 struct 字段查。
        if (cur.name == "Field" && mem == "value") return;
        if (cur.isTuple()) {
            if (!isPureDigits(mem)) {
                throw RiuError(line, col, ErrorCode::E3040, cur.getFullName(), mem);
            }
            auto idx = static_cast<size_t>(std::stoul(mem));
            const auto& elems = cur.tupleElements();
            // Phase C：读 / 赋值 / `&obj.N` 越界都在此抛。getType 对非泛型已报；
            // 模板体 T 不是元组，实例化后 subst 才看见具体元组，必须在这里查。
            if (idx >= elems.size()) {
                throw RiuError(line, col, ErrorCode::E3100, mem, cur.getFullName(), std::to_string(elems.size()));
            }
            if (!elems[idx]) return;
            cur = peel(*elems[idx]);
            continue;
        }
        if (cur.name.empty()) return;
        if (isBuiltinType(cur.name)) {
            throw RiuError(line, col, ErrorCode::E3041, cur.name);
        }
        // Type / Field / String 等是 #Builtin 占位 struct，与 compileDotExpr 一样要看见。
        StructDeclNode* decl = _names.lookupStruct(cur, /*includeBuiltin=*/true);
        if (!decl) {
            throw RiuError(line, col, ErrorCode::E3041, cur.name);
        }
        if (decl->staticField(mem)) {
            throw RiuError(line, col, ErrorCode::E3152, mem, cur.name, cur.name, mem);
        }
        int fi = decl->fieldIndex(mem);
        if (fi < 0) {
            throw RiuError(line, col, ErrorCode::E3040, cur.name, mem);
        }
        TypeInfo fieldTy = decl->fields()[static_cast<size_t>(fi)]->getType();
        map<string, TypeInfo> fieldSubst;
        if (const auto* s = currentInstSubst()) fieldSubst = *s;
        if (decl->isGeneric() && fieldSubst.empty()) {
            fillSubstFromGenericArgs(decl->typeParams(), cur.genericArgs, fieldSubst);
        }
        if (!fieldSubst.empty()) fieldTy = fieldTy.substitute(fieldSubst);
        cur = peel(fieldTy);
    }
}

void SemaPass::fillMatchArmBindingTypes(ExprMatchNode* n) {
    if (!n) return;

    // 绑定类型跟 scrut 这一次的 enumType（含 genericArgs），不是声明 payload 原文。
    TypeInfo enumType;
    EnumDeclNode* scrutEnum = nullptr;
    try {
        if (auto* scrut = n->scrutinee()) {
            enumType = scrut->hasResolvedType() ? scrut->resolvedType() : scrut->getType();
            enumType = sema::resolveAlias(applyInstSubst(enumType), _file, _sdkFile);
            auto peelIfEnum = [&](const TypeInfo& wrapped) {
                TypeInfo in = sema::resolveAlias(applyInstSubst(wrapped), _file, _sdkFile);
                if (_names.lookupEnum(in)) enumType = std::move(in);
            };
            if (enumType.isRc()) {
                if (auto inner = enumType.rcElementType()) peelIfEnum(*inner);
            } else if (enumType.isHeap()) {
                if (auto inner = enumType.heapElementType()) peelIfEnum(*inner);
            } else if (enumType.isRef()) {
                if (auto inner = enumType.refElementType()) peelIfEnum(*inner);
            }
            scrutEnum = _names.lookupEnum(enumType);
        }
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // 预扫时 getType 可能未就绪；visit match 会再填一次。
    }

    for (auto* arm : n->arms()) {
        if (!arm) continue;
        auto* pat = arm->pattern();
        if (!pat || pat->isElse() || pat->binds().empty()) continue;
        EnumDeclNode* enumDecl = scrutEnum;
        if (!enumDecl) {
            try {
                auto r =
                    sema::resolveExprTypeLhs(pat, _file, _riu, pat->enumPath(), pat->getLineNumber(), pat->getColumn());
                enumDecl = r.enumDecl;
                if (!enumDecl) enumDecl = _names.lookupEnum(r.type);
            } catch (const RiuError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                continue;
            }
        }
        EnumVariantNode* variant = enumDecl ? enumDecl->variant(pat->variantName().getText()) : nullptr;
        auto subst = sema::enumInstSubst(enumDecl, enumType);
        for (size_t i = 0; i < pat->binds().size(); ++i) {
            const string& bn = pat->binds()[i].getText();
            TypeInfo bindType;
            if (variant && i < variant->payloadArity() && variant->payloadTypes()[i]) {
                try {
                    bindType = variant->payloadTypes()[i]->getType();
                    if (!subst.empty()) bindType = bindType.substitute(subst);
                    bindType = applyInstSubst(bindType);
                } catch (const RiuError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
            }
            arm->registerSymbol(bn, {SymbolKind::Variable, bn, bindType, false});
        }
    }
}

void SemaPass::fillMatchBindingsInBlock(StatementBlockNode* block) {
    if (!block) return;
    for (auto& s : block->statements())
        fillMatchBindingsInStmt(s);
    if (block->hasResult()) fillMatchBindingsInExpr(block->resultExpr());
}

void SemaPass::refreshInferredLetType(StatementDeclareAssignNode* da) {
    if (!da || da->varType() || !da->expr()) return;
    TypeInfo ty;
    try {
        ty = da->expr()->hasResolvedType() ? da->expr()->resolvedType() : da->expr()->getType();
        ty = applyInstSubst(ty);
    } catch (const RiuError&) {
        return;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return;
    }
    if (ty.empty()) return;
    auto* sc = da->findNearestScope();
    if (!sc) return;
    const string name = da->name().getText();
    if (!sc->localSymbols().contains(name)) return;
    if (auto* sym = sc->lookupSymbol(name)) {
        if (sym->kind == SymbolKind::Variable) sym->type = std::move(ty);
    }
}

void SemaPass::fillMatchBindingsInStmt(StatementNode* s) {
    if (!s) return;
    if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
        fillMatchBindingsInExpr(da->expr());
        refreshInferredLetType(da);
        return;
    }
    if (auto n = dynamic_cast<StatementSetNode*>(s)) {
        fillMatchBindingsInExpr(n->arrayExpr());
        for (auto& idx : n->indices())
            fillMatchBindingsInExpr(idx);
        fillMatchBindingsInExpr(n->valueExpr());
        return;
    }
    if (auto n = dynamic_cast<StatementStaticFieldSetNode*>(s)) {
        fillMatchBindingsInExpr(n->valueExpr());
        return;
    }
    if (auto n = dynamic_cast<StatementLoopNode*>(s)) {
        if (n->hasInit()) fillMatchBindingsInExpr(n->initExpr());
        fillMatchBindingsInBlock(n->block());
        return;
    }
    if (auto n = dynamic_cast<StatementForInNode*>(s)) {
        fillMatchBindingsInExpr(n->expr());
        fillMatchBindingsInBlock(n->block());
        return;
    }
    if (auto n = dynamic_cast<StatementExprNode*>(s)) {
        fillMatchBindingsInExpr(n->expr());
    }
}

void SemaPass::fillMatchBindingsInExpr(ExprNode* e) {
    if (!e) return;
    if (auto n = dynamic_cast<ExprMatchNode*>(e)) {
        fillMatchArmBindingTypes(n);
        fillMatchBindingsInExpr(n->scrutinee());
        for (auto& arm : n->arms()) {
            if (!arm) continue;
            if (arm->hasBlock())
                fillMatchBindingsInBlock(arm->block());
            else
                fillMatchBindingsInExpr(arm->body());
        }
        return;
    }
    if (auto n = dynamic_cast<ExprIfElseNode*>(e)) {
        fillMatchBindingsInExpr(n->condition());
        fillMatchBindingsInBlock(n->thenBlock());
        for (auto& elif : n->elifs()) {
            if (!elif) continue;
            fillMatchBindingsInExpr(elif->condition());
            fillMatchBindingsInBlock(elif->block());
        }
        fillMatchBindingsInBlock(n->elseBlock());
        return;
    }
    if (auto n = dynamic_cast<ExprOneLineIfElseNode*>(e)) {
        fillMatchBindingsInExpr(n->condition());
        fillMatchBindingsInExpr(n->trueValue());
        fillMatchBindingsInExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<ExprTryCatchNode*>(e)) {
        fillMatchBindingsInBlock(n->tryBlock());
        for (auto& c : n->catches()) {
            if (c) fillMatchBindingsInBlock(c->body());
        }
        return;
    }
    if (auto n = dynamic_cast<LambdaExprNode*>(e)) {
        if (n->form() == LambdaExprNode::Form::Expr) {
            fillMatchBindingsInExpr(n->bodyExpr());
        } else {
            for (auto& st : n->bodyStmts())
                fillMatchBindingsInStmt(st);
        }
        return;
    }
    if (auto n = dynamic_cast<ExprCallNode*>(e)) {
        fillMatchBindingsInExpr(n->getCalleeExpr());
        for (auto& a : n->getArgs())
            fillMatchBindingsInExpr(a);
        return;
    }
    if (auto n = dynamic_cast<ExprPathCallNode*>(e)) {
        for (auto& a : n->args())
            fillMatchBindingsInExpr(a);
        return;
    }
    if (auto n = dynamic_cast<ExprDotNode*>(e)) {
        fillMatchBindingsInExpr(n->baseExpr());
        return;
    }
    if (auto n = dynamic_cast<ExprAddSubNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprMulDivModNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprBinOpNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprCompareNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprParenNode*>(e)) {
        fillMatchBindingsInExpr(n->expr());
        return;
    }
    if (auto n = dynamic_cast<ExprUnaryNode*>(e)) {
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprGetNode*>(e)) {
        fillMatchBindingsInExpr(n->arrayExpr());
        for (auto& idx : n->indices())
            fillMatchBindingsInExpr(idx);
        return;
    }
    if (auto n = dynamic_cast<ExprArrayNode*>(e)) {
        for (auto& el : n->elements())
            fillMatchBindingsInExpr(el);
        return;
    }
    if (auto n = dynamic_cast<ExprTupleNode*>(e)) {
        for (auto& el : n->elements())
            fillMatchBindingsInExpr(el);
        return;
    }
    if (auto n = dynamic_cast<ExprStructLitNode*>(e)) {
        for (auto& f : n->fields()) {
            if (f) fillMatchBindingsInExpr(f->value());
        }
        fillMatchBindingsInExpr(n->positional());
        return;
    }
    if (auto n = dynamic_cast<ExprDynCtorNode*>(e)) {
        fillMatchBindingsInExpr(n->arg());
        return;
    }
    if (auto n = dynamic_cast<ExprMoveAssignNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprNullElseNode*>(e)) {
        fillMatchBindingsInExpr(n->left());
        fillMatchBindingsInExpr(n->right());
        return;
    }
    if (auto n = dynamic_cast<ExprLiteralNode*>(e)) {
        if (auto tpl = dynamic_cast<StringTemplateNode*>(n->literal())) {
            for (auto& interp : tpl->interps())
                fillMatchBindingsInExpr(interp);
        }
    }
}

void SemaPass::tryValidateMatchScrut(ExprMatchNode* n) {
    // 与 compileMatchExpr 同款。模板形参等实例化后再查；先前只对 builtin / String
    // 报 E2022，用户 struct 与泛型体 subst 后的非 enum 会漏给 codegen。
    if (!n || !n->scrutinee()) return;
    try {
        auto* scrut = n->scrutinee();
        TypeInfo checkType = scrut->hasResolvedType() ? scrut->resolvedType() : scrut->getType();
        checkType = sema::resolveAlias(applyInstSubst(checkType), _file, _sdkFile);
        int line = n->getLineNumber();
        int col = n->getColumn();
        auto peelEnumWrapper = [&](bool isRc) {
            auto inner = isRc ? checkType.rcElementType() : checkType.heapElementType();
            if (!inner) return;
            TypeInfo in = sema::resolveAlias(applyInstSubst(*inner), _file, _sdkFile);
            if (isCurrentTypeParam(in)) return;
            if (!_names.lookupEnum(in)) return;
            if (isFreshHandleExpr(n->scrutinee())) {
                throw RiuError(line, col, ErrorCode::E2022, checkType.name)
                    .withHint(isRc ? "不支持对临时 Rc<E> 直接 match；先 `let b Rc<E> = ...` 落地再 match b"
                                   : "不支持对临时 Heap<E> 直接 match；先 `let h Heap<E> = ...` 落地再 match h");
            }
            checkType = std::move(in);
        };
        if (checkType.isRc()) {
            peelEnumWrapper(true);
        } else if (checkType.isHeap()) {
            peelEnumWrapper(false);
        } else if (checkType.isRef()) {
            if (auto inner = checkType.refElementType()) {
                TypeInfo in = sema::resolveAlias(applyInstSubst(*inner), _file, _sdkFile);
                if (_names.lookupEnum(in)) checkType = std::move(in);
            }
        }
        if (isCurrentTypeParam(checkType)) return;
        if (checkType.name.empty()) return;

        auto* enumDecl = _names.lookupEnum(checkType);
        if (enumDecl) {
            sema::validateMatchArms(enumDecl, checkType, n, _file);
        } else {
            throw RiuError(line, col, ErrorCode::E2022, checkType.name);
        }
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateSafeDot(ExprDotNode* n) {
    // 与 ExprDotNode::getType / compileSafeDotExpr 同款。
    // 模板形参等实例化后再查；getType 在模板体把 E3024/E3044/E3040 吞掉。
    if (!n || !n->isSafe() || !n->baseExpr()) return;
    try {
        auto* base = n->baseExpr();
        TypeInfo bt = base->hasResolvedType() ? base->resolvedType() : base->getType();
        bt = peelRefIfNullable(applyInstSubst(bt));
        if (isCurrentTypeParam(bt)) return;
        if (bt.name.empty()) return;
        if (!bt.isNullable()) {
            throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3024, bt.name);
        }
        auto inner = bt.nullableInnerType();
        if (!inner) return;
        TypeInfo actual = applyInstSubst(*inner);
        if (actual.isRc()) {
            if (auto rc = actual.rcElementType()) actual = applyInstSubst(*rc);
        }
        if (isCurrentTypeParam(actual)) return;
        if (actual.name.empty()) return;

        string mem = n->member();
        if (mem.starts_with("to_")) return;
        if (receiverHasMethod(actual, mem, _file, _sdkFile)) return;

        StructDeclNode* decl = _names.lookupStruct(actual, /*includeBuiltin=*/true);
        if (!decl) {
            throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3044, actual.name);
        }
        if (decl->fieldIndex(mem) < 0) {
            throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3040, actual.name, mem);
        }
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateIfElse(ExprIfElseNode* n) {
    // 与 ExprIfElseNode::getType 对齐：流终止臂跳过；非终止且无尾值则不是值 if，不报。
    // 模板形参等实例化后再查。
    if (!n) return;
    ScopeNode* sc = n->findNearestScope();
    auto branchKind = [this, sc](StatementBlockNode* block, TypeInfo& out) -> int {
        // 0 流终止跳过；1 有值；-1 无值（语句形态）
        if (!block) return -1;
        if (blockTerminatesFlow(sc, block)) return 0;
        if (!block->hasResult() || !block->resultExpr()) return -1;
        ExprNode* e = block->resultExpr();
        try {
            out = e->hasResolvedType() ? e->resolvedType() : e->getType();
        } catch (const RiuError&) {
            return -1;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return -1;
        }
        out = applyInstSubst(out);
        return 1;
    };
    TypeInfo resultType;
    bool have = false;
    auto consider = [&](StatementBlockNode* block) -> bool {
        TypeInfo t;
        int k = branchKind(block, t);
        if (k == 0) return true;
        if (k < 0) return false;
        if (typeStillTemplate(t)) return false;
        if (!have) {
            resultType = std::move(t);
            have = true;
            return true;
        }
        if (!blockMergeTypesEq(t, resultType)) {
            throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3005, resultType.name, t.name);
        }
        if (isEmptyArrayType(resultType) && !isEmptyArrayType(t)) resultType = std::move(t);
        return true;
    };
    if (!consider(n->thenBlock())) return;
    for (auto& el : n->elifs()) {
        if (!el || !consider(el->block())) return;
    }
    if (n->elseBlock() && !consider(n->elseBlock())) return;
}

void SemaPass::tryValidateOneLineIfElse(ExprOneLineIfElseNode* n) {
    // 与 ExprOneLineIfElseNode::getType 对齐。流终止臂跳过。模板形参等实例化后再查。
    if (!n) return;
    ScopeNode* sc = n->findNearestScope();
    auto exprType = [this](ExprNode* e, TypeInfo& out) -> bool {
        if (!e) return false;
        try {
            out = e->hasResolvedType() ? e->resolvedType() : e->getType();
        } catch (const RiuError&) {
            return false;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return false;
        }
        out = applyInstSubst(out);
        return true;
    };
    bool trueTerm = exprTerminatesFlow(sc, n->trueValue());
    bool falseTerm = exprTerminatesFlow(sc, n->falseValue());
    if (trueTerm || falseTerm) return;
    TypeInfo trueType;
    TypeInfo falseType;
    if (!exprType(n->trueValue(), trueType) || !exprType(n->falseValue(), falseType)) return;
    if (typeStillTemplate(trueType) || typeStillTemplate(falseType)) return;
    if (!blockMergeTypesEq(trueType, falseType)) {
        throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3005, trueType.name, falseType.name);
    }
}

void SemaPass::tryValidateToString(ExprNode* e) {
    // 与 compileStringPlusChain / compileStringTemplate 同款。
    // 模板形参等实例化后再查；getType 在模板体把依赖 T 的类型吞掉。
    if (!e) return;
    try {
        TypeInfo t = e->hasResolvedType() ? e->resolvedType() : e->getType();
        t = applyInstSubst(t);
        if (isCurrentTypeParam(t)) return;
        if (t.name.empty()) return;
        if (sema::typeImplementsToString(_file, _sdkFile, t)) return;
        throw RiuError(e->getLineNumber(), e->getColumn(), ErrorCode::E3026, t.name);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateStringPlus(ExprAddSubNode* n) {
    // 与 compileStringPlusChain 同款：`+` 结果为 String 时沿左脊展开叶子。
    // `"a" + x` 在模板期 x 是 T，跳过；实例化后再查 E3026。
    if (!n || n->op() != ExprAddSubNode::Op::Add) return;
    try {
        auto exprTypeOf = [&](ExprNode* e) -> TypeInfo {
            TypeInfo t = e->hasResolvedType() ? e->resolvedType() : e->getType();
            return applyInstSubst(t);
        };
        TypeInfo result = exprTypeOf(n);
        TypeInfo lt = exprTypeOf(n->left()).peelAutoDeref();
        TypeInfo rt = exprTypeOf(n->right()).peelAutoDeref();
        if (!(result.isString() || result.name == "String" || lt.isString() || rt.isString())) return;

        vector<ExprNode*> leaves;
        ExprAddSubNode* cur = n;
        while (true) {
            leaves.push_back(cur->right());
            auto leftExpr = cur->left();
            auto* innerAdd = dynamic_cast<ExprAddSubNode*>(leftExpr);
            bool isStringAdd = false;
            if (innerAdd && innerAdd->op() == ExprAddSubNode::Op::Add) {
                TypeInfo innerT = exprTypeOf(innerAdd);
                isStringAdd = innerT.isString() || innerT.name == "String";
            }
            if (isStringAdd) {
                cur = innerAdd;
                continue;
            }
            leaves.push_back(leftExpr);
            break;
        }
        std::ranges::reverse(leaves);
        for (auto& leaf : leaves)
            tryValidateToString(leaf);
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::checkArrayElemAgainst(ExprNode* elem, const TypeInfo& want, int line, int col) {
    if (!elem) return;
    TypeInfo w0 = applyInstSubst(want);
    if (isCurrentTypeParam(w0)) return;
    if (isFlexibleIntExpr(elem) && isIntTypeName(w0.name)) return;
    if (w0.isNullable() && isFlexibleNullExpr(elem)) return;

    TypeInfo got;
    if (elem->hasResolvedType()) {
        got = elem->resolvedType();
    } else {
        try {
            got = elem->getType();
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return;
        }
    }
    got = applyInstSubst(got);
    if (isEmptyArrayType(got)) return;
    if (got == w0) return;
    if (got.peelRef() == w0) return;
    if (w0.isNullable()) {
        if (auto inner = w0.nullableInnerType()) {
            if (got == *inner || got.peelRef() == *inner) return;
        }
    }
    throw RiuError(line, col, ErrorCode::E3009, w0.getFullName(), got.getFullName());
}

void SemaPass::checkArrayLiteral(ExprArrayNode* n, const TypeInfo& expected) {
    if (!n) return;
    TypeInfo want = expected.peelRef();
    checkEmptyArrayLiteral(n, &want);
    if (want.isArray() && want.arraySize != n->elements().size()) {
        throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3012, want.arraySize,
                       n->elements().size());
    }
    auto elemWant = arrayElemTarget(want);
    for (auto& e : n->elements()) {
        visitExpr(e, elemWant ? elemWant.get() : nullptr);
        rejectEscapingRefCaptureLambda(e);
        if (!elemWant) continue;
        int eline = e->resolveLineNumber();
        int ecol = e->resolveColumn();
        if (eline <= 0) eline = n->resolveLineNumber();
        if (ecol < 0) ecol = n->resolveColumn();
        checkArrayElemAgainst(e, *elemWant, eline, ecol);
    }
    n->setResolvedType(want);
}

void SemaPass::checkArrayInit(ExprArrayInitNode* n, const TypeInfo* expected) {
    if (!n) return;
    TypeInfo elemType;
    if (n->explicitType()) {
        elemType = n->explicitType()->getType();
        inferFillLiteralInt(n->value(), elemType);
        auto fillType = n->value()->getType();
        if (fillType != elemType) {
            throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3009, elemType.getFullName(),
                           fillType.getFullName());
        }
    } else {
        elemType = n->value()->getType();
        if (expected) {
            if (auto wantElem = arrayElemTarget(expected->peelRef())) {
                inferFillLiteralInt(n->value(), *wantElem);
                elemType = n->value()->getType();
            }
        }
    }

    if (auto* ilit = dynamic_cast<LiteralIntNode*>(n->value())) {
        (void)sema::parseIntLiteral(ilit->getValue().getText(), n->getLineNumber(), n->getColumn(),
                                    ilit->getType().name);
    }

    auto rejectUnsupportedFill = [&]() {
        // Phase C：fill 须是 int / float / bool 字面量（与 compileArrayInitExpr 对齐）。
        // string / null / 码点 / 变量（literalObj）→ E3080。与 T 无关，模板期也报。
        // 类型不符仍先走上面的 E3009（`[true ... i32]`）。
        auto fill = n->value();
        if (!fill) return;
        if (dynamic_cast<LiteralIntNode*>(fill) || dynamic_cast<LiteralFloatNode*>(fill) ||
            dynamic_cast<LiteralBoolNode*>(fill)) {
            return;
        }
        throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3080);
    };

    if (expected) {
        TypeInfo want = expected->peelRef();
        if (auto wantElem = arrayElemTarget(want)) {
            if (!isCurrentTypeParam(*wantElem) && elemType != *wantElem) {
                throw RiuError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3009, wantElem->getFullName(),
                               elemType.getFullName());
            }
            rejectUnsupportedFill();
            n->setResolvedType(want);
            return;
        }
    }
    rejectUnsupportedFill();
    n->setResolvedType(TypeInfo(make_shared<TypeInfo>(elemType), 0));
}

bool SemaPass::hasFieldValueReceiver() const {
    // 与 compileDotExpr 对齐：要有当前 struct 且 `$` 可用（非 #Static）。
    if (_currentStructName.empty()) return false;
    if (_currentFn && _currentFn->header() && _currentFn->header()->isStatic()) return false;
    return true;
}

void SemaPass::tryValidateReflectFieldValueRead(ExprDotNode* n) {
    // 与 ExprDotNode::getType / compileDotExpr 对齐。
    // getType 对运行期 Field 已抛 E3133；模板体吞掉后这里再报。
    // 编译期可定但无 `$` → E3134（getType 会成功改写，codegen 才报）。
    if (!n || n->member() != "value") return;
    try {
        if (n->isReflectFieldValue()) {
            if (!hasFieldValueReceiver()) {
                throw RiuError(n->getLineNumber(), n->getColumn(), ErrorCode::E3134);
            }
            return;
        }
        auto* base = n->baseExpr();
        if (!base) return;
        TypeInfo bt = base->hasResolvedType() ? base->resolvedType() : base->getType();
        TypeInfo peeled = applyInstSubst(bt).peelAutoDeref();
        if (peeled.name == "Field") {
            throw RiuError(n->getLineNumber(), n->getColumn(), ErrorCode::E3133);
        }
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 失败，留 Compiler
    }
}

void SemaPass::tryValidateReflectFieldValueWrite(const string& objName, const TypeInfo& objType,
                                                 const vector<string>& members, int line, int col) {
    // 与 compileAssignStatement 的 Field.value 写路径对齐：只搜 fn 体 let。
    if (members.size() != 1 || members[0] != "value") return;
    TypeInfo peeled = applyInstSubst(objType).peelAutoDeref();
    if (peeled.name != "Field") return;
    ExprNode* init = nullptr;
    if (_currentFn) {
        for (auto& stmt : _currentFn->body()) {
            if (auto* letStmt = dynamic_cast<StatementDeclareAssignNode*>(stmt)) {
                if (letStmt->name().getText() == objName) {
                    init = letStmt->expr();
                    break;
                }
            }
        }
    }
    if (init) {
        auto [sd, idx] = tryResolveReflectField(init);
        if (sd && idx >= 0) {
            if (!hasFieldValueReceiver()) {
                throw RiuError(line, col, ErrorCode::E3134);
            }
            return;
        }
    }
    throw RiuError(line, col, ErrorCode::E3133);
}
