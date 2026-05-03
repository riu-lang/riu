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
    // §8.6.5.8 块作用域栈：每层记录本块新增的 declared 名 / 借用名，
    // 弹栈时把它们从 _declared / _refToRoot 摘除并减 _activeBorrows。
    struct Scope {
        std::vector<std::string> declared;
        std::vector<std::string> borrows;
    };
    std::vector<Scope> _scopes;

    // 当前所有活跃作用域并集（O(1) 查询根是否仍在视野内）
    std::set<std::string> _declared;

    // 根对象名 → 当前活跃借用计数
    std::map<std::string, int> _activeBorrows;

    // T& 局部 / 参数 → 终极根对象名（拷绑跟链直达根）
    std::map<std::string, std::string> _refToRoot;

    // 根对象名 → 类型（仅 owned 类型，T& 不在内）。用于 §8.4.2.5 借用期 Array 修改方法检测。
    std::map<std::string, TypeInfo> _rootType;

    // §8.4.2.5 Array<T> 借用期不可调用的修改方法名单
    static const std::set<std::string>& arrayMutatingMethods() {
        static const std::set<std::string> s = {"push", "pop", "clear", "set_len", "insert", "remove"};
        return s;
    }

public:
    void run(p<FnNode> fn, const std::string& selfStructName) {
        pushScope();
        if (!selfStructName.empty()) {
            declare("$");
        }
        for (auto& param : fn->header()->params()) {
            auto pname = param->name().getText();
            declare(pname);
            if (param->type()) {
                auto ty = param->type()->getType();
                // T& 参数：根对象就是参数自身——参数作用域 ⊇ 函数体内任何借用
                if (ty.isRef()) {
                    _refToRoot[pname] = pname;
                } else {
                    _rootType[pname] = ty;
                }
            }
        }
        for (auto& s : fn->body()) {
            visitStmt(s);
        }
        popScope();
    }

private:
    void pushScope() {
        _scopes.push_back({});
    }

    void popScope() {
        auto& s = _scopes.back();
        for (auto& n : s.declared) {
            _declared.erase(n);
        }
        for (auto& r : s.borrows) {
            auto it = _refToRoot.find(r);
            if (it != _refToRoot.end()) {
                auto cit = _activeBorrows.find(it->second);
                if (cit != _activeBorrows.end() && cit->second > 0) {
                    cit->second--;
                }
                _refToRoot.erase(it);
            }
        }
        _scopes.pop_back();
    }

    void declare(const std::string& name) {
        _declared.insert(name);
        _scopes.back().declared.push_back(name);
    }

    void registerBorrow(const std::string& refName, const std::string& rootName, int line) {
        if (_declared.find(rootName) == _declared.end()) {
            // 根对象不在当前活跃作用域链中：§8.6.5.1 违规
            throw YuxError(line, ErrorCode::E4002, refName, rootName);
        }
        _refToRoot[refName] = rootName;
        _activeBorrows[rootName]++;
        _scopes.back().borrows.push_back(refName);
    }

    void checkRootReassign(const std::string& name, int line) {
        auto it = _activeBorrows.find(name);
        if (it != _activeBorrows.end() && it->second > 0) {
            throw YuxError(line, ErrorCode::E4003, name);
        }
    }

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
        // §8.6.5.7 as_ref(box) 站点根追溯：根 = box 的根
        if (auto callExpr = dynamic_cast<ExprCallNode*>(expr)) {
            std::string calleeName;
            if (auto litCallee = dynamic_cast<ExprLiteralNode*>(callExpr->getCalleeExpr())) {
                if (auto obj = dynamic_cast<LiteralObjNode*>(litCallee->literal())) {
                    calleeName = obj->getValue().getText();
                }
            }
            if (calleeName == "as_ref" && callExpr->getArgs().size() == 1) {
                auto arg0 = callExpr->getArgs()[0];
                if (auto litArg = dynamic_cast<ExprLiteralNode*>(arg0)) {
                    if (auto obj = dynamic_cast<LiteralObjNode*>(litArg->literal())) {
                        return resolveRoot(obj->getValue().getText());
                    }
                }
            }
        }
        throw YuxError(line, ErrorCode::E4001)
            .withHint("T& 借用初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 `val r T& = as_ref(box)`");
    }

    // ------- 遍历 -------

    void visitBlock(p<StatementBlockNode> blk) {
        if (!blk) return;
        pushScope();
        for (auto& s : blk->statements()) {
            visitStmt(s);
        }
        if (blk->hasResult() && blk->resultExpr()) {
            visitExpr(blk->resultExpr());
        }
        popScope();
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
            declare(decl->name().getText());
            return;
        }

        if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
            auto vname = da->name().getText();
            declare(vname);

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
            } else if (!varType.empty()) {
                _rootType[vname] = varType;
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
        if (auto call = dynamic_cast<ExprCallNode*>(e)) {
            checkArrayMutation(call);
            // 递归实参与 callee 中嵌套的 if/else 块
            if (call->getCalleeExpr()) visitExpr(call->getCalleeExpr());
            for (auto& a : call->getArgs()) {
                if (a) visitExpr(a);
            }
            return;
        }
        // 其余表达式不需深入；本检查不依赖完整数据流。
        // 嵌套 block 只通过 if/else 表达式承载，已上面覆盖。
    }

    // §8.4.2.5：在某 Array<T> 句柄存在活跃借用时，对其调用修改方法应当编译期报错。
    void checkArrayMutation(ExprCallNode* call) {
        auto dotCallee = dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
        if (!dotCallee) return;
        auto methodName = dotCallee->member();
        if (arrayMutatingMethods().find(methodName) == arrayMutatingMethods().end()) return;

        // 提取 receiver 名（仅最简单的 ID.method() 形态；嵌套 / 字段链不在 v0.4 范围）
        auto litBase = dynamic_cast<ExprLiteralNode*>(dotCallee->baseExpr());
        if (!litBase) return;
        auto objLit = dynamic_cast<LiteralObjNode*>(litBase->literal());
        if (!objLit) return;
        auto recvName = objLit->getValue().getText();
        auto rootName = resolveRoot(recvName);

        // 类型必须是 Array<T>
        auto tit = _rootType.find(rootName);
        if (tit == _rootType.end() || !tit->second.isArrayGeneric()) return;

        // 该根对象必须有活跃借用
        auto bit = _activeBorrows.find(rootName);
        if (bit == _activeBorrows.end() || bit->second <= 0) return;

        throw YuxError(call->getLineNumber(), call->getColumn(), ErrorCode::E2010,
                       methodName, recvName)
            .withHint("Array<T>& 借用存活区间内禁止 push / pop / clear / set_len 等修改操作（spec §8.4.2.5）；先让借用结束再修改");
    }
};

} // namespace

void checkBorrows(p<FnNode> fn, const std::string& selfStructName) {
    if (!fn) return;
    BorrowChecker bc;
    bc.run(fn, selfStructName);
}
