// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#include "expr_node.h"
#include "fn_node.h"

const p<ExprNode>& ExprCallNode::getCalleeExpr() const { return _calleeExpr; }

const std::vector<p<ExprNode>>& ExprCallNode::getArgs() const { return _args; }

std::string ExprCallNode::getType() const {
    auto type = _calleeExpr->getType();

    if (type.starts_with("fn() ")) {
        return type.substr(5);
    }
    
    auto scope = findNearestScope();
    if (scope) {
        auto sym = scope->lookupSymbol(type);
        if (sym && sym->kind != SymbolKind::Function) {
            throw YuxError("Type {} is not a Function", sym->name);
        }
        auto fn = scope->lookupFnSymbol(type);
        if (fn) {
            return fn->retType;
        }
    }
    
    return type;
}

const p<LiteralNode>& ExprLiteralNode::literal() const {
    return _literal;
}

string ExprLiteralNode::getType() const {
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

string ExprAddSubNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
    if (leftType != rightType) {
        throw YuxError("Type mismatch in +-/ operation: left is {}, right is {}", leftType, rightType);
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

string ExprMulDivModNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
    if (leftType != rightType) {
        throw YuxError("Type mismatch in */% operation: left is {}, right is {}", leftType, rightType);
    }
    return leftType;
}

const p<ExprNode>& ExprParenNode::expr() const {
    return _inner;
}

string ExprParenNode::getType() const {
    return _inner->getType();
}

const p<ExprNode>& ExprDotNode::baseExpr() const {
    return _baseExpr;
}

string ExprDotNode::member() const {
    return _member->getText();
}

string ExprDotNode::getType() const {
    auto member = _member->getText();
    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        return "fn() " + dstType;
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

string ExprCompareNode::getType() const {
    auto leftType = _left->getType();
    auto rightType = _right->getType();
    if (leftType != rightType) {
        throw YuxError("Type mismatch in comparison: left is {}, right is {}", leftType, rightType);
    }
    return "bool";
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

string ExprIfElseNode::getType() const {
    if (!_thenBlock->hasResult()) {
        return "";
    }
    string resultType = _thenBlock->resultExpr()->getType();
    
    for (auto& elif : _elifs) {
        if (!elif->block()->hasResult()) {
            return "";
        }
        auto elifType = elif->block()->resultExpr()->getType();
        if (elifType != resultType) {
            throw YuxError("Type mismatch in if-elif branches: {} vs {}", resultType, elifType);
        }
    }
    
    if (_elseBlock && _elseBlock->hasResult()) {
        auto elseType = _elseBlock->resultExpr()->getType();
        if (elseType != resultType) {
            throw YuxError("Type mismatch in if-else branches: {} vs {}", resultType, elseType);
        }
    } else if (!_elseBlock || !_elseBlock->hasResult()) {
        return "";
    }
    
    return resultType;
}
