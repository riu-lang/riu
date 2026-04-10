// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#include "expr_node.h"
#include "fn_node.h"

const p<ExprNode>& ExprCallNode::getCalleeExpr() const { return _calleeExpr; }

const std::vector<p<ExprNode>>& ExprCallNode::getArgs() const { return _args; }

TypeInfo ExprCallNode::getType() const {
    auto type = _calleeExpr->getType();

    if (type.name.starts_with("fn() ")) {
        return TypeInfo(type.name.substr(5));
    }
    
    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(type.name);
        if (sym && sym->kind != SymbolKind::Function) {
            throw YuxError("Type {} is not a Function", sym->name);
        }
        auto fn = scope->lookupFnSymbol(type.name);
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
    std::cerr << "[DEBUG] ExprAddSubNode::getType called" << std::endl;
    auto leftType = _left->getType();
    std::cerr << "[DEBUG]   leftType: " << leftType.name << std::endl;
    std::cerr << "[DEBUG]   _right type: " << typeid(*_right).name() << std::endl;
    auto rightType = _right->getType();
    std::cerr << "[DEBUG]   rightType: " << rightType.name << std::endl;
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
    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        return TypeInfo("fn() " + dstType);
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
    std::cerr << "[DEBUG] ExprGetNode::getType called" << std::endl;
    auto arrayType = _arrayExpr->getType();
    std::cerr << "[DEBUG] ExprGetNode::getType - arrayType: " << arrayType.name 
              << ", isArray: " << arrayType.isArray() 
              << ", elementType: " << (arrayType.elementType ? arrayType.elementType->name : "null") << std::endl;
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
