// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "borrow_checker.h"

#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "node/type_node.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

class BorrowChecker {
    // 已声明的本函数局部 / 参数（含 `$`）。fn-wide 扁平作用域：
    // 现行 ast_builder 把所有局部 var 都注册在 FnNode 上（StatementBlockNode
    // 没有压入 _scopeStack），因此真正的嵌套作用域信息在 AST 之外。这里采用
    // 同样的扁平模型——一个函数内一旦借用了某根对象，整函数余下不可重赋。
    // 比 §3.5 严格但完全本地、O(1)、无误放，且无现有用例触碰禁忌。
    std::set<std::string> _declared;

    // 根对象名 → 当前活跃借用计数。
    std::map<std::string, int> _activeBorrows;

    // T& 局部 / 参数 → 终极根对象名（拷绑跟链直达根）。
    std::map<std::string, std::string> _refToRoot;

public:
    void run(p<FnNode> fn, const std::string& selfStructName) {
        if (!selfStructName.empty()) {
            _declared.insert("$");
        }
        for (auto& param : fn->header()->params()) {
            auto pname = param->name().getText();
            _declared.insert(pname);
            // T& 参数：根对象就是参数自身（实参侧的根在 caller，callee 端
            // 视参数自己为根，与"参数作用域 ⊇ 借用作用域"相符）
            if (param->type() && param->type()->getType().isRef()) {
                _refToRoot[pname] = pname;
            }
        }
        for (auto& s : fn->body()) {
            visitStmt(s);
        }
    }

private:
    std::string resolveRoot(const std::string& name) const {
        auto it = _refToRoot.find(name);
        return it != _refToRoot.end() ? it->second : name;
    }

    // 从 `var r T& = expr` 的 RHS 推根对象名。
    // - &x.f.f → 根 = x
    // - 现有 T& 拷绑（LiteralObj 单 ID）→ 根 = 该 ref 的链上根
    // 其他形式（grammar 不允许，到这里也兜底报错）。
    std::string rootFromRefInit(p<ExprNode> expr, int line) {
        if (auto getRef = dynamic_cast<ExprGetRefNode*>(expr)) {
            auto name = getRef->obj().getText();
            return resolveRoot(name);
        }
        if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                auto name = obj->getValue().getText();
                // 拷绑：name 必定是 T&，refToRoot 应有记录。退路：当作 name 自己。
                return resolveRoot(name);
            }
        }
        throw YuxError(line, ErrorCode::E4001)
            .withHint("T& 借用初始化形如 `val r T& = &x` 或 `val r2 T& = r1`（拷绑已有 T& 变量）");
    }

    void registerBorrow(const std::string& refName, const std::string& rootName, int line) {
        if (_declared.find(rootName) == _declared.end()) {
            // 根对象不在当前函数 declared 集合：在扁平作用域下意味着
            // 来源根对象的作用域不能覆盖借用方（§3.5 违规）。
            throw YuxError(line, ErrorCode::E4002, refName, rootName);
        }
        _refToRoot[refName] = rootName;
        _activeBorrows[rootName]++;
    }

    void checkRootReassign(const std::string& name, int line) {
        auto it = _activeBorrows.find(name);
        if (it != _activeBorrows.end() && it->second > 0) {
            throw YuxError(line, ErrorCode::E4003, name);
        }
    }

    // ------- 遍历 -------

    void visitBlock(p<StatementBlockNode> blk) {
        if (!blk) return;
        for (auto& s : blk->statements()) {
            visitStmt(s);
        }
        if (blk->hasResult() && blk->resultExpr()) {
            visitExpr(blk->resultExpr());
        }
    }

    void visitStmt(p<StatementNode> s) {
        if (!s) return;

        if (auto blk = dynamic_cast<StatementBlockNode*>(s)) {
            visitBlock(p<StatementBlockNode>(blk));
            return;
        }

        if (auto loop = dynamic_cast<StatementLoopNode*>(s)) {
            visitBlock(loop->block());
            return;
        }

        if (auto decl = dynamic_cast<StatementDeclareNode*>(s)) {
            _declared.insert(decl->name().getText());
            return;
        }

        if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
            auto vname = da->name().getText();
            _declared.insert(vname);

            // 先访问 RHS 内嵌的块结构（可能含 if-else 表达式）
            if (da->expr()) visitExpr(da->expr());

            // 类型可能在节点上显式给出，或来自 expr。
            TypeInfo varType;
            if (da->varType()) {
                varType = da->varType()->getType();
            } else if (da->expr()) {
                varType = da->expr()->getType();
            }
            if (varType.isRef() && da->expr()) {
                auto root = rootFromRefInit(da->expr(), s->getLineNumber());
                registerBorrow(vname, root, s->getLineNumber());
            }
            return;
        }

        if (auto as = dynamic_cast<StatementAssignNode*>(s)) {
            // 顶层重赋（无字段 subs）才视作"根对象赋值"。
            // 字段写 / 通过 T& 写不算根对象本身的重赋。
            if (as->subs().empty()) {
                checkRootReassign(as->obj().getText(), s->getLineNumber());
            }
            if (as->expr()) visitExpr(as->expr());
            return;
        }

        if (auto setN = dynamic_cast<StatementSetNode*>(s)) {
            // arr[i] = v 不影响根对象语义，仅扫表达式
            if (setN->arrayExpr()) visitExpr(setN->arrayExpr());
            for (auto& idx : setN->indices()) {
                if (idx) visitExpr(idx);
            }
            if (setN->valueExpr()) visitExpr(setN->valueExpr());
            return;
        }

        if (auto ret = dynamic_cast<StatementRetNode*>(s)) {
            if (ret->expr()) visitExpr(ret->expr());
            return;
        }

        if (auto se = dynamic_cast<StatementExprNode*>(s)) {
            if (se->expr()) visitExpr(se->expr());
            return;
        }
        // 其它 statement 类型（StatementBreakNode / StatementRetVoidNode 等）无需处理
    }

    // 表达式仅遍历可能包含嵌套块的位置。完整 AST 遍历不必要——
    // 我们只在乎 if/else 表达式块里隐藏的 borrow 声明 / 重赋语句。
    void visitExpr(p<ExprNode> e) {
        if (!e) return;

        if (auto ie = dynamic_cast<ExprIfElseNode*>(e)) {
            if (ie->condition()) visitExpr(ie->condition());
            if (ie->thenBlock()) visitBlock(ie->thenBlock());
            for (auto& el : ie->elifs()) {
                if (!el) continue;
                if (el->condition()) visitExpr(el->condition());
                if (el->block()) visitBlock(el->block());
            }
            if (ie->elseBlock()) visitBlock(ie->elseBlock());
            return;
        }
        if (auto pe = dynamic_cast<ExprIfElsePreValueNode*>(e)) {
            if (pe->condition()) visitExpr(pe->condition());
            if (pe->trueValue()) visitExpr(pe->trueValue());
            if (pe->falseValue()) visitExpr(pe->falseValue());
            return;
        }
        if (auto ol = dynamic_cast<ExprOneLineIfElseNode*>(e)) {
            if (ol->condition()) visitExpr(ol->condition());
            if (ol->trueValue()) visitExpr(ol->trueValue());
            if (ol->falseValue()) visitExpr(ol->falseValue());
            return;
        }
        // 其余表达式不需深入；本检查不依赖完整数据流。
        // 嵌套 block 只通过 if/else 表达式承载，已上面覆盖。
    }
};

} // namespace

void checkBorrows(p<FnNode> fn, const std::string& selfStructName) {
    if (!fn) return;
    BorrowChecker bc;
    bc.run(fn, selfStructName);
}
