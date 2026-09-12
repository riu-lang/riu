// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "expr_node.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

#include "analyzer/symbol_suggest.h"
#include "file_node.h"
#include "fn_node.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"
#include "spec_node.h"
#include "statement_node.h"
#include "struct_node.h"
#include "type_node.h"
#include "types.h"

// 安全方法调用结果：方法返回 U → U?；省略返回（unit）用 ()。
static TypeInfo wrapAsNullable(TypeInfo inner) {
    if (inner.empty()) inner = TypeInfo(TupleTag{}, {});
    return {"Nullable", {std::make_shared<TypeInfo>(std::move(inner))}};
}

// 方法点的静态类型：真实 TypeKind::Fn，仅编码返回类型（形参空）。
// ExprCallNode::getType 走 isFn() 取 fnReturnType()；这不是 fat-ptr 一等函数值。
// fallibleErr 非空时挂到返回类型上，供 checkErrPropagateForFnValueCall 认 T ! E。
static TypeInfo makeCallFnType(TypeInfo ret, const string& fallibleErr = "") {
    if (!fallibleErr.empty()) {
        // 方法符号把成功类型与错误类型分开保存。这里只需把错误类型挂回返回值；
        // attachFallibleErr 会把 Array<T> 的完整显示名写进 name，随后
        // withoutFallible 再叠加 genericArgs，错误得到 Array<T><T>。
        ret.fallibleErr = fallibleErr;
    }
    sp<TypeInfo> rt = nullptr;
    if (ret.isFallible() || (!ret.empty() && ret.name != "()")) {
        rt = make_shared<TypeInfo>(std::move(ret));
    }
    return TypeInfo(FnTag{}, {}, std::move(rt));
}

static bool isEmptyArrayLitType(const TypeInfo& t) {
    return t.isArray() && t.elementType && t.elementType->name == "__empty";
}

// 块值汇合：空 `[]` 可与 Array<T> / [T*N] 同型（靶向后 resolved 已是 Array）。
static bool blockValueTypesMatch(const TypeInfo& a, const TypeInfo& b) {
    if (a == b) return true;
    if (isEmptyArrayLitType(a) && (b.isArrayGeneric() || b.isArray())) return true;
    if (isEmptyArrayLitType(b) && (a.isArrayGeneric() || a.isArray())) return true;
    return false;
}

static FileNode* enclosingFileFrom(const Node* n) {
    const Node* cur = n;
    while (cur) {
        if (auto f = dynamic_cast<const FileNode*>(cur)) return const_cast<FileNode*>(f);
        cur = cur->parent();
    }
    auto scope = n ? n->findNearestScope() : nullptr;
    while (scope) {
        if (auto f = dynamic_cast<FileNode*>(scope)) return f;
        scope = scope->parentScope();
    }
    return nullptr;
}

static sema::NameResolver namesFromFile(FileNode* file) {
    FileNode* sdk = nullptr;
    if (file) {
        for (auto* s = file->parentScope(); s; s = s->parentScope()) {
            if (auto* pf = dynamic_cast<FileNode*>(s)) {
                sdk = pf;
                break;
            }
        }
    }
    return {file, sdk};
}

static void unwrapRecvType(TypeInfo& t) {
    if (t.isRef()) {
        if (auto e = t.refElementType()) t = *e;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) t = *e;
    }
    if (t.isRc()) {
        if (auto e = t.rcElementType()) t = *e;
    }
}

// 泛型函数调用：把 ret 按 typeParams → 显式实参 / 从实参 unify 推断 替换。
static TypeInfo substGenericFnRet(const Node* from, const vector<TypeNode*>& typeArgs, const vector<ExprNode*>& args,
                                  const string& fnName, TypeInfo retType) {
    FileNode* file = enclosingFileFrom(from);
    FnNode* fnNode = nullptr;
    if (file) {
        fnNode = file->getFunction(fnName);
        if (!fnNode) {
            ScopeNode* p = file->parentScope();
            while (p && !fnNode) {
                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                    fnNode = pf->getFunction(fnName);
                    if (!fnNode) {
                        for (auto* imp : pf->wildcardImports()) {
                            fnNode = imp->getFunction(fnName);
                            if (fnNode) break;
                        }
                    }
                }
                p = p->parentScope();
            }
        }
    }
    if (!fnNode || !fnNode->header() || !fnNode->header()->isGeneric()) return retType;

    auto typeParams = fnNode->header()->typeParams();
    std::map<std::string, TypeInfo> subst;
    if (!typeArgs.empty() && typeParams.size() == typeArgs.size()) {
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            subst[typeParams[i]] = typeArgs[i]->getType();
        }
    } else if (typeArgs.empty()) {
        std::function<void(const TypeInfo&, const TypeInfo&)> unify = [&](const TypeInfo& pT, const TypeInfo& aT) {
            if (pT.isNormal()) {
                for (auto& tp : typeParams) {
                    if (pT.name == tp) {
                        subst[tp] = aT;
                        return;
                    }
                }
            }
            if (pT.hasGenericArgs() && aT.hasGenericArgs() && pT.kind == aT.kind && pT.name == aT.name &&
                pT.genericArgs.size() == aT.genericArgs.size()) {
                for (size_t i = 0; i < pT.genericArgs.size(); ++i) {
                    if (pT.genericArgs[i] && aT.genericArgs[i]) {
                        unify(*pT.genericArgs[i], *aT.genericArgs[i]);
                    }
                }
            }
            if (pT.isRef()) {
                auto elem = pT.refElementType();
                if (elem) {
                    if (aT.isRef()) {
                        auto aElem = aT.refElementType();
                        if (aElem) unify(*elem, *aElem);
                    } else {
                        unify(*elem, aT);
                    }
                }
            }
            if (pT.isNullable()) {
                auto inner = pT.nullableInnerType();
                if (inner) {
                    if (aT.isNullable()) {
                        auto aInner = aT.nullableInnerType();
                        if (aInner) unify(*inner, *aInner);
                    } else {
                        unify(*inner, aT);
                    }
                }
            }
        };
        auto params = fnNode->header()->params();
        for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
            if (params[i]->type()) {
                unify(params[i]->type()->getType(), args[i]->getType());
            }
        }
    }
    if (!subst.empty()) retType = retType.substitute(subst);
    return retType;
}

// 已解析到目标模块的泛型函数时，直接按该声明替换返回类型。
// 模块点调用不能复用上面的名字查找：调用方 scope 中只有模块别名，没有目标函数符号。
static TypeInfo substResolvedGenericFnRet(const FnNode* fnNode, const vector<TypeNode*>& typeArgs,
                                          const vector<ExprNode*>& args, TypeInfo retType) {
    if (!fnNode || !fnNode->header() || !fnNode->header()->isGeneric()) return retType;

    const auto& typeParams = fnNode->header()->typeParams();
    std::map<std::string, TypeInfo> subst;
    if (!typeArgs.empty() && typeParams.size() == typeArgs.size()) {
        for (size_t i = 0; i < typeArgs.size(); ++i) {
            subst[typeParams[i]] = typeArgs[i]->getType();
        }
    } else if (typeArgs.empty()) {
        std::function<void(const TypeInfo&, const TypeInfo&)> unify = [&](const TypeInfo& param, const TypeInfo& arg) {
            if (param.isNormal() && std::ranges::find(typeParams, param.name) != typeParams.end()) {
                subst[param.name] = arg;
                return;
            }
            if (param.hasGenericArgs() && arg.hasGenericArgs() && param.kind == arg.kind && param.name == arg.name &&
                param.genericArgs.size() == arg.genericArgs.size()) {
                for (size_t i = 0; i < param.genericArgs.size(); ++i) {
                    if (param.genericArgs[i] && arg.genericArgs[i]) unify(*param.genericArgs[i], *arg.genericArgs[i]);
                }
            }
            if (param.isRef()) {
                if (auto elem = param.refElementType()) {
                    if (arg.isRef()) {
                        if (auto argElem = arg.refElementType()) unify(*elem, *argElem);
                    } else {
                        unify(*elem, arg);
                    }
                }
            }
            if (param.isNullable()) {
                if (auto inner = param.nullableInnerType()) {
                    if (arg.isNullable()) {
                        if (auto argInner = arg.nullableInnerType()) unify(*inner, *argInner);
                    } else {
                        unify(*inner, arg);
                    }
                }
            }
        };
        auto params = fnNode->header()->params();
        for (size_t i = 0; i < params.size() && i < args.size(); ++i) {
            if (params[i]->type()) unify(params[i]->type()->getType(), args[i]->getType());
        }
    }
    return subst.empty() ? retType : retType.substitute(subst);
}

// §12.4：在生成式 AST 中遇到 `x.m()`（x:T 为泛型形参）时，
// 用形参声明位的 draft 边界查 m 的返回类型；走包含 SDK 回退的 file 链。
static TypeInfo lookupSpecBoundMethodRetType(Node* contextParent, const string& typeParamName,
                                             const string& methodName) {
    Node* cur = contextParent;
    FnHeaderNode* header = nullptr;
    while (cur) {
        if (auto fn = dynamic_cast<FnNode*>(cur)) {
            header = fn->header();
            break;
        }
        cur = cur->parent();
    }
    if (!header || !header->isGeneric()) return {};
    const auto& tps = header->typeParams();
    const auto& bounds = header->typeParamBounds();
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < tps.size(); ++i) {
        if (tps[i] == typeParamName) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX || idx >= bounds.size()) return {};

    auto scope = contextParent ? contextParent->findNearestScope() : nullptr;
    auto* file = dynamic_cast<FileNode*>(scope);
    while (!file && scope) {
        scope = scope->parentScope();
        file = dynamic_cast<FileNode*>(scope);
    }
    if (!file) return {};

    for (auto& dname : bounds[idx]) {
        SpecDeclNode* draft = file->getSpecDecl(dname);
        if (!draft) {
            ScopeNode* p = file->parentScope();
            while (p && !draft) {
                if (auto pf = dynamic_cast<FileNode*>(p)) {
                    draft = pf->getSpecDecl(dname);
                }
                p = p->parentScope();
            }
        }
        if (!draft) continue;
        for (auto& sig : draft->signatures()) {
            if (sig->name().getText() != methodName) continue;
            if (sig->retType()) {
                auto ret = sig->retType()->getType();
                return ret.substitute({{"Self", TypeInfo(typeParamName)}});
            }
            return {};
        }
    }
    return {};
}

