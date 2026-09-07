// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 表达式语义检查：visitExpr / visitExprList。
// Phase 3.2a：每个表达式节点写 setResolvedType(getType())。
// Phase B：SemaPass 为 getType 诊断的权威抛出点。默认重抛所有 YuxError。
// Phase C：泛型 fn/impl 体再吞一批依赖 T 具体化的码（见 isMorphologicalGenericCode）。
// 方法点 callee 的 E3095：getType 会把找不到的方法回落成基类型再抛「不是函数」，
// 挡住 E1101/E1140，故先按形态记下，Dot 分支校验 @Spec 后再决定重抛。
// 字段非 Fn 值由 Dot 分支直接报 E3095。ID-literal 的 E3095 仍立即重抛。
// 非 YuxError 在 debug 下 assert，禁止静默吞。

#include "sema/builtin_methods.h"
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

#include "analyzer/borrow_checker.h"
#include "analyzer/const_mut_checker.h"
#include "analyzer/flow_terminate_checker.h"
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
#include "ast/yux.h"
#include "tools/diagnostic.h"
#include "types.h"

using namespace sema::pass;

namespace {

bool hasPublicTypeIn(FileNode* f, const string& name) {
    if (!f || name.empty() || name[0] == '_') return false;
    return f->localStructDecl(name, true) || f->localEnumDecl(name) || f->localAliasDecl(name);
}

bool symbolIsPathPrefix(SymbolInfo* s) {
    return s && (s->kind == SymbolKind::Module || s->kind == SymbolKind::Package);
}

bool hasLocalValueNamed(ExprNode* n, const string& name) {
    for (auto* sc = n->findNearestScope(); sc; sc = sc->parentScope()) {
        if (dynamic_cast<FileNode*>(sc)) break;
        auto it = sc->localSymbols().find(name);
        if (it == sc->localSymbols().end()) continue;
        auto k = it->second.kind;
        if (k == SymbolKind::Variable || k == SymbolKind::Function) return true;
    }
    return false;
}

// 泛型方法体在调用方文件上实例化时，Self 的 owner 是声明模块，不是 _file。
FileNode* fnDeclFile(FnNode* fn, FileNode* fallback) {
    if (fn) {
        if (auto* f = fn->enclosingFile()) return f;
    }
    return fallback;
}

} // namespace

void SemaPass::visitExprList(const vector<p<ExprNode>>& args, const vector<TypeInfo>* expected) {
    for (size_t i = 0; i < args.size(); ++i) {
        const TypeInfo* exp = nullptr;
        if (expected && i < expected->size() && !(*expected)[i].empty()) exp = &(*expected)[i];
        visitExpr(args[i], exp);
    }
}

