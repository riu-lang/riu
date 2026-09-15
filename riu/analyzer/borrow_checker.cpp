// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "borrow_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/type_node.h"

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

    // 当前函数返回 T& 时启用：每条 ret expr 的根必须 ∈ _returnAllowedSources。
    bool _returnsRef = false;
    std::set<std::string> _returnAllowedSources;
    // 用于 E4020 错误消息中的人类可读描述（如 "`$`" 或 "T& parameter `p`"）。
    std::string _returnAllowedDesc;

    // DRAFT-heap-types §8.3a.4.1 (Phase 2.8)：fn 返回 Heap<T> 时启用
    // v1 无 NRVO，任意 `ret <expr>` 一律 E4023。Phase 3 NRVO 落地后再放宽。
    bool _returnsHeap = false;
    std::string _returnHeapInnerName; // 错误消息里的 T

    // §8.4.2.5 Array<T> 借用期不可调用的修改方法名单
    static const std::set<std::string>& arrayMutatingMethods() {
        static const std::set<std::string> s = {"push",   "pop",       "clear",   "set_len", "insert",
                                                "remove", "remove_at", "reverse", "reserve"};
        return s;
    }

    // DRAFT-static-ref: 持有当前函数节点，用于 lookupSymbol 判断名字是否文件级全局
    FnNode* _fn = nullptr;

public:
    void run(FnNode* fn, const std::string& selfStructName) {
        _fn = fn;
        pushScope();
        bool isMethod = !selfStructName.empty();
        if (isMethod) {
            declare("$");
        }
        for (auto& param : fn->header()->params()) {
            auto pname = param->name().getText();
            declare(pname);
            if (param->type()) {
                auto ty = param->type()->getType();
                // T& / Dyn<D&> 形参：根即参数自身。Dyn<D&> 不是 Ref<T>，不能作 T& 返回源。
                if (ty.isRef() || ty.isDynBorrow()) {
                    _refToRoot[pname] = pname;
                } else {
                    _rootType[pname] = ty;
                }
            }
        }

        // DRAFT-heap-types §8.3a.4.1 (Phase 2.8)：fn 返回 Heap<T> 即标记
        // _returnsHeap，每条 ret expr 触发 E4023（NRVO 留待 Phase 3）。
        if (fn->header()->retType() && fn->header()->retType()->getType().isHeap()) {
            _returnsHeap = true;
            auto inner = fn->header()->retType()->getType().heapElementType();
            _returnHeapInnerName = inner ? inner->name : std::string("?");
        }

        // 返回 T& 的溯源约束（spec §8.6.10）：
        //   - 方法：源恒为 `$`（Self& / 字段 T&）；T& 形参不可作返回根
        //   - 自由函数 / lambda：只允许 `$rodata`（静态 / 全局）；形参透传改 E4020
        if (fn->header()->retType() && fn->header()->retType()->getType().isRef()) {
            _returnsRef = true;
            if (isMethod) {
                _returnAllowedSources.insert("$");
                _returnAllowedDesc = "`$`";
            } else {
                _returnAllowedDesc = "global/static reference";
            }
            // $rodata (全局/静态引用) 永不过期，始终允许作为 T& 返回源
            _returnAllowedSources.insert("$rodata");
        }

        for (auto& s : fn->body()) {
            visitStmt(s);
        }
        popScope();
    }