// Phase 4a：Dyn<D>/Dyn<D&> 上的 `.m` 静态类型查找。
// 把 dot 表达式的类型表示为 TypeKind::Fn（仅编码返回类型），让外层 ExprCallNode
// 在静态阶段算出确切返回类型，避免下游拿到 Dyn 类型而失败。
static TypeInfo lookupDynMethodRetType(Node* contextParent, const TypeInfo& dynType, const string& methodName) {
    if (!dynType.isDyn()) return {};
    auto specTy = dynType.dynSpecType();
    if (!specTy) return {};
    const string& specName = specTy->name;

    // 走 parent() 链而不是 parentScope()：struct 方法的 FnNode 在 AST 构造时
    // 不一定挂上 parentScope，但 parent() 链一定连到 FileNode。
    Node* cur = contextParent;
    FileNode* file = nullptr;
    while (cur) {
        if (auto f = dynamic_cast<FileNode*>(cur)) {
            file = f;
            break;
        }
        cur = cur->parent();
    }
    if (!file) return {};

    SpecDeclNode* draft = file->getSpecDecl(specName);
    if (!draft) {
        ScopeNode* p = file->parentScope();
        while (p && !draft) {
            if (auto pf = dynamic_cast<FileNode*>(p)) {
                draft = pf->getSpecDecl(specName);
            }
            p = p->parentScope();
        }
    }
    if (!draft) return {};
    for (auto& sig : draft->signatures()) {
        if (sig->name().getText() != methodName) continue;
        if (sig->retType()) return sig->retType()->getType();
        return {};
    }
    return {};
}

static bool isBuiltinMethod(ScopeNode* scope, const string& structName, const string& methodName) {
    if (!scope) return false;

    auto* file = dynamic_cast<FileNode*>(scope);
    auto s = scope;
    while (!file && s) {
        s = s->parentScope();
        file = dynamic_cast<FileNode*>(s);
    }

    if (!file) return false;

    auto structImpl = file->getStructImpl(structName);
    if (!structImpl) return false;

    for (auto& method : structImpl->methods()) {
        if (method->header()->name().getText() == methodName) {
            return method->header()->hasAnno("Builtin");
        }
    }

    return false;
}

static ExprNode* unwrapParen(ExprNode* e) {
    while (auto paren = dynamic_cast<ExprParenNode*>(e)) {
        e = paren->expr();
    }
    return e;
}

bool isFlexibleIntExpr(ExprNode* expr) {
    expr = unwrapParen(expr);
    if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto ilit = dynamic_cast<LiteralIntNode*>(lit->literal())) {
            return !ilit->hasSuffix();
        }
        return false;
    }
    if (auto u = dynamic_cast<ExprUnaryNode*>(expr)) {
        return u->op() != ExprUnaryNode::Op::Not && isFlexibleIntExpr(u->right());
    }
    if (auto a = dynamic_cast<ExprAddSubNode*>(expr)) {
        return isFlexibleIntExpr(a->left()) && isFlexibleIntExpr(a->right());
    }
    if (auto m = dynamic_cast<ExprMulDivModNode*>(expr)) {
        return isFlexibleIntExpr(m->left()) && isFlexibleIntExpr(m->right());
    }
    if (auto b = dynamic_cast<ExprBinOpNode*>(expr)) {
        return isFlexibleIntExpr(b->left()) && isFlexibleIntExpr(b->right());
    }
    if (auto call = dynamic_cast<ExprCallNode*>(expr)) {
        auto* dot = dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
        if (!dot) return false;
        const string m = dot->member();
        if (m == "inv") {
            return call->getArgs().empty() && isFlexibleIntExpr(dot->baseExpr());
        }
        if ((m == "and" || m == "or" || m == "xor" || m == "shl" || m == "shr") && call->getArgs().size() == 1) {
            return isFlexibleIntExpr(dot->baseExpr()) && isFlexibleIntExpr(call->getArgs()[0]);
        }
    }
    return false;
}

bool tryInferIntType(ExprNode* expr, const TypeInfo& target) {
    if (!isIntTypeName(target.name)) return false;
    expr = unwrapParen(expr);
    if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto ilit = dynamic_cast<LiteralIntNode*>(lit->literal())) {
            if (!ilit->hasSuffix()) {
                ilit->setType(target);
                return true;
            }
            return ilit->getType() == target;
        }
        return false;
    }
    if (auto u = dynamic_cast<ExprUnaryNode*>(expr)) {
        if (u->op() == ExprUnaryNode::Op::Not) return false;
        return tryInferIntType(u->right(), target);
    }
    if (auto a = dynamic_cast<ExprAddSubNode*>(expr)) {
        return tryInferIntType(a->left(), target) && tryInferIntType(a->right(), target);
    }
    if (auto m = dynamic_cast<ExprMulDivModNode*>(expr)) {
        return tryInferIntType(m->left(), target) && tryInferIntType(m->right(), target);
    }
    if (auto b = dynamic_cast<ExprBinOpNode*>(expr)) {
        return tryInferIntType(b->left(), target) && tryInferIntType(b->right(), target);
    }
    if (auto call = dynamic_cast<ExprCallNode*>(expr)) {
        auto* dot = dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
        if (dot) {
            const string m = dot->member();
            if (m == "inv" && call->getArgs().empty()) {
                return tryInferIntType(dot->baseExpr(), target);
            }
            if ((m == "and" || m == "or" || m == "xor" || m == "shl" || m == "shr") && call->getArgs().size() == 1) {
                return tryInferIntType(dot->baseExpr(), target) && tryInferIntType(call->getArgs()[0], target);
            }
        }
    }
    try {
        return expr->getType() == target;
    } catch (...) {
        return false;
    }
}

// ==================== null 字面量灵活类型推断 ====================

bool isFlexibleNullExpr(ExprNode* expr) {
    expr = unwrapParen(expr);
    if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
        return dynamic_cast<LiteralNullNode*>(lit->literal()) != nullptr;
    }
    return false;
}

bool tryInferNullType(ExprNode* expr, const TypeInfo& nullableTarget) {
    if (!nullableTarget.isNullable()) return false;
    expr = unwrapParen(expr);
    if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto nullLit = dynamic_cast<LiteralNullNode*>(lit->literal())) {
            nullLit->setType(nullableTarget);
            return true;
        }
    }
    return false;
}

ExprNode* ExprCallNode::getCalleeExpr() const {
    return _calleeExpr;
}

const std::vector<ExprNode*>& ExprCallNode::getArgs() const {
    return _args;
}

