// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "expr_node.h"

#include <algorithm>
#include <set>

#include "fn_node.h"
#include "file_node.h"
#include "draft_node.h"
#include "analyzer/symbol_suggest.h"

// §12.4：在生成式 AST 中遇到 `x.m()`（x:T 为泛型形参）时，
// 用形参声明位的 draft 边界查 m 的返回类型；走包含 SDK 回退的 file 链。
static TypeInfo lookupDraftBoundMethodRetType(
    Node* contextParent, const string& typeParamName, const string& methodName) {
    Node* cur = contextParent;
    p<FnHeaderNode> header = nullptr;
    while (cur) {
        if (auto fn = dynamic_cast<FnNode*>(cur)) {
            header = fn->header();
            break;
        }
        cur = cur->parent();
    }
    if (!header || !header->isGeneric()) return TypeInfo();
    const auto& tps = header->typeParams();
    const auto& bounds = header->typeParamBounds();
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < tps.size(); ++i) {
        if (tps[i] == typeParamName) { idx = i; break; }
    }
    if (idx == SIZE_MAX || idx >= bounds.size()) return TypeInfo();

    auto scope = contextParent ? contextParent->findNearestScope() : nullptr;
    FileNode* file = dynamic_cast<FileNode*>(scope);
    while (!file && scope) {
        scope = scope->parentScope();
        file = dynamic_cast<FileNode*>(scope);
    }
    if (!file) return TypeInfo();

    for (auto& dname : bounds[idx]) {
        DraftDeclNode* draft = file->getDraftDecl(dname);
        if (!draft) {
            ScopeNode* p = file->parentScope();
            while (p && !draft) {
                if (auto pf = dynamic_cast<FileNode*>(p)) {
                    draft = pf->getDraftDecl(dname);
                }
                p = p->parentScope();
            }
        }
        if (!draft) continue;
        for (auto& sig : draft->signatures()) {
            if (sig->name().getText() != methodName) continue;
            if (sig->retType()) return sig->retType()->getType();
            return TypeInfo();
        }
    }
    return TypeInfo();
}

