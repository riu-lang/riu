// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "expr_node.h"

#include <algorithm>
#include <set>

#include "analyzer/symbol_suggest.h"
#include "file_node.h"
#include "fn_node.h"
#include "sema/call_resolve.h"
#include "spec_node.h"
#include "statement_node.h"
#include "struct_node.h"
#include "types.h"

// §12.4：在生成式 AST 中遇到 `x.m()`（x:T 为泛型形参）时，
// 用形参声明位的 draft 边界查 m 的返回类型；走包含 SDK 回退的 file 链。
static TypeInfo lookupSpecBoundMethodRetType(Node* contextParent, const string& typeParamName,
                                             const string& methodName) {
    Node* cur = contextParent;
    p<FnHeaderNode> header = nullptr;
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
            if (sig->retType()) return sig->retType()->getType();
            return {};
        }
    }
    return {};
}

// Phase 4a：Dyn<D>/Dyn<D&> 上的 `.m` 静态类型查找。
// 把 dot 表达式的类型表示为 `fn() <ret>`，让外层 ExprCallNode 在静态阶段算出确切返回类型，
// 避免下游（assert_eq 推断 / implicit ret / 形参匹配）拿到 Dyn 类型而失败。
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

static p<ExprNode> unwrapParen(p<ExprNode> e) {
    while (auto paren = dynamic_cast<ExprParenNode*>(e)) {
        e = paren->expr();
    }
    return e;
}

bool isFlexibleIntExpr(p<ExprNode> expr) {
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
    return false;
}

bool tryInferIntType(p<ExprNode> expr, const TypeInfo& target) {
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
    try {
        return expr->getType() == target;
    } catch (...) {
        return false;
    }
}

const p<ExprNode>& ExprCallNode::getCalleeExpr() const {
    return _calleeExpr;
}

const std::vector<p<ExprNode>>& ExprCallNode::getArgs() const {
    return _args;
}