void SemaPass::visitExpr(p<ExprNode> expr, const TypeInfo* expected, bool callCallee) {
    if (!expr) return;

    // Phase C：有靶向类型时，数组 / 元组字面量先按 expected 走，避免 getType
    // 用首元素推断造成 E3009 假阳性（嵌套 Array<Array<T>>、灵活整数、空数组）。
    if (expected) {
        TypeInfo want = expected->peelRef();
        if (auto paren = dynamic_cast<p<ExprParenNode>>(expr)) {
            visitExpr(paren->expr(), expected);
            if (paren->expr() && paren->expr()->hasResolvedType()) {
                paren->setResolvedType(paren->expr()->resolvedType());
            }
            return;
        }
        if (auto lam = dynamic_cast<p<LambdaExprNode>>(expr)) {
            if (want.isFn()) applyLambdaFnExpected(lam, want);
        }
        if (isFlexibleIntExpr(expr) && isIntTypeName(want.name)) {
            tryInferIntType(expr, want);
        } else if (isFlexibleIntExpr(expr) && want.isNullable()) {
            if (auto inner = want.nullableInnerType()) {
                if (isIntTypeName(inner->name)) tryInferIntType(expr, *inner);
            }
        } else if (isFlexibleIntExpr(expr) && want.isRc()) {
            if (auto inner = want.rcElementType()) {
                if (isIntTypeName(inner->name)) tryInferIntType(expr, *inner);
            }
        }
        if (isFlexibleNullExpr(expr) && want.isNullable()) {
            tryInferNullType(expr, want);
        }
        if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
            if (want.isArray() || want.isArrayGeneric()) {
                checkArrayLiteral(n, want);
                return;
            }
        }
        if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
            checkArrayInit(n, &want);
            return;
        }
        if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
            if (want.isTuple()) {
                const auto& w = want.tupleElements();
                const auto& src = n->elements();
                for (size_t i = 0; i < src.size(); ++i) {
                    visitExpr(src[i], (i < w.size() && w[i]) ? w[i].get() : nullptr);
                }
                n->setResolvedType(want);
                return;
            }
        }
    }

    // Phase B：getType 诊断默认由 SemaPass 重抛。
    // Phase C：泛型模板体内再吞依赖 T 具体化的码；形态检查仍重抛。
    // 方法点 E3095 先记下，给后面的 Dot 分支报 E1101/E1140；ID-literal 的 E3095 重抛。
    // if / match / try：先下钻子树带靶向（空 `[]` → Array<T>），再 getType 汇合，
    // 否则 `[]` 的 `[__empty * 0]` 会在子节点 resolved 写好之前假阳性 E3005/E7010。
    std::optional<YuxError> deferredMethodE3095;
    const bool delayCtrlResolved = dynamic_cast<p<ExprIfElseNode>>(expr) ||
                                   dynamic_cast<p<ExprOneLineIfElseNode>>(expr) ||
                                   dynamic_cast<p<ExprMatchNode>>(expr) || dynamic_cast<p<ExprTryCatchNode>>(expr);
    auto writeResolvedFromGetType = [&](p<ExprNode> n) {
        try {
            n->setResolvedType(n->getType());
        } catch (const YuxError& e) {
            const bool methodPointE3095 =
                isMethodPointCall(n) && e.getCode() && std::string_view(e.getCode()) == "E3095";
            const bool swallow =
                methodPointE3095 || (!_currentTypeParams.empty() && !isMorphologicalGenericCode(e.getCode()));
            if (!swallow) throw;
            if (methodPointE3095) deferredMethodE3095 = e;
        } catch (...) { // NOLINT(bugprone-empty-catch) — release 仍吞非 YuxError；debug 下 assert
#ifndef NDEBUG
            assert(false && "getType threw non-YuxError; SemaPass must not swallow unknown failures");
#endif
        }
    };
    auto finishCtrlResolved = [&](p<ExprNode> n) {
        writeResolvedFromGetType(n);
        if (!expected || !n->hasResolvedType()) return;
        TypeInfo want = expected->peelRef();
        if (!want.isArrayGeneric()) return;
        auto got = n->resolvedType();
        if (isEmptyArrayType(got) || got.isArray()) n->setResolvedType(want);
    };
    if (!delayCtrlResolved) writeResolvedFromGetType(expr);

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // Phase 2e 构造模型重构: `#Static fn` 体内禁用 `$` (E3128).
        // `$` 在 ast_builder 里生成 ExprLiteralNode(LiteralObjNode("$")),
        // `$.field` / `$.method()` 读路径会递归到此, 一处拦截即覆盖.
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(n->literal())) {
            if (obj->getValue().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
            }
            // Phase B-1: use-after-move 检查 (E4033)
            if (_currentFn && obj->getValue().getText() != "$") {
                string varName = obj->getValue().getText();
                if (_movedVars.count(varName)) {
                    // lambda 体内仅当 varName 不是 lambda 形参时才报错
                    if (!_currentLambda) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                    } else {
                        bool isParam = false;
                        for (auto& p : _currentLambda->params()) {
                            if (p.name.getText() == varName) {
                                isParam = true;
                                break;
                            }
                        }
                        if (!isParam) {
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                        }
                    }
                }
            }
        }
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps()) {
                visitExpr(e);
                tryValidateToString(e);
            }
        }
        // v0.16 闭包捕获: lambda body 内标识符引用检查。
        // - 引用外层 Heap<T> (非空) 变量 → E4024 (Heap 按值捕获禁止, §7.3 / [#18])
        // - 引用外层 T& 变量 → 标记 hasRefCapture (E4022 数据收集)
        // - 不支持的捕获类型（struct / enum / Fn 等）→ E2029；堆句柄记下供
        //   退出 lambda 时与 T& 混捕对打。
        // 非 ID-obj / 全局 / template 插值等其它字面量形态不触发捕获, 跳过。
        // $ 在方法体内 lambda 是 Self&, 同样标记 hasRefCapture。
        // 查找起点：lambda body 的 *父* 作用域（块作用域后 let 不在 FnNode 上；
        // 也避免把 lambda 体内 / 嵌套块的本地 Heap let 误判为捕获）。
        if (_currentLambda && _currentFn) {
            auto obj2 = dynamic_cast<p<LiteralObjNode>>(n->literal());
            if (obj2) {
                string varName = obj2->getValue().getText();
                bool isParam = false;
                for (auto& p : _currentLambda->params()) {
                    if (p.name.getText() == varName) {
                        isParam = true;
                        break;
                    }
                }
                if (!isParam) {
                    SymbolInfo* sym = nullptr;
                    ScopeNode* lookupFrom = nullptr;
                    if (auto body = _currentLambda->bodyScope()) {
                        lookupFrom = body->parentScope();
                        if (lookupFrom) {
                            sym = lookupFrom->lookupSymbol(varName);
                        }
                    }
                    if (!sym) {
                        lookupFrom = _currentFn;
                        sym = _currentFn->lookupSymbol(varName);
                    }
                    if (sym) {
                        const auto& t = sym->type;
                        // 函数名 / 类型名不是变量捕获，跳过 Heap/Ref 捕获检查
                        bool isVarOrParam = sym->kind != SymbolKind::Function && sym->kind != SymbolKind::Struct;
                        if (isVarOrParam && t.isHeap()) {
                            auto elem = t.heapElementType();
                            string elemName = elem ? elem->getFullName() : string("?");
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4024, elemName,
                                           varName, elemName);
                        }
                        if (isVarOrParam && t.isRef()) {
                            _currentLambdaHasRefCapture = true;
                        }
                        // E2029：仅外层 local（非全局）。模板形参等实例化后再查。
                        if (sym->kind == SymbolKind::Variable && isOuterLocalCapture(lookupFrom, sym, varName)) {
                            TypeInfo capTy = applyInstSubst(t);
                            if (!isCurrentTypeParam(capTy)) {
                                auto kind = classifyLambdaCapture(capTy);
                                if (kind == LambdaCapKind::Unsupported) {
                                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E2029,
                                                   varName, capTy.name);
                                }
                                if (kind == LambdaCapKind::Handle || kind == LambdaCapKind::HeapNullable) {
                                    if (!_currentLambdaHasHandleCapture) {
                                        _currentLambdaHasHandleCapture = true;
                                        _currentLambdaHandleCapName = varName;
                                        _currentLambdaHandleCapTypeName = capTy.getFullName();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        // Phase 3.4.f.2: int 字面量越界 (E3103) — getType 仅返回类型不解析值,
        // 这里主动调 sema::parseIntLiteral 触发越界 / 非法格式校验.
        if (auto intLit = dynamic_cast<p<LiteralIntNode>>(n->literal())) {
            (void)sema::parseIntLiteral(intLit->getValue().getText(), n->getLineNumber(), n->getColumn());
        }
        // Phase B：标识符解析挂到 AST，codegen 读 resolvedSymbol。
        if (auto objSym = dynamic_cast<p<LiteralObjNode>>(n->literal())) {
            string varName = objSym->getValue().getText();
            if (varName != "$") {
                SymbolInfo* sym = nullptr;
                if (auto sc = n->findNearestScope()) {
                    sym = sc->lookupSymbol(varName);
                }
                if (!sym && _currentFn) {
                    sym = _currentFn->lookupSymbol(varName);
                }
                if (sym) n->setResolvedVar(sym);
                if (sym && !callCallee && !_inDotBase) {
                    if (symbolIsPathPrefix(sym)) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5016, varName);
                    }
                    if (sym->kind == SymbolKind::Struct) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5017, varName);
                    }
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 单点: 自定义 struct 二元运算符方法解析 (E3073 + byval hint).
        string m = (n->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        tryValidateStringPlus(n);
        return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprMulDivModNode::Op::Mul:
            m = "mul";
            break;
        case ExprMulDivModNode::Op::Div:
            m = "div";
            break;
        case ExprMulDivModNode::Op::Mod:
            m = "mod";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprBinOpNode::Op::And:
            m = "and";
            break;
        case ExprBinOpNode::Op::Or:
            m = "or";
            break;
        case ExprBinOpNode::Op::Xor:
            m = "xor";
            break;
        case ExprBinOpNode::Op::Shl:
            m = "shl";
            break;
        case ExprBinOpNode::Op::Shr:
            m = "shr";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Phase C：subst 后再查 Weak ==/!= / Ptr 排序 / &&·|| 两侧类型（模板形参跳过）。
        tryValidateCompareForm(n);
        // Bucket 6 单点: 自定义 struct 比较运算符方法解析 (E3073 + byval hint).
        // AndAnd / OrOr 是逻辑短路, 无方法名映射, 类型一致性由 tryValidateCompareForm 查.
        string m;
        switch (n->op()) {
        case ExprCompareNode::Op::Eq:
            m = "eq";
            break;
        case ExprCompareNode::Op::Ne:
            m = "ne";
            break;
        case ExprCompareNode::Op::Lt:
            m = "lt";
            break;
        case ExprCompareNode::Op::Le:
            m = "le";
            break;
        case ExprCompareNode::Op::Gt:
            m = "gt";
            break;
        case ExprCompareNode::Op::Ge:
            m = "ge";
            break;
        default:
            break;
        }
        if (!m.empty()) {
            tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr(), nullptr, true);

        // Phase C：重载前实参靶向类型。不看实参类型即可确定的形参才下钻：
        // 单 arity 非泛型候选 / Fn 值 callee / 非泛型方法，以及同 arity 重载
        // 各位都相同的类型。不一致的位置留空，不猜候选。
        // 泛型：显式 typeArgs 替换后的形参；接收者已带 typeArgs 的泛型 struct 方法。
        vector<TypeInfo> callArgExpected;
        const vector<TypeInfo>* callArgExpPtr = nullptr;
        const bool hasTypeArgs = !n->getTypeArgs().empty();
        if (hasTypeArgs) {
            if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                    string fnName = obj->getValue().getText();
                    if (auto* genFn = uniqueNonBuiltinGenericFn(_file, fnName)) {
                        map<string, TypeInfo> subst;
                        if (fillSubstFromTypeNodes(genFn->header()->typeParams(), n->getTypeArgs(), subst) &&
                            substHeaderParams(genFn->header(), subst, callArgExpected)) {
                            callArgExpPtr = &callArgExpected;
                        }
                    }
                }
            }
        } else if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                auto* structDecl = _names.lookupStruct(fnName);
                if (structDecl && !structDecl->isGeneric()) {
                    vector<FnSymbolInfo*> cands;
                    collectOverloadsBoth(_file, _sdkFile, fnName + "." + fnName, cands);
                    if (agreedArityParamTypes(cands, n->getArgs().size() + 1, 1, callArgExpected)) {
                        callArgExpPtr = &callArgExpected;
                    }
                } else if (!structDecl && !_file->getGenericFunction(fnName).first) {
                    vector<FnSymbolInfo*> cands;
                    collectOverloadsBoth(_file, _sdkFile, fnName, cands);
                    if (agreedArityParamTypes(cands, n->getArgs().size(), 0, callArgExpected)) {
                        callArgExpPtr = &callArgExpected;
                    }
                }
                if (!callArgExpPtr) {
                    // 函数名 getType 可能只编码某一个重载，不能当靶向类型。
                    // 仅局部 / 形参上的 Fn 值可以。
                    SymbolInfo* sym = nullptr;
                    if (auto sc = lit->findNearestScope()) {
                        sym = sc->lookupSymbol(fnName);
                    }
                    if (!sym && _currentFn) {
                        sym = _currentFn->lookupSymbol(fnName);
                    }
                    if (sym && sym->kind == SymbolKind::Variable && sym->type.isFn()) {
                        copyFnParamTypes(sym->type, callArgExpected);
                        callArgExpPtr = &callArgExpected;
                    }
                }
            }
        } else if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            TypeInfo baseType;
            bool baseOk = true;
            try {
                baseType = dotCallee->baseExpr()->hasResolvedType() ? dotCallee->baseExpr()->resolvedType()
                                                                    : dotCallee->baseExpr()->getType();
                baseType = applyInstSubst(baseType);
                if (dotCallee->isSafe()) baseType = peelSafeDotInner(baseType);
                baseType = baseType.peelAutoDeref();
            } catch (...) { // NOLINT(bugprone-empty-catch)
                baseOk = false;
            }
            if (baseOk && !baseType.name.empty() && !isBuiltinType(baseType.name) && !baseType.isArrayGeneric() &&
                !baseType.isPtr() && !baseType.isDyn()) {
                if (auto* sd = _names.lookupStruct(baseType)) {
                    string member = dotCallee->member();
                    if (dotCallee->hasSpecQualifier()) {
                        member = member + "@" + dotCallee->specQualifier();
                    }
                    if (!sd->isGeneric()) {
                        vector<FnSymbolInfo*> cands;
                        collectOverloadsBoth(_file, _sdkFile, baseType.name + "." + member, cands);
                        if (agreedArityParamTypes(cands, n->getArgs().size() + 1, 1, callArgExpected)) {
                            callArgExpPtr = &callArgExpected;
                        }
                    } else {
                        map<string, TypeInfo> subst;
                        if (fillSubstFromGenericArgs(sd->typeParams(), baseType.genericArgs, subst)) {
                            auto* impl = lookupStructImpl(_file, _sdkFile, baseType);
                            if (auto* hdr = uniqueMethodHeader(impl, dotCallee->member(), n->getArgs().size(),
                                                               /*wantStatic=*/false)) {
                                if (substHeaderParams(hdr, subst, callArgExpected)) {
                                    callArgExpPtr = &callArgExpected;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            TypeInfo calleeType;
            try {
                calleeType = n->getCalleeExpr()->hasResolvedType() ? n->getCalleeExpr()->resolvedType()
                                                                   : n->getCalleeExpr()->getType();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            if (calleeType.isFn()) {
                copyFnParamTypes(calleeType, callArgExpected);
                callArgExpPtr = &callArgExpected;
            }
        }
        visitExprList(n->getArgs(), callArgExpPtr);

        // E4025 (DRAFT-heap-types §8.3a.5.1): 容器构造 turbofish 内嵌 Heap 拦截.
        // 形态: `Rc:<Heap<T>>(...)` / `Weak:<Heap<T>>(...)` / `Array:<Heap<T>>(...)`
        // 以及任意 call 的 turbofish 内出现 `Rc<Heap<T>>` / `Weak<...>` / `Array<...>` 嵌套.
        // 与 compiler_types.cpp:438/464/484 镜像.
        if (!n->getTypeArgs().empty()) {
            int eline = n->getLineNumber();
            int ecol = n->getColumn();
            string calleeName;
            if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                    calleeName = obj->getValue().getText();
                }
            }
            try {
                auto t0 = n->getTypeArgs()[0]->getType();
                if ((calleeName == "rc" || calleeName == "Rc" || calleeName == "Weak" || calleeName == "Array") &&
                    t0.isHeap()) {
                    auto inner = t0.heapElementType();
                    throw YuxError(eline, ecol, ErrorCode::E4025, calleeName, inner ? inner->name : std::string("?"));
                }
                if ((calleeName == "rc" || calleeName == "Rc" || calleeName == "Weak") && t0.isDyn()) {
                    throw YuxError(eline, ecol, ErrorCode::E1132, calleeName + "<" + t0.getFullName() + ">");
                }
                for (auto& tn : n->getTypeArgs()) {
                    try {
                        validateRcContainerBans(tn->getType(), eline, ecol);
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
            }
        }

        // Phase 3.3 前置.4: ID-callee / 非-ID-callee 的错误传播校验
        // (E7001/E7004/E7006/E7016). 协议与 Compiler::compileCallExpr 顶部
        // 完全一致 —— ID-literal 走 checkErrPropagateForIdCall (按 fnName
        // 取第一候选, 10e 视多重载为同质); 非 ID-literal + ! + 不在 try 内
        // 走 checkBangWithoutFallibleCaller. _tryStack 顶端的 vector* 用作
        // tryBlockSeenErrs (callee 是 #Fallible 时把 errType append 进去,
        // 供下方 try/catch 分支的 E7002 穷尽性使用).
        auto calleeExpr = n->getCalleeExpr();
        if (auto litCallee = dynamic_cast<p<ExprLiteralNode>>(calleeExpr)) {
            if (auto objLit = dynamic_cast<p<LiteralObjNode>>(litCallee->literal())) {
                string fnNameProp = objLit->getValue().getText();
                vector<FnSymbolInfo*> cands;
                _file->collectFnOverloads(fnNameProp, cands);
                if (_sdkFile && _sdkFile != _file) {
                    _sdkFile->collectFnOverloads(fnNameProp, cands);
                }
                const FnSymbolInfo* sym = cands.empty() ? nullptr : cands.front();
                vector<string>* seen = _tryStack.empty() ? nullptr : &_tryStack.back();
                sema::checkErrPropagateForIdCall(_currentFn, n, fnNameProp, sym, seen, _sourcePath, _currentLambda);
            } else if (n->errPropagate()) {
                if (_tryStack.empty()) {
                    sema::checkBangWithoutFallibleCaller(_currentFn, n, _currentLambda);
                }
            }
        } else if (n->errPropagate() && !dynamic_cast<p<ExprDotNode>>(calleeExpr)) {
            if (_tryStack.empty()) {
                sema::checkBangWithoutFallibleCaller(_currentFn, n, _currentLambda);
            }
        }

        // Phase 3.3 前置.2：SemaPass 主动驱动重载解析 + 灵活整数推断.
        // 只接管"纯 ID callee + 无显式类型实参 + 非泛型"的情形, 与
        // compiler_call.cpp 中 `else if (structDecl)` / `else` 分支的进入条件保持一致;
        // 其余 (Fn 类型 callee / 泛型 fn 或 ctor / 方法调用) 仍由 codegen 自行处理.
        //
        // 副作用幂等性: tryInferIntType 仅在 isFlexibleIntExpr 为真时改写; SemaPass
        // 跑完后字面量已带类型, Compiler 端再次调用 resolve* 时 isFlexibleIntExpr 返回 false,
        // 不会重复推断 (见 call_resolve.h 的契约说明).
        if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                int line = n->getLineNumber();
                int col = n->getColumn();
                bool hasTypeArgs = !n->getTypeArgs().empty();

                // Phase 3.3.2.f: 自由 intrinsic arity 校验 (E6027).
                // helper 仅对清单内 fnName 实际校验, 其他 fnName 是 no-op,
                // 故无条件调用安全; 与 Compiler 端 compileExternalOrSdkFunctionCall
                sema::validateFreeIntrinsicArity(fnName, n->getArgs().size(), line, col);

                auto* structDecl = _file->getStructDecl(fnName);
                if (!structDecl && _sdkFile) structDecl = _sdkFile->getStructDecl(fnName);

                // Phase 3.3.2.f: Builtin 泛型 intrinsic 的 shape + type-shape 校验.
                // 接管 E6017/E6018/E6026-E6029/E6030/E6031/E6032（与
                // Compiler::compileGenericFunctionCall 的 #Builtin 分支镜像）.
                // 显式 typeArgs 或推断后查；as_ref(Heap<T>) 抽内层（unify 对不上 Rc<T>）.
                // assert_eq 的 String& 非泛型重载优先，不走 #Builtin.
                // 模板形参跳过依赖 T 的形态；与 T 无关的形态模板期也报.
                if (!structDecl) {
                    auto [genFn, _] = _file->getGenericFunction(fnName);
                    // getGenericFunction 已搜索 wildcardImports，不再需要手动 SDK 回退
                    if (genFn && genFn->header()->hasAnno("Builtin")) {
                        vector<TypeInfo> argTypes;
                        bool argTypesOk = true;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(applyInstSubst(a->getType()));
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }

                        vector<TypeInfo> typeArgs;
                        bool typeArgsOk = true;
                        if (hasTypeArgs) {
                            try {
                                for (auto& tn : n->getTypeArgs())
                                    typeArgs.push_back(applyInstSubst(tn->getType()));
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else {
                            auto peelStr = [](TypeInfo t) {
                                if (t.isRef() && t.refElementType()) t = *t.refElementType();
                                return t;
                            };
                            const bool stringOverload = fnName == "assert_eq" && argTypesOk && argTypes.size() >= 2 &&
                                                        peelStr(argTypes[0]).isString() &&
                                                        peelStr(argTypes[1]).isString();
                            if (!argTypesOk || stringOverload) {
                                typeArgsOk = false;
                            } else if (fnName == "as_ref" && !argTypes.empty() && argTypes[0].isHeap()) {
                                auto inner = argTypes[0].heapElementType();
                                if (!inner) {
                                    throw YuxError(line, col, ErrorCode::E6029, fnName, argTypes[0].getFullName());
                                }
                                typeArgs.push_back(applyInstSubst(*inner));
                            } else {
                                try {
                                    sema::inferGenericFnTypeArgs(n, genFn, fnName, argTypes, typeArgs);
                                    for (auto& t : typeArgs)
                                        t = applyInstSubst(t);
                                } catch (const YuxError&) {
                                    if (_currentTypeParams.empty() || !_instSubst.empty()) throw;
                                    typeArgsOk = false;
                                } catch (...) {
                                    typeArgsOk = false;
                                }
                            }
                        }

                        if (typeArgsOk && !typeArgs.empty()) {
                            sema::validateBuiltinIntrinsicShape(fnName, typeArgs.size(), n->getArgs().size(), line,
                                                                col);
                            if (argTypesOk) {
                                bool skipTypeShape = false;
                                if (isCurrentTypeParam(typeArgs[0]) && (fnName == "assert_eq" || fnName == "same_ref" ||
                                                                        fnName == "ptr_of" || fnName == "copy_of")) {
                                    skipTypeShape = true;
                                }
                                if (!argTypes.empty() && isCurrentTypeParam(argTypes[0]) &&
                                    (fnName == "as_ref" || fnName == "weak")) {
                                    skipTypeShape = true;
                                }
                                if (!skipTypeShape) {
                                    sema::validateBuiltinIntrinsicTypeShape(fnName, typeArgs, argTypes, n->getArgs(),
                                                                            _file, _sdkFile, line, col);
                                }

                                // Phase B-1: copy_of 拒绝 #NoCopy 类型（含 Array<T>，深拷贝统一用 .clone()）
                                if (fnName == "copy_of" && !isCurrentTypeParam(typeArgs[0])) {
                                    const auto& T = typeArgs[0];
                                    if (isNoCopyTypeIn(T, _file, _sdkFile)) {
                                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031, T.name,
                                                       "copy_of", T.name);
                                    }
                                }
                            }
                        }
                    }
                }

                // Phase 6D: 同名 ctor 已被 sema E3130 拦截在定义点; 调用点 `Foo(args)`
                // 不再分派 ctor, 形态 / arity 校验全部失效, 整段块移除.

                // Phase B-1: move:<T>(var) — 标记源变量为 moved (E4033 判定依据)
                if (_currentFn && fnName == "move" && !n->getArgs().empty()) {
                    if (auto argLit = dynamic_cast<p<ExprLiteralNode>>(n->getArgs()[0])) {
                        if (auto argObj = dynamic_cast<p<LiteralObjNode>>(argLit->literal())) {
                            _movedVars.insert(argObj->getValue().getText());
                        }
                    }
                }

                // 泛型 fn + 显式 typeArgs 的 arity 校验 (E6010, 与 compileCallExpr 入口一致)
                if (!structDecl && hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn2, _] = _file->getFunctionWithOwner(fnName);
                    if (genFn2 && genFn2->header()->isGeneric()) {
                        sema::validateGenericTypeArgsArity(fnName, genFn2->header()->typeParams().size(),
                                                           n->getTypeArgs().size(), line, col);
                    }
                }

                // Bucket 4 收口 (CURRENT-check.md): 泛型 fn typeArgs 的 spec bound
                // 校验 (E1106, E3032 由 helper 内部抛 draft 名未声明). 显式 typeArgs
                // 直接收取; 隐式 typeArgs 走 sema::inferGenericFnTypeArgs.
                // E6012/E6013：调用点与实例化复查重抛；未实例化的模板体内吞掉
                // （与未调用泛型 fn 一致，yux-check 是 yux build 的子集）.
                if (_yux && !structDecl) {
                    // 收集所有同名泛型重载（如 print<T>(x T) + print<T>(x T&)），
                    // 用 resolveBestGenericOverload 选最佳匹配后再 infer + spec-bound 校验。
                    // collectGenericFunctions 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
                    vector<pair<FnNode*, FileNode*>> genericFns;
                    _file->collectGenericFunctions(fnName, genericFns, _file);
                    FnNode* genericFn = nullptr;
                    p<FileNode> fnOwner = _file;
                    if (!genericFns.empty()) {
                        // 多泛型重载消歧：用实参类型驱动，选 Ref/Ref 匹配最佳者
                        if (genericFns.size() > 1) {
                            vector<TypeInfo> disambigArgTypes;
                            bool disambigOk = true;
                            for (auto& a : n->getArgs()) {
                                try {
                                    disambigArgTypes.push_back(a->getType());
                                } catch (...) {
                                    disambigOk = false;
                                    break;
                                }
                            }
                            if (disambigOk && !disambigArgTypes.empty()) {
                                auto [best, bestOwner] =
                                    sema::resolveBestGenericOverload(genericFns, n, fnName, disambigArgTypes);
                                if (best) {
                                    genericFn = best;
                                    fnOwner = bestOwner;
                                }
                            }
                        }
                        if (!genericFn) {
                            genericFn = genericFns[0].first;
                            fnOwner = genericFns[0].second;
                        }
                    }
                    if (genericFn && genericFn->header()->isGeneric() && !genericFn->header()->hasAnno("Builtin")) {
                        vector<TypeInfo> typeArgs;
                        bool argTypesOk = true;
                        vector<TypeInfo> argTypes;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(applyInstSubst(a->getType()));
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }
                        // BUG5: 非泛型重载优先 (call_fn.cpp Phase 4b)；
                        // 同名存在严格匹配的非泛型时，spec-bound / infer 不应越过重载消歧。
                        // 泛型自身的符号表项（形参与声明一致，含 `wrap<U>(x i32)` 这种
                        // T 不出现在形参里的）不当成非泛型。
                        if (argTypesOk && !hasTypeArgs) {
                            auto* nonGen = _file->lookupFnSymbolWithParams(fnName, argTypes);
                            if (!nonGen && _sdkFile && _sdkFile != _file) {
                                nonGen = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                            }
                            if (nonGen) {
                                bool isGenericOwnSym = false;
                                for (auto& [gFn, _] : genericFns) {
                                    auto gp = gFn->header()->params();
                                    if (nonGen->params.size() != gp.size()) continue;
                                    bool paramsMatch = true;
                                    for (size_t i = 0; i < gp.size(); ++i) {
                                        if (!gp[i]->type()) continue;
                                        if (nonGen->params[i] != gp[i]->type()->getType()) {
                                            paramsMatch = false;
                                            break;
                                        }
                                    }
                                    if (paramsMatch) {
                                        isGenericOwnSym = true;
                                        break;
                                    }
                                }
                                if (!isGenericOwnSym) {
                                    argTypesOk = false; // 真非泛型命中，跳过泛型 infer
                                }
                            }
                        }
                        bool typeArgsOk = true;
                        if (hasTypeArgs) {
                            try {
                                for (auto& tn : n->getTypeArgs()) {
                                    typeArgs.push_back(applyInstSubst(tn->getType()));
                                }
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else if (argTypesOk) {
                            try {
                                sema::inferGenericFnTypeArgs(n, genericFn, fnName, argTypes, typeArgs);
                                for (auto& t : typeArgs)
                                    t = applyInstSubst(t);
                            } catch (const YuxError&) {
                                // 调用点 / 实例化后：E6012 arity、E6013 无法反推。
                                // 未实例化模板体内仍吞（Compiler 同样不编未调用泛型体）.
                                if (_currentTypeParams.empty() || !_instSubst.empty()) throw;
                                typeArgsOk = false;
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else {
                            typeArgsOk = false;
                        }
                        if (typeArgsOk && typeArgs.size() == genericFn->header()->typeParams().size()) {
                            sema::validateGenericTypeArgsSpecBound(&_yux->specRegistry(), &_yux->specImplChecker(),
                                                                   fnOwner, genericFn->header(), typeArgs, line, col);
                            // Phase C：typeArgs 已知后按替换后的形参检查实参（E3014）。
                            vector<TypeInfo> instParams;
                            if (substGenericCallParams(genericFn->header(), genericFn->header()->typeParams(), typeArgs,
                                                       instParams)) {
                                for (size_t i = 0; i < n->getArgs().size() && i < instParams.size(); ++i) {
                                    checkCallArgAgainst(n->getArgs()[i], instParams[i], line, col, _file, _sdkFile,
                                                        _currentTypeParams, &_instSubst);
                                }
                            }
                            checkGenericFnInst(genericFn, typeArgs);
                        }
                    }
                }

                // 仅在无显式 typeArgs + 非泛型路径上才驱动重载解析:
                // 泛型 fn 的 typeArgs 替换后实参检查在上方；非泛型走 resolveFnOverload。
                if (!hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn3, _] = _file->getFunctionWithOwner(fnName);
                    if (!genFn3 || !genFn3->header()->isGeneric()) {
                        if (structDecl && !structDecl->isGeneric()) {
                            sema::resolveCtorOverload(_file, fnName, n->getArgs(), line);
                            if (_sdkFile && _sdkFile != _file) {
                                sema::resolveCtorOverload(_sdkFile, fnName, n->getArgs(), line);
                            }
                        } else if (!structDecl) {
                            sema::resolveFnOverload(_file, _sdkFile, fnName, n->getArgs(), line);
                            // Phase 3.3.1.c: 非泛型 ID-callee 解析到 fnSymbol 后做可见性校验 (E6006).
                            // argTypes 经 getType() 计算; 若任一实参未推断 (lambda 形参等),
                            // 跳过并交给 Compiler 兜底.
                            vector<TypeInfo> argTypes;
                            bool ok = true;
                            for (auto& a : n->getArgs()) {
                                try {
                                    argTypes.push_back(a->getType());
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok) {
                                auto* fnSym = _file->lookupFnSymbolWithParams(fnName, argTypes);
                                if (!fnSym && _sdkFile && _sdkFile != _file) {
                                    fnSym = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                                }
                                sema::validateFnSymbolVisibility(fnSym, _file->moduleName(), fnName, line, col);
                                if (fnSym) n->setResolvedFn(fnSym);
                                // Phase B-1: #NoCopy 类型不可按值传参
                                if (fnSym) {
                                    for (size_t i = 0; i < n->getArgs().size() && i < fnSym->params.size(); ++i) {
                                        const auto& pt = fnSym->params[i];
                                        if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                            if (!isFreshHandleExpr(n->getArgs()[i])) {
                                                throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031,
                                                               pt.name, "按值传参", pt.name);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Phase 3.3.1.a: Dot-callee 包/模块别名调用 (E6001-E6005).
        // 与 compileMethodCall line 195-254 同款条件; argTypes 经 getType()
        // 计算, 任一参数未推断时跳过, 交给 Compiler 兜底.
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            // DRAFT-spec-disambig-at §3.3: `$.m@SpecA()` / `obj.m@SpecA()` 显式消歧.
            // 校验三件: (1) baseType T 必须在 #Impl 列表里出现 SpecA; (2) SpecA 必须
            // 含名为 m 的签名; (3) SpecA.m 必须带默认体 (纯抽象签名无法 disambiguate).
            if (dotCallee->hasSpecQualifier()) {
                TypeInfo baseType;
                bool baseOk = true;
                try {
                    baseType = dotCallee->baseExpr()->getType();
                } catch (...) {
                    baseOk = false;
                }
                if (baseOk && !baseType.name.empty()) {
                    const string& specName = dotCallee->specQualifier();
                    const string memberName = dotCallee->member();
                    int dline = dotCallee->resolveLineNumber();
                    int dcol = dotCallee->resolveColumn();
                    // Dyn<D> / Dyn<D&> 上 `d.m@SpecA()`: SpecA 必须等于 D (vtable 只
                    // 携带 D 的槽位, 无法 dispatch 到其它 spec). 等于 D 时直接走常规
                    // Dyn dispatch (codegen 不重写 member).
                    if (baseType.isDyn()) {
                        if (_yux) {
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolvedDyn = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            if (resolvedDyn.decl && resolvedDyn.decl->name().getText() != specName) {
                                throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                            }
                        }
                        // OK: 等价于 d.m(); 走 Dyn dispatch
                    } else {
                        auto* implNode = _file->getStructImpl(baseType.name);
                        if (!implNode && _sdkFile) {
                            implNode = _sdkFile->getStructImpl(baseType.name);
                        }
                        bool inImplList = false;
                        if (implNode) {
                            for (const auto& ref : implNode->specRefs()) {
                                if (ref.name == specName) {
                                    inImplList = true;
                                    break;
                                }
                            }
                        }
                        if (!inImplList) {
                            throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                        }
                        SpecDeclNode* specDecl = nullptr;
                        if (_yux) {
                            auto resolved = _yux->specRegistry().resolve(specName, _file);
                            if (resolved) specDecl = resolved->decl;
                        }
                        if (specDecl) {
                            constexpr size_t kNoIdx = ~size_t{0};
                            auto sigIdx = kNoIdx;
                            for (size_t i = 0; i < specDecl->signatures().size(); ++i) {
                                if (specDecl->signatures()[i]->name().getText() == memberName) {
                                    sigIdx = i;
                                    break;
                                }
                            }
                            if (sigIdx == kNoIdx) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " (referenced via `@" + specName + "` — method missing in spec)");
                            }
                            if (!specDecl->hasDefaultBody(sigIdx)) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " with a default body (`@" + specName +
                                                   "` disambiguation requires a default-body method)");
                            }
                        }
                    } // end else (non-Dyn)
                }
            }
            // Phase C：字段当 callee 且类型不是 Fn 值 → E3095。
            // @Spec 已在上面报完 E1101/E1140；未知方法的 getType 假阳性不走这里。
            if (!dotCallee->hasSpecQualifier()) {
                bool isField = false;
                try {
                    isField = dotCallee->isFieldAccess();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                if (isField) {
                    TypeInfo fieldTy;
                    bool tyOk = true;
                    try {
                        fieldTy = dotCallee->hasResolvedType() ? dotCallee->resolvedType() : dotCallee->getType();
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                        tyOk = false;
                    }
                    if (tyOk && !fieldTy.empty() && !isFnCalleeType(fieldTy, _file, _sdkFile)) {
                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3095, fieldTy.getFullName());
                    }
                }
            }
            vector<TypeInfo> argTypes;
            bool ok = true;
            for (auto& a : n->getArgs()) {
                try {
                    argTypes.push_back(a->getType());
                } catch (...) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                auto modCall = sema::resolveModuleFnCall(_file, _yux, n, dotCallee, argTypes);
                if (!modCall.matched) {
                    // Phase 3.3.2.f: 镜像 Compiler::compileMethodCall 的 baseType 派发,
                    // 主动调用 3.3.2.a / 3.3.2.e 抠出的 helper.
                    //   * baseType.isArrayGeneric() → validateArrayMethodCall (E3055/E6042/E6027)
                    //   * isBuiltinType + isBuiltinMethodIn → validateOperatorMethodCall (E6027/E3070)
                    // baseType 经 getType() 计算; 任一异常 (lambda 形参等) → 跳过, 交 Compiler 兜底.
                    // SemaPass 走非泛型 fn / 非泛型 impl 路径, 不需要 applySubst (替换栈为空).
                    TypeInfo baseType;
                    bool baseOk = true;
                    try {
                        baseType = dotCallee->baseExpr()->getType();
                        baseType = applyInstSubst(baseType);
                        if (dotCallee->isSafe()) baseType = peelSafeDotInner(baseType);
                    } catch (...) {
                        baseOk = false;
                    }
                    if (baseOk) {
                        const string& member = dotCallee->member();
                        size_t argsCount = n->getArgs().size();
                        int dline = n->getLineNumber();
                        int dcol = n->getColumn();
                        if (baseType.isArrayGeneric()) {
                            bool baseIsLvalue = isLvalueArrayBase(dotCallee->baseExpr());
                            sema::validateArrayMethodCall(baseType, member, argsCount, baseIsLvalue, dline, dcol);
                        } else if (isBuiltinType(baseType.name) && isBuiltinMethodIn(_sdkFile, baseType.name, member)) {
                            sema::validateOperatorMethodCall(member, baseType, argsCount, dline, dcol);
                        } else if (baseType.isDyn() && _yux) {
                            // Bucket 4 收口 (CURRENT-check.md): Dyn<D> 方法调用 (E1131/E6016/
                            // E6012/E6015). 镜像 Compiler::compileDynMethodCall 顶部 — 通过
                            // resolveDynCalleeSpec 拿 specDecl, 再 resolveDynMethodSig 校验
                            // member 存在 + arity + 形参类型.
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolved = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            sema::resolveDynMethodSig(resolved.decl, resolved.qualified, baseType, member, argTypes,
                                                      dline, dcol);
                        }
                    }
                }
            }
        }

        // Fn-typed callee 实参类型校验：当 callee 静态类型为 Fn(...)R 时，
        // 检查每个实参与形参类型是否匹配（Ref<T> 不能隐式转为 T 等）。
        // 非 ID-literal callee（如 lambda 变量 f(xs[i])）走 compiler_lambda.cpp
        // 的 compileFnValueCall，此处提前检测避免 "bad signature" LLVM 断言。
        // 排除函数名字面量：它们虽然现在返回准确的 Fn TypeInfo，但实参类型
        // 校验（含 extern Ptr 自动转换）已在 codegen 的 matchFnParams 中完成。
        {
            TypeInfo calleeType;
            try {
                calleeType = n->getCalleeExpr()->getType();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }

            if (calleeType.isFn()) {
                // 函数名字面量（如 `strLen(a)`）的参数类型匹配（含 extern Ptr
                // 自动转换）由 codegen matchFnParams 负责，不在此处重复校验
                bool isFnNameLiteral = false;
                if (auto* lit = dynamic_cast<ExprLiteralNode*>(n->getCalleeExpr())) {
                    if (auto* objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                        auto scope = objLit->findNearestScope();
                        if (scope) {
                            auto* sym = scope->lookupSymbol(objLit->getValue().getText());
                            if (sym && sym->kind == SymbolKind::Function) {
                                isFnNameLiteral = true;
                            }
                        }
                    }
                }
                if (!isFnNameLiteral) {
                    vector<string>* seen = _tryStack.empty() ? nullptr : &_tryStack.back();
                    sema::checkErrPropagateForFnValueCall(_currentFn, n, calleeType, seen, _sourcePath, _currentLambda);
                    const auto& expectedParams = calleeType.fnParamTypes();
                    for (size_t idx = 0; idx < n->getArgs().size() && idx < expectedParams.size(); ++idx) {
                        try {
                            auto argType = n->getArgs()[idx]->getType();
                            if (expectedParams[idx] && !argType.name.empty() && !argType.isSelf() &&
                                !expectedParams[idx]->isSelf()) {
                                // Ref<T> 实参传给值类型形参 T
                                if (argType.isRef() && !expectedParams[idx]->isRef()) {
                                    auto inner = argType.refElementType();
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                                   expectedParams[idx]->getFullName(), argType.getFullName())
                                        .withHint(
                                            std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                                        "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                                        inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                                        inner ? inner->name : "?", inner ? inner->name : "?"));
                                }
                                // 值类型不匹配
                                if (!argType.isRef() && !expectedParams[idx]->isRef() &&
                                    argType != *expectedParams[idx]) {
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                                   expectedParams[idx]->getFullName(), argType.getFullName())
                                        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配",
                                                              argType.getFullName(),
                                                              expectedParams[idx]->getFullName()));
                                }
                            }
                        } catch (const YuxError&) {
                            throw;
                        } catch (...) { // NOLINT(bugprone-empty-catch)
                            // getType 失败: 留 Compiler 兜底
                        }
                    }
                } // if (!isFnNameLiteral)
            }
        }
        // struct 方法私有可见性检查（E6007）——因 yux-check 不跑 LLVM
        // codegen，必须在 sema 阶段独立校验。与 codegen compileStructMethodCall
        // 中的 validateStructMethodVisibility 同义，构成双重保障。
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            try {
                TypeInfo rawBase = dotCallee->baseExpr()->getType();
                if (rawBase.isRef()) {
                    if (auto inner = rawBase.refElementType()) rawBase = *inner;
                }
                if (rawBase.isRc()) {
                    if (auto inner = rawBase.rcElementType()) rawBase = *inner;
                }
                TypeInfo baseType = applyInstSubst(rawBase);
                if (dotCallee->isSafe()) baseType = peelSafeDotInner(baseType);
                // Phase C：实例化后 TypeParam 已换成具体类型，查方法是否存在。
                // 模板期 raw 仍是 T → 跳过。`<T : D>` 边界方法按边界认。
                if (!isCurrentTypeParam(baseType) && !_instSubst.empty() && isCurrentTypeParam(rawBase) &&
                    !baseType.isDyn() && !baseType.isPtr() && !baseType.name.empty()) {
                    string member = dotCallee->member();
                    auto rt = instantiatedMethodRet(_currentFn, _file, _sdkFile, rawBase, baseType, member);
                    if (!rt) {
                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3095, baseType.getFullName());
                    }
                    n->setResolvedType(*rt);
                }
                if (isCurrentTypeParam(baseType)) {
                    // Phase C：不透明 TypeParam，方法存在性等实例化后再查
                } else if (!baseType.name.empty() && !baseType.isDyn() && !isBuiltinType(baseType.name) &&
                           !baseType.isArrayGeneric() && !baseType.isPtr()) {
                    string member = dotCallee->member();
                    if (dotCallee->hasSpecQualifier()) {
                        member = member + "@" + dotCallee->specQualifier();
                    }
                    // Phase B：与 compileCallExpr 对齐，方法重载 + 灵活整数推断进 SemaPass。
                    sema::resolveMethodOverload(_file, _sdkFile, baseType.name, member, n->getArgs(),
                                                n->getLineNumber());
                    string methodFullName = baseType.name + "." + member;
                    vector<TypeInfo> methodParamTypes;
                    methodParamTypes.push_back(baseType);
                    for (auto& a : n->getArgs()) {
                        try {
                            methodParamTypes.emplace_back(a->getType());
                        } catch (...) {
                            methodParamTypes.emplace_back();
                        }
                    }
                    auto* methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    if (!methodSymbol && _sdkFile && _sdkFile != _file) {
                        methodSymbol = _sdkFile->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    }
                    if (methodSymbol) n->setResolvedFn(methodSymbol);
                    sema::validateStructMethodVisibility(methodSymbol, _currentStructName, baseType.name,
                                                         dotCallee->member(), n->getLineNumber(), n->getColumn());
                    // Phase C：泛型 struct 实例方法，用接收者 typeArgs 替换形参后检查实参
                    if (auto* sd = _names.lookupStruct(baseType)) {
                        if (sd->isGeneric()) {
                            map<string, TypeInfo> subst;
                            if (fillSubstFromGenericArgs(sd->typeParams(), baseType.genericArgs, subst)) {
                                auto* impl = lookupStructImpl(_file, _sdkFile, baseType);
                                if (auto* hdr = uniqueMethodHeader(impl, dotCallee->member(), n->getArgs().size(),
                                                                   /*wantStatic=*/false)) {
                                    vector<TypeInfo> instParams;
                                    if (substHeaderParams(hdr, subst, instParams)) {
                                        for (size_t i = 0; i < n->getArgs().size() && i < instParams.size(); ++i) {
                                            checkCallArgAgainst(n->getArgs()[i], instParams[i], n->getLineNumber(),
                                                                n->getColumn(), _file, _sdkFile, _currentTypeParams,
                                                                &_instSubst);
                                        }
                                    }
                                }
                                checkGenericImplInst(impl, subst);
                            }
                        }
                    }
                    // Phase B-1: 方法调用的 #NoCopy 按值传参检查
                    if (methodSymbol) {
                        for (size_t i = 0; i < n->getArgs().size() && i < methodSymbol->params.size(); ++i) {
                            const auto& pt = methodSymbol->params[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->getArgs()[i])) {
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031, pt.name,
                                                   "按值传参", pt.name);
                                }
                            }
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败或符号查找失败——留 Compiler 兜底
            }
        }
        // 未知方法等：getType 回落成基类型抛的 E3095，@Spec / TypeParam 已排除。
        if (deferredMethodE3095) {
            auto* dot = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr());
            if (dot && !dot->hasSpecQualifier()) {
                TypeInfo baseType;
                bool baseOk = true;
                try {
                    baseType = dot->baseExpr()->hasResolvedType() ? dot->baseExpr()->resolvedType()
                                                                  : dot->baseExpr()->getType();
                    baseType = applyInstSubst(baseType.peelAutoDeref());
                    if (dot->isSafe()) baseType = peelSafeDotInner(baseType);
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    baseOk = false;
                }
                if (!(baseOk && isCurrentTypeParam(baseType))) {
                    bool isField = false;
                    try {
                        isField = dot->isFieldAccess();
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                    }
                    // 实例化后接收者已有该方法：getType 仍按 T 抛的 E3095 不重抛。
                    if (!isField && !(baseOk && receiverHasMethod(baseType, dot->member(), _file, _sdkFile))) {
                        throw *deferredMethodE3095;
                    }
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        bool savedDotBase = _inDotBase;
        _inDotBase = true;
        visitExpr(n->baseExpr());
        _inDotBase = savedDotBase;

        // DRAFT-spec-reflect Phase 4: 实例形访问 `c.type` / `c.fields` / `c.methods` /
        // `c.variants` / `$.type` / `$.fields` 拦截 (草案 §5 / [#1.AB]);
        // 提示用 `<Type>::field` / `Self::field`.
        // 仅当 base 是已知 struct 且不含同名 instance 字段时触发 (用户若自己声明
        // `type` 字段, 走常规字段访问).
        {
            string mem = n->member();
            if (mem == "type" || mem == "fields" || mem == "methods" || mem == "variants") {
                TypeInfo bt;
                try {
                    bt = n->baseExpr()->getType();
                } catch (...) {
                    bt = TypeInfo();
                }
                if (bt.isRef()) {
                    if (auto inner = bt.refElementType()) bt = *inner;
                }
                if (bt.isRc()) {
                    if (auto inner = bt.rcElementType()) bt = *inner;
                }
                if (bt.kind == TypeKind::Normal && !bt.name.empty()) {
                    auto* sd = _file ? _file->getStructDecl(bt.name) : nullptr;
                    if (!sd && _sdkFile && _sdkFile != _file) sd = _sdkFile->getStructDecl(bt.name);
                    if (sd && sd->fieldIndex(mem) < 0) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E1138, mem, bt.name,
                                       bt.name, mem);
                    }
                }
            }
        }

        // Phase 3.4.d.2: 字段私有可见性 (E3042). safe `?.` 路径在 helper 内
        // 自跳过 (走 getType, SemaPass 默认重抛). 异常静默吞掉, 留 Compiler.
        try {
            sema::validateDotFieldPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }

        // Phase C：`?.` 在 subst 后查 Nullable / 内层字段（getType 在模板体吞掉）。
        if (n->isSafe()) {
            tryValidateSafeDot(n);
            return;
        }

        // Phase C：Field.value 读路径 E3133 / E3134（getType 对 E3134 不报）。
        if (!callCallee) {
            tryValidateReflectFieldValueRead(n);
        }

        // 点链首段既是路径前缀又是局部值：调用与取值都 E5015（不靠 callCallee）。
        // E5016/E5017 仍只在非调用取值时报（`io.read_file()` 合法）。
        if (!_inDotBase && _file) {
            string aliasName;
            vector<string> segs;
            if (ExprDotNode::parseChain(n, aliasName, segs)) {
                auto* pathSym = _file->lookupSymbol(aliasName);
                if (symbolIsPathPrefix(pathSym) && hasLocalValueNamed(n, aliasName)) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5015, aliasName,
                                   string("path prefix and local value"));
                }
                if (!callCallee) {
                    if (pathSym && pathSym->kind == SymbolKind::Module) {
                        FileNode* target = _file->moduleAlias(aliasName);
                        if (!target && _yux) target = _yux->module(pathSym->moduleName);
                        if (target && segs.size() == 1) {
                            const string& mem = segs[0];
                            if (hasPublicTypeIn(target, mem)) {
                                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5017,
                                               aliasName + "." + mem);
                            }
                            if (!target->lookupFnSymbol(mem)) {
                                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5016, aliasName);
                            }
                        }
                    } else if (pathSym && pathSym->kind == SymbolKind::Package) {
                        if (segs.size() == 1) {
                            if (_yux) {
                                (void)_yux->resolvePkgPath(_file, segs[0], n->resolveLineNumber(), pathSym->moduleName);
                            }
                            string dotted = aliasName + "." + segs[0];
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5016, dotted);
                        }
                        string childKey;
                        for (size_t i = 0; i + 1 < segs.size(); ++i) {
                            if (i) childKey += '.';
                            childKey += segs[i];
                        }
                        if (_yux) {
                            (void)_yux->resolvePkgPath(_file, childKey, n->resolveLineNumber(), pathSym->moduleName);
                        }
                        auto* target = _file->packageChild(aliasName, childKey);
                        if (!target) {
                            string dotted = aliasName;
                            for (auto& s : segs) {
                                dotted += '.';
                                dotted += s;
                            }
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5016, dotted);
                        }
                        const string& last = segs.back();
                        if (hasPublicTypeIn(target, last)) {
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5017,
                                           aliasName + "." + childKey + "." + last);
                        }
                        if (!target->lookupFnSymbol(last)) {
                            string dotted = aliasName + "." + childKey;
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E5016, dotted);
                        }
                    }
                }
            }
        }

        // Phase C：读路径字段（非调用 callee）。`x.foo()` 留给调用路径；
        // `to_*` 是内置转换；模块 / 包链不按字段查。
        // 方法当值 / 非字段 Fn·Dyn → E3090。模块函数值 `io.read_file` 是 fn_overload，不报。
        if (!callCallee) {
            string mem = n->member();
            bool skipField = mem.starts_with("to_");
            bool skipPkg = false;
            if (!skipField && n->hasResolvedType()) {
                const TypeInfo& rt = n->resolvedType();
                if (rt.name == "pkg_chain" || rt.name == "fn_overload") {
                    skipField = true;
                    skipPkg = true;
                } else if (rt.isFn() || rt.isDyn()) {
                    skipField = true;
                }
            }
            if (!skipField) {
                string aliasName;
                vector<string> segs;
                if (ExprDotNode::parseChain(n, aliasName, segs)) {
                    SymbolInfo* sym = nullptr;
                    if (auto sc = n->findNearestScope()) {
                        sym = sc->lookupSymbol(aliasName);
                    }
                    if (sym && (sym->kind == SymbolKind::Module || sym->kind == SymbolKind::Package)) {
                        skipField = true;
                        skipPkg = true;
                    }
                }
            }
            bool isMethod = false;
            if (!skipField) {
                try {
                    auto* base = n->baseExpr();
                    TypeInfo bt = base->hasResolvedType() ? base->resolvedType() : base->getType();
                    TypeInfo peeled = applyInstSubst(bt).peelAutoDeref();
                    isMethod = receiverHasMethod(peeled, mem, _file, _sdkFile) ||
                               (isCurrentTypeParam(peeled) &&
                                typeParamBoundHasMethod(_currentFn, _file, _sdkFile, peeled.name, mem));
                    if (!isMethod) {
                        tryValidateFieldChain(bt, {mem}, n->resolveLineNumber(), n->resolveColumn());
                    }
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // getType 失败，留 Compiler
                }
            }
            // 方法当值 / 非字段 Fn·Dyn：E3090。模块/包链是 `pkg.mod.fn()` 的路径前缀，不报。
            if ((skipField || isMethod) && !mem.starts_with("to_") && !skipPkg) {
                bool isField = false;
                try {
                    isField = n->isFieldAccess();
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                if (!isField) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3090);
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        // Phase B-1: 分支 _movedVars 汇合 — 各分支分别从 saved 出发，最后取并集
        // Phase C：块末尾值带靶向类型（嵌套数组 E3009）。
        auto savedMoved = _movedVars;
        visitBlock(n->thenBlock(), expected);
        auto afterThenMoved = std::move(_movedVars);
        _movedVars = savedMoved;

        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            auto savedElif = _movedVars;
            visitBlock(el->block(), expected);
            for (auto& v : _movedVars)
                afterThenMoved.insert(v);
            _movedVars = savedElif;
        }

        if (n->elseBlock()) {
            visitBlock(n->elseBlock(), expected);
            for (auto& v : afterThenMoved)
                _movedVars.insert(v);
        } else {
            _movedVars = std::move(afterThenMoved);
        }
        finishCtrlResolved(n);
        tryValidateIfElse(n);
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitExpr(n->trueValue(), expected);
        visitExpr(n->falseValue(), expected);
        finishCtrlResolved(n);
        tryValidateOneLineIfElse(n);
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices())
            visitExpr(i);
        // 与 compileArrayGetExpr 同款：空下标 E3060（g4 死防御）。读路径不要求 lvalue。
        if (n->indices().empty()) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3060, "access");
        }
        tryValidateIndexBase(n->arrayExpr(), n->resolveLineNumber(), n->resolveColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        checkEmptyArrayLiteral(n, expected);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprUnaryNode::Op::Neg:
            m = "neg";
            break;
        case ExprUnaryNode::Op::Rev:
            m = "inv";
            break;
        case ExprUnaryNode::Op::Not:
            m = "not";
            break;
        }
        tryValidateUnaryOpMethod(n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // v0.16 闭包捕获: sema 下钻 lambda body (策略 2b 宽松模式)。
        // - 形参类型可能缺 (由调用点反推), 不依赖形参类型的检查 deferred 给 codegen。
        // - 不依赖形参类型的检查在此完成: E2030 (捕获写禁) / E4024 (Heap 非空捕获禁) /
        //   E2029 (不支持的捕获 / T& 混堆句柄) / E4022 数据收集 (hasRefCapture)。
        // - 下钻前保存外层 lambda 状态, 支持嵌套闭包。
        auto savedLambda = _currentLambda;
        auto savedHasRef = _currentLambdaHasRefCapture;
        auto savedHasHandle = _currentLambdaHasHandleCapture;
        auto savedHandleName = _currentLambdaHandleCapName;
        auto savedHandleType = _currentLambdaHandleCapTypeName;
        _currentLambda = n;
        _currentLambdaHasRefCapture = false;
        _currentLambdaHasHandleCapture = false;
        _currentLambdaHandleCapName.clear();
        _currentLambdaHandleCapTypeName.clear();

        const TypeInfo* bodyExp = nullptr;
        TypeInfo bodyRetStorage;
        TypeInfo bodyRetResolved;
        if (lambdaExpectedRetType(n, bodyRetStorage)) {
            bodyRetResolved = resolveForRet(
                bodyRetStorage,
                RetCheck{.file = _file, .sdk = _sdkFile, .fn = _currentFn, .structName = _currentStructName});
            bodyExp = &bodyRetResolved;
        }
        RetCheck lamRetCtx{.file = _file,
                           .sdk = _sdkFile,
                           .fn = _currentFn,
                           .structName = _currentStructName,
                           .fallibleErr = {},
                           .typeParams = &_currentTypeParams,
                           .subst = &_instSubst};
        if (n->fallibleErrTypeNode()) {
            lamRetCtx.fallibleErr = n->fallibleErrTypeNode()->getType().name;
        } else if (bodyRetStorage.isFallible()) {
            lamRetCtx.fallibleErr = bodyRetStorage.fallibleErr;
        }
        if (n->bodyExpr()) {
            visitExpr(n->bodyExpr(), bodyExp);
            if (bodyExp) {
                int line = n->bodyExpr()->resolveLineNumber();
                checkRetExpr(n->bodyExpr(), bodyRetStorage, !bodyRetStorage.empty(), line, lamRetCtx);
            }
        } else {
            const auto& stmts = n->bodyStmts();
            for (size_t i = 0; i < stmts.size(); ++i) {
                const bool lastBare = (i + 1 == stmts.size()) && isBareTailExprStmt(stmts[i]);
                if (lastBare && bodyExp) {
                    auto se = dynamic_cast<p<StatementExprNode>>(stmts[i]);
                    if (!se || !se->expr()) {
                        visitStmt(stmts[i]);
                        continue;
                    }
                    if (auto ma = dynamic_cast<p<ExprMoveAssignNode>>(se->expr())) {
                        DiagnosticEngine::emit(
                            _sourcePath, YuxError(ma->resolveLineNumber(), ma->resolveColumn(), ErrorCode::E4030));
                    }
                    visitExpr(se->expr(), bodyExp);
                    int line = se->expr()->resolveLineNumber();
                    checkRetExpr(se->expr(), bodyRetStorage, !bodyRetStorage.empty(), line, lamRetCtx);
                } else {
                    visitStmt(stmts[i]);
                }
            }
        }

        // 4c：栈嵌入路径不能混入堆句柄字段（混合释放未实现）→ E2029。
        if (_currentLambdaHasRefCapture && _currentLambdaHasHandleCapture) {
            throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E2029, _currentLambdaHandleCapName,
                           _currentLambdaHandleCapTypeName);
        }

        // 将 hasRefCapture 写回 LambdaExprNode, 供 E4022 检查 (StatementRetNode /
        // StatementDeclareAssignNode) 读取。
        if (_currentLambdaHasRefCapture) {
            n->setHasRefCapture(true);
        }

        _currentLambda = savedLambda;
        _currentLambdaHasRefCapture = savedHasRef;
        _currentLambdaHasHandleCapture = savedHasHandle;
        _currentLambdaHandleCapName = std::move(savedHandleName);
        _currentLambdaHandleCapTypeName = std::move(savedHandleType);
        return;
    }
    if (auto n = dynamic_cast<p<ExprStructLitNode>>(expr)) {
        // Phase 2d 构造模型重构: `Self { ... }` 字段字面量校验.
        //   * 出现位: 仅 `#Static fn` 体内 (E3124, 仅 Self 形态).
        //   * 完整性: 必须列全所属结构体所有字段 (E3125).
        //   * 已知字段: `.name` 必须是所属结构体的字段 (E3126).
        //   * 唯一: 同名 `.field` 出现两次报 (E3127).
        // DRAFT-const-eval Phase 5: TypeName{...} 形态放行至任意 expr 位.
        int line = n->resolveLineNumber();
        int col = n->resolveColumn();
        string structName;
        TypeInfo structTy;
        StructDeclNode* decl = nullptr;
        if (n->isSelfForm()) {
            if (!_currentFn || !_currentFn->header()->isStatic() || _currentStructName.empty()) {
                throw YuxError(line, col, ErrorCode::E3124);
            }
            structName = _currentStructName;
            structTy = TypeInfo(structName);
            if (auto* ownerFile = fnDeclFile(_currentFn, _file)) {
                structTy.ownerModule = ownerFile->moduleName();
            }
            decl = _names.lookupStruct(structTy);
        } else {
            auto r = sema::resolveExprTypeLhs(_file, _yux, n->typePath(), line, col);
            structTy = r.type;
            structName = structTy.name;
            decl = r.structDecl ? r.structDecl : _names.lookupStruct(structTy);
        }
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E3124);
        }
        std::set<string> seen;
        for (auto& fi : n->fields()) {
            string fname = fi->name().getText();
            size_t fline = fi->name().getLine();
            int fcol = static_cast<int>(fi->name().getCharPositionInLine());
            if (decl->fieldIndex(fname) < 0) {
                throw YuxError(fline, fcol, ErrorCode::E3126, structName, fname);
            }
            if (!seen.insert(fname).second) {
                throw YuxError(fline, fcol, ErrorCode::E3127, fname);
            }
            TypeInfo fieldExpected;
            const TypeInfo* fieldExpPtr = nullptr;
            if (!decl->isGeneric()) {
                int fieldIdx = decl->fieldIndex(fname);
                if (fieldIdx >= 0) {
                    fieldExpected = decl->fields()[static_cast<size_t>(fieldIdx)]->getType();
                    fieldExpPtr = &fieldExpected;
                }
            }
            visitExpr(fi->value(), fieldExpPtr);
            // Phase B-1: #NoCopy 字段不可从现有变量隐式复制
            if (decl) {
                int fieldIdx = decl->fieldIndex(fname);
                if (fieldIdx >= 0) {
                    auto* fieldDecl = decl->fields()[fieldIdx];
                    auto fieldType = fieldDecl->getType();
                    if (isNoCopyTypeIn(fieldType, _file, _sdkFile)) {
                        if (!isFreshHandleExpr(fi->value())) {
                            throw YuxError(fline, fcol, ErrorCode::E4031, fieldType.name, "struct 字面量字段初始化",
                                           fieldType.name);
                        }
                    }
                }
            }
        }
        if (seen.size() != decl->fields().size()) {
            for (auto& f : decl->fields()) {
                if (!seen.count(f->name().getText())) {
                    throw YuxError(line, col, ErrorCode::E3125, structName, f->name().getText());
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprPathCallNode>>(expr)) {
        const bool selfForm = n->enumName().getText() == "Self";
        TypeInfo lhsTy = n->resolvedLhsType();
        if (selfForm && !_currentStructName.empty()) {
            lhsTy = TypeInfo(_currentStructName);
            if (auto* ownerFile = fnDeclFile(_currentFn, _file)) {
                lhsTy.ownerModule = ownerFile->moduleName();
            }
        }
        string lhsName = lhsTy.name;
        vector<TypeInfo> pathArgExpected;
        const vector<TypeInfo>* pathArgExpPtr = nullptr;
        if (n->lhsTypeArgs().empty() && agreedStaticMethodParams(_file, _sdkFile, lhsTy, n->variantName().getText(),
                                                                 n->args().size(), pathArgExpected)) {
            pathArgExpPtr = &pathArgExpected;
        } else if (!n->lhsTypeArgs().empty()) {
            // Phase C：泛型 struct #Static fn + turbofish，替换后的形参作靶向类型
            auto* sd = _names.lookupStruct(lhsTy);
            if (sd && sd->isGeneric()) {
                map<string, TypeInfo> subst;
                if (fillSubstFromTypeNodes(sd->typeParams(), n->lhsTypeArgs(), subst)) {
                    for (auto& [_, t] : subst)
                        t = applyInstSubst(t);
                    auto* impl = lookupStructImpl(_file, _sdkFile, lhsTy);
                    if (auto* hdr = uniqueMethodHeader(impl, n->variantName().getText(), n->args().size(),
                                                       /*wantStatic=*/true)) {
                        if (substHeaderParams(hdr, subst, pathArgExpected)) {
                            pathArgExpPtr = &pathArgExpected;
                        }
                    }
                }
            }
        }
        visitExprList(n->args(), pathArgExpPtr);

        // §7.10.2.3：`Self::name` 仅 struct body 内合法。
        if (selfForm && lhsName == "Self") {
            throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3123);
        }

        // Phase B：Array:<T>::with_capacity 形态校验（E6011 / E3131）。
        // 泛型 struct 的 #Static fn 路径 skipTypeCheck，必须在此单独接管。
        if (lhsName == "Array" && n->variantName().getText() == "with_capacity") {
            sema::validateArrayWithCapacity(n);
        }

        // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` /
        // `<Struct>::methods` / `<Struct>::variants` reflect 静态访问.
        // 优先于 impl-method / enum-ctor 分流 (struct 无需 impl 也能取反射元数据).
        {
            string rhsName = n->variantName().getText();
            if (n->args().empty() &&
                (rhsName == "type" || rhsName == "fields" || rhsName == "methods" || rhsName == "variants")) {
                auto* sd = _names.lookupStruct(lhsTy);
                if (sd) {
                    // DRAFT-spec-reflect §2：variants 仅 enum；struct 上访问 → E3135。
                    if (rhsName == "variants") {
                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3135, lhsName);
                    }
                    return;
                }
            }
        }

        // Phase 2c 构造模型重构: `Type::name(...)` 按 LHS 分流.
        //   * LHS 是 struct -> 必须是 #Static 方法 (E3120/E3121); codegen Phase 3 落地.
        //   * LHS 是 enum   -> 走原 validateEnumCtorShape 路径 (E2019/E2020/E2021/E2032).
        // struct/enum 重名在 yux 里非法 (E2017), 此处直接按 lhsName 查 struct 优先.
        {

            // DRAFT-static-vars Phase 4: 零参且 LHS 是 struct 且 RHS 是静态字段 → 放行
            // includeBuiltin=true：允许 #Builtin struct（如 i8）上的静态字段访问（如 i8::MAX）
            if (n->args().empty()) {
                auto* structDecl = _names.lookupStruct(lhsTy, /*includeBuiltin=*/true);
                if (structDecl) {
                    if (auto* sf = structDecl->staticField(n->variantName().getText())) {
                        // 设置正确类型（字段类型而非 struct 类型）
                        n->setResolvedType(sf->type->getType());
                        return;
                    }
                }
            }

            auto* structImpl = _names.lookupStructImpl(lhsTy);
            if (structImpl) {
                string rhsName = n->variantName().getText();

                // DRAFT-static-vars Phase 4: 若零参且 RHS 是静态字段名 → 放行
                if (n->args().empty()) {
                    auto* structDecl = _names.lookupStruct(lhsTy, /*includeBuiltin=*/true);
                    if (structDecl && structDecl->staticField(rhsName)) {
                        return; // 静态字段读，放行
                    }
                }

                p<FnHeaderNode> methodHeader = nullptr;
                for (auto& m : structImpl->methods()) {
                    if (m->header()->name().getText() == rhsName) {
                        methodHeader = m->header();
                        break;
                    }
                }
                int line = n->resolveLineNumber();
                int col = n->resolveColumn();
                if (!methodHeader) {
                    throw YuxError(line, col, ErrorCode::E3121, lhsName, rhsName);
                }
                if (!methodHeader->isStatic()) {
                    throw YuxError(line, col, ErrorCode::E3120, lhsName, rhsName, rhsName);
                }
                // Bucket 4 收口 (CURRENT-check.md): #Static fn 调用站点的 arity +
                // 类型校验 (E3131). 镜像 compiler_expr.cpp::compileEnumCtorExpr 的
                // #Static fn 分派 (2343-2377). 泛型 struct 在 turbofish 齐时
                // TypeInfo::substitute 替换形参，不再 skip。
                bool skipTypeCheck = false;
                map<string, TypeInfo> staticSubst;
                auto* structDecl = _names.lookupStruct(lhsTy);
                // 非泛型 struct 写 `Type:<T>::name`：expects 0。与 T 无关，模板期也报。
                // 原先 Compiler 用占位 E0000。
                if (structDecl && !structDecl->isGeneric() && !n->lhsTypeArgs().empty()) {
                    throw YuxError(line, col, ErrorCode::E6011, lhsName, static_cast<size_t>(0),
                                   n->lhsTypeArgs().size())
                        .withHint(std::format("非泛型 struct `{}` 不能写 `:<...>` turbofish，去掉类型实参", lhsName));
                }
                if (structDecl && structDecl->isGeneric()) {
                    const auto& lhsTArgs = n->lhsTypeArgs();
                    size_t want = structDecl->typeParams().size();
                    // 写出了 turbofish 但个数不对：模板期也报（E6011 是形态码）。
                    // 无 turbofish 仍 skip（推断 / 实例化后再查）。
                    if (!lhsTArgs.empty() && lhsTArgs.size() != want) {
                        throw YuxError(line, col, ErrorCode::E6011, lhsName, want, lhsTArgs.size())
                            .withHint(
                                std::format("实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型",
                                            lhsName, std::string(want == 1 ? "T" : "T1, T2, ..."), want));
                    }
                    if (!fillSubstFromTypeNodes(structDecl->typeParams(), lhsTArgs, staticSubst)) {
                        // `Self::name` 无 turbofish：实例化复查绑当前单态（§7.10.2.3 / §7.10.3.1）。
                        bool boundSelf = false;
                        if (selfForm && lhsTArgs.empty() && !_instSubst.empty()) {
                            boundSelf = true;
                            staticSubst.clear();
                            for (auto& tp : structDecl->typeParams()) {
                                auto it = _instSubst.find(tp);
                                if (it == _instSubst.end()) {
                                    boundSelf = false;
                                    break;
                                }
                                staticSubst[tp] = it->second;
                            }
                        }
                        if (!boundSelf) {
                            skipTypeCheck = true;
                        } else {
                            for (auto& [_, t] : staticSubst)
                                t = applyInstSubst(t);
                        }
                    } else {
                        for (auto& [_, t] : staticSubst)
                            t = applyInstSubst(t);
                    }
                }
                if (!skipTypeCheck) {
                    vector<TypeInfo> paramTypes;
                    bool paramTypesOk = true;
                    for (auto p : methodHeader->params()) {
                        if (p->type()) {
                            try {
                                TypeInfo pt = p->type()->getType();
                                if (!staticSubst.empty()) pt = pt.substitute(staticSubst);
                                paramTypes.push_back(std::move(pt));
                            } catch (...) {
                                paramTypesOk = false;
                                break;
                            }
                        } else {
                            paramTypesOk = false;
                            break;
                        }
                    }
                    if (paramTypesOk) {
                        // 灵活整数实参按形参类型回填 (与 Compiler 端 2340 一致)
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            tryInferIntType(n->args()[i], paramTypes[i]);
                        }
                        auto renderTypes = [](const vector<TypeInfo>& ts) {
                            string s;
                            for (size_t i = 0; i < ts.size(); ++i) {
                                if (i) s += ", ";
                                s += ts[i].getFullName();
                            }
                            return s;
                        };
                        // 与形参同一张 subst：turbofish 用 staticSubst，`Self::name` 绑当前单态。
                        auto substArgType = [&](const TypeInfo& t) {
                            return !staticSubst.empty() ? t.substitute(staticSubst) : applyInstSubst(t);
                        };
                        const map<string, TypeInfo>* cmpSubst = !staticSubst.empty() ? &staticSubst : &_instSubst;
                        // arity 校验
                        if (n->args().size() != paramTypes.size()) {
                            string expected = renderTypes(paramTypes);
                            vector<TypeInfo> argTypesRaw;
                            bool ok = true;
                            for (auto& a : n->args()) {
                                try {
                                    argTypesRaw.push_back(substArgType(a->getType()));
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            string got = ok ? renderTypes(argTypesRaw) : string("<unresolved>");
                            throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(), expected,
                                           n->args().size(), got);
                        }
                        // 类型逐位比对
                        vector<TypeInfo> argTypes;
                        bool argOk = true;
                        for (auto& a : n->args()) {
                            try {
                                argTypes.push_back(substArgType(a->getType()));
                            } catch (...) {
                                argOk = false;
                                break;
                            }
                        }
                        if (argOk) {
                            for (size_t i = 0; i < argTypes.size(); ++i) {
                                if (argTypes[i].empty()) continue;
                                // 实例化后仍是模板形参（`Self::make(v)` 的 v:T）跳过，与 checkCallArgAgainst 一致。
                                if (stillTemplateType(argTypes[i], _currentTypeParams, cmpSubst)) continue;
                                if (!(argTypes[i] == paramTypes[i])) {
                                    // Nullable<T> 形参接受 T 值实参（自动包装）
                                    bool nullableMatch = false;
                                    if (paramTypes[i].isNullable()) {
                                        auto inner = paramTypes[i].nullableInnerType();
                                        if (inner && *inner == argTypes[i]) nullableMatch = true;
                                    }
                                    if (!nullableMatch) {
                                        throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(),
                                                       renderTypes(paramTypes), argTypes.size(), renderTypes(argTypes));
                                    }
                                }
                            }
                        }
                        // Phase B-1: #NoCopy 类型不可按值传参（#Static fn 调用）
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            const auto& pt = paramTypes[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->args()[i])) {
                                    throw YuxError(line, col, ErrorCode::E4031, pt.name, "按值传参", pt.name);
                                }
                            }
                        }
                    }
                }
                if (!staticSubst.empty()) {
                    checkGenericImplInst(structImpl, staticSubst);
                }
                // 路径调用无 `!` 后缀（g4 exprEnumCtor）；T ! E 的 #Static 只能在 try 内裸调。
                {
                    string calleeErr = methodHeader->resolvedFallibleErr();
                    if (!calleeErr.empty()) {
                        if (!_tryStack.empty()) {
                            _tryStack.back().push_back(calleeErr);
                        } else {
                            throw YuxError(line, col, ErrorCode::E7006, lhsName + "::" + rhsName);
                        }
                    }
                }
                return;
            }
        }

        // Phase 3.4.a: SemaPass 接管 E2019/E2020/E2021/E2032.
        // node->setResolvedType 已在 visitExpr 顶部写好 (getType 抛错时已在白名单
        // 重抛, 否则吞掉; 这里能跑到说明 getType 至少没抛已迁移码).
        // 任一异常被 helper 内部 try/catch (E2032 路径) 吞掉; E2019/E2020/E2021
        // 由 helper 主动抛出, SemaPass 实际接管.
        try {
            sema::validateEnumCtorShape(_file, _sdkFile, n);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御: helper 内部异常 (理论不应出现) 跳过, 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms()) {
            if (arm->hasBlock())
                visitBlock(arm->block(), expected);
            else
                visitExpr(arm->body(), expected);
        }
        finishCtrlResolved(n);
        checkMatchArmTypes(n->arms(), _currentTypeParams, &_instSubst);
        tryValidateMatchScrut(n);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTryCatchNode>>(expr)) {
        // Phase 3.3 前置.5: SemaPass 接管 E7011 (catch 类型必须是已声明 enum)
        // 与 E7002 (try block 内 callee 错误类型未被任一 catch 覆盖).
        //
        // 顺序:
        //   1) 逐 arm 校验 errType 为已声明 enum (E7011), 同时收集 catchTypes;
        //   2) push 新的 seenErrTypes 层, visitBlock(tryBlock) —— 内部 ID-callee
        //      检查把 #Fallible callee 的错误类型 append 进栈顶;
        //   3) pop 取出 seenErrTypes, 与 catchTypes 比对穷尽性 (E7002);
        //   4) 再访问每个 catch arm body (catches 在外层 try 视野之外).
        vector<string> catchTypes;
        catchTypes.reserve(n->catches().size());
        int line = n->getLineNumber();
        int col = n->getColumn();
        for (auto& arm : n->catches()) {
            const auto& errTi = arm->errTypeInfo();
            auto* enumDecl = _names.lookupEnum(errTi);
            if (!enumDecl) {
                int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                throw YuxError(aline, acol, ErrorCode::E7011, arm->errName().getText(), arm->errType(), arm->errType());
            }
            catchTypes.push_back(arm->errType());
        }

        _tryStack.emplace_back();
        visitBlock(n->tryBlock(), expected);
        vector<string> seenErrTypes = std::move(_tryStack.back());
        _tryStack.pop_back();

        for (auto& seen : seenErrTypes) {
            bool covered = false;
            for (auto& ct : catchTypes) {
                if (ct == seen) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                throw YuxError(line, col, ErrorCode::E7002, seen, string("<unknown>"), seen);
            }
        }

        for (auto& c : n->catches())
            visitBlock(c->body(), expected);

        finishCtrlResolved(n);

        // E7010：catch arm 末类型与 try 块一致。须用 resolved（空 `[]` 的 getType）
        // 是 `[__empty * 0]`，靶向后 resolved 才是 Array<T>）。
        if (n->tryBlock()->hasResult() && n->tryBlock()->resultExpr()) {
            TypeInfo resultType;
            if (tryGetExprType(n->tryBlock()->resultExpr(), resultType)) {
                resultType = applyInstSubst(resultType);
                for (auto& arm : n->catches()) {
                    if (!arm->body()->hasResult() || !arm->body()->resultExpr()) continue;
                    TypeInfo armT;
                    if (!tryGetExprType(arm->body()->resultExpr(), armT)) continue;
                    armT = applyInstSubst(armT);
                    if (blockMergeTypesEq(armT, resultType)) continue;
                    int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                    int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                    throw YuxError(aline, acol, ErrorCode::E7010, armT.name, resultType.name);
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg());
        // Bucket 4 收口 (CURRENT-check.md): Dyn<D>(x) 构造的 E1131/E1132/E1134/E1133
        // 接管. 镜像 compiler_expr.cpp::compileDynCtorExpr 顶部 (line 2497-2576).
        // 仅在 _yux 就绪时校验 (spec 注册表 + impl 检查器都从 Yux 取); SDK 自构建
        // 等无 Yux 场景 skip, 留 Compiler 兜底.
        if (!_yux) return;
        try {
            auto resultType = n->getType();
            int line = n->getLineNumber();
            int col = n->getColumn();
            auto specInner = resultType.dynSpecType();
            string specBareName = specInner ? specInner->name : string();

            auto& reg = _yux->specRegistry();
            SpecDeclNode* specDecl = nullptr;
            string specQualified;
            if (!specBareName.empty()) {
                if (auto resolved = reg.resolve(specBareName, _file)) {
                    specDecl = resolved->decl;
                    specQualified = resolved->qualifiedName;
                }
            }
            if (!specDecl) {
                throw YuxError(line, col, ErrorCode::E1131, specBareName.empty() ? string("?") : specBareName);
            }
            if (specInner && specInner->isDyn()) {
                throw YuxError(line, col, ErrorCode::E1132, resultType.getFullName());
            }
            auto& checker = _yux->specImplChecker();
            if (!checker.specIsObjectSafe(specDecl)) {
                throw YuxError(line, col, ErrorCode::E1134, specQualified, specQualified, specQualified);
            }

            auto argType = n->arg()->getType();
            bool isBorrow = n->isBorrow();
            string concreteBare;
            if (isBorrow) {
                if (argType.isRef()) {
                    if (auto inner = argType.refElementType()) concreteBare = inner->name;
                } else if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            } else {
                if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            }
            if (concreteBare.empty()) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
            TypeInfo concreteTI(concreteBare);
            vector<TypeInfo> specTypeArgs;
            if (!checker.boundSatisfied(concreteTI, specDecl, specQualified, specTypeArgs)) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 等内部异常 (lambda 形参未推断等): 留 Compiler 兜底
        }
        return;
    }
    // heap:<T>(v) / rc:<T>(v) 由 #Builtin generic 路径在 call_fn.cpp 处理，
    // 类型校验由 sema::validateBuiltinIntrinsicShape/TypeShape 覆盖，不在此处重复。
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left());
        // 右侧按左侧 Nullable 内层靶向：`a ?? []` 的 `[]` 须是 Array<T>，否则 E3063。
        // 与 compileMoveAssignExpr / if 块值同一套 visitExpr(..., expected)。
        TypeInfo inner;
        const TypeInfo* rightExp = nullptr;
        TypeInfo leftType;
        if (tryGetExprType(n->left(), leftType)) {
            leftType = peelRefIfNullable(applyInstSubst(leftType));
            if (leftType.isNullable()) {
                if (auto innerType = leftType.nullableInnerType()) {
                    inner = applyInstSubst(*innerType);
                    if (!isCurrentTypeParam(inner)) rightExp = &inner;
                }
            }
        }
        visitExpr(n->right(), rightExp);
        // Bucket 6 收口+ (CURRENT-check.md): E3024 (左侧非 Nullable) + E3014 (右侧
        // 类型不匹配). 镜像 compileNullElseExpr. 模板形参等实例化后再查.
        try {
            if (!tryGetExprType(n->left(), leftType)) return;
            leftType = peelRefIfNullable(applyInstSubst(leftType));
            if (isCurrentTypeParam(leftType)) return;
            if (!leftType.isNullable()) {
                if (!leftType.name.empty()) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3024, leftType.name);
                }
                return;
            }
            auto innerType = leftType.nullableInnerType();
            if (!innerType) return;
            inner = applyInstSubst(*innerType);
            if (isCurrentTypeParam(inner)) return;
            if (isIntTypeName(inner.name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), inner);
            }
            tryInferNullType(n->right(), inner);
            // 空 `[]` 的 getType 是 `[__empty * 0]`，比对必须读 resolved。
            TypeInfo rightType;
            if (!tryGetExprType(n->right(), rightType)) return;
            rightType = applyInstSubst(rightType);
            if (isCurrentTypeParam(rightType)) return;
            if (!(rightType == inner)) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3014, inner.name,
                               rightType.name);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 抛 std::runtime_error 等: 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMoveAssignNode>>(expr)) {
        visitExpr(n->left());
        // 与 compileLvalueAddr 同款：LHS 须是变量 / `$` / 字段 / 元组 `.N`。
        // 与 T 无关的形态模板期也报（字面量 / 调用 / 索引）。
        if (!isMoveAssignLvalue(n->left())) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4036);
        }
        // 与 compileMoveAssignExpr 对齐：空 `[]` 用左侧类型当下靶（否则 E3063）。
        TypeInfo leftTy;
        const TypeInfo* rightExp = nullptr;
        if (tryGetExprType(n->left(), leftTy)) {
            leftTy = applyInstSubst(leftTy);
            rightExp = &leftTy;
        }
        visitExpr(n->right(), rightExp);
        // 类型兼容：right 须能赋给 left（相同或灵活整数字面量）。
        // LHS 形态已在上方按 compileLvalueAddr 查过（E4036）。
        try {
            auto leftType = n->left()->getType();
            if (isIntTypeName(leftType.name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), leftType);
            }
            auto rightType = n->right()->getType();
            if (leftType != rightType) {
                // 非内置类型允许跨类型形参 (§7.2.3.3)
                if (isBuiltinType(leftType.name)) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3001, "arithmetic",
                                   leftType.name, rightType.name);
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 异常, 留 codegen 兜底
        }
        return;
    }
    // Phase 3.4.d.1: ExprGetRefNode —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprGetRefNode::getType 抛
    // E3040/E3041 (SemaPass 默认重抛), 由此 Compiler 端
    // compileGetRefExpr 的 1656/1661 内联 throw 在正常 codepath 下不可达。
    // Phase 3.4.d.2: 补 E3042 链式私有字段可见性校验.
    if (auto n = dynamic_cast<p<ExprGetRefNode>>(expr)) {
        // Phase 2e: `&$.x` 在 `#Static fn` 体内禁用 (E3128).
        if (n->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
        }
        // #Inline #Cval 检查：内联常量无存储地址，不可取址。
        // 先查本文件，再查 SDK 文件的全局常量列表。
        auto checkInlineConst = [&](p<FileNode> f) {
            if (!f) return;
            for (const auto& gc : f->getGlobalConsts()) {
                if (gc->name().getText() == n->obj().getText() && gc->isInline()) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3118, n->obj().getText());
                }
            }
        };
        checkInlineConst(_file);
        checkInlineConst(_sdkFile);
        try {
            sema::validateGetRefPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }
        // Phase C：实例化后字段链 E3040 / E3041（getType 在模板体吞掉）。
        if (!n->subs().empty() && _currentFn) {
            string objName = n->obj().getText();
            SymbolInfo* sym = nullptr;
            if (auto sc = n->findNearestScope()) {
                sym = sc->lookupSymbol(objName);
            }
            if (!sym) {
                sym = _currentFn->lookupSymbol(objName);
            }
            if (sym) {
                vector<string> members;
                members.reserve(n->subs().size());
                for (auto& t : n->subs())
                    members.push_back(t.getText());
                tryValidateFieldChain(sym->type, members, n->resolveLineNumber(), n->resolveColumn());
                tryValidateReflectFieldValueWrite(objName, sym->type, members, n->resolveLineNumber(),
                                                  n->resolveColumn());
            }
        }
        return;
    }
    // Phase C：ExprArrayInitNode 无靶向类型时仍校验 explicitType vs fill（E3009）。
    if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
        checkArrayInit(n, nullptr);
        return;
    }
    // 其余未识别节点 3.2 起补 assert。
}