static bool isCompilerInnerMethod(ScopeNode* scope, const string& structName, const string& methodName) {
    if (!scope) return false;
    
    FileNode* file = dynamic_cast<FileNode*>(scope);
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
            return method->header()->hasAnno("CompilerInner");
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

const p<ExprNode>& ExprCallNode::getCalleeExpr() const { return _calleeExpr; }

const std::vector<p<ExprNode>>& ExprCallNode::getArgs() const { return _args; }

TypeInfo ExprCallNode::getType() const {
    auto type = _calleeExpr->getType();

    // Phase 2b: callee 自身就是 Fn 类型值（lambda 字面量 / fn-typed 变量 / 字段）
    // 调用结果即 fn 返回类型；void 时返回空 TypeInfo
    if (type.isFn()) {
        if (auto rt = type.fnReturnType()) return *rt;
        return TypeInfo();
    }

    // Phase 3c: callee 为 Box<fn(...)R>，自动解引取 fat-ptr 调用，结果同 fn 返回类型
    if (type.isBox()) {
        if (auto inner = type.boxElementType(); inner && inner->isFn()) {
            if (auto rt = inner->fnReturnType()) return *rt;
            return TypeInfo();
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
                FileNode* file = dynamic_cast<FileNode*>(scope);
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
                            for (auto& arg : _args) argTypes.push_back(arg->getType());
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
                    FileNode* file = dynamic_cast<FileNode*>(scope);
                    while (!file && scope) {
                        scope = scope->parentScope();
                        file = dynamic_cast<FileNode*>(scope);
                    }
                    if (file) {
                        if (auto* target = file->moduleAlias(aliasName)) {
                            vector<TypeInfo> argTypes;
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
        return TypeInfo();
    }

    // 方法调用（callee 为 ExprDotNode）：直接查方法符号，拿到完整 retType（含泛型 typeArgs）。
    // 避免走 "fn() <name>" 字符串编码丢失泛型信息（例如 Array<u8> 被编码为 Array_u8）。
    if (type.name.starts_with("fn() ")) {
        if (auto dotNode = dynamic_cast<ExprDotNode*>(_calleeExpr)) {
            auto baseType = dotNode->baseExpr()->getType();
            TypeInfo actualType = baseType;
            if (baseType.isRef()) {
                if (auto e = baseType.refElementType()) actualType = *e;
            } else if (baseType.isBox()) {
                if (auto e = baseType.boxElementType()) actualType = *e;
            }
            auto scope = findNearestScope();
            FileNode* file = dynamic_cast<FileNode*>(scope);
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
                    if (actualType.isGeneric()) {
                        // 用户 file 找不到时（例如 Nullable/Array 等 SDK 泛型），
                        // 沿父作用域回退到 SDK file
                        StructDeclNode* structDecl = file->getStructDecl(actualType.name);
                        if (!structDecl) {
                            ScopeNode* p = file->parentScope();
                            while (p && !structDecl) {
                                if (auto* pf = dynamic_cast<FileNode*>(p)) {
                                    structDecl = pf->getStructDecl(actualType.name);
                                }
                                p = p->parentScope();
                            }
                        }
                        if (structDecl && structDecl->isGeneric()
                            && structDecl->typeParams().size() == actualType.genericArgs.size()) {
                            std::map<std::string, TypeInfo> subst;
                            for (size_t i = 0; i < actualType.genericArgs.size(); ++i) {
                                auto& a = actualType.genericArgs[i];
                                subst[structDecl->typeParams()[i]] = a ? *a : TypeInfo();
                            }
                            rt = rt.substitute(subst);
                        }
                    }
                    DEBUG_LOG_VAL("ExprCallNode::getType - method returning", rt.getFullName());
                    return rt;
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
                    for (auto& arg : _args) argTypes.push_back(arg->getType());
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
                    if (auto f = dynamic_cast<FileNode*>(cur)) { file = f; break; }
                    cur = cur->parent();
                }
                p<FnNode> fnNode = nullptr;
                if (file) {
                    fnNode = file->getFunction(fnName);
                    // 用户文件查不到时回退 SDK
                    if (!fnNode) {
                        ScopeNode* p = file->parentScope();
                        while (p && !fnNode) {
                            if (auto* pf = dynamic_cast<FileNode*>(p)) {
                                fnNode = pf->getFunction(fnName);
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
                        std::function<void(const TypeInfo&, const TypeInfo&)> unify =
                            [&](const TypeInfo& pT, const TypeInfo& aT) {
                                if (pT.isNormal()) {
                                    for (auto& tp : typeParams) {
                                        if (pT.name == tp) { subst[tp] = aT; return; }
                                    }
                                }
                                if (pT.kind == TypeKind::Generic && aT.kind == TypeKind::Generic
                                    && pT.name == aT.name
                                    && pT.genericArgs.size() == aT.genericArgs.size()) {
                                    for (size_t i = 0; i < pT.genericArgs.size(); ++i) {
                                        if (pT.genericArgs[i] && aT.genericArgs[i]) {
                                            unify(*pT.genericArgs[i], *aT.genericArgs[i]);
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
                    return TypeInfo(type.name, genericArgs);
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3001, leftType.name, rightType.name);
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3002, leftType.name, rightType.name);
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3003, leftType.name, rightType.name);
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
    std::reverse(segments.begin(), segments.end());
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
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3025, baseT.name);
        }
        auto innerType = baseT.nullableInnerType();
        if (!innerType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3051);
        }
        // Phase 5: Box<T>? 自动 deref —— 把 Box<U> 视为 U 进字段查
        if (innerType->isBox()) {
            auto boxInner = innerType->boxElementType();
            if (!boxInner) {
                throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3050);
            }
            innerType = boxInner;
        }
        auto scope = findNearestScope();
        FileNode* file = dynamic_cast<FileNode*>(scope);
        while (!file && scope) {
            scope = scope->parentScope();
            file = dynamic_cast<FileNode*>(scope);
        }
        if (!file) {
            return TypeInfo();
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
        if (innerType->isGeneric() && sd->isGeneric()
            && innerType->genericArgs.size() == sd->typeParams().size()) {
            std::map<std::string, TypeInfo> subst;
            for (size_t i = 0; i < sd->typeParams().size(); ++i) {
                subst[sd->typeParams()[i]] =
                    innerType->genericArgs[i] ? *innerType->genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        // 包装为 Nullable<U>
        std::vector<sp<TypeInfo>> args;
        args.push_back(make_shared<TypeInfo>(fieldType));
        return TypeInfo("Nullable", args);
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
                    FileNode* f = dynamic_cast<FileNode*>(scope);
                    auto s = scope;
                    while (!f && s) { s = s->parentScope(); f = dynamic_cast<FileNode*>(s); }
                    if (f && f->isAmbiguousAlias(aliasName)) {
                        f->throwAmbiguousAlias(aliasName, resolveLineNumber());
                    }
                }
                if (sym && sym->kind == SymbolKind::Module && segs.size() == 1) {
                    return TypeInfo("fn_overload");
                }
                if (sym && sym->kind == SymbolKind::Package) {
                    FileNode* file = dynamic_cast<FileNode*>(scope);
                    auto s = scope;
                    while (!file && s) { s = s->parentScope(); file = dynamic_cast<FileNode*>(s); }
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
    if (!member.empty() && std::all_of(member.begin(), member.end(),
                                       [](char c) { return c >= '0' && c <= '9'; })) {
        TypeInfo resolved = baseType;
        if (resolved.kind == TypeKind::Normal) {
            auto scope = findNearestScope();
            FileNode* file = dynamic_cast<FileNode*>(scope);
            while (!file && scope) { scope = scope->parentScope(); file = dynamic_cast<FileNode*>(scope); }
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
            size_t idx = static_cast<size_t>(std::stoul(member));
            auto& elems = resolved.tupleElements();
            if (idx >= elems.size()) {
                throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3100,
                               member, baseType.getFullName(),
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
    
    if (baseType.isBox()) {
        auto boxElemType = baseType.boxElementType();
        if (boxElemType) {
            actualType = *boxElemType;
        }
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
                    DEBUG_LOG_VAL("ExprDotNode::getType - found SDK method for builtin type, returning fn()", methodSym->retType.name);
                    return TypeInfo("fn() " + methodSym->retType.name);
                }
            }
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
            auto structDecl = file->getStructDecl(actualType.name);

            // 若 actualType 是泛型实例，构造 T→具体 的替换表
            map<string, TypeInfo> genSubst;
            if (actualType.isGeneric() && structDecl && structDecl->isGeneric()
                && structDecl->typeParams().size() == actualType.genericArgs.size()) {
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
        auto rt = lookupDraftBoundMethodRetType(parent(), actualType.name, member);
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3004, leftType.name, rightType.name);
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

StatementBlockNode::StatementBlockNode(const p<Node>& parent, vector<p<StatementNode>> statements, p<ExprNode> resultExpr, bool hasResult) :
    ScopeNode(parent),
    _statements(std::move(statements)),
    _resultExpr(resultExpr),
    _hasResult(hasResult) {
}

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
        return TypeInfo();
    }
    TypeInfo resultType = _thenBlock->resultExpr()->getType();

    for (auto& elif : _elifs) {
        if (!elif->block()->hasResult()) {
            return TypeInfo();
        }
        auto elifType = elif->block()->resultExpr()->getType();
        if (elifType != resultType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3005, resultType.name, elifType.name);
        }
    }

    if (_elseBlock && _elseBlock->hasResult()) {
        auto elseType = _elseBlock->resultExpr()->getType();
        if (elseType != resultType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3006, resultType.name, elseType.name);
        }
    } else if (!_elseBlock || !_elseBlock->hasResult()) {
        return TypeInfo();
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3007, trueType.name, falseType.name);
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
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3008, trueType.name, falseType.name);
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
    
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3057);
        }
        return *elemType;
    }

    if (!arrayType.isArray()) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3062, arrayType.name);
    }

    if (!arrayType.elementType) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3057);
    }

    return *arrayType.elementType;
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
        return TypeInfo(make_shared<TypeInfo>("__empty"), 0);
    }

    TypeInfo elementType = _elements[0]->getType();
    for (size_t i = 1; i < _elements.size(); ++i) {
        auto elemType = _elements[i]->getType();
        if (elemType != elementType) {
            throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3011, elementType.name, elemType.name);
        }
    }

    auto elemShared = make_shared<TypeInfo>(elementType);
    return TypeInfo(elemShared, _elements.size());
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
        if (slot.type) ps.push_back(make_shared<TypeInfo>(slot.type->getType()));
        else ps.push_back(make_shared<TypeInfo>());  // 占位，等 Phase 2b 反推回填
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
    } else {
        elementType = _value->getType();
    }
    
    auto elemShared = make_shared<TypeInfo>(elementType);
    return TypeInfo(elemShared, 0);
}