TypeInfo ExprCallNode::getType() const {
    auto type = _calleeExpr->getType();

    // Array.map<U> 的返回类型依赖方法自己的 U；方法点只编码占位返回，
    // 在调用节点用显式类型实参或 Function<T&, U> 实参补全。
    if (auto dot = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
        TypeInfo recv;
        try {
            recv = dot->baseExpr()->getType().peelAutoDeref();
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        if (dot->member() == "inv" && recv.isFloat()) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3070, recv.name);
        }
        if (recv.isArrayGeneric() || recv.isArray()) {
            if (auto* spec = sema::lookupInstanceBuiltin(recv, dot->member());
                spec && spec->ret == sema::BuiltinRet::TypeArg0Array) {
                vector<TypeInfo> methodTypeArgs;
                methodTypeArgs.reserve(_typeArgs.size());
                for (auto& tn : _typeArgs)
                    methodTypeArgs.push_back(tn->getType());
                vector<TypeInfo> argTypes;
                argTypes.reserve(_args.size());
                for (auto& arg : _args)
                    argTypes.push_back(arg->getType());
                auto ret = sema::builtinMethodCallReturnType(*spec, recv, methodTypeArgs, argTypes);
                if (!ret.empty()) return ret;
            }
        }
    }

    // callee 为 TypeKind::Fn：lambda / fn 变量 / 字段 / 方法点（方法只编码返回类型）。
    // 调用结果即 fn 返回类型；unit 时返回空 TypeInfo。
    if (type.isFn()) {
        TypeInfo retType;
        if (auto rt = type.fnReturnType()) retType = rt->withoutFallible();

        if (auto literalNode = dynamic_cast<ExprLiteralNode*>(_calleeExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                auto fnName = objLiteral->getValue().getText();
                auto scope = findNearestScope();
                if (scope) {
                    vector<TypeInfo> argTypes;
                    argTypes.reserve(_args.size());
                    for (auto& arg : _args)
                        argTypes.push_back(arg->getType());
                    auto fn = scope->lookupFnSymbolWithParams(fnName, argTypes);
                    if (!fn) fn = scope->lookupFnSymbol(fnName);
                    if (fn) retType = fn->retType;
                }
                retType = substGenericFnRet(this, _typeArgs, _args, fnName, std::move(retType));
            }
        }

        if (auto fallbackDot = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
            if (fallbackDot->isSafe() && fallbackDot->baseExpr()->getType().isNullable()) {
                return wrapAsNullable(retType);
            }
        }
        return retType;
    }

    // Phase 3c: callee 为 Rc<fn(...)R>，自动解引取 fat-ptr 调用，结果同 fn 返回类型
    if (type.isRc()) {
        if (auto inner = type.rcElementType(); inner && inner->isFn()) {
            if (auto rt = inner->fnReturnType()) return rt->withoutFallible();
            return {};
        }
    }

    // v0.16: [] 返回 T&——callee 为 Ref<fn(...)R> 时自动解引用，取 fn 返回类型
    if (type.isRef()) {
        if (auto inner = type.refElementType(); inner && inner->isFn()) {
            if (auto rt = inner->fnReturnType()) return rt->withoutFallible();
            return {};
        }
    }

    DEBUG_LOG_VAL("ExprCallNode::getType - type.name", type.name);
    DEBUG_LOG_VAL("ExprCallNode::getType - isFn", type.isFn());
    DEBUG_LOG_VAL("ExprCallNode::getType - calleeExpr type", typeid(*_calleeExpr).name());

    // 重载 / 跨模块别名调用在尚未看到实参时无法选出唯一 Fn 签名，故留下伪类型。
    // ExprCall 用实参 lookupFnSymbolWithParams 再取 retType。不能改成查符号表：
    // 同名可有多个重载，无参时无法唯一确定 TypeKind::Fn。
    if (type.name == "fn_overload") {
        if (auto literalNode = dynamic_cast<ExprLiteralNode*>(_calleeExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                auto fnName = objLiteral->getValue().getText();
                auto scope = findNearestScope();
                if (scope) {
                    vector<TypeInfo> argTypes;
                    argTypes.reserve(_args.size());
                    for (auto& arg : _args) {
                        argTypes.push_back(arg->getType());
                    }
                    auto fn = scope->lookupFnSymbolWithParams(fnName, argTypes);
                    if (fn) {
                        return fn->retType;
                    }
                }
            }
        }
        // 包链式调用 `pkg.s1. .. .sN(args)`：s1..s(N-1) 为子路径，sN 为函数名。
        if (auto dotNode = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
            string aliasName;
            vector<string> segs;
            if (ExprDotNode::parseChain(dotNode, aliasName, segs) && segs.size() >= 2) {
                auto scope = findNearestScope();
                auto* file = dynamic_cast<FileNode*>(scope);
                while (!file && scope) {
                    scope = scope->parentScope();
                    file = dynamic_cast<FileNode*>(scope);
                }
                if (file) {
                    auto sym = file->lookupSymbol(aliasName);
                    if (sym && sym->kind == SymbolKind::Package) {
                        string childKey;
                        for (size_t i = 0; i + 1 < segs.size(); ++i) {
                            if (i) childKey += '.';
                            childKey += segs[i];
                        }
                        if (auto* target = file->packageChild(aliasName, childKey)) {
                            vector<TypeInfo> argTypes;
                            argTypes.reserve(_args.size());
                            for (auto& arg : _args)
                                argTypes.push_back(arg->getType());
                            auto* fn = target->lookupFnSymbolWithParams(segs.back(), argTypes);
                            if (fn) return fn->retType;
                            auto [genericFn, _] = target->getGenericFunction(segs.back());
                            if (genericFn && genericFn->header()->retType()) {
                                return substResolvedGenericFnRet(genericFn, _typeArgs, _args,
                                                                 genericFn->header()->retType()->getType());
                            }
                        }
                    }
                }
            }
        }
        // 模块别名调用 `alias.fn(args)`：在目标模块内解析 fn 的重载。
        if (auto dotNode = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
            if (auto baseLit = dynamic_cast<ExprLiteralNode*>(dotNode->baseExpr())) {
                if (auto objLit = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
                    auto aliasName = objLit->getValue().getText();
                    auto scope = findNearestScope();
                    auto* file = dynamic_cast<FileNode*>(scope);
                    while (!file && scope) {
                        scope = scope->parentScope();
                        file = dynamic_cast<FileNode*>(scope);
                    }
                    if (file) {
                        if (auto* target = file->moduleAlias(aliasName)) {
                            vector<TypeInfo> argTypes;
                            argTypes.reserve(_args.size());
                            for (auto& arg : _args) {
                                argTypes.push_back(arg->getType());
                            }
                            auto* fn = target->lookupFnSymbolWithParams(dotNode->member(), argTypes);
                            if (fn) return fn->retType;
                            auto [genericFn, _] = target->getGenericFunction(dotNode->member());
                            if (genericFn && genericFn->header()->retType()) {
                                return substResolvedGenericFnRet(genericFn, _typeArgs, _args,
                                                                 genericFn->header()->retType()->getType());
                            }
                        }
                    }
                }
            }
        }
        return {};
    }

    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(type.name);
        if (sym && sym->kind == SymbolKind::Struct) {
            string ctorFullName = type.name + "." + type.name;
            auto fn = scope->lookupFnSymbol(ctorFullName);
            if (fn) {
                if (!_typeArgs.empty()) {
                    vector<sp<TypeInfo>> genericArgs;
                    genericArgs.reserve(_typeArgs.size());
                    for (auto& tn : _typeArgs) {
                        genericArgs.push_back(make_shared<TypeInfo>(tn->getType()));
                    }
                    return {type.name, genericArgs};
                }
                return TypeInfo(type.name);
            }
        }
        // §6.4.4 / §12.4：泛型形参 T 出现在 callee 路径（如 `x.m()`，x:T）时，
        // 这里的 type.name 会回落成 "T"。此时不能按非函数符号抛 E3095，
        // 实例化期 (compileMethodCall) 会用 substStack 把 T 替换成具体类型再分发。
        if (sym && sym->kind != SymbolKind::Function && sym->kind != SymbolKind::TypeParam) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3095, sym->name);
        }

        vector<TypeInfo> argTypes;
        argTypes.reserve(_args.size());
        for (auto& arg : _args) {
            argTypes.push_back(arg->getType());
        }

        auto fn = scope->lookupFnSymbolWithParams(type.name, argTypes);
        if (fn) {
            return fn->retType;
        }
    }

    return type;
}

LiteralNode* ExprLiteralNode::literal() const {
    return _literal;
}

TypeInfo ExprLiteralNode::getType() const {
    return _literal->getType();
}

ExprAddSubNode::Op ExprAddSubNode::op() const {
    return _op;
}

ExprNode* ExprAddSubNode::left() const {
    return _left;
}

ExprNode* ExprAddSubNode::right() const {
    return _right;
}

TypeInfo ExprAddSubNode::getType() const {
    auto leftType = _left->getType().peelAutoDeref();
    auto rightType = _right->getType().peelAutoDeref();
    // v0.6 Phase 2c：`+` 任一操作数为 String 时整链结果即 String，
    // codegen 期 lower 为 StringBuilder 累加（详见 spec §4.4.1.4 / §4.3.1.7）。
    // 仅 Add 适用；Sub 仍按原算术规则。
    if (_op == Op::Add && (leftType.isString() || rightType.isString())) {
        return TypeInfo("String");
    }
    if (leftType != rightType) {
        if (isFlexibleIntExpr(_right) && tryInferIntType(_right, leftType)) {
            return leftType;
        }
        if (isFlexibleIntExpr(_left) && tryInferIntType(_left, rightType)) {
            return rightType;
        }
        // spec §7.2.3.3: 非内置类型允许跨类型形参，类型匹配由 codegen 方法解析完成
        if (!isBuiltinType(leftType.name)) {
            return leftType;
        }
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3001, "arithmetic", leftType.name,
                       rightType.name);
    }
    return leftType;
}

int ExprAddSubNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
}

int ExprAddSubNode::resolveColumn() const {
    if (_line > 0) return _col;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return _left->resolveColumn();
    return _right->resolveColumn();
}

ExprMulDivModNode::Op ExprMulDivModNode::op() const {
    return _op;
}

ExprNode* ExprMulDivModNode::left() const {
    return _left;
}

ExprNode* ExprMulDivModNode::right() const {
    return _right;
}

TypeInfo ExprMulDivModNode::getType() const {
    auto leftType = _left->getType().peelAutoDeref();
    auto rightType = _right->getType().peelAutoDeref();
    if (leftType != rightType) {
        if (isFlexibleIntExpr(_right) && tryInferIntType(_right, leftType)) {
            return leftType;
        }
        if (isFlexibleIntExpr(_left) && tryInferIntType(_left, rightType)) {
            return rightType;
        }
        // spec §7.2.3.3: 非内置类型允许跨类型形参，类型匹配由 codegen 方法解析完成
        if (!isBuiltinType(leftType.name)) {
            return leftType;
        }
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3001, "mul/div/mod", leftType.name,
                       rightType.name);
    }
    return leftType;
}

int ExprMulDivModNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
}

int ExprMulDivModNode::resolveColumn() const {
    if (_line > 0) return _col;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return _left->resolveColumn();
    return _right->resolveColumn();
}

ExprBinOpNode::Op ExprBinOpNode::op() const {
    return _op;
}

ExprNode* ExprBinOpNode::left() const {
    return _left;
}

ExprNode* ExprBinOpNode::right() const {
    return _right;
}

TypeInfo ExprBinOpNode::getType() const {
    auto leftType = _left->getType().peelAutoDeref();
    auto rightType = _right->getType().peelAutoDeref();
    if (leftType != rightType) {
        if (isFlexibleIntExpr(_right) && tryInferIntType(_right, leftType)) {
            return leftType;
        }
        if (isFlexibleIntExpr(_left) && tryInferIntType(_left, rightType)) {
            return rightType;
        }
        // spec §7.2.3.3: 非内置类型允许跨类型形参，类型匹配由 codegen 方法解析完成
        if (!isBuiltinType(leftType.name)) {
            return leftType;
        }
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3001, "bitwise", leftType.name,
                       rightType.name);
    }
    return leftType;
}

int ExprBinOpNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
}

int ExprBinOpNode::resolveColumn() const {
    if (_line > 0) return _col;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return _left->resolveColumn();
    return _right->resolveColumn();
}

ExprNode* ExprParenNode::expr() const {
    return _inner;
}

TypeInfo ExprParenNode::getType() const {
    return _inner->getType();
}

ExprNode* ExprDotNode::baseExpr() const {
    return _baseExpr;
}

string ExprDotNode::member() const {
    // 元组成员访问 token 是 DOT_NUM（形如 ".0"），剥掉前导点后下游"全数字 → tuple index"判定继续生效
    auto text = _member.getText();
    if (!text.empty() && text.front() == '.') return text.substr(1);
    return text;
}

int ExprDotNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int memberLine = static_cast<int>(_member.getLine());
    if (memberLine > 0) return memberLine;
    if (_baseExpr) return _baseExpr->resolveLineNumber();
    return 0;
}

int ExprDotNode::resolveColumn() const {
    if (_line > 0) return _col;
    int memberLine = static_cast<int>(_member.getLine());
    if (memberLine > 0) return static_cast<int>(_member.getCharPositionInLine()) + 1;
    if (_baseExpr) return _baseExpr->resolveColumn();
    return 0;
}

