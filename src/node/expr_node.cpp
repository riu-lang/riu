// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#include "expr_node.h"
#include "fn_node.h"
#include "file_node.h"

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
                auto fnName = objLiteral->getValue()->getText();
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
        return TypeInfo();
    }

    if (type.name.starts_with("fn() ")) {
        DEBUG_LOG_VAL("ExprCallNode::getType - returning", type.name.substr(5));
        return TypeInfo(type.name.substr(5));
    }

    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(type.name);
        if (sym && sym->kind == SymbolKind::Struct) {
            string ctorFullName = type.name + "." + type.name;
            auto fn = scope->lookupFnSymbol(ctorFullName);
            if (fn) {
                return TypeInfo(type.name);
            }
        }
        if (sym && sym->kind != SymbolKind::Function) {
            throw YuxError("Type {} is not a Function", sym->name);
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
        throw YuxError("Type mismatch in +-/ operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
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
        throw YuxError("Type mismatch in */% operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
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
        throw YuxError("Type mismatch in &|^ operation: left is {}, right is {}", leftType.name, rightType.name);
    }
    return leftType;
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
    return _member->getText();
}

TypeInfo ExprDotNode::getType() const {
    auto member = _member->getText();
    DEBUG_LOG_VAL("ExprDotNode::getType - member", member);
    DEBUG_LOG_VAL("ExprDotNode::getType - starts_with('to_')", member.starts_with("to_"));
    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("ExprDotNode::getType - returning fn()", dstType);
        return TypeInfo("fn() " + dstType);
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
    
    auto scope = findNearestScope();
    if (scope) {
        auto file = dynamic_cast<FileNode*>(scope);
        while (!file && scope) {
            scope = scope->parentScope();
            file = dynamic_cast<FileNode*>(scope);
        }
        if (file) {
            auto structDecl = file->getStructDecl(actualType.name);
            if (structDecl) {
                auto field = structDecl->field(member);
                if (field) {
                    return field->getType();
                }
            }
            
            string methodFullName = actualType.name + "." + member;
            auto methodSym = file->lookupFnSymbol(methodFullName);
            if (methodSym) {
                DEBUG_LOG_VAL("ExprDotNode::getType - found method, returning fn()", methodSym->retType.name);
                return TypeInfo("fn() " + methodSym->retType.name);
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
        throw YuxError("Type mismatch in comparison: left is {}, right is {}", leftType.name, rightType.name);
    }
    return TypeInfo("bool");
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
            throw YuxError("Type mismatch in if-elif branches: {} vs {}", resultType.name, elifType.name);
        }
    }

    if (_elseBlock && _elseBlock->hasResult()) {
        auto elseType = _elseBlock->resultExpr()->getType();
        if (elseType != resultType) {
            throw YuxError("Type mismatch in if-else branches: {} vs {}", resultType.name, elseType.name);
        }
    } else if (!_elseBlock || !_elseBlock->hasResult()) {
        return TypeInfo();
    }

    return resultType;
}

const p<ExprNode>& ExprGetNode::arrayExpr() const {
    return _arrayExpr;
}

const vector<p<ExprNode>>& ExprGetNode::indices() const {
    return _indices;
}

TypeInfo ExprGetNode::getType() const {
    auto arrayType = _arrayExpr->getType();
    if (!arrayType.isArray()) {
        throw YuxError("Cannot index non-array type: {}", arrayType.name);
    }

    if (!arrayType.elementType) {
        throw YuxError("Invalid array type: missing element type");
    }

    return *arrayType.elementType;
}

const vector<p<ExprNode>>& ExprArrayNode::elements() const {
    return _elements;
}

TypeInfo ExprArrayNode::getType() const {
    if (_elements.empty()) {
        throw YuxError("Cannot infer type of empty array");
    }

    TypeInfo elementType = _elements[0]->getType();
    for (size_t i = 1; i < _elements.size(); ++i) {
        auto elemType = _elements[i]->getType();
        if (elemType != elementType) {
            throw YuxError("Array elements must have the same type: {} vs {}", elementType.name, elemType.name);
        }
    }

    auto elemShared = make_shared<TypeInfo>(elementType);
    return TypeInfo(elemShared, _elements.size());
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
        throw YuxError("Cannot determine type for reference expression: no scope");
    }
    
    auto sym = scope->lookupSymbol(_obj->getText());
    if (!sym) {
        throw YuxError("Undefined variable: {}", _obj->getText());
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
            throw YuxError("Cannot find struct declaration for field access");
        }
        
        auto structDecl = file->getStructDecl(baseType.name);
        if (!structDecl) {
            throw YuxError("Cannot access field on non-struct type: {}", baseType.name);
        }
        
        auto field = structDecl->field(sub->getText());
        if (!field) {
            throw YuxError("Struct {} has no field: {}", baseType.name, sub->getText());
        }
        
        baseType = field->getType();
    }
    
    vector<sp<TypeInfo>> genericArgs;
    genericArgs.push_back(make_shared<TypeInfo>(baseType));
    return TypeInfo("Ref", genericArgs);
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
