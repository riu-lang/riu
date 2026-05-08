// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ctor_daa.h"

#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"

#include <map>
#include <string>
#include <vector>

namespace {

class CtorDAA {
public:
    enum class State { Uninit, Init };
    using FieldStates = std::map<std::string, State>;

private:
    std::vector<std::string> _fields;     // 声明顺序，遍历用
    FieldStates              _state;      // field → Uninit / Init
    std::string              _structName;

public:
    void run(p<FnNode> fn,
             const std::string& structName,
             const std::vector<std::string>& fieldNames) {
        _structName = structName;
        _fields = fieldNames;
        for (auto& f : _fields) _state[f] = State::Uninit;

        for (auto& s : fn->body()) {
            visitStmt(s);
        }

        // 函数末尾隐式 ret：要求全字段 Init
        // 注意：若所有分支都已 ret 终止，body 末仍走到这里——保守再查一次也无害。
        requireAllInitAtExit(fn->body().empty()
                             ? 0
                             : fn->body().back()->getLineNumber());
    }

private:
    static bool isSelfLiteral(p<ExprNode> e) {
        if (auto lit = dynamic_cast<ExprLiteralNode*>(e)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                return obj->getValue().getText() == "$";
            }
        }
        return false;
    }

    void requireFieldInit(const std::string& f, int line, const char* what) {
        auto it = _state.find(f);
        if (it == _state.end()) {
            // 未知字段名——交给语义层报错，DAA 不挡
            return;
        }
        if (it->second != State::Init) {
            throw YuxError(line, ErrorCode::E4010, f, what);
        }
    }

    void requireAllInit(int line, const char* reason) {
        for (auto& f : _fields) {
            auto it = _state.find(f);
            if (it == _state.end() || it->second != State::Init) {
                throw YuxError(line, ErrorCode::E4011, f, reason);
            }
        }
    }

    void requireAllInitAtExit(int line) {
        for (auto& f : _fields) {
            auto it = _state.find(f);
            if (it == _state.end() || it->second != State::Init) {
                throw YuxError(line, ErrorCode::E4012, f);
            }
        }
    }

    // -------- statements --------

    void visitStmt(p<StatementNode> s) {
        if (!s) return;

        if (auto blk = dynamic_cast<StatementBlockNode*>(s)) {
            visitBlock(p<StatementBlockNode>(blk));
            return;
        }

        if (auto loop = dynamic_cast<StatementLoopNode*>(s)) {
            // 循环保守：循环可能 0 次执行，body 内的写不计入 post-loop。
            // body 内的读按当前快照（=进入循环前的状态）检查。
            FieldStates snap = _state;
            visitBlock(loop->block());
            _state = snap;
            return;
        }

        if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
            if (da->expr()) visitExpr(da->expr());
            return;
        }

        if (dynamic_cast<StatementDeclareNode*>(s)) {
            return;
        }

        if (auto as = dynamic_cast<StatementAssignNode*>(s)) {
            auto obj = as->obj().getText();
            auto& subs = as->subs();
            if (obj == "$" && subs.size() == 1) {
                // `$.field = expr`：先 visit RHS（RHS 中 $.x 必须已 Init），再标记
                if (as->expr()) visitExpr(as->expr());
                _state[subs[0].getText()] = State::Init;
                return;
            }
            if (obj == "$" && subs.size() > 1) {
                // `$.a.b... = expr`：要求 $.a 已 Init
                requireFieldInit(subs[0].getText(), s->getLineNumber(),
                    "is read");
                if (as->expr()) visitExpr(as->expr());
                return;
            }
            // 其它顶层 / 字段赋值
            if (as->expr()) visitExpr(as->expr());
            return;
        }

        if (auto setN = dynamic_cast<StatementSetNode*>(s)) {
            if (setN->arrayExpr()) visitExpr(setN->arrayExpr());
            for (auto& idx : setN->indices()) {
                if (idx) visitExpr(idx);
            }
            if (setN->valueExpr()) visitExpr(setN->valueExpr());
            return;
        }

        if (auto ret = dynamic_cast<StatementRetNode*>(s)) {
            if (ret->expr() && isSelfLiteral(ret->expr())) {
                // §8.3：`ret $;` 禁止（返回 Self& 而非 Self by value）
                throw YuxError(s->getLineNumber(), s->getColumn(), ErrorCode::E4013);
            }
            if (ret->expr()) visitExpr(ret->expr());
            requireAllInit(s->getLineNumber(), "ret");
            return;
        }

        if (dynamic_cast<StatementRetVoidNode*>(s)) {
            requireAllInit(s->getLineNumber(), "ret");
            return;
        }