bool ExprDotNode::parseChain(const ExprDotNode* top, string& aliasName, vector<string>& segments) {
    segments.clear();
    segments.push_back(top->member());
    ExprNode* cur = top->_baseExpr;
    while (auto* d = dynamic_cast<ExprDotNode*>(cur)) {
        segments.push_back(d->member());
        cur = d->_baseExpr;
    }
    auto* baseLit = dynamic_cast<ExprLiteralNode*>(cur);
    if (!baseLit) return false;
    auto* objLit = dynamic_cast<LiteralObjNode*>(baseLit->literal());
    if (!objLit) return false;
    aliasName = objLit->getValue().getText();
    std::ranges::reverse(segments);
    return true;
}

bool ExprDotNode::isFieldAccess() const {
    TypeInfo baseT = _baseExpr->getType();
    if (_safe) {
        if (!baseT.isNullable()) return false;
        auto inner = baseT.nullableInnerType();
        if (!inner) return false;
        baseT = *inner;
    }
    unwrapRecvType(baseT);
    auto* file = enclosingFileFrom(this);
    if (!file) return false;
    auto sd = namesFromFile(file).lookupStruct(baseT, /*includeBuiltin=*/true);
    return sd && sd->field(member()) != nullptr;
}

TypeInfo ExprDotNode::getType() const {
    auto member = this->member();
    DEBUG_LOG_VAL("ExprDotNode::getType - member", member);

    // 安全访问 a?.b / a?.foo()：base 必须是 Nullable<T>
    // a?.b 字段访问 → 结果为 Nullable<U>（U = b 的字段类型）
    // a?.foo() 方法调用 → 先返回 TypeKind::Fn（仅编码返回类型），由 ExprCallNode 包装为 Nullable<ret>
    if (_safe) {
        auto baseT = _baseExpr->getType();
        if (!baseT.isNullable()) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3024, baseT.name);
        }
        auto innerType = baseT.nullableInnerType();
        if (!innerType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
        }
        // Phase 5: Rc<T>? 自动 deref —— 把 Rc<U> 视为 U 进字段/方法查
        auto rawInnerType = innerType; // Rc<U>（用于泛型替换等场景）
        if (innerType->isRc()) {
            auto rcInner = innerType->rcElementType();
            if (!rcInner) {
                throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
            }
            innerType = rcInner;
        }
        auto scope = findNearestScope();
        auto* file = dynamic_cast<FileNode*>(scope);
        while (!file && scope) {
            scope = scope->parentScope();
            file = dynamic_cast<FileNode*>(scope);
        }
        if (!file) {
            return {};
        }

        auto sd = namesFromFile(file).lookupStruct(*innerType, /*includeBuiltin=*/true);

        // 创建泛型替换表（innerType 有泛型实参时使用）
        map<string, TypeInfo> genSubst;
        if (innerType->isGeneric() && sd && sd->isGeneric() &&
            innerType->genericArgs.size() == sd->typeParams().size()) {
            for (size_t i = 0; i < innerType->genericArgs.size(); ++i) {
                auto& a = innerType->genericArgs[i];
                genSubst[sd->typeParams()[i]] = a ? *a : TypeInfo();
            }
        } else if (innerType->hasGenericArgs() && innerType->genericArgs.size() == 1) {
            genSubst["T"] = innerType->genericArgs[0] ? *innerType->genericArgs[0] : TypeInfo();
        }

        // 先尝试字段访问
        if (sd) {
            int idx = sd->fieldIndex(member);
            if (idx >= 0) {
                auto fieldType = sd->fields()[idx]->getType();
                if (!genSubst.empty()) fieldType = fieldType.substitute(genSubst);
                // 包装为 Nullable<U>
                std::vector<sp<TypeInfo>> args;
                args.push_back(make_shared<TypeInfo>(fieldType));
                return {"Nullable", args};
            }
        }

        // 字段未找到 → 尝试方法调用（a?.foo()）
        // 方法查找走 fnSymbol，与下方非 safe 路径模式一致
        {
            // 对 Rc<T>? 的 actualType 需要用 inner type 做方法查找
            const TypeInfo& actualType = *innerType;

            // 内建类型方法
            if (isBuiltinType(actualType.name)) {
                if (member.starts_with("to_")) {
                    string dstType = member.substr(3);
                    if (isBuiltinType(dstType)) {
                        return makeCallFnType(TypeInfo(dstType));
                    }
                }
                // SDK 注册的内建类型方法（如 String.len、i32.to_string 等）
                string methodFullName = actualType.name + "." + member;
                auto methodSym = file->lookupFnSymbol(methodFullName);
                if (!methodSym) {
                    ScopeNode* p = file->parentScope();
                    while (p && !methodSym) {
                        if (auto pf = dynamic_cast<FileNode*>(p)) {
                            methodSym = pf->lookupFnSymbol(methodFullName);
                        }
                        p = p->parentScope();
                    }
                }
                if (methodSym) {
                    DEBUG_LOG_VAL("ExprDotNode::getType - safe builtin method, returning Fn",
                                  methodSym->retType.getFullName());
                    return makeCallFnType(methodSym->retType);
                }
            }

            if (actualType.isArrayGeneric() || actualType.isArray()) {
                if (auto* spec = sema::lookupInstanceBuiltin(actualType, member)) {
                    return makeCallFnType(sema::builtinMethodReturnType(*spec, actualType));
                }
            }

            // 结构体方法
            auto methodSym = namesFromFile(file).lookupMethod(actualType, member);
            if (methodSym) {
                auto rt = methodSym->retType;
                if (!genSubst.empty()) rt = rt.substitute(genSubst);
                DEBUG_LOG_VAL("ExprDotNode::getType - safe method, returning Fn", rt.getFullName());
                return makeCallFnType(std::move(rt), methodSym->fallibleErrType);
            }

            // §12.4: dyn 边界方法
            if (actualType.kind == TypeKind::Normal && !actualType.isGeneric()) {
                auto rt = lookupSpecBoundMethodRetType(parent(), actualType.name, member);
                if (!rt.empty()) {
                    return makeCallFnType(std::move(rt));
                }
            }
        }

        // 字段和方法都未找到 → 报错
        if (sd) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3040, innerType->name, member);
        } else {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3044, innerType->name);
        }
    }

    // DRAFT-spec-reflect §6: Field.value → compile-time field name rewrite.
    // When .value is accessed on a Field-typed expression, attempt to statically resolve
    // which concrete struct field is referenced.
    // Resolution is done via AST pattern matching first (independent of type resolution,
    // which may fail due to scope chain issues in nested contexts),
    // then falls back to type-based check for non-compile-time-known Field references.
    if (member == "value") {
        // Helper: trace an expression to {structDecl, fieldIndex}.
        std::function<std::pair<const StructDeclNode*, int>(ExprNode*)> tryResolve;
        tryResolve = [&](ExprNode* expr) -> std::pair<const StructDeclNode*, int> {
            // Case 1: Direct .get(N) call on a static fields array
            if (auto* call = dynamic_cast<ExprCallNode*>(expr)) {
                auto* callee = call->getCalleeExpr();
                auto* dot = dynamic_cast<ExprDotNode*>(callee);
                if (dot && dot->member() == "get" && call->getArgs().size() == 1) {
                    auto* base = dot->baseExpr();
                    auto* path = dynamic_cast<ExprPathCallNode*>(base);
                    if (path && path->variantName().getText() == "fields") {
                        std::string sn = path->resolvedLhsName();
                        if (sn == "Self") return {nullptr, -1};
                        // Evaluate the index argument as a compile-time integer
                        auto* idxExpr = call->getArgs()[0];
                        if (auto* idxLit = dynamic_cast<ExprLiteralNode*>(idxExpr)) {
                            if (auto* intLit = dynamic_cast<LiteralIntNode*>(idxLit->literal())) {
                                Token tok = intLit->getValue();
                                i64 idx = sema::parseIntLiteral(tok.getText(), static_cast<int>(tok.getLine()),
                                                                static_cast<int>(tok.getCharPositionInLine()) + 1);
                                if (idx >= 0) {
                                    // Look up struct declaration from scope
                                    auto* s = expr->findNearestScope();
                                    auto* file = dynamic_cast<FileNode*>(s);
                                    while (!file && s) {
                                        s = s->parentScope();
                                        file = dynamic_cast<FileNode*>(s);
                                    }
                                    StructDeclNode* sd = nullptr;
                                    if (file) {
                                        sd = file->getStructDecl(sn);
                                        if (!sd) {
                                            auto* p = dynamic_cast<ScopeNode*>(file);
                                            while (p) {
                                                p = dynamic_cast<ScopeNode*>(p->parentScope());
                                                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                                                    sd = pf->getStructDecl(sn);
                                                    if (sd) break;
                                                }
                                            }
                                        }
                                    }
                                    if (sd) {
                                        int nonStaticCount = 0;
                                        for (auto& f : sd->fields()) {
                                            if (f->isStatic()) continue;
                                            if (nonStaticCount == idx) {
                                                return {sd, sd->fieldIndex(f->name().getText())};
                                            }
                                            ++nonStaticCount;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Case 1b: Direct [N] indexing on a static fields array (ExprGetNode)
            //   e.g. `Point::fields[0].value`
            //   ExprGetNode with base ExprPathCallNode("fields") and literal int index.
            if (auto* get = dynamic_cast<ExprGetNode*>(expr)) {
                if (get->indices().size() == 1) {
                    auto* path = dynamic_cast<ExprPathCallNode*>(get->arrayExpr());
                    if (path && path->variantName().getText() == "fields") {
                        std::string sn = path->resolvedLhsName();
                        if (sn == "Self") return {nullptr, -1};
                        auto* idxExpr = get->indices()[0];
                        if (auto* idxLit = dynamic_cast<ExprLiteralNode*>(idxExpr)) {
                            if (auto* intLit = dynamic_cast<LiteralIntNode*>(idxLit->literal())) {
                                Token tok = intLit->getValue();
                                i64 idx = sema::parseIntLiteral(tok.getText(), static_cast<int>(tok.getLine()),
                                                                static_cast<int>(tok.getCharPositionInLine()) + 1);
                                if (idx >= 0) {
                                    auto* s = expr->findNearestScope();
                                    auto* file = dynamic_cast<FileNode*>(s);
                                    while (!file && s) {
                                        s = s->parentScope();
                                        file = dynamic_cast<FileNode*>(s);
                                    }
                                    StructDeclNode* sd = nullptr;
                                    if (file) {
                                        sd = file->getStructDecl(sn);
                                        if (!sd) {
                                            auto* p = dynamic_cast<ScopeNode*>(file);
                                            while (p) {
                                                p = dynamic_cast<ScopeNode*>(p->parentScope());
                                                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                                                    sd = pf->getStructDecl(sn);
                                                    if (sd) break;
                                                }
                                            }
                                        }
                                    }
                                    if (sd) {
                                        int nonStaticCount = 0;
                                        for (auto& f : sd->fields()) {
                                            if (f->isStatic()) continue;
                                            if (nonStaticCount == idx) {
                                                return {sd, sd->fieldIndex(f->name().getText())};
                                            }
                                            ++nonStaticCount;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            // Case 2: Variable reference — trace through let definition
            if (auto* exLit = dynamic_cast<ExprLiteralNode*>(expr)) {
                if (auto* objLit = dynamic_cast<LiteralObjNode*>(exLit->literal())) {
                    std::string varName = objLit->getValue().getText();
                    auto* s = expr->findNearestScope();
                    while (s) {
                        if (auto* fn = dynamic_cast<FnNode*>(s)) {
                            for (auto& stmt : fn->body()) {
                                if (auto* letStmt = dynamic_cast<StatementDeclareAssignNode*>(stmt)) {
                                    if (letStmt->name().getText() == varName) {
                                        return tryResolve(letStmt->expr());
                                    }
                                }
                            }
                            break;
                        }
                        s = s->parentScope();
                    }
                }
            }
            return {nullptr, -1};
        };

        // Try AST pattern matching first (works even when type resolution is
        // incomplete due to scope chain issues in nested expression contexts).
        auto [sd, fieldIdx] = tryResolve(_baseExpr);
        if (sd && fieldIdx >= 0) {
            _reflectFieldResolved = true;
            _reflectStructDecl = sd;
            _reflectFieldIndex = fieldIdx;
            return sd->fields()[fieldIdx]->getType();
        }

        // Pattern matching failed — check if the base type indicates a
        // non-compile-time-known Field reference (e.g. for-loop variable).
        TypeInfo baseT = _baseExpr->getType();
        if (baseT.isRef()) {
            auto refElem = baseT.refElementType();
            if (refElem) baseT = *refElem;
        }
        if (baseT.name == "Field") {
            // Non-compile-time-known Field reference → E3133
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3133);
        }
        // Not a Field reference — fall through to normal dot resolution.
    }

    // 链式 Dot 访问 alias-rooted：
    //   模块别名：`alias.fn` → fn_overload。
    //   包别名：`pkg.s1. .. .sN` → 若 s1..s(N-1) 指向已加载的子文件，则 sN 为 fn
    //   交给 ExprCall 分派（fn_overload）；否则仍在包树中导航（pkg_chain）。
    {
        string aliasName;
        vector<string> segs;
        if (parseChain(this, aliasName, segs)) {
            auto scope = findNearestScope();
            if (scope) {
                auto sym = scope->lookupSymbol(aliasName);
                if (sym && (sym->kind == SymbolKind::Module || sym->kind == SymbolKind::Package)) {
                    auto* f = dynamic_cast<FileNode*>(scope);
                    auto s = scope;
                    while (!f && s) {
                        s = s->parentScope();
                        f = dynamic_cast<FileNode*>(s);
                    }
                    if (f && f->isAmbiguousAlias(aliasName)) {
                        f->throwAmbiguousAlias(aliasName, resolveLineNumber());
                    }
                }
                if (sym && sym->kind == SymbolKind::Module && segs.size() == 1) {
                    return TypeInfo("fn_overload");
                }
                if (sym && sym->kind == SymbolKind::Package) {
                    auto* file = dynamic_cast<FileNode*>(scope);
                    auto s = scope;
                    while (!file && s) {
                        s = s->parentScope();
                        file = dynamic_cast<FileNode*>(s);
                    }
                    if (file && segs.size() >= 2) {
                        string childKey;
                        for (size_t i = 0; i + 1 < segs.size(); ++i) {
                            if (i) childKey += '.';
                            childKey += segs[i];
                        }
                        if (file->packageChild(aliasName, childKey)) {
                            return TypeInfo("fn_overload");
                        }
                    }
                    return TypeInfo("pkg_chain");
                }
            }
        }
    }

    auto baseType = _baseExpr->getType();

    // 元组成员访问 a.N：member 为纯数字，base 为 Tuple（或别名透明展开后的 Tuple）
    // 透明别名解析覆盖 Normal（如 IPair = (i32,i32)）和 Generic（如 Pair<i32> 实例化自 Pair<T> = (T,T)）
    if (!member.empty() && std::ranges::all_of(member, [](char c) { return c >= '0' && c <= '9'; })) {
        TypeInfo resolved = baseType;
        if (resolved.kind == TypeKind::Normal || resolved.kind == TypeKind::Generic) {
            auto scope = findNearestScope();
            auto* file = dynamic_cast<FileNode*>(scope);
            while (!file && scope) {
                scope = scope->parentScope();
                file = dynamic_cast<FileNode*>(scope);
            }
            if (file) {
                std::set<std::string> visited;
                auto cur = resolved;
                while (cur.kind == TypeKind::Normal || cur.kind == TypeKind::Generic) {
                    auto* alias = file->getAliasDecl(cur.name);
                    // 非泛型别名：直接把目标类型作为新 cur 继续展开（如 A = IPair → IPair → (i32,i32)）
                    if (alias && !alias->isGeneric() && alias->target()) {
                        if (visited.count(cur.name)) break;
                        visited.insert(cur.name);
                        cur = alias->target()->getType();
                        continue;
                    }
                    // 泛型别名实例化：Pair<T> = (T,T) 遇 Pair<i32> → 替换 T→i32 得 (i32,i32)
                    if (alias && alias->isGeneric() && alias->target() &&
                        alias->typeParams().size() == cur.genericArgs.size()) {
                        if (visited.count(cur.name)) break;
                        visited.insert(cur.name);
                        std::map<std::string, TypeInfo> subst;
                        for (size_t i = 0; i < alias->typeParams().size(); ++i) {
                            subst[alias->typeParams()[i]] = cur.genericArgs[i] ? *cur.genericArgs[i] : TypeInfo();
                        }
                        cur = alias->target()->getType().substitute(subst);
                        continue;
                    }
                    break;
                }
                resolved = cur;
            }
        }
        if (resolved.isTuple()) {
            auto idx = static_cast<size_t>(std::stoul(member));
            auto& elems = resolved.tupleElements();
            if (idx >= elems.size()) {
                throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3100, member, baseType.getFullName(),
                               std::to_string(elems.size()));
            }
            return elems[idx] ? *elems[idx] : TypeInfo();
        }
    }

    TypeInfo actualType = baseType;

    if (baseType.isRef()) {
        auto refElemType = baseType.refElementType();
        if (refElemType) {
            actualType = *refElemType;
        }
    }

    if (actualType.isHeap()) {
        auto heapElemType = actualType.heapElementType();
        if (heapElemType) {
            actualType = *heapElemType;
        }
    }

    if (actualType.isRc()) {
        auto rcElemType = actualType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
        }
    }

    // Dyn<D> / Dyn<D&> 的 `.m`：用 draft 签名表回填返回类型，包装成 TypeKind::Fn
    // 让 ExprCallNode 在静态阶段算出确切类型（与 §12.4 draft 边界查找走同形分支）
    // v0.16: [] 返回 T&——baseType 可能是 Dyn<Shape>&，用剥 Ref 后的 actualType 做 Dyn 判断
    if (actualType.isDyn()) {
        auto rt = lookupDynMethodRetType(parent(), actualType, member);
        if (!rt.empty()) {
            return makeCallFnType(std::move(rt));
        }
        return makeCallFnType({});
    }

    if (isBuiltinType(actualType.name)) {
        if (member.starts_with("to_")) {
            string dstType = member.substr(3);
            if (isBuiltinType(dstType)) {
                DEBUG_LOG_VAL("ExprDotNode::getType - returning Fn for builtin cast", dstType);
                return makeCallFnType(TypeInfo(dstType));
            }
        }

        auto scope = findNearestScope();
        if (scope) {
            auto file = dynamic_cast<FileNode*>(scope);
            while (!file && scope) {
                scope = scope->parentScope();
                file = dynamic_cast<FileNode*>(scope);
            }
            if (file) {
                string methodFullName = actualType.name + "." + member;
                auto methodSym = file->lookupFnSymbol(methodFullName);
                if (methodSym) {
                    DEBUG_LOG_VAL("ExprDotNode::getType - found SDK method for builtin type, returning Fn",
                                  methodSym->retType.getFullName());
                    return makeCallFnType(methodSym->retType);
                }
            }
        }

        return _baseExpr->getType();
    }

    // Array<T> 方法调用（[T * N] 是 Array<T> 的语法糖）。
    if (actualType.isArray() || actualType.isArrayGeneric()) {
        if (auto* spec = sema::lookupInstanceBuiltin(actualType, member)) {
            return makeCallFnType(sema::builtinMethodReturnType(*spec, actualType));
        }
        return _baseExpr->getType();
    }

    auto scope = findNearestScope();
    if (scope) {
        auto file = dynamic_cast<FileNode*>(scope);
        while (!file && scope) {
            scope = scope->parentScope();
            file = dynamic_cast<FileNode*>(scope);
        }
        if (file) {
            // file 自身找不到时走父 FileNode (SDK) 链, 与 lookupSpecBoundMethodRetType
            // 同款; 不打通会导致 t.<SdkStructField> getType 回落到 baseExpr type
            // (sema 不知道字段实类型, 重载解析挑错).
            auto structDecl = namesFromFile(file).lookupStruct(actualType, /*includeBuiltin=*/true);

            // 若 actualType 是泛型实例，构造 T→具体 的替换表
            map<string, TypeInfo> genSubst;
            if (actualType.hasGenericArgs() && structDecl && structDecl->isGeneric() &&
                structDecl->typeParams().size() == actualType.genericArgs.size()) {
                for (size_t i = 0; i < actualType.genericArgs.size(); ++i) {
                    auto& a = actualType.genericArgs[i];
                    genSubst[structDecl->typeParams()[i]] = a ? *a : TypeInfo();
                }
            } else if (actualType.hasGenericArgs() && actualType.genericArgs.size() == 1) {
                genSubst["T"] = actualType.genericArgs[0] ? *actualType.genericArgs[0] : TypeInfo();
            }

            if (structDecl) {
                auto field = structDecl->field(member);
                if (field) {
                    auto ft = field->getType();
                    if (!genSubst.empty()) ft = ft.substitute(genSubst);
                    return ft;
                }
            }

            auto methodSym = namesFromFile(file).lookupMethod(actualType, member);
            if (methodSym) {
                auto rt = methodSym->retType;
                if (!genSubst.empty()) rt = rt.substitute(genSubst);
                DEBUG_LOG_VAL("ExprDotNode::getType - found method, returning Fn", rt.getFullName());
                return makeCallFnType(std::move(rt), methodSym->fallibleErrType);
            }
        }
    }

    // §12.4：actualType 为外层泛型形参 T，且 T 有 draft 边界声明 `<T : D>`，
    // 在 D 的签名表里找 member 的返回类型；命中即返回 TypeKind::Fn，
    // 让 ExprCallNode 在静态阶段算出确切类型，避免下游函数重载查找拿到 T 而失败。
    if (actualType.kind == TypeKind::Normal && !actualType.isGeneric()) {
        auto rt = lookupSpecBoundMethodRetType(parent(), actualType.name, member);
        if (!rt.empty()) {
            return makeCallFnType(std::move(rt));
        }
    }

    return _baseExpr->getType();
}

ExprCompareNode::Op ExprCompareNode::op() const {
    return _op;
}

ExprNode* ExprCompareNode::left() const {
    return _left;
}

ExprNode* ExprCompareNode::right() const {
    return _right;
}

TypeInfo ExprCompareNode::getType() const {
    auto leftType = _left->getType().peelAutoDeref();
    auto rightType = _right->getType().peelAutoDeref();
    if (leftType != rightType) {
        if (isFlexibleIntExpr(_right) && tryInferIntType(_right, leftType)) {
            return TypeInfo("bool");
        }
        if (isFlexibleIntExpr(_left) && tryInferIntType(_left, rightType)) {
            return TypeInfo("bool");
        }
        // spec §7.2.3.3: 非内置类型允许跨类型形参，类型匹配由 codegen 方法解析完成
        if (!isBuiltinType(leftType.name)) {
            return TypeInfo("bool");
        }
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3001, "comparison", leftType.name,
                       rightType.name);
    }
    return TypeInfo("bool");
}

int ExprCompareNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
}

int ExprCompareNode::resolveColumn() const {
    if (_line > 0) return _col;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return _left->resolveColumn();
    return _right->resolveColumn();
}

StatementBlockNode::StatementBlockNode(Node* parent, vector<StatementNode*> statements, ExprNode* resultExpr,
                                       bool hasResult)
    : ScopeNode(parent), _statements(std::move(statements)), _resultExpr(resultExpr), _hasResult(hasResult) {}

const vector<StatementNode*>& StatementBlockNode::statements() const {
    return _statements;
}

ExprNode* StatementBlockNode::resultExpr() const {
    return _resultExpr;
}

bool StatementBlockNode::hasResult() const {
    return _hasResult;
}

bool callIsNoReturn(ScopeNode* scope, ExprCallNode* call) {
    if (!call) return false;
    if (call->hasResolvedSymbol() && call->resolvedSymbol().isFn() && call->resolvedSymbol().fn &&
        call->resolvedSymbol().fn->isNoReturn) {
        return true;
    }
    auto callee = call->getCalleeExpr();
    auto litCallee = dynamic_cast<ExprLiteralNode*>(callee);
    if (!litCallee) return false;
    auto obj = dynamic_cast<LiteralObjNode*>(litCallee->literal());
    if (!obj) return false;
    ScopeNode* sc = scope ? scope : call->findNearestScope();
    if (!sc) return false;
    auto* sym = sc->lookupFnSymbol(obj->getValue().getText());
    return sym && sym->isNoReturn;
}

static bool stmtTerminatesFlow(ScopeNode* scope, StatementNode* stmt);

bool exprTerminatesFlow(ScopeNode* scope, ExprNode* expr) {
    if (!expr) return false;
    ScopeNode* sc = scope ? scope : expr->findNearestScope();
    if (auto call = dynamic_cast<ExprCallNode*>(expr)) {
        return callIsNoReturn(sc, call);
    }
    if (auto ife = dynamic_cast<ExprIfElseNode*>(expr)) {
        if (!ife->elseBlock()) return false;
        if (!blockTerminatesFlow(sc, ife->thenBlock())) return false;
        for (auto& el : ife->elifs()) {
            if (!blockTerminatesFlow(sc, el->block())) return false;
        }
        return blockTerminatesFlow(sc, ife->elseBlock());
    }
    if (auto ol = dynamic_cast<ExprOneLineIfElseNode*>(expr)) {
        return exprTerminatesFlow(sc, ol->trueValue()) && exprTerminatesFlow(sc, ol->falseValue());
    }
    if (auto m = dynamic_cast<ExprMatchNode*>(expr)) {
        if (m->arms().empty()) return false;
        for (auto& arm : m->arms()) {
            if (!arm) return false;
            if (arm->hasBlock()) {
                if (!blockTerminatesFlow(sc, arm->block())) return false;
            } else if (!exprTerminatesFlow(sc, arm->body())) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static bool stmtTerminatesFlow(ScopeNode* scope, StatementNode* stmt) {
    if (!stmt) return false;
    if (dynamic_cast<StatementRetNode*>(stmt) || dynamic_cast<StatementRetVoidNode*>(stmt)) return true;
    if (auto se = dynamic_cast<StatementExprNode*>(stmt)) {
        return exprTerminatesFlow(scope, se->expr());
    }
    return false;
}

bool blockTerminatesFlow(ScopeNode* scope, StatementBlockNode* block) {
    if (!block) return false;
    ScopeNode* sc = scope ? scope : block;
    for (auto& s : block->statements()) {
        if (stmtTerminatesFlow(sc, s)) return true;
    }
    if (block->hasResult() && block->resultExpr()) {
        return exprTerminatesFlow(sc, block->resultExpr());
    }
    return false;
}

ExprNode* ExprElIfNode::condition() const {
    return _condition;
}

StatementBlockNode* ExprElIfNode::block() const {
    return _block;
}

ExprNode* ExprIfElseNode::condition() const {
    return _condition;
}

StatementBlockNode* ExprIfElseNode::thenBlock() const {
    return _thenBlock;
}

const vector<ExprElIfNode*>& ExprIfElseNode::elifs() const {
    return _elifs;
}

StatementBlockNode* ExprIfElseNode::elseBlock() const {
    return _elseBlock;
}

TypeInfo ExprIfElseNode::getType() const {
    // §4.9.3.5：ret / #NoReturn 臂流终止，不参与类型合并；其余无尾值则整体 void。
    ScopeNode* sc = findNearestScope();
    TypeInfo resultType;
    bool haveValue = false;

    auto consider = [&](StatementBlockNode* block) -> bool {
        if (!block) return false;
        if (blockTerminatesFlow(sc, block)) return true;
        if (!block->hasResult() || !block->resultExpr()) return false;
        auto t = block->resultExpr()->resolvedOrGetType();
        if (!haveValue) {
            resultType = std::move(t);
            haveValue = true;
            return true;
        }
        if (!blockValueTypesMatch(t, resultType)) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, resultType.name, t.name);
        }
        if (isEmptyArrayLitType(resultType) && !isEmptyArrayLitType(t)) resultType = std::move(t);
        return true;
    };

    if (!consider(_thenBlock)) return {};
    for (auto& elif : _elifs) {
        if (!consider(elif->block())) return {};
    }
    if (_elseBlock) {
        if (!consider(_elseBlock)) return {};
    } else if (haveValue) {
        return {};
    }
    return haveValue ? resultType : TypeInfo{};
}

int ExprIfElseNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _condition->resolveLineNumber();
}

int ExprIfElseNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _condition->resolveColumn();
}

TypeInfo ExprOneLineIfElseNode::getType() const {
    ScopeNode* sc = findNearestScope();
    bool trueTerm = exprTerminatesFlow(sc, _trueValue);
    bool falseTerm = exprTerminatesFlow(sc, _falseValue);
    if (trueTerm && falseTerm) return {};
    if (trueTerm) return _falseValue->resolvedOrGetType();
    if (falseTerm) return _trueValue->resolvedOrGetType();

    auto trueType = _trueValue->resolvedOrGetType();
    auto falseType = _falseValue->resolvedOrGetType();
    if (!blockValueTypesMatch(trueType, falseType)) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, trueType.name, falseType.name);
    }
    if (isEmptyArrayLitType(trueType) && !isEmptyArrayLitType(falseType)) return falseType;
    return trueType;
}

int ExprOneLineIfElseNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _condition->resolveLineNumber();
}

int ExprOneLineIfElseNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _condition->resolveColumn();
}

ExprNode* ExprGetNode::arrayExpr() const {
    return _arrayExpr;
}

const vector<ExprNode*>& ExprGetNode::indices() const {
    return _indices;
}

TypeInfo ExprGetNode::getType() const {
    auto arrayType = _arrayExpr->getType();

    // Auto-deref: [T * N]& → [T * N] (Ref<Array<...>>)
    if (arrayType.isRef()) {
        auto inner = arrayType.refElementType();
        if (inner) arrayType = *inner;
    }

    // v0.16: [] 语法糖同步——arr[i] 返回 T&，与 arr.get(i) 一致。
    // Array<T> 不可含 T&（§3.2.3.2），无需防双重包装；[T * N] 仅 reflect [Field& * N] 的 elem 已是 Ref，保留不包。
    auto wrapRef = [](const TypeInfo& elem) -> TypeInfo {
        if (elem.isRef()) return elem; // [T& * N] 的 elem 已是 T&，不双重包
        return {"Ref", {std::make_shared<TypeInfo>(elem)}};
    };

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
        }
        return wrapRef(*elemType);
    }

    if (!arrayType.isArray()) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3062, arrayType.name);
    }

    if (!arrayType.elementType) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
    }

    return wrapRef(*arrayType.elementType);
}

int ExprGetNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _arrayExpr->resolveLineNumber();
}

int ExprGetNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _arrayExpr->resolveColumn();
}

const vector<ExprNode*>& ExprArrayNode::elements() const {
    return _elements;
}

TypeInfo ExprArrayNode::getType() const {
    if (_elements.empty()) {
        return {make_shared<TypeInfo>("__empty"), 0};
    }

    TypeInfo elementType = _elements[0]->getType();
    for (size_t i = 1; i < _elements.size(); ++i) {
        auto elemType = _elements[i]->getType();
        if (elemType != elementType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3009, elementType.name, elemType.name);
        }
    }

    auto elemShared = make_shared<TypeInfo>(elementType);
    return {elemShared, _elements.size()};
}

int ExprArrayNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    if (!_elements.empty()) {
        return _elements[0]->resolveLineNumber();
    }
    return 0;
}

// Lambda 字面量类型：Fn TypeInfo（结构等同，§3.4）
// 形参类型缺失 → 槽位放 empty TypeInfo 占位，由调用 / 赋值点反推
// 返回类型：显式标注用之；否则从 body / 期望类型推断
// 调用点 / 赋值点反推后 _inferredFnType 持完整类型，优先返回
TypeInfo LambdaExprNode::getType() const {
    if (_inferredFnType.isFn()) return _inferredFnType;
    vector<sp<TypeInfo>> ps;
    ps.reserve(_params.size());
    for (auto& slot : _params) {
        if (slot.type)
            ps.push_back(make_shared<TypeInfo>(slot.type->getType()));
        else
            ps.push_back(make_shared<TypeInfo>()); // 占位，等调用 / 赋值点反推回填
    }
    sp<TypeInfo> rt = nullptr;
    if (_retType) {
        rt = make_shared<TypeInfo>(_retType->getType());
        if (_fallibleErrType) {
            rt->attachFallibleErr(_fallibleErrType->getType().name);
        }
    } else if (_bodyExpr) {
        auto bt = _bodyExpr->getType();
        if (!bt.empty()) rt = make_shared<TypeInfo>(bt);
    } else if (!_bodyStmts.empty()) {
        auto* last = _bodyStmts.back();
        if (auto retStmt = dynamic_cast<StatementRetNode*>(last)) {
            auto bt = retStmt->expr()->getType();
            if (!bt.empty()) rt = make_shared<TypeInfo>(bt);
        } else if (auto exprStmt = dynamic_cast<StatementExprNode*>(last)) {
            if (!exprStmt->hasSemicolon()) {
                auto bt = exprStmt->expr()->getType();
                if (!bt.empty()) rt = make_shared<TypeInfo>(bt);
            }
        }
    }
    // nullable=false：lambda 字面量本身永非空
    return TypeInfo(FnTag{}, std::move(ps), rt, false);
}