TypeInfo ExprCallNode::getType() const {
    auto type = _calleeExpr->getType();

    // Phase 2b: callee 自身就是 Fn 类型值（lambda 字面量 / fn-typed 变量 / 字段）
    // 调用结果即 fn 返回类型；void 时返回空 TypeInfo
    if (type.isFn()) {
        if (auto rt = type.fnReturnType()) return *rt;
        return {};
    }

    // Phase 3c: callee 为 Rc<fn(...)R>，自动解引取 fat-ptr 调用，结果同 fn 返回类型
    if (type.isRc()) {
        if (auto inner = type.rcElementType(); inner && inner->isFn()) {
            if (auto rt = inner->fnReturnType()) return *rt;
            return {};
        }
    }

    // v0.16: [] 返回 T&——callee 为 Ref<fn(...)R> 时自动解引用，取 fn 返回类型
    if (type.isRef()) {
        if (auto inner = type.refElementType(); inner && inner->isFn()) {
            if (auto rt = inner->fnReturnType()) return *rt;
            return {};
        }
    }

    DEBUG_LOG_VAL("ExprCallNode::getType - type.name", type.name);
    DEBUG_LOG_VAL("ExprCallNode::getType - starts_with('fn() ')", type.name.starts_with("fn() "));
    DEBUG_LOG_VAL("ExprCallNode::getType - calleeExpr type", typeid(*_calleeExpr).name());

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
                            if (i) childKey += ".";
                            childKey += segs[i];
                        }
                        if (auto* target = file->packageChild(aliasName, childKey)) {
                            vector<TypeInfo> argTypes;
                            argTypes.reserve(_args.size());
                            for (auto& arg : _args)
                                argTypes.push_back(arg->getType());
                            auto* fn = target->lookupFnSymbolWithParams(segs.back(), argTypes);
                            if (fn) return fn->retType;
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
                        }
                    }
                }
            }
        }
        return {};
    }

    // 方法调用（callee 为 ExprDotNode）：直接查方法符号，拿到完整 retType（含泛型 typeArgs）。
    // 避免走 "fn() <name>" 字符串编码丢失泛型信息（例如 Array<u8> 被编码为 Array_u8）。
    if (type.name.starts_with("fn() ")) {
        if (auto dotNode = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
            auto baseType = dotNode->baseExpr()->getType();
            TypeInfo actualType = baseType;
            if (baseType.isRef()) {
                if (auto e = baseType.refElementType()) actualType = *e;
            } else if (baseType.isRc()) {
                if (auto e = baseType.rcElementType()) actualType = *e;
            }
            auto scope = findNearestScope();
            auto* file = dynamic_cast<FileNode*>(scope);
            while (!file && scope) {
                scope = scope->parentScope();
                file = dynamic_cast<FileNode*>(scope);
            }
            if (file) {
                string methodFullName = actualType.name + "." + dotNode->member();
                auto methodSym = file->lookupFnSymbol(methodFullName);
                if (methodSym) {
                    auto rt = methodSym->retType;
                    // 结构体泛型实参替换：T→具体类型
                    if (actualType.isGeneric() && !actualType.genericArgs.empty()) {
                        // 结构体泛型实参替换：T → 具体类型。
                        // 优先用 structDecl 的 typeParams 做精确映射；structDecl 查不到时（内建
                        // 容器如 Array<T>/Rc<T>）回退到单参数约定 T。
                        StructDeclNode* structDecl = file->getStructDecl(actualType.name, /*includeBuiltin=*/true);
                        if (!structDecl) {
                            ScopeNode* p = file->parentScope();
                            while (p && !structDecl) {
                                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                                    structDecl = pf->getStructDecl(actualType.name, /*includeBuiltin=*/true);
                                }
                                p = p->parentScope();
                            }
                        }
                        std::map<std::string, TypeInfo> subst;
                        if (structDecl && structDecl->isGeneric() &&
                            structDecl->typeParams().size() == actualType.genericArgs.size()) {
                            for (size_t i = 0; i < actualType.genericArgs.size(); ++i) {
                                auto& a = actualType.genericArgs[i];
                                subst[structDecl->typeParams()[i]] = a ? *a : TypeInfo();
                            }
                        } else if (actualType.genericArgs.size() == 1) {
                            // 单参数泛型：类型参数名约定为 T
                            subst["T"] = actualType.genericArgs[0] ? *actualType.genericArgs[0] : TypeInfo();
                        }
                        if (!subst.empty()) rt = rt.substitute(subst);
                    }
                    DEBUG_LOG_VAL("ExprCallNode::getType - method returning", rt.getFullName());
                    return rt;
                }

                // 内建容器方法（Array<T>/Rc<T> 等），methodSym 未注册时直接按表推导返回类型，
                // 避免走下方 "fn() <ret>" 字符串编码丢失泛型结构（如 Ref<T> 编码后变成 Normal "Ref_T"）
                if (actualType.isArrayGeneric()) {
                    if (auto elem = actualType.arrayGenericElementType()) {
                        const auto& m = dotNode->member();
                        if (m == "get" || m == "first" || m == "last") {
                            return TypeInfo("Ref", {elem});
                        }
                        if (m == "pop") {
                            return *elem;
                        }
                    }
                    const auto& m = dotNode->member();
                    if (m == "len" || m == "cap") return TypeInfo("i64");
                    if (m == "is_empty") return TypeInfo("bool");
                    if (m == "push" || m == "clear" || m == "set_len") return {};
                }
            }
        }
        // 通过函数符号查 retType（保留 genericArgs，避免被字符串编码拍平）
        TypeInfo retType(type.name.substr(5));
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
            }
        }
        // 显式泛型调用 e<T>(...): 将 retType 按 typeParams → typeArgs 替换
        // 隐式（_typeArgs 为空）则尝试从实参推断（递归 unify Generic<T> ↔ Generic<U>）
        if (auto literalNode = dynamic_cast<ExprLiteralNode*>(_calleeExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                auto fnName = objLiteral->getValue().getText();
                p<Node> cur = _parent;
                p<FileNode> file = nullptr;
                while (cur) {
                    if (auto f = dynamic_cast<FileNode*>(cur)) {
                        file = f;
                        break;
                    }
                    cur = cur->parent();
                }
                p<FnNode> fnNode = nullptr;
                if (file) {
                    fnNode = file->getFunction(fnName);
                    // 用户文件查不到时回退 SDK（含 wildcardImports——SDK 平铺文件拆分后
                    // 泛型函数定义在子文件中，_sdkFile 空壳通过 wildcardImports 指向它们）
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
                if (fnNode && fnNode->header() && fnNode->header()->isGeneric()) {
                    auto typeParams = fnNode->header()->typeParams();
                    std::map<std::string, TypeInfo> subst;
                    if (!_typeArgs.empty() && typeParams.size() == _typeArgs.size()) {
                        for (size_t i = 0; i < _typeArgs.size(); ++i) {
                            subst[typeParams[i]] = _typeArgs[i]->getType();
                        }
                    } else if (_typeArgs.empty()) {
                        // 隐式推断：unify 形参类型与实参类型
                        std::function<void(const TypeInfo&, const TypeInfo&)> unify = [&](const TypeInfo& pT,
                                                                                          const TypeInfo& aT) {
                            if (pT.isNormal()) {
                                for (auto& tp : typeParams) {
                                    if (pT.name == tp) {
                                        subst[tp] = aT;
                                        return;
                                    }
                                }
                            }
                            if (pT.kind == TypeKind::Generic && aT.kind == TypeKind::Generic && pT.name == aT.name &&
                                pT.genericArgs.size() == aT.genericArgs.size()) {
                                for (size_t i = 0; i < pT.genericArgs.size(); ++i) {
                                    if (pT.genericArgs[i] && aT.genericArgs[i]) {
                                        unify(*pT.genericArgs[i], *aT.genericArgs[i]);
                                    }
                                }
                            }
                            // 形参为 Ref<X>、实参为非引用: 剥 Ref 继续
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
                            // 形参为 Nullable<X>、实参非 Nullable: 剥 Nullable 继续
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
                        for (size_t i = 0; i < params.size() && i < _args.size(); ++i) {
                            if (params[i]->type()) {
                                unify(params[i]->type()->getType(), _args[i]->getType());
                            }
                        }
                    }
                    if (!subst.empty()) {
                        retType = retType.substitute(subst);
                    }
                }
            }
        }
        DEBUG_LOG_VAL("ExprCallNode::getType - returning", retType.name);
        return retType;
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

const p<LiteralNode>& ExprLiteralNode::literal() const {
    return _literal;
}

TypeInfo ExprLiteralNode::getType() const {
    return _literal->getType();
}

ExprAddSubNode::Op ExprAddSubNode::op() const {
    return _op;
}

const p<ExprNode>& ExprAddSubNode::left() const {
    return _left;
}

const p<ExprNode>& ExprAddSubNode::right() const {
    return _right;
}

TypeInfo ExprAddSubNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
    // v0.6 Phase 2c：`+` 任一操作数为 String 时整链结果即 String，
    // codegen 期 lower 为 StringBuilder 累加（详见 spec §4.4.1.4 / §4.3.1.7）。
    // 仅 Add 适用；Sub 仍按原算术规则。
    if (_op == Op::Add && (leftType.name == "String" || rightType.name == "String")) {
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

const p<ExprNode>& ExprMulDivModNode::left() const {
    return _left;
}

const p<ExprNode>& ExprMulDivModNode::right() const {
    return _right;
}

TypeInfo ExprMulDivModNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
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

const p<ExprNode>& ExprBinOpNode::left() const {
    return _left;
}

const p<ExprNode>& ExprBinOpNode::right() const {
    return _right;
}

TypeInfo ExprBinOpNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
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

const p<ExprNode>& ExprParenNode::expr() const {
    return _inner;
}

TypeInfo ExprParenNode::getType() const {
    return _inner->getType();
}

const p<ExprNode>& ExprDotNode::baseExpr() const {
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

TypeInfo ExprDotNode::getType() const {
    auto member = this->member();
    DEBUG_LOG_VAL("ExprDotNode::getType - member", member);

    // 安全访问 a?.b：base 必须是 Nullable<T>
    // 类型规则：解出 T，从 T 的字段查 b 的类型 U，结果为 Nullable<U>
    // 不支持方法调用形式（要求 exprCall 同时知道 safe，目前只解到字段）
    if (_safe) {
        auto baseT = _baseExpr->getType();
        if (!baseT.isNullable()) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3024, baseT.name);
        }
        auto innerType = baseT.nullableInnerType();
        if (!innerType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
        }
        // Phase 5: Rc<T>? 自动 deref —— 把 Rc<U> 视为 U 进字段查
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
        auto sd = file->getStructDecl(innerType->name);
        if (!sd) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3044, innerType->name);
        }

        int idx = sd->fieldIndex(member);
        if (idx < 0) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3040, innerType->name, member);
        }
        auto fieldType = sd->fields()[idx]->getType();
        // 泛型实参替换 T → 实际类型
        if (innerType->isGeneric() && sd->isGeneric() && innerType->genericArgs.size() == sd->typeParams().size()) {
            std::map<std::string, TypeInfo> subst;
            for (size_t i = 0; i < sd->typeParams().size(); ++i) {
                subst[sd->typeParams()[i]] = innerType->genericArgs[i] ? *innerType->genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        // 包装为 Nullable<U>
        std::vector<sp<TypeInfo>> args;
        args.push_back(make_shared<TypeInfo>(fieldType));
        return {"Nullable", args};
    }

    // DRAFT-spec-reflect §6: Field.value → compile-time field name rewrite.
    // When .value is accessed on a Field-typed expression, attempt to statically resolve
    // which concrete struct field is referenced.
    // Resolution is done via AST pattern matching first (independent of type resolution,
    // which may fail due to scope chain issues in nested contexts),
    // then falls back to type-based check for non-compile-time-known Field references.
    if (member == "value") {
        // Helper: trace an expression to {structDecl, fieldIndex}.
        std::function<std::pair<const StructDeclNode*, int>(const p<ExprNode>&)> tryResolve;
        tryResolve = [&](const p<ExprNode>& expr) -> std::pair<const StructDeclNode*, int> {
            // Case 1: Direct .get(N) call on a static fields array
            if (auto* call = dynamic_cast<ExprCallNode*>(expr)) {
                auto& callee = call->getCalleeExpr();
                auto* dot = dynamic_cast<ExprDotNode*>(callee);
                if (dot && dot->member() == "get" && call->getArgs().size() == 1) {
                    auto* base = dot->baseExpr();
                    auto* path = dynamic_cast<ExprPathCallNode*>(base);
                    if (path && path->variantName().getText() == "fields") {
                        std::string sn = path->enumName().getText();
                        // Resolve "Self" by walking up to enclosing StructDeclNode
                        if (sn == "Self") {
                            auto* s = expr->findNearestScope();
                            while (s) {
                                if (auto* sd = dynamic_cast<StructDeclNode*>(s)) {
                                    sn = sd->name().getText();
                                    break;
                                }
                                s = s->parentScope();
                            }
                            if (sn == "Self") return {nullptr, -1};
                        }
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
                        std::string sn = path->enumName().getText();
                        if (sn == "Self") {
                            auto* s = expr->findNearestScope();
                            while (s) {
                                if (auto* sd2 = dynamic_cast<StructDeclNode*>(s)) {
                                    sn = sd2->name().getText();
                                    break;
                                }
                                s = s->parentScope();
                            }
                            if (sn == "Self") return {nullptr, -1};
                        }
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
                            if (i) childKey += ".";
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
    // 透明别名仅做一层手工解析（只处理顶层 Normal alias 名，泛型 alias 留给 Compiler::applySubst 在 codegen 阶段兜底）
    if (!member.empty() && std::ranges::all_of(member, [](char c) { return c >= '0' && c <= '9'; })) {
        TypeInfo resolved = baseType;
        if (resolved.kind == TypeKind::Normal) {
            auto scope = findNearestScope();
            auto* file = dynamic_cast<FileNode*>(scope);
            while (!file && scope) {
                scope = scope->parentScope();
                file = dynamic_cast<FileNode*>(scope);
            }
            if (file) {
                std::set<std::string> visited;
                auto cur = resolved;
                while (cur.kind == TypeKind::Normal) {
                    auto* alias = file->getAliasDecl(cur.name);
                    if (!alias || alias->isGeneric() || !alias->target()) break;
                    if (visited.count(cur.name)) break;
                    visited.insert(cur.name);
                    cur = alias->target()->getType();
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

    if (actualType.isRc()) {
        auto rcElemType = actualType.rcElementType();
        if (rcElemType) {
            actualType = *rcElemType;
        }
    }

    // Dyn<D> / Dyn<D&> 的 `.m`：用 draft 签名表回填返回类型，包装成 `fn() <ret>`
    // 让 ExprCallNode 在静态阶段算出确切类型（与 §12.4 draft 边界查找走同形分支）
    // v0.16: [] 返回 T&——baseType 可能是 Dyn<Shape>&，用剥 Ref 后的 actualType 做 Dyn 判断
    if (actualType.isDyn()) {
        auto rt = lookupDynMethodRetType(parent(), actualType, member);
        if (!rt.empty()) {
            return TypeInfo("fn() " + rt.getFullName());
        }
        return TypeInfo("fn() void");
    }

    if (isBuiltinType(actualType.name)) {
        if (member.starts_with("to_")) {
            string dstType = member.substr(3);
            if (isBuiltinType(dstType)) {
                DEBUG_LOG_VAL("ExprDotNode::getType - returning fn() for builtin cast", dstType);
                return TypeInfo("fn() " + dstType);
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
                    DEBUG_LOG_VAL("ExprDotNode::getType - found SDK method for builtin type, returning fn()",
                                  methodSym->retType.name);
                    return TypeInfo("fn() " + methodSym->retType.name);
                }
            }
        }

        return _baseExpr->getType();
    }

    // Array<T> 方法调用（[T * N] 是 Array<T> 的语法糖）。
    // Array fnSymbol 未显式注册（纯 codegen），此处按内建方法表直接构造返回类型。
    // ExprCallNode::getType() 走 "fn() <ret>" 路径需要此处返回完整方法签名。
    auto tryArrayMethodRetType = [&](const TypeInfo& elemTy) -> std::string {
        if (member == "get" || member == "first" || member == "last") {
            // 返回 T&（Ref<T>）
            TypeInfo refTy("Ref", {std::make_shared<TypeInfo>(elemTy)});
            return "fn() " + refTy.getFullName();
        }
        if (member == "pop") {
            // 返回 T（owned）
            return "fn() " + elemTy.getFullName();
        }
        if (member == "len" || member == "cap") {
            return "fn() usize";
        }
        if (member == "is_empty") {
            return "fn() bool";
        }
        if (member == "push") {
            return "fn() void";
        }
        return ""; // 未知方法
    };

    if (actualType.isArray()) {
        if (auto elemType = actualType.elementType) {
            auto retSig = tryArrayMethodRetType(*elemType);
            if (!retSig.empty()) return TypeInfo(retSig);
        }
        return _baseExpr->getType();
    }

    if (actualType.isArrayGeneric()) {
        if (auto elemType = actualType.arrayGenericElementType()) {
            auto retSig = tryArrayMethodRetType(*elemType);
            if (!retSig.empty()) return TypeInfo(retSig);
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
            auto structDecl = file->getStructDecl(actualType.name, /*includeBuiltin=*/true);
            if (!structDecl) {
                ScopeNode* p = file->parentScope();
                while (p && !structDecl) {
                    if (auto pf = dynamic_cast<FileNode*>(p)) {
                        structDecl = pf->getStructDecl(actualType.name, /*includeBuiltin=*/true);
                    }
                    p = p->parentScope();
                }
            }

            // 若 actualType 是泛型实例，构造 T→具体 的替换表
            map<string, TypeInfo> genSubst;
            if (actualType.isGeneric() && structDecl && structDecl->isGeneric() &&
                structDecl->typeParams().size() == actualType.genericArgs.size()) {
                for (size_t i = 0; i < actualType.genericArgs.size(); ++i) {
                    auto& a = actualType.genericArgs[i];
                    genSubst[structDecl->typeParams()[i]] = a ? *a : TypeInfo();
                }
            }

            if (structDecl) {
                auto field = structDecl->field(member);
                if (field) {
                    auto ft = field->getType();
                    if (!genSubst.empty()) ft = ft.substitute(genSubst);
                    return ft;
                }
            }

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
                auto rt = methodSym->retType;
                if (!genSubst.empty()) rt = rt.substitute(genSubst);
                DEBUG_LOG_VAL("ExprDotNode::getType - found method, returning fn()", rt.name);
                return TypeInfo("fn() " + rt.getFullName());
            }
        }
    }

    // §12.4：actualType 为外层泛型形参 T，且 T 有 draft 边界声明 `<T : D>`，
    // 在 D 的签名表里找 member 的返回类型；命中即返回 fn() ret，
    // 让 ExprCallNode 在静态阶段算出确切类型，避免下游函数重载查找拿到 T 而失败。
    if (actualType.kind == TypeKind::Normal && !actualType.isGeneric()) {
        auto rt = lookupSpecBoundMethodRetType(parent(), actualType.name, member);
        if (!rt.empty()) {
            return TypeInfo("fn() " + rt.getFullName());
        }
    }

    return _baseExpr->getType();
}

ExprCompareNode::Op ExprCompareNode::op() const {
    return _op;
}

const p<ExprNode>& ExprCompareNode::left() const {
    return _left;
}

const p<ExprNode>& ExprCompareNode::right() const {
    return _right;
}

TypeInfo ExprCompareNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
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

StatementBlockNode::StatementBlockNode(const p<Node>& parent, vector<p<StatementNode>> statements,
                                       p<ExprNode> resultExpr, bool hasResult)
    : ScopeNode(parent), _statements(std::move(statements)), _resultExpr(resultExpr), _hasResult(hasResult) {}

const vector<p<StatementNode>>& StatementBlockNode::statements() const {
    return _statements;
}

const p<ExprNode>& StatementBlockNode::resultExpr() const {
    return _resultExpr;
}

bool StatementBlockNode::hasResult() const {
    return _hasResult;
}

const p<ExprNode>& ExprElIfNode::condition() const {
    return _condition;
}

const p<StatementBlockNode>& ExprElIfNode::block() const {
    return _block;
}

const p<ExprNode>& ExprIfElseNode::condition() const {
    return _condition;
}

const p<StatementBlockNode>& ExprIfElseNode::thenBlock() const {
    return _thenBlock;
}

const vector<p<ExprElIfNode>>& ExprIfElseNode::elifs() const {
    return _elifs;
}

const p<StatementBlockNode>& ExprIfElseNode::elseBlock() const {
    return _elseBlock;
}

TypeInfo ExprIfElseNode::getType() const {
    if (!_thenBlock->hasResult()) {
        return {};
    }
    TypeInfo resultType = _thenBlock->resultExpr()->getType();

    for (auto& elif : _elifs) {
        if (!elif->block()->hasResult()) {
            return {};
        }
        auto elifType = elif->block()->resultExpr()->getType();
        if (elifType != resultType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, resultType.name, elifType.name);
        }
    }

    if (_elseBlock && _elseBlock->hasResult()) {
        auto elseType = _elseBlock->resultExpr()->getType();
        if (elseType != resultType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, resultType.name, elseType.name);
        }
    } else if (!_elseBlock || !_elseBlock->hasResult()) {
        return {};
    }

    return resultType;
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
    auto trueType = _trueValue->getType();
    auto falseType = _falseValue->getType();
    if (trueType != falseType) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, trueType.name, falseType.name);
    }
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

TypeInfo ExprIfElsePreValueNode::getType() const {
    auto trueType = _trueValue->getType();
    auto falseType = _falseValue->getType();
    if (trueType != falseType) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, trueType.name, falseType.name);
    }
    return trueType;
}

int ExprIfElsePreValueNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _condition->resolveLineNumber();
}

int ExprIfElsePreValueNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _condition->resolveColumn();
}

