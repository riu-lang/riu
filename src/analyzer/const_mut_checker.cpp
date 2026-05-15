// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "const_mut_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "error_code.h"

namespace {

// 拿到表达式所属作用域：优先 expr 自身的 findNearestScope，失败则回退到附近 stmt。
p<ScopeNode> exprScope(p<ExprNode> e, p<ScopeNode> fallback) {
    if (e) {
        if (auto sc = e->findNearestScope()) return sc;
    }
    return fallback;
}

// 判定一个表达式是否符合 §3.3 的"常量表达式"。
// 不通过时抛 E3104，errExpr 指向最里层不合规子表达式。
// scope 用于解析 LiteralObjNode 的符号引用（必须是 cval）。
void requireConstExpr(p<ExprNode> e, p<ScopeNode> scope);

void throwNonConst(p<ExprNode> e, const std::string& what) {
    int line = e->resolveLineNumber();
    int col = e->resolveColumn();
    throw YuxError(line, col, ErrorCode::E3104, what);
}

void requireConstExpr(p<ExprNode> e, p<ScopeNode> scope) {
    if (!e) return;

    if (auto paren = dynamic_cast<p<ExprParenNode>>(e)) {
        requireConstExpr(paren->expr(), scope);
        return;
    }
    if (auto u = dynamic_cast<p<ExprUnaryNode>>(e)) {
        requireConstExpr(u->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprAddSubNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprMulDivModNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprBinOpNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<p<ExprCompareNode>>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }

    if (auto le = dynamic_cast<p<ExprLiteralNode>>(e)) {
        auto lit = le->literal();
        if (dynamic_cast<p<LiteralIntNode>>(lit)) return;
        if (dynamic_cast<p<LiteralFloatNode>>(lit)) return;
        if (dynamic_cast<p<LiteralBoolNode>>(lit)) return;
        if (dynamic_cast<p<LiteralNullNode>>(lit)) return;
        if (dynamic_cast<p<LiteralStringNode>>(lit)) return;
        if (dynamic_cast<p<LiteralCodePointNode>>(lit)) return;
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit)) {
            auto name = obj->getValue().getText();
            auto sc = exprScope(e, scope);
            SymbolInfo* sym = sc ? sc->lookupSymbol(name) : nullptr;
            if (sym && sym->isConst) return;
            throwNonConst(e, "reference to non-`cval` symbol `" + name + "`");
        }
        if (dynamic_cast<p<StringTemplateNode>>(lit)) {
            throwNonConst(e, "string template interpolation");
        }
        throwNonConst(e, "unsupported literal");
    }

    if (dynamic_cast<p<ExprCallNode>>(e))         throwNonConst(e, "function / method call");
    if (dynamic_cast<p<ExprDotNode>>(e))          throwNonConst(e, "member access");
    if (dynamic_cast<p<ExprGetNode>>(e))          throwNonConst(e, "array indexing");
    if (dynamic_cast<p<ExprGetRefNode>>(e))       throwNonConst(e, "address-of (&) expression");
    if (dynamic_cast<p<ExprArrayNode>>(e))        throwNonConst(e, "array literal");
    if (dynamic_cast<p<ExprArrayInitNode>>(e))    throwNonConst(e, "array fill expression");
    if (dynamic_cast<p<ExprTupleNode>>(e))        throwNonConst(e, "tuple construction");
    if (dynamic_cast<p<ExprIfElseNode>>(e))       throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprOneLineIfElseNode>>(e))   throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprIfElsePreValueNode>>(e))  throwNonConst(e, "if-else expression");
    if (dynamic_cast<p<ExprMatchNode>>(e))        throwNonConst(e, "match expression");
    if (dynamic_cast<p<ExprTryCatchNode>>(e))     throwNonConst(e, "try-catch expression");
    if (dynamic_cast<p<ExprEnumCtorNode>>(e))     throwNonConst(e, "enum constructor");
    if (dynamic_cast<p<ExprDynCtorNode>>(e))      throwNonConst(e, "Dyn<...> construction");
    if (dynamic_cast<p<ExprNullElseNode>>(e))     throwNonConst(e, "`??` expression");
    if (dynamic_cast<p<LambdaExprNode>>(e))       throwNonConst(e, "lambda expression");

    throwNonConst(e, "unsupported expression");
}