private:
    void pushScope() { _scopes.push_back({}); }

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
        // $rodata: immortal root (reflect rodata / static data), never goes out of scope
        if (rootName == "$rodata") {
            _refToRoot[refName] = rootName;
            _scopes.back().borrows.push_back(refName);
            return;
        }
        if (_declared.find(rootName) == _declared.end()) {
            // 根对象不在当前活跃作用域链中：§8.6.5.1 违规
            throw RiuError(line, ErrorCode::E4002, refName, rootName);
        }
        _refToRoot[refName] = rootName;
        _activeBorrows[rootName]++;
        _scopes.back().borrows.push_back(refName);
    }

    void checkRootReassign(const std::string& name, int line) {
        auto it = _activeBorrows.find(name);
        if (it != _activeBorrows.end() && it->second > 0) {
            throw RiuError(line, ErrorCode::E4003, name);
        }
    }

    [[nodiscard]] std::string resolveRoot(const std::string& name) const {
        auto it = _refToRoot.find(name);
        return it != _refToRoot.end() ? it->second : name;
    }

    // ret 表达式溯源：复用借用初始化推根逻辑。
    // 接受形态：
    //   - `ret $`               → 根 = "$"（LiteralObj 路径，resolveRoot 兜底）
    //   - `ret &$.f` / `ret &p.f.f` → 根 = `$` / `p`
    //   - `ret r`               → r 是 T& 局部 / 形参，解链到根
    //   - `ret as_ref(box)`     → 根 = box 的根
    //   - `ret recv.foo(...)` / `ret f(args)` 返 T& → P3 扩展（rootFromRefInit 暂不识别会抛 E4001，
    //     由 P3 在 ExprCallNode 分支补齐）
    std::string rootFromRetExpr(ExprNode* expr, int line) { return rootFromRefInit(expr, line); }

    // Phase 2e: `val d Dyn<D&> = Dyn:<D&>(x)` 的根推导.
    // 期望 RHS 是 ExprDynCtorNode(isBorrow=true); x 形态在 Phase 2b 限定为:
    //   - U& 形态 (ExprGetRefNode `&y.f` 或 T& 拷绑 `r`) → 根 = y / resolveRoot(r)
    //   - Rc<U> 形态 (LiteralObj 变量名) → 根 = 该 Rc 变量自身
    //   (其它形态构造站已 E1133 拒绝; 这里到不了)
    // 非 DynCtor RHS (例如 Dyn<D&> 参数 / 局部之间的拷绑) 走 refToRoot 链.
    std::string rootFromDynBorrowInit(ExprNode* expr, int line) {
        if (auto ctor = dynamic_cast<ExprDynCtorNode*>(expr)) {
            auto inner = ctor->arg();
            if (auto getRef = dynamic_cast<ExprGetRefNode*>(inner)) {
                return resolveRoot(getRef->obj().getText());
            }
            if (auto litExpr = dynamic_cast<ExprLiteralNode*>(inner)) {
                if (auto obj = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                    return resolveRoot(obj->getValue().getText());
                }
            }
            // 兜底: 落到 E4001 (借用初始化形态不被识别)
            throw RiuError(line, ErrorCode::E4001).withHint("Dyn<D&>(...) 的参数应为 `&y.f` / T& 变量 / Rc<U> 变量名");
        }
        // RHS 是已有 Dyn<D&> 变量 (拷绑形态): 顺 refToRoot 链解根
        if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                return resolveRoot(obj->getValue().getText());
            }
        }
        throw RiuError(line, ErrorCode::E4001)
            .withHint("Dyn<D&> 局部应由 `Dyn:<D&>(x)` 构造或从已有 Dyn<D&> 变量 / 形参拷绑");
    }

    // 从 `var r T& = expr` 的 RHS 推根对象名。
    // - &x.f.f → 根 = x
    // - 现有 T& 拷绑（LiteralObj 单 ID）→ 根 = 该 ref 的链上根
    // 其他形式（grammar 不允许，到这里也兜底报错）。
    std::string rootFromRefInit(ExprNode* expr, int line) {
        if (auto getRef = dynamic_cast<ExprGetRefNode*>(expr)) {
            auto name = getRef->obj().getText();
            auto resolved = resolveRoot(name);
            // DRAFT-static-ref: 若 name 非局部/非 T& 但文件级符号存在 → 全局变量 → immortal
            if (resolved == name && _declared.find(name) == _declared.end()) {
                if (_fn && _fn->lookupSymbol(name)) {
                    return "$rodata";
                }
            }
            return resolved;
        }
        if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                auto name = obj->getValue().getText();
                // 拷绑：name 必定是 T&，refToRoot 应有记录。退路：当作 name 自己。
                return resolveRoot(name);
            }
        }
        // [T& * N] 索引返回 T&: arr[i] → 通过 arr 追根
        if (auto getNode = dynamic_cast<ExprGetNode*>(expr)) {
            if (getNode->getType().isRef()) {
                auto* arrExpr = getNode->arrayExpr();
                // 变量数组: fs[0] 其中 fs 是 T& 局部变量
                if (auto litArr = dynamic_cast<ExprLiteralNode*>(arrExpr)) {
                    if (auto obj = dynamic_cast<LiteralObjNode*>(litArr->literal())) {
                        return resolveRoot(obj->getValue().getText());
                    }
                }
                // 静态路径: Counter::fields[0] → rodata, 使用 immortal sentinel
                if (dynamic_cast<ExprPathCallNode*>(arrExpr)) {
                    return "$rodata";
                }
                // 链式索引: arr[i][j]
                return rootFromRefInit(arrExpr, line);
            }
        }
        // 静态路径返回 T&: Counter::fields / Counter::type → rodata reference
        if (auto path = dynamic_cast<ExprPathCallNode*>(expr)) {
            if (path->getType().isRef()) {
                return "$rodata";
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
            // 用户函数返回 T&：
            //   方法调用 recv.foo(...) → 根 = recv 的根（方法源恒为 $）
            //   自由函数调用 f(args)   → 根 = `$rodata`
            if (callExpr->getType().isRef()) {
                if (auto dot = dynamic_cast<ExprDotNode*>(callExpr->getCalleeExpr())) {
                    if (auto litBase = dynamic_cast<ExprLiteralNode*>(dot->baseExpr())) {
                        if (auto recvObj = dynamic_cast<LiteralObjNode*>(litBase->literal())) {
                            return resolveRoot(recvObj->getValue().getText());
                        }
                    }
                    // 复杂 receiver（嵌套调用 / 字段链）：v1 不支持，落到错误兜底
                } else {
                    // 自由函数定义侧只允许 `$rodata`；调用点不再把 T& 实参当返回根
                    return "$rodata";
                }
                return "$rodata";
            }
        }
        throw RiuError(line, ErrorCode::E4001)
            .withHint(
                "T& 借用初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 `val r T& = as_ref(box)`");
    }

    // ------- 遍历 -------

    void visitBlock(StatementBlockNode* blk) {
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

    void visitStmt(StatementNode* s) {
        if (!s) return;

        if (auto blk = dynamic_cast<StatementBlockNode*>(s)) {
            visitBlock(static_cast<StatementBlockNode*>(blk));
            return;
        }

        if (auto loop = dynamic_cast<StatementLoopNode*>(s)) {
            // loop init 与 body 共享同一块作用域
            pushScope();
            for (auto& n : loop->initNames()) {
                declare(n.getText());
            }
            if (auto blk = loop->block()) {
                for (auto& st : blk->statements()) {
                    visitStmt(st);
                }
                if (blk->hasResult() && blk->resultExpr()) {
                    visitExpr(blk->resultExpr());
                }
            }
            popScope();
            return;
        }

        if (auto forin = dynamic_cast<StatementForInNode*>(s)) {
            visitExpr(forin->expr());
            pushScope();
            declare(forin->item().getText());
            if (auto blk = forin->block()) {
                for (auto& st : blk->statements()) {
                    visitStmt(st);
                }
                if (blk->hasResult() && blk->resultExpr()) {
                    visitExpr(blk->resultExpr());
                }
            }
            popScope();
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
            } else if (varType.isDynBorrow() && da->expr()) {
                // Phase 2e: Dyn<D&> 局部变量是借用形态 (fat ptr 的 data 槽借用源),
                // 与 T& 同样登记 refToRoot + activeBorrows; 根从 Dyn:<D&>(x) 的
                // x 反推. x 形态在 Phase 2b 已限定为 U& / Rc<U>.
                auto root = rootFromDynBorrowInit(da->expr(), s->getLineNumber());
                registerBorrow(vname, root, s->getLineNumber());
            } else if (!varType.empty()) {
                // DRAFT-heap-types §8.3a.3.2 (Phase 2.8)：Heap<T> 单所有权，
                // 不允许 `let h2 Heap<T> = h1` 这类 by-value move（会双释放）；
                // RHS 必须是 heap:<T>(...) 构造调用（ExprCallNode）。
                // Phase 3 引入 Heap<T>? 可移动槽后，由 nullable 路径接管 move。
                if (varType.isHeap()) {
                    bool ok = false;
                    // Phase 3c: 函数返回 Heap<T> / heap:<T>(...) builtin 调用走 NRVO 移交
                    if (auto call = dynamic_cast<ExprCallNode*>(da->expr())) {
                        auto rt = call->getType();
                        if (rt.isHeap() && rt == varType) ok = true;
                    }
                    if (!ok) {
                        auto inner = varType.heapElementType();
                        std::string innerName = inner ? inner->name : std::string("?");
                        throw RiuError(s->getLineNumber(), ErrorCode::E4024, innerName, vname, innerName);
                    }
                }
                // DRAFT-heap-types §8.3a.4.3 (Phase 3d.1)：Heap<T>? 槽的 RHS 形态白名单。
                // 合法：null 字面量 / 同型 ExprCallNode（heap_some / heap_null / 用户 fn 返回 Heap<T>?）。
                // 禁止：Heap<T> 隐式 widen（E4027）；Heap<T>? lvalue 按值 move（E4024，B 档落地前一律拦）。
                else if (varType.isNullable() && da->expr()) {
                    auto inner = varType.nullableInnerType();
                    if (inner && inner->isHeap()) {
                        bool ok = isFlexibleNullExpr(da->expr());
                        if (!ok) {
                            if (auto call = dynamic_cast<ExprCallNode*>(da->expr())) {
                                if (call->getType() == varType) ok = true;
                            }
                        }
                        if (!ok) {
                            auto heapInner = inner->heapElementType();
                            std::string innerName = heapInner ? heapInner->name : std::string("?");
                            auto exprType = da->expr()->getType();
                            if (exprType.isHeap()) {
                                throw RiuError(s->getLineNumber(), ErrorCode::E4027, innerName, innerName);
                            }
                            throw RiuError(s->getLineNumber(), ErrorCode::E4024, innerName, vname, innerName);
                        }
                    }
                }
                _rootType[vname] = varType;
            }
            return;
        }

        if (auto as = dynamic_cast<StatementAssignNode*>(s)) {
            // 顶层重赋（无字段 subs）才视作"根对象赋值"。
            // 字段写 / 通过 T& 写不算根对象本身的重赋。
            if (as->subs().empty()) {
                auto lhsName = as->obj().getText();
                checkRootReassign(lhsName, s->getLineNumber());
                // DRAFT-heap-types §8.3a.3.2 (Phase 2.8)：Heap<T> 局部重赋
                // RHS 必须是 heap:<T>(...) 构造调用（同 decl-assign 理由）。
                auto rit = _rootType.find(lhsName);
                if (rit != _rootType.end() && rit->second.isHeap() && as->expr()) {
                    bool ok = false;
                    if (auto call = dynamic_cast<ExprCallNode*>(as->expr())) {
                        auto rt = call->getType();
                        if (rt.isHeap() && rt == rit->second) ok = true;
                    }
                    if (!ok) {
                        auto inner = rit->second.heapElementType();
                        std::string innerName = inner ? inner->name : std::string("?");
                        throw RiuError(s->getLineNumber(), ErrorCode::E4024, innerName, lhsName, innerName);
                    }
                }
                // Phase 3d.1：Heap<T>? 顶层重赋同样走白名单（同上）。
                else if (rit != _rootType.end() && rit->second.isNullable() && as->expr()) {
                    auto inner = rit->second.nullableInnerType();
                    if (inner && inner->isHeap()) {
                        bool ok = isFlexibleNullExpr(as->expr());
                        if (!ok) {
                            if (auto call = dynamic_cast<ExprCallNode*>(as->expr())) {
                                if (call->getType() == rit->second) ok = true;
                            }
                        }
                        if (!ok) {
                            auto heapInner = inner->heapElementType();
                            std::string innerName = heapInner ? heapInner->name : std::string("?");
                            auto exprType = as->expr()->getType();
                            if (exprType.isHeap()) {
                                throw RiuError(s->getLineNumber(), ErrorCode::E4027, innerName, innerName);
                            }
                            throw RiuError(s->getLineNumber(), ErrorCode::E4024, innerName, lhsName, innerName);
                        }
                    }
                }
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
            if (ret->expr()) {
                visitExpr(ret->expr());
                if (_returnsRef) {
                    auto root = rootFromRetExpr(ret->expr(), s->getLineNumber());
                    if (_returnAllowedSources.find(root) == _returnAllowedSources.end()) {
                        throw RiuError(s->getLineNumber(), ErrorCode::E4020, _returnAllowedDesc, root);
                    }
                }
                // DRAFT-heap-types §8.3a.4 (Phase 3c)：A 档 NRVO（最小集）。
                // 仅放行 `ret <ID>`，其中 ID 是匹配返回类型的局部 Heap<T>；
                // 其他形态（fresh ctor、调用、字段等）仍报 E4023 escape。
                // 多 ret：每条独立，各自接管自身 slot，无需多源汇合分析。
                // 外发借用 vs ret：留下一切片（依赖 §8.6.5 借用流分析延伸）。
                if (_returnsHeap) {
                    bool nrvoEligible = false;
                    std::string name = "<expr>";
                    if (auto litE = dynamic_cast<ExprLiteralNode*>(ret->expr())) {
                        if (auto obj = dynamic_cast<LiteralObjNode*>(litE->literal())) {
                            name = obj->getValue().getText();
                            auto it = _rootType.find(name);
                            if (it != _rootType.end() && it->second.isHeap()) {
                                auto inner = it->second.heapElementType();
                                if (inner && inner->name == _returnHeapInnerName) {
                                    nrvoEligible = true;
                                }
                            }
                        }
                    }
                    if (!nrvoEligible) {
                        throw RiuError(s->getLineNumber(), ErrorCode::E4023, _returnHeapInnerName, name);
                    }
                }
            }
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
    void visitExpr(ExprNode* e) {
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
        if (auto lam = dynamic_cast<LambdaExprNode*>(e)) {
            visitLambda(lam);
            return;
        }
        if (auto m = dynamic_cast<ExprMatchNode*>(e)) {
            if (m->scrutinee()) visitExpr(m->scrutinee());
            for (auto& arm : m->arms()) {
                if (!arm) continue;
                if (arm->hasBlock())
                    visitBlock(arm->block());
                else if (arm->body())
                    visitExpr(arm->body());
            }
            return;
        }
        // 其余表达式不需深入；本检查不依赖完整数据流。
        // 嵌套 block 通过 if/else / match / lambda 承载，已上面覆盖。
    }

    // Phase 4e（spec §6.5）：lambda body 借用检查。
    // - 捕获来的 T& 不进允许源集
    // - lambda 返回 T& 只允许 `$rodata`；形参透传 → E4020
    void visitLambda(LambdaExprNode* lam) {
        if (!lam) return;
        pushScope();

        // 保存外层 ret-ref 状态（避免 lambda 体内 ret 被外层规则误检）
        bool savedReturnsRef = _returnsRef;
        auto savedAllowedSources = _returnAllowedSources;
        auto savedAllowedDesc = _returnAllowedDesc;
        _returnsRef = false;
        _returnAllowedSources.clear();
        _returnAllowedDesc.clear();

        // 注册 lambda 形参：T& 形参参与 refToRoot，与外层 fn 同路径处理
        std::vector<std::string> addedRefs; // 待回滚
        for (auto& slot : lam->params()) {
            auto pname = slot.name.getText();
            declare(pname);
            if (slot.type) {
                auto ty = slot.type->getType();
                if (ty.isRef()) {
                    _refToRoot[pname] = pname;
                    addedRefs.push_back(pname);
                } else {
                    _rootType[pname] = ty;
                }
            }
        }

        bool lamReturnsRef = lam->retType() && lam->retType()->getType().isRef();
        if (lamReturnsRef) {
            _returnsRef = true;
            _returnAllowedDesc = "global/static reference";
            _returnAllowedSources.insert("$rodata");
        }

        // 遍历 body：表达式体视作隐式 ret；语句体走 stmt 通路
        if (lam->bodyExpr()) {
            visitExpr(lam->bodyExpr());
            if (_returnsRef) {
                auto root = rootFromRetExpr(lam->bodyExpr(), lam->getLineNumber());
                if (_returnAllowedSources.find(root) == _returnAllowedSources.end()) {
                    throw RiuError(lam->getLineNumber(), ErrorCode::E4020, _returnAllowedDesc, root);
                }
            }
        } else {
            for (auto& s : lam->bodyStmts()) {
                visitStmt(s);
            }
        }

        // 摘除 lambda T& 形参 refToRoot 记录
        for (auto& n : addedRefs) {
            _refToRoot.erase(n);
        }
        // 恢复 ret-ref 状态
        _returnsRef = savedReturnsRef;
        _returnAllowedSources = std::move(savedAllowedSources);
        _returnAllowedDesc = std::move(savedAllowedDesc);

        popScope();
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

        throw RiuError(call->getLineNumber(), call->getColumn(), ErrorCode::E2010, methodName, recvName)
            .withHint("Array<T>& 借用存活区间内禁止 push / pop / clear / set_len 等修改操作（spec "
                      "§8.4.2.5）；先让借用结束再修改");
    }
};

} // namespace

void checkBorrows(FnNode* fn, const std::string& selfStructName) {
    if (!fn) return;
    BorrowChecker bc;
    bc.run(fn, selfStructName);
}