const p<ExprNode>& ExprGetNode::arrayExpr() const {
    return _arrayExpr;
}

const vector<p<ExprNode>>& ExprGetNode::indices() const {
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

const vector<p<ExprNode>>& ExprArrayNode::elements() const {
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
// 形参类型缺失（待上下文反推）→ 槽位放 empty TypeInfo 占位
// 返回类型：显式标注用之；否则 nullptr 表示"待 §4.2 / 上下文决定"
// Phase 2b：调用点 / 赋值点反推后 _inferredFnType 持完整类型，优先返回
TypeInfo LambdaExprNode::getType() const {
    if (_inferredFnType.isFn()) return _inferredFnType;
    vector<sp<TypeInfo>> ps;
    ps.reserve(_params.size());
    for (auto& slot : _params) {
        if (slot.type)
            ps.push_back(make_shared<TypeInfo>(slot.type->getType()));
        else
            ps.push_back(make_shared<TypeInfo>()); // 占位，等 Phase 2b 反推回填
    }
    sp<TypeInfo> rt = nullptr;
    if (_retType) rt = make_shared<TypeInfo>(_retType->getType());
    // nullable=false：lambda 字面量本身永非空（fn?(...)R 是类型层 nullable，与字面量值无关）
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

const p<LiteralNode>& ExprArrayInitNode::value() const {
    return _value;
}

const p<TypeNode>& ExprArrayInitNode::explicitType() const {
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
        TypeInfo lookupType = baseType;
        if (lookupType.isRc()) {
            if (auto inner = lookupType.rcElementType()) lookupType = *inner;
        }

        auto structDecl = file->getStructDecl(lookupType.name);
        if (!structDecl) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3041, lookupType.name);
        }

        auto field = structDecl->field(sub.getText());
        if (!field) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3040, lookupType.name, sub.getText());
        }

        TypeInfo fieldType = field->getType();
        // 泛型实例：按 typeParam→arg 替换字段类型
        if (lookupType.isGeneric() && structDecl->isGeneric() &&
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

const p<ExprNode>& ExprUnaryNode::right() const {
    return _right;
}

TypeInfo ExprUnaryNode::getType() const {
    auto rightType = _right->getType();
    // Phase 3.4.h: 内置类型的一元 op 形态校验 (与 compileUnaryExpr 内置分支同义).
    // 非 builtin 走自定义方法路径 (customMethodOp), 不在此校验; 与 codegen `if
    // (!isBuiltinType(rightType.name)) compileCustomTypeUnaryOp(...)` 顺序一致.
    if (isBuiltinType(rightType.name)) {
        if (_op == Op::Rev && rightType.startsWith('f')) {
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
    // 左侧若为 Nullable<T>，结果为 T
    if (leftType.kind == TypeKind::Generic && leftType.name == "Nullable" && leftType.genericArgs.size() == 1) {
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
TypeInfo ExprMatchNode::getType() const {
    TypeInfo first;
    bool firstSet = false;
    for (auto& arm : _arms) {
        auto t = arm->body()->getType();
        if (!firstSet) {
            first = t;
            firstSet = true;
            continue;
        }
        if (t != first) {
            // 不在 getType 抛错，留给编译期更稳：返回首个，编译期再校验
            return first;
        }
    }
    return first;
}

// try-catch 表达式：取 try block 末表达式 + 所有 catch arm body 末表达式的共同类型
// 流终止 arm（body 末以 ret / panic 结尾，hasResult=false）不参与类型合并；
// 与 if-else / match 同档：若任何参与方为 void 则整体 void，类型不一致返回首个，
// 编译期再校验（保持与 ExprMatchNode::getType 一致风格）。
TypeInfo ExprTryCatchNode::getType() const {
    if (!_tryBlock->hasResult()) {
        return {};
    }
    TypeInfo first = _tryBlock->resultExpr()->getType();
    for (auto& arm : _catches) {
        // body 无 result（以 ret / panic 终结）→ 流终止 arm，跳过类型合并
        if (!arm->body()->hasResult()) continue;
        auto t = arm->body()->resultExpr()->getType();
        if (t != first) return first;
    }
    return first;
}

// Dyn<D>(x) / Dyn<D&>(x) 的整体类型 = `Dyn<D>` 或 `Dyn<D&>`。
// 内层 TypeNode 已携带借用形态（Ref<D>），这里直接包一层 `Dyn` 即可。
TypeInfo ExprDynCtorNode::getType() const {
    if (!_specType) return {};
    auto inner = make_shared<TypeInfo>(_specType->getType());
    return TypeInfo("Dyn", {inner});
}

// Heap:<T>(arg) 的整体类型 = Heap<T>
TypeInfo ExprHeapCtorNode::getType() const {
    if (!_innerType) return {};
    auto inner = make_shared<TypeInfo>(_innerType->getType());
    return TypeInfo("Heap", {inner});
}

// Phase 3b: 类型 = 所属结构体, structName 由 ast_builder 扫 _scopeStack 时填入
TypeInfo ExprStructLitNode::getType() const {
    return _structName.empty() ? TypeInfo() : TypeInfo(_structName);
}

TypeInfo ExprPathCallNode::getType() const {
    string n = _enumName.getText();
    auto* scope = parent() ? parent()->findNearestScope() : nullptr;
    FileNode* file = nullptr;
    while (scope) {
        file = dynamic_cast<FileNode*>(scope);
        if (file) break;
        scope = scope->parentScope();
    }
    if (file) {
        std::set<std::string> visited;
        while (true) {
            if (visited.count(n)) break;
            visited.insert(n);
            auto* a = file->getAliasDecl(n);
            if (!a || !a->target()) break;
            auto t = a->target()->getType();
            if (t.kind != TypeKind::Normal) break;
            n = t.name;
        }

        // DRAFT-static-vars Phase 4: 若零参且 RHS 是 struct 静态字段，返回字段类型
        if (_args.empty()) {
            auto* structDecl = file->getStructDecl(n);
            if (structDecl) {
                if (auto* sf = structDecl->staticField(_variantName.getText())) {
                    return sf->type->getType();
                }
            }
        }
    }

    // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` 走 reflect
    // 静态路径; getType 返回对应 SDK 类型供下游 (assignment / call) 推断.
    // LHS 必须是 struct (有 StructDecl), 才能区分于 enum::variant.
    string rhs = _variantName.getText();
    if (_args.empty() && (rhs == "type" || rhs == "fields" || rhs == "methods" || rhs == "variants")) {
        auto findStruct = [&](const string& sn) -> StructDeclNode* {
            if (!file) return nullptr;
            if (auto* sd = file->getStructDecl(sn)) return sd;
            ScopeNode* p = file->parentScope();
            while (p) {
                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                    if (auto* sd = pf->getStructDecl(sn)) return sd;
                }
                p = p->parentScope();
            }
            return nullptr;
        };
        if (auto* sd = findStruct(n)) {
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

    return TypeInfo(n);
}