        if (auto se = dynamic_cast<StatementExprNode*>(s)) {
            if (se->expr()) visitExpr(se->expr());
            return;
        }
        // 其它（StatementBreakNode 等）跳过
    }

    void visitBlock(p<StatementBlockNode> blk) {
        if (!blk) return;
        for (auto& s : blk->statements()) {
            visitStmt(s);
        }
        if (blk->hasResult() && blk->resultExpr()) {
            visitExpr(blk->resultExpr());
        }
    }

    // -------- expressions --------

    void visitExpr(p<ExprNode> e) {
        if (!e) return;

        // `$` 字面量直接出现（作 arg / operand）→ 视为读所有字段
        if (isSelfLiteral(e)) {
            requireAllInit(e->getLineNumber(), "passing `$` as a value");
            return;
        }

        // `$.field` / `$.method(args)` 入口先看 callee
        if (auto ce = dynamic_cast<ExprCallNode*>(e)) {
            // `$.method(args)`：视为读所有字段（保守）
            if (auto dot = dynamic_cast<ExprDotNode*>(ce->getCalleeExpr())) {
                if (isSelfLiteral(dot->baseExpr())) {
                    requireAllInit(e->getLineNumber(),
                        "calling a method on `$`");
                    for (auto& a : ce->getArgs()) visitExpr(a);
                    return;
                }
                visitExpr(dot->baseExpr());
            } else {
                visitExpr(ce->getCalleeExpr());
            }
            for (auto& a : ce->getArgs()) visitExpr(a);
            return;
        }

        if (auto dot = dynamic_cast<ExprDotNode*>(e)) {
            if (isSelfLiteral(dot->baseExpr())) {
                requireFieldInit(dot->member(), e->getLineNumber(), "is read");
                return;
            }
            visitExpr(dot->baseExpr());
            return;
        }

        if (auto gr = dynamic_cast<ExprGetRefNode*>(e)) {
            if (gr->obj().getText() == "$") {
                if (gr->subs().empty()) {
                    // `&$`：借用 Self&——视为读所有字段
                    requireAllInit(e->getLineNumber(), "borrowing `$`");
                } else {
                    requireFieldInit(gr->subs()[0].getText(),
                        e->getLineNumber(), "is borrowed");
                }
            }
            return;
        }

        if (auto pe = dynamic_cast<ExprParenNode*>(e)) {
            visitExpr(pe->expr());
            return;
        }
        if (auto u = dynamic_cast<ExprUnaryNode*>(e)) {
            visitExpr(u->right());
            return;
        }
        if (auto a = dynamic_cast<ExprAddSubNode*>(e)) {
            visitExpr(a->left()); visitExpr(a->right()); return;
        }
        if (auto m = dynamic_cast<ExprMulDivModNode*>(e)) {
            visitExpr(m->left()); visitExpr(m->right()); return;
        }
        if (auto b = dynamic_cast<ExprBinOpNode*>(e)) {
            visitExpr(b->left()); visitExpr(b->right()); return;
        }
        if (auto c = dynamic_cast<ExprCompareNode*>(e)) {
            visitExpr(c->left()); visitExpr(c->right()); return;
        }
        if (auto g = dynamic_cast<ExprGetNode*>(e)) {
            visitExpr(g->arrayExpr());
            for (auto& i : g->indices()) visitExpr(i);
            return;
        }
        if (auto ar = dynamic_cast<ExprArrayNode*>(e)) {
            for (auto& el : ar->elements()) visitExpr(el);
            return;
        }
        if (dynamic_cast<ExprArrayInitNode*>(e)) {
            return;
        }
        if (auto ne = dynamic_cast<ExprNullElseNode*>(e)) {
            visitExpr(ne->left()); visitExpr(ne->right()); return;
        }

        // 分支汇合：if-else 块（每分支独立从入口快照运行，汇合取交集）
        if (auto ie = dynamic_cast<ExprIfElseNode*>(e)) {
            if (ie->condition()) visitExpr(ie->condition());
            FieldStates pre = _state;
            std::vector<FieldStates> branchStates;

            // then 分支
            _state = pre;
            visitBlock(ie->thenBlock());
            branchStates.push_back(_state);

            // elif 分支
            for (auto& el : ie->elifs()) {
                if (!el) continue;
                _state = pre;
                if (el->condition()) visitExpr(el->condition());
                visitBlock(el->block());
                branchStates.push_back(_state);
            }

            // else 分支：缺失则视作"该路径维持入口快照"
            if (ie->elseBlock()) {
                _state = pre;
                visitBlock(ie->elseBlock());
                branchStates.push_back(_state);
            } else {
                branchStates.push_back(pre);
            }

            // 汇合：每字段 Init iff 所有分支末态都是 Init
            FieldStates merged = pre;
            for (auto& f : _fields) {
                bool allInit = true;
                for (auto& bs : branchStates) {
                    auto it = bs.find(f);
                    if (it == bs.end() || it->second != State::Init) {
                        allInit = false;
                        break;
                    }
                }
                merged[f] = allInit ? State::Init : State::Uninit;
            }
            _state = merged;
            return;
        }

        if (auto pe = dynamic_cast<ExprIfElsePreValueNode*>(e)) {
            visitExpr(pe->condition());
            visitExpr(pe->trueValue());
            visitExpr(pe->falseValue());
            return;
        }
        if (auto ol = dynamic_cast<ExprOneLineIfElseNode*>(e)) {
            visitExpr(ol->condition());
            visitExpr(ol->trueValue());
            visitExpr(ol->falseValue());
            return;
        }

        // 字面量、数字 / 布尔 / 字符串 / null / 普通 ID（已在 isSelfLiteral 上方处理 `$`）
        // 都不需要进一步检查
    }
};

} // namespace

void checkConstructorDAA(p<FnNode> fn,
                         const std::string& structName,
                         const std::vector<std::string>& fieldNames) {
    if (!fn) return;
    CtorDAA daa;
    daa.run(fn, structName, fieldNames);
}
