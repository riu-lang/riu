// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

//
// Created by yjl_1 on 2026/4/4.
//

#include "expr_node.h"
#include "fn_node.h"
#include "file_node.h"

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
                        auto structDecl = file->getStructDecl(actualType.name);
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
        TypeInfo retType(type.name.substr(5));
        // 显式泛型调用 e<T>(...): 将 retType 按 typeParams → typeArgs 替换
        if (!_typeArgs.empty()) {
            if (auto literalNode = dynamic_cast<ExprLiteralNode*>(_calleeExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                    auto fnName = objLiteral->getValue().getText();
                    p<Node> cur = _parent;
                    p<FileNode> file = nullptr;
                    while (cur) {
                        if (auto f = dynamic_cast<FileNode*>(cur)) { file = f; break; }
                        cur = cur->parent();
                    }
                    if (file) {
                        auto fnNode = file->getFunction(fnName);
                        if (fnNode && fnNode->header() && fnNode->header()->isGeneric()
                            && fnNode->header()->typeParams().size() == _typeArgs.size()) {
                            std::map<std::string, TypeInfo> subst;
                            for (size_t i = 0; i < _typeArgs.size(); ++i) {
                                subst[fnNode->header()->typeParams()[i]] = _typeArgs[i]->getType();
                            }
                            retType = retType.substitute(subst);
                        }
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
        if (sym && sym->kind != SymbolKind::Function) {
            throw YuxError(resolveLineNumber(), "Type {} is not a Function", sym->name);
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
    if (leftType != rightType) {
        if (isFlexibleIntExpr(_right) && tryInferIntType(_right, leftType)) {
            return leftType;
        }
        if (isFlexibleIntExpr(_left) && tryInferIntType(_left, rightType)) {
            return rightType;
        }
        throw YuxError(resolveLineNumber(), "Type mismatch in +-/ operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
}

int ExprAddSubNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
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
        throw YuxError(resolveLineNumber(), "Type mismatch in */% operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
}

int ExprMulDivModNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
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
        throw YuxError(resolveLineNumber(), "Type mismatch in &|^ operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
}

int ExprBinOpNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
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
    return _member.getText();
}

int ExprDotNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int memberLine = static_cast<int>(_member.getLine());
    if (memberLine > 0) return memberLine;
    if (_baseExpr) return _baseExpr->resolveLineNumber();
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
    auto member = _member.getText();
    DEBUG_LOG_VAL("ExprDotNode::getType - member", member);

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
    
    if (baseType.isPtr()) {
        if (member == "_value") {
            return TypeInfo("u64");
        }
    }
    
    if (baseType.isArrayGeneric()) {
        if (member == "_len" || member == "_cap") {
            return TypeInfo("fn() i64");
        }
        if (member == "_push" || member == "_clear" || member == "_set_len") {
            return TypeInfo("fn() ");
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
        throw YuxError(resolveLineNumber(), "Type mismatch in comparison: left is {}, right is {}", leftType.name, rightType.name);
    }
    return TypeInfo("bool");
}

int ExprCompareNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    int leftLine = _left->resolveLineNumber();
    if (leftLine > 0) return leftLine;
    return _right->resolveLineNumber();
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
            throw YuxError(resolveLineNumber(), "Type mismatch in if-elif branches: {} vs {}", resultType.name, elifType.name);
        }
    }

    if (_elseBlock && _elseBlock->hasResult()) {
        auto elseType = _elseBlock->resultExpr()->getType();
        if (elseType != resultType) {
            throw YuxError(resolveLineNumber(), "Type mismatch in if-else branches: {} vs {}", resultType.name, elseType.name);
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

TypeInfo ExprOneLineIfElseNode::getType() const {
    auto trueType = _trueValue->getType();
    auto falseType = _falseValue->getType();
    if (trueType != falseType) {
        throw YuxError(resolveLineNumber(), "Type mismatch in one-line if-else: true branch is {}, false branch is {}", trueType.name, falseType.name);
    }
    return trueType;
}

int ExprOneLineIfElseNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _condition->resolveLineNumber();
}

TypeInfo ExprIfElsePreValueNode::getType() const {
    auto trueType = _trueValue->getType();
    auto falseType = _falseValue->getType();
    if (trueType != falseType) {
        throw YuxError(resolveLineNumber(), "Type mismatch in if-else expression: true branch is {}, false branch is {}", trueType.name, falseType.name);
    }
    return trueType;
}

int ExprIfElsePreValueNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _condition->resolveLineNumber();
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
            throw YuxError(resolveLineNumber(), "Invalid Array<T> type: missing element type");
        }
        return *elemType;
    }

    if (!arrayType.isArray()) {
        throw YuxError(resolveLineNumber(), "Cannot index non-array type: {}", arrayType.name);
    }

    if (!arrayType.elementType) {
        throw YuxError(resolveLineNumber(), "Invalid array type: missing element type");
    }

    return *arrayType.elementType;
}

int ExprGetNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return _arrayExpr->resolveLineNumber();
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
            throw YuxError(resolveLineNumber(), "Array elements must have the same type: {} vs {}", elementType.name, elemType.name);
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
        throw YuxError(resolveLineNumber(), "Cannot determine type for reference expression: no scope");
    }
    
    auto sym = scope->lookupSymbol(_obj.getText());
    if (!sym) {
        throw YuxError(resolveLineNumber(), "Undefined variable: {}", _obj.getText());
    }
    
    TypeInfo baseType = sym->type;
    
    for (auto& sub : _subs) {
        auto file = dynamic_cast<FileNode*>(scope);
        auto currentScope = scope;
        while (!file && currentScope) {
            currentScope = currentScope->parentScope();
            file = dynamic_cast<FileNode*>(currentScope);
        }
        if (!file) {
            throw YuxError(resolveLineNumber(), "Cannot find struct declaration for field access");
        }
        
        auto structDecl = file->getStructDecl(baseType.name);
        if (!structDecl) {
            throw YuxError(resolveLineNumber(), "Cannot access field on non-struct type: {}", baseType.name);
        }
        
        auto field = structDecl->field(sub.getText());
        if (!field) {
            throw YuxError(resolveLineNumber(), "Struct {} has no field: {}", baseType.name, sub.getText());
        }
        
        baseType = field->getType();
    }
    
    vector<sp<TypeInfo>> genericArgs;
    genericArgs.push_back(make_shared<TypeInfo>(baseType));
    return TypeInfo("Ref", genericArgs);
}

int ExprGetRefNode::resolveLineNumber() const {
    if (_line > 0) return _line;
    return static_cast<int>(_obj.getLine());
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
