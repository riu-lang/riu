// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "flow_terminate_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "error_code.h"

namespace {

// 通过 callee 表达式的字面量名查到 FnSymbolInfo，判断是否带 #NoReturn。
// 简化：只看顶层身份引用（如 `panic("x")`）；方法调用、Rc 调用等暂不参与
// 流终止判定（保守判 false，不错杀但可能漏报）。
bool callIsNoReturn(p<ScopeNode> scope, p<ExprCallNode> call) {
    if (!scope || !call) return false;
    auto callee = call->getCalleeExpr();
    auto litCallee = dynamic_cast<p<ExprLiteralNode>>(callee);
    if (!litCallee) return false;
    auto obj = dynamic_cast<p<LiteralObjNode>>(litCallee->literal());
    if (!obj) return false;
    auto name = obj->getValue().getText();
    auto* sym = scope->lookupFnSymbol(name);
    return sym && sym->isNoReturn;
}

bool blockTerminates(p<ScopeNode> scope, p<StatementBlockNode> block);
bool stmtTerminates(p<ScopeNode> scope, p<StatementNode> stmt);
bool exprTerminates(p<ScopeNode> scope, p<ExprNode> expr);

// loop body 是否含可达 break（属于目标 label 对应的 loop）。
// 裸 break 总是属于最内层 loop；break@L 精确匹配 label L。
// forLabel 为空时匹配所有裸 break（用于非 labeled 场景的兼容）。
bool blockHasOwnBreak(p<StatementBlockNode> block, const string& forLabel);

bool stmtsHaveOwnBreak(const vector<p<StatementNode>>& stmts, const string& forLabel) {
    for (auto s : stmts) {
        if (auto br = dynamic_cast<p<StatementBreakNode>>(s)) {
            const auto& bl = br->label();
            if (bl.getText().empty()) return true;     // 裸 break → 当前层 own break
            if (bl.getText() == forLabel) return true; // break@L 匹配当前 loop label
            continue;                                  // break@other → 不属于当前 loop
        }
        if (auto nestedLoop = dynamic_cast<p<StatementLoopNode>>(s)) {
            // 嵌套 loop：裸 break 只跳出内层，不属外层；
            // 但 break@forLabel 即使在内层体内也会跳出外层 → 递归检查
            if (!forLabel.empty() && blockHasOwnBreak(nestedLoop->block(), forLabel)) return true;
            continue;
        }
        if (auto nestedFor = dynamic_cast<p<StatementForInNode>>(s)) {
            if (!forLabel.empty() && blockHasOwnBreak(nestedFor->block(), forLabel)) return true;
            continue;
        }
        if (auto se = dynamic_cast<p<StatementExprNode>>(s)) {
            auto e = se->expr();
            if (auto ife = dynamic_cast<p<ExprIfElseNode>>(e)) {
                if (blockHasOwnBreak(ife->thenBlock(), forLabel)) return true;
                for (auto& el : ife->elifs()) {
                    if (blockHasOwnBreak(el->block(), forLabel)) return true;
                }
                if (ife->elseBlock() && blockHasOwnBreak(ife->elseBlock(), forLabel)) return true;
                continue;
            }
            // match arm 可为表达式或块；块内 break 属当前 loop
            if (auto m = dynamic_cast<p<ExprMatchNode>>(e)) {
                for (auto& arm : m->arms()) {
                    if (arm && arm->hasBlock() && blockHasOwnBreak(arm->block(), forLabel)) return true;
                }
                continue;
            }
        }
    }
    return false;
}

bool blockHasOwnBreak(p<StatementBlockNode> block, const string& forLabel) {
    if (!block) return false;
    return stmtsHaveOwnBreak(block->statements(), forLabel);
}

bool exprTerminates(p<ScopeNode> scope, p<ExprNode> expr) {
    if (!expr) return false;
    if (auto call = dynamic_cast<p<ExprCallNode>>(expr)) {
        return callIsNoReturn(scope, call);
    }
    if (auto ife = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        if (!ife->elseBlock()) return false;
        if (!blockTerminates(scope, ife->thenBlock())) return false;
        for (auto& el : ife->elifs()) {
            if (!blockTerminates(scope, el->block())) return false;
        }
        return blockTerminates(scope, ife->elseBlock());
    }
    if (auto m = dynamic_cast<p<ExprMatchNode>>(expr)) {
        if (m->arms().empty()) return false;
        for (auto& arm : m->arms()) {
            if (arm->hasBlock()) {
                if (!blockTerminates(scope, arm->block())) return false;
            } else if (!exprTerminates(scope, arm->body())) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bool stmtTerminates(p<ScopeNode> scope, p<StatementNode> stmt) {
    if (!stmt) return false;
    if (dynamic_cast<p<StatementRetNode>>(stmt)) return true;
    if (dynamic_cast<p<StatementRetVoidNode>>(stmt)) return true;
    if (auto loop = dynamic_cast<p<StatementLoopNode>>(stmt)) {
        return !blockHasOwnBreak(loop->block(), loop->label().getText());
    }
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        return exprTerminates(scope, se->expr());
    }
    return false;
}

bool blockTerminates(p<ScopeNode> scope, p<StatementBlockNode> block) {
    if (!block) return false;
    for (auto& s : block->statements()) {
        if (stmtTerminates(scope, s)) return true;
    }
    if (block->hasResult() && block->resultExpr()) {
        return exprTerminates(scope, block->resultExpr());
    }
    return false;
}

bool fnBodyTerminates(p<FnNode> fn) {
    p<ScopeNode> scope = fn;
    for (auto& s : fn->body()) {
        if (stmtTerminates(scope, s)) return true;
    }
    return false;
}

} // namespace

void checkFlowTerminate(p<FnNode> fn) {
    if (!fn) return;
    auto header = fn->header();
    if (!header || !header->hasAnno("NoReturn")) return;

    if (fnBodyTerminates(fn)) return;

    int line = header->getLineNumber();
    int col = header->getColumn();
    throw YuxError(line, col, ErrorCode::E7014, header->name().getText());
}