// 元组构造表达式：把每个元素类型组合为 TupleTag TypeInfo
TypeInfo ExprTupleNode::getType() const {
    vector<sp<TypeInfo>> elems;
    elems.reserve(_elements.size());
    for (auto& e : _elements) {
        elems.push_back(make_shared<TypeInfo>(e->getType()));
    }
    return TypeInfo(TupleTag{}, std::move(elems));
}

int ExprTupleNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    if (!_elements.empty()) return _elements[0]->resolveLineNumber();
    return 0;
}

int ExprTupleNode::resolveColumn() const {
    if (_line > 0) return _col;
    if (!_elements.empty()) return _elements[0]->resolveColumn();
    return 0;
}

int ExprArrayNode::resolveColumn() const {
    if (_line > 0) return _col;
    if (!_elements.empty()) {
        return _elements[0]->resolveColumn();
    }
    return 0;
}

LiteralNode* ExprArrayInitNode::value() const {
    return _value;
}

TypeNode* ExprArrayInitNode::explicitType() const {
    return _explicitType;
}

TypeInfo ExprArrayInitNode::getType() const {
    TypeInfo elementType;
    if (_explicitType) {
        elementType = _explicitType->getType();
        // Phase 3.4.f.1: explicitType 与 value 字面量类型不匹配抛 E3009.
        // 不依赖目标 targetType (那条留 E3010 在 codegen 兜底), 可在 AST 层判定;
        // 进 kMigratedCodes 后由 SemaPass.visitExpr 顶部自动重抛.
        auto valueType = _value->getType();
        if (valueType != elementType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3009, valueType.name, elementType.name);
        }
    } else {
        elementType = _value->getType();
    }

    auto elemShared = make_shared<TypeInfo>(elementType);
    return {elemShared, 0};
}