// 遍历整棵 fn body，命中 cval 局部声明就走 §3.3 校验。
class ConstMutWalker {
public:
    void run(p<FnNode> fn) {
        for (auto& s : fn->body()) visitStmt(s);
    }

private:
    void visitBlock(p<StatementBlockNode> blk) {
        if (!blk) return;
        for (auto& s : blk->statements()) visitStmt(s);
        if (blk->hasResult() && blk->resultExpr()) visitExpr(blk->resultExpr());
    }

    void visitStmt(p<StatementNode> s) {
        if (!s) return;

        if (auto blk = dynamic_cast<p<StatementBlockNode>>(s)) {
            visitBlock(blk);
            return;
        }
        if (auto loop = dynamic_cast<p<StatementLoopNode>>(s)) {
            visitBlock(loop->block());
            return;
        }
        if (auto da = dynamic_cast<p<StatementDeclareAssignNode>>(s)) {
            if (da->declareType() == DeclareType::CVal && da->expr()) {
                auto scope = s->findNearestScope();
                requireConstExpr(da->expr(), scope);
            }
            if (da->expr()) visitExpr(da->expr());
            return;
        }
        if (auto se = dynamic_cast<p<StatementExprNode>>(s)) {
            if (se->expr()) visitExpr(se->expr());
            return;
        }
        // 其他 stmt 类型（Declare 无 init / Assign / Set / Break / Ret / RetVoid）
        // 没有需要检查的 cval 初值，掠过。
    }

    // 表达式遍历仅深入可能嵌套语句块的结构，便于覆盖 if-else / lambda / 调用实参 / 块表达式
    // 中的 cval 声明。
    void visitExpr(p<ExprNode> e) {
        if (!e) return;
        if (auto ie = dynamic_cast<p<ExprIfElseNode>>(e)) {
            visitExpr(ie->condition());
            visitBlock(ie->thenBlock());
            for (auto& el : ie->elifs()) {
                if (!el) continue;
                visitExpr(el->condition());
                visitBlock(el->block());
            }
            visitBlock(ie->elseBlock());
            return;
        }
        if (auto pe = dynamic_cast<p<ExprIfElsePreValueNode>>(e)) {
            visitExpr(pe->condition());
            visitExpr(pe->trueValue());
            visitExpr(pe->falseValue());
            return;
        }
        if (auto ol = dynamic_cast<p<ExprOneLineIfElseNode>>(e)) {
            visitExpr(ol->condition());
            visitExpr(ol->trueValue());
            visitExpr(ol->falseValue());
            return;
        }
        if (auto call = dynamic_cast<p<ExprCallNode>>(e)) {
            visitExpr(call->getCalleeExpr());
            for (auto& a : call->getArgs()) visitExpr(a);
            return;
        }
        if (auto lam = dynamic_cast<p<LambdaExprNode>>(e)) {
            if (lam->bodyExpr()) visitExpr(lam->bodyExpr());
            for (auto& s : lam->bodyStmts()) visitStmt(s);
            return;
        }
        if (auto m = dynamic_cast<p<ExprMatchNode>>(e)) {
            visitExpr(m->scrutinee());
            for (auto& arm : m->arms()) {
                if (arm) visitExpr(arm->body());
            }
            return;
        }
        if (auto tc = dynamic_cast<p<ExprTryCatchNode>>(e)) {
            visitBlock(tc->tryBlock());
            for (auto& c : tc->catches()) {
                if (c) visitBlock(c->body());
            }
            return;
        }
        // 其余 expr 不承载嵌套 stmt 块。
    }
};

} // anon namespace

void checkConstMut(p<FnNode> fn) {
    ConstMutWalker w;
    w.run(fn);
}