TypeInfo ExprGetRefNode::getType() const {
    auto scope = findNearestScope();
    if (!scope) {
        throw YuxError(resolveLineNumber(), resolveColumn(), ErrorCode::E3097);
    }
    
    auto sym = scope->lookupSymbol(_obj.getText());
    if (!sym) {
        SymbolSuggest::throwSymbolNotFound(scope,
            resolveLineNumber(), resolveColumn(), ErrorCode::E3030, _obj.getText());
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

        // Phase 4c: Box<T>.field 自动解引用到 payload 上找字段（&box.field → field&）
        TypeInfo lookupType = baseType;
        if (lookupType.isBox()) {
            if (auto inner = lookupType.boxElementType()) lookupType = *inner;
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
        if (lookupType.isGeneric() && structDecl->isGeneric()
            && lookupType.genericArgs.size() == structDecl->typeParams().size()) {
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
    return TypeInfo("Ref", genericArgs);
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
    return _right->getType();
}

int ExprUnaryNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _right->resolveLineNumber();
}

int ExprUnaryNode::resolveColumn() const {
    if (_line > 0) return _col;
    return _right->resolveColumn();
}

// ExprNullElseNode: a ?? b
// 类型规则：a 必须是 Nullable<T>，结果类型为 T；b 必须能转为 T
TypeInfo ExprNullElseNode::getType() const {
    auto leftType = _left->getType();
    // 左侧若为 Nullable<T>，结果为 T
    if (leftType.kind == TypeKind::Generic && leftType.name == "Nullable"
        && leftType.genericArgs.size() == 1) {
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
        return TypeInfo();
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
    if (!_draftType) return TypeInfo();
    auto inner = make_shared<TypeInfo>(_draftType->getType());
    return TypeInfo("Dyn", {inner});
}

TypeInfo ExprEnumCtorNode::getType() const {
    string n = _enumName.getText();
    auto* scope = parent() ? parent()->findNearestScope() : nullptr;
    FileNode* file = nullptr;
    while (scope) {
        if ((file = dynamic_cast<FileNode*>(scope))) break;
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
    }
    return TypeInfo(n);
}