TypeInfo ExprGetRefNode::getType() const {
    auto scope = findNearestScope();
    if (!scope) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3097);
    }

    auto sym = scope->lookupSymbol(_obj.getText());
    if (!sym) {
        SymbolSuggest::throwSymbolNotFound(scope, resolveLineNumber(), resolveColumn(), ErrorCode::E3030,
                                           _obj.getText());
    }

    TypeInfo baseType = sym->type;
    // Phase 4a: 若 sym 为 T&，剥到 T 后再走字段（&ref.f 与 &x.f 同义）
    if (baseType.isRef()) {
        if (auto inner = baseType.refElementType()) baseType = *inner;
    }

    for (auto& sub : _subs) {
        auto file = dynamic_cast<FileNode*>(scope);
        auto currentScope = scope;
        while (!file && currentScope) {
            currentScope = currentScope->parentScope();
            file = dynamic_cast<FileNode*>(currentScope);
        }
        if (!file) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3043);
        }

        // Phase 4c: Rc<T>.field 自动解引用到 payload 上找字段（&rc.field → field&）
        // Heap<T> 同理解引用到堆上 struct 再找字段
        TypeInfo lookupType = baseType;
        if (lookupType.isHeap()) {
            if (auto inner = lookupType.heapElementType()) lookupType = *inner;
        }
        if (lookupType.isRc()) {
            if (auto inner = lookupType.rcElementType()) lookupType = *inner;
        }

        auto structDecl = namesFromFile(file).lookupStruct(lookupType);
        if (!structDecl) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3041, lookupType.name);
        }

        auto field = structDecl->field(sub.getText());
        if (!field) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3040, lookupType.name, sub.getText());
        }

        TypeInfo fieldType = field->getType();
        // 泛型实例：按 typeParam→arg 替换字段类型
        if (lookupType.hasGenericArgs() && structDecl->isGeneric() &&
            lookupType.genericArgs.size() == structDecl->typeParams().size()) {
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    lookupType.genericArgs[i] ? *lookupType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        baseType = fieldType;
    }

    vector<sp<TypeInfo>> genericArgs;
    genericArgs.push_back(make_shared<TypeInfo>(baseType));
    return {"Ref", genericArgs};
}

int ExprGetRefNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return static_cast<int>(_obj.getLine());
}

int ExprGetRefNode::resolveColumn() const {
    if (_line > 0) return _col;
    return static_cast<int>(_obj.getCharPositionInLine()) + 1;
}

ExprUnaryNode::Op ExprUnaryNode::op() const {
    return _op;
}

ExprNode* ExprUnaryNode::right() const {
    return _right;
}

TypeInfo ExprUnaryNode::getType() const {
    auto rightType = _right->getType();
    // Heap<T> → T：一元运算符穿透 Heap wrapper，结果类型为内部 T
    if (rightType.isHeap()) {
        if (auto inner = rightType.heapElementType()) rightType = *inner;
    }
    // Rc<T> → T：一元运算符穿透 Rc wrapper，结果类型为内部 T
    if (rightType.isRc()) {
        if (auto inner = rightType.rcElementType()) rightType = *inner;
    }
    // Phase 3.4.h: 内置类型的一元 op 形态校验 (与 compileUnaryExpr 内置分支同义).
    // 非 builtin 走自定义方法路径 (customMethodOp), 不在此校验; 与 codegen `if
    // (!isBuiltinType(rightType.name)) compileCustomTypeUnaryOp(...)` 顺序一致.
    if (isBuiltinType(rightType.name)) {
        if (_op == Op::Rev && rightType.isFloat()) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3070, rightType.name);
        }
        if (_op == Op::Not && rightType.name != "bool") {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3071, rightType.name);
        }
    }
    return rightType;
}

int ExprUnaryNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _right->resolveLineNumber();
}

int ExprUnaryNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _right->resolveColumn();
}

// ExprMoveAssignNode: a <- b
// 移出旧值、替换新值、返回旧值。
// 类型：left 的类型（即旧值的类型），与 left / right 是否匹配无关（由 sema 校验）
TypeInfo ExprMoveAssignNode::getType() const {
    return _left->getType();
}

int ExprMoveAssignNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _left->resolveLineNumber();
}

int ExprMoveAssignNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _left->resolveColumn();
}

// ExprNullElseNode: a ?? b
// 类型规则：a 必须是 Nullable<T>，结果类型为 T；b 必须能转为 T
TypeInfo ExprNullElseNode::getType() const {
    auto leftType = _left->getType();
    // Ref<Nullable<T>> → 剥 Ref 取 Nullable（如 Array<Nullable<T>> 下标返回 T?&）
    if (leftType.isRef()) {
        if (auto refInner = leftType.refElementType(); refInner && refInner->isNullable()) {
            return *refInner->genericArgs[0];
        }
    }
    // 左侧若为 Nullable<T>，结果为 T
    if (leftType.isNullable()) {
        return *leftType.genericArgs[0];
    }
    // 容错：非 Nullable 时退回右侧类型，由后续语义检查报错
    return _right->getType();
}

int ExprNullElseNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _left->resolveLineNumber();
}

int ExprNullElseNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _left->resolveColumn();
}

// 枚举构造表达式：返回 enum 类型 TypeInfo
// 处理类型别名透传（[#3.F]）：若用户写的名字是别名，沿别名链解析到真正的 enum 名
// 别名解析失败时（链中出现非 Normal 或解不到底）退回原始名，留给编译期 getLLVMType 报错
// match 表达式：取所有非-else arm body 的共同类型；任一 arm 为 void 则整体 void
// 类型不一致抛 E3027（match arm 体类型失配，spec §5.5）
TypeInfo MatchArmNode::resultType() const {
    if (_block) {
        if (!_block->hasResult() || !_block->resultExpr()) return {};
        return _block->resultExpr()->resolvedOrGetType();
    }
    if (_body) return _body->resolvedOrGetType();
    return {};
}

bool MatchArmNode::skipsTypeMerge() const {
    auto* sc = const_cast<MatchArmNode*>(this);
    if (_block) return blockTerminatesFlow(sc, _block);
    if (_body) return exprTerminatesFlow(sc, _body);
    return false;
}

int MatchArmNode::resultLine() const {
    if (_block) {
        if (_block->hasResult() && _block->resultExpr()) return _block->resultExpr()->resolveLineNumber();
        return _block->getLineNumber();
    }
    if (_body) return _body->resolveLineNumber();
    return getLineNumber();
}

int MatchArmNode::resultCol() const {
    if (_block) {
        if (_block->hasResult() && _block->resultExpr()) return _block->resultExpr()->resolveColumn();
        return _block->getColumn();
    }
    if (_body) return _body->resolveColumn();
    return getColumn();
}

TypeInfo ExprMatchNode::getType() const {
    TypeInfo first;
    bool firstSet = false;
    for (auto& arm : _arms) {
        if (arm->skipsTypeMerge()) continue;
        auto t = arm->resultType();
        if (!firstSet) {
            first = t;
            firstSet = true;
            continue;
        }
        if (!blockValueTypesMatch(t, first)) {
            // 不在 getType 抛错，留给编译期更稳：返回首个，编译期再校验
            return first;
        }
        if (isEmptyArrayLitType(first) && !isEmptyArrayLitType(t)) first = t;
    }
    return first;
}

// try-catch 表达式：取 try block 末表达式 + 所有 catch arm body 末表达式的共同类型
// 流终止 arm（body 末以 ret / panic 结尾，hasResult=false）不参与类型合并；
// 与 if-else / match 同档：若任何参与方为 void 则整体 void，类型不一致返回首个，
// 编译期再校验（保持与 ExprMatchNode::getType 一致风格）。
TypeInfo ExprTryCatchNode::getType() const {
    ScopeNode* sc = findNearestScope();
    if (!blockTerminatesFlow(sc, _tryBlock) && (!_tryBlock->hasResult() || !_tryBlock->resultExpr())) {
        return {};
    }
    TypeInfo first;
    bool have = false;
    auto consider = [&](StatementBlockNode* block) {
        if (!block || blockTerminatesFlow(sc, block)) return;
        if (!block->hasResult() || !block->resultExpr()) return;
        auto t = block->resultExpr()->resolvedOrGetType();
        if (!have) {
            first = std::move(t);
            have = true;
            return;
        }
        if (!blockValueTypesMatch(t, first)) return;
        if (isEmptyArrayLitType(first) && !isEmptyArrayLitType(t)) first = std::move(t);
    };
    consider(_tryBlock);
    for (auto& arm : _catches) {
        if (arm) consider(arm->body());
    }
    return have ? first : TypeInfo{};
}

// Dyn<D>(x) / Dyn<D&>(x) 的整体类型 = `Dyn<D>` 或 `Dyn<D&>`。
// 内层 TypeNode 已携带借用形态（Ref<D>），这里直接包一层 `Dyn` 即可。
TypeInfo ExprDynCtorNode::getType() const {
    if (!_specType) return {};
    auto inner = make_shared<TypeInfo>(_specType->getType());
    return TypeInfo("Dyn", {inner});
}

// Phase 3b: 类型 = 所属结构体, structName 由 ast_builder 扫 _scopeStack 时填入
TypeInfo ExprStructLitNode::getType() const {
    if (_isSelfForm) {
        if (_structName.empty()) return {};
        TypeInfo t(_structName);
        if (auto* f = enclosingFile()) t.ownerModule = f->moduleName();
        return t;
    }
    auto r = sema::resolveExprTypeLhs(enclosingFile(), nullptr, _typePath, getLineNumber(), getColumn());
    return r.type;
}

string ExprPathCallNode::resolvedLhsName() const {
    string n = _enumName.getText();
    if (n != "Self") return n;
    Node* cur = parent();
    while (cur) {
        if (auto* impl = dynamic_cast<StructImplNode*>(cur)) return impl->structName();
        cur = cur->parent();
    }
    for (auto* s = findNearestScope(); s; s = s->parentScope()) {
        if (auto* impl = dynamic_cast<StructImplNode*>(s)) return impl->structName();
    }
    return n;
}

TypeInfo ExprPathCallNode::resolvedLhsType() const {
    if (_enumName.getText() == "Self") {
        string n = resolvedLhsName();
        TypeInfo t(n);
        if (auto* f = enclosingFile()) t.ownerModule = f->moduleName();
        return t;
    }
    auto r = sema::resolveExprTypeLhs(enclosingFile(), nullptr, _lhsPath, getLineNumber(), getColumn());
    return r.type;
}

TypeInfo ExprPathCallNode::getType() const {
    TypeInfo lhs = resolvedLhsType();
    string n = lhs.name;
    FileNode* file = enclosingFile();
    FileNode* sdk = nullptr;
    if (file) {
        for (auto* s = file->parentScope(); s; s = s->parentScope()) {
            if (auto* pf = dynamic_cast<FileNode*>(s)) {
                sdk = pf;
                break;
            }
        }
    }
    sema::NameResolver nr(file, sdk);

    // DRAFT-static-vars Phase 4: 若零参且 RHS 是 struct 静态字段，返回字段类型
    // includeBuiltin=true：允许 #Builtin struct 上的静态字段（如 i8::MAX）
    if (_args.empty()) {
        auto* structDecl = nr.lookupStruct(lhs, /*includeBuiltin=*/true);
        if (structDecl) {
            if (auto* sf = structDecl->staticField(_variantName.getText())) {
                return sf->type->getType();
            }
        }
    }

    // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` 走 reflect
    // 静态路径; getType 返回对应 SDK 类型供下游 (assignment / call) 推断.
    // LHS 必须是 struct (有 StructDecl), 才能区分于 enum::variant.
    string rhs = _variantName.getText();
    if (_args.empty() && (rhs == "type" || rhs == "fields" || rhs == "methods" || rhs == "variants")) {
        if (auto* sd = nr.lookupStruct(lhs)) {
            if (rhs == "type") return TypeInfo("Type");
            // fields / methods / variants → [T& * N]&
            auto withArrayRef = [&](const string& elemTypeName) -> TypeInfo {
                auto elemType = std::make_shared<TypeInfo>(elemTypeName);
                auto elemRef = std::make_shared<TypeInfo>("Ref", vector<sp<TypeInfo>>{elemType});
                u64 N = 0;
                if (rhs == "fields" && sd) {
                    for (auto& f : sd->fields()) {
                        if (!f->isStatic()) ++N;
                    }
                }
                // methods / variants: N=0 for now (not yet populated)
                auto arr = std::make_shared<TypeInfo>(elemRef, N);
                return {"Ref", {arr}};
            };
            if (rhs == "fields") return withArrayRef("Field");
            if (rhs == "methods") return withArrayRef("Method");
            if (rhs == "variants") return withArrayRef("Variant");
        }
    }

    // Array:<T>::factory(...)：#Builtin #Static 工厂（with_capacity）返回 Array<T>。
    // 无此分支时 getType 只给出裸名 Array，debug 下与 resolvedType 不一致，
    // recordTemp 也认不出 isArrayGeneric。
    if (n == "Array" && !_lhsTypeArgs.empty()) {
        vector<sp<TypeInfo>> args;
        args.reserve(_lhsTypeArgs.size());
        for (auto& ta : _lhsTypeArgs) {
            args.push_back(make_shared<TypeInfo>(ta->getType()));
        }
        return {n, std::move(args)};
    }

    // #Static fn：返回类型是方法 retType，不是 LHS 结构体名。
    // 工厂 `Type::make` 碰巧返回 Self，旧实现 `return TypeInfo(n)` 蒙对；
    // `Type::parse(...) i32 ! E` 必须查签名。enum 构造无 StructImpl，落到下面的 LHS 名。
    StructImplNode* impl = nr.lookupStructImpl(lhs);
    if (impl) {
        const string rhsName = _variantName.getText();
        const size_t arity = _args.size();
        FnHeaderNode* found = nullptr;
        int nfound = 0;
        for (auto& m : impl->methods()) {
            auto h = m->header();
            if (!h || h->name().getText() != rhsName || !h->isStatic()) continue;
            if (h->params().size() != arity) continue;
            found = h;
            ++nfound;
        }
        if (nfound == 1 && found) {
            TypeInfo rt;
            if (found->retType()) rt = found->retType()->getType();
            // TypeSelfNode.getType 灌入所属 struct 名，`isSelf()`（name=="Self"）为 false。
            // 无 turbofish 时旧实现 `return TypeInfo(n)` 对非泛型工厂蒙对；泛型工厂若仍
            // 返回裸名，调用点 recordTemp 会对含 Array 字段的实例走 getLLVMType(裸名) → E3091。
            const bool selfRet = rt.isSelf() || (rt.kind == TypeKind::Normal && rt.name == n && rt.genericArgs.empty());
            if (selfRet) {
                if (!_lhsTypeArgs.empty()) {
                    vector<sp<TypeInfo>> args;
                    args.reserve(_lhsTypeArgs.size());
                    for (auto& ta : _lhsTypeArgs) {
                        args.push_back(make_shared<TypeInfo>(ta->getType()));
                    }
                    TypeInfo inst{n, std::move(args)};
                    inst.ownerModule = lhs.ownerModule;
                    return inst;
                }
                return lhs;
            }
            return rt;
        }
    }

    return lhs;
}
