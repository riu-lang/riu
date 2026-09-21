// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "borrow_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/type_node.h"

#include <utility>

const std::set<std::string>& BorrowChecker::arrayMutatingMethods() {
    static const std::set<std::string> s = {"push",   "pop",       "clear",   "set_len", "insert",
                                            "remove", "remove_at", "reverse", "reserve"};
    return s;
}

void BorrowChecker::begin(FnNode* fn, const std::string& selfStructName) {
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
            const TypeInfo& ty = param->type()->getType();
            // T& / Dyn<D&> 形参：根即参数自身。Dyn<D&> 不是 Ref<T>，不能作 T& 返回源。
            if (ty.isRef() || ty.isDynBorrow()) {
                _refToRoot[pname] = pname;
            } else {
                _rootType[pname] = &ty;
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
}

void BorrowChecker::finish() {
    popScope();
}

void BorrowChecker::pushScope() {
    _scopes.push_back({});
}

void BorrowChecker::popScope() {
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

void BorrowChecker::declare(const std::string& name) {
    _declared.insert(name);
    _scopes.back().declared.push_back(name);
}

void BorrowChecker::registerBorrow(const std::string& refName, const std::string& rootName, int line) {
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

void BorrowChecker::checkRootReassign(const std::string& name, int line) {
    auto it = _activeBorrows.find(name);
    if (it != _activeBorrows.end() && it->second > 0) {
        throw RiuError(line, ErrorCode::E4003, name);
    }
}

std::string BorrowChecker::resolveRoot(const std::string& name) const {
    auto it = _refToRoot.find(name);
    return it != _refToRoot.end() ? it->second : name;
}

std::string BorrowChecker::rootFromRetExpr(ExprNode* expr, int line) {
    return rootFromRefInit(expr, line);
}

std::string BorrowChecker::rootFromDynBorrowInit(ExprNode* expr, int line) {
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

std::string BorrowChecker::rootFromRefInit(ExprNode* expr, int line) {
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
    // 静态路径返回 T&: Counter::fields / Counter::type_info → rodata reference
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
        if (calleeName == "overlay" && callExpr->getArgs().size() == 1) {
            return rootFromRefInit(callExpr->getArgs()[0], line);
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
        .withHint("T& 借用初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 `val r T& = "
                  "as_ref(box)` / `overlay:<U>(&x)`");
}

void BorrowChecker::afterDeclareAssign(StatementDeclareAssignNode* da, int line) {
    if (!da) return;
    auto vname = da->name().getText();

    TypeInfo varType;
    if (da->varType()) {
        varType = da->varType()->getType();
    } else if (da->expr()) {
        varType = da->expr()->getType();
    }
    if (varType.isRef() && da->expr()) {
        auto root = rootFromRefInit(da->expr(), line);
        registerBorrow(vname, root, line);
    } else if (varType.isDynBorrow() && da->expr()) {
        // Phase 2e: Dyn<D&> 局部变量是借用形态 (fat ptr 的 data 槽借用源),
        // 与 T& 同样登记 refToRoot + activeBorrows; 根从 Dyn:<D&>(x) 的
        // x 反推. x 形态在 Phase 2b 已限定为 U& / Rc<U>.
        auto root = rootFromDynBorrowInit(da->expr(), line);
        registerBorrow(vname, root, line);
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
                throw RiuError(line, ErrorCode::E4024, innerName, vname, innerName);
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
                        throw RiuError(line, ErrorCode::E4027, innerName, innerName);
                    }
                    throw RiuError(line, ErrorCode::E4024, innerName, vname, innerName);
                }
            }
        }
        _rootType[vname] = &internType(varType);
    }
}

void BorrowChecker::onAssign(StatementAssignNode* as, int line) {
    if (!as) return;
    // 顶层重赋（无字段 subs）才视作"根对象赋值"。
    // 字段写 / 通过 T& 写不算根对象本身的重赋。
    if (!as->subs().empty()) return;
    auto lhsName = as->obj().getText();
    checkRootReassign(lhsName, line);
    // DRAFT-heap-types §8.3a.3.2 (Phase 2.8)：Heap<T> 局部重赋
    // RHS 必须是 heap:<T>(...) 构造调用（同 decl-assign 理由）。
    auto rit = _rootType.find(lhsName);
    if (rit != _rootType.end() && rit->second->isHeap() && as->expr()) {
        bool ok = false;
        if (auto call = dynamic_cast<ExprCallNode*>(as->expr())) {
            auto rt = call->getType();
            if (rt.isHeap() && rt == *rit->second) ok = true;
        }
        if (!ok) {
            auto inner = rit->second->heapElementType();
            std::string innerName = inner ? inner->name : std::string("?");
            throw RiuError(line, ErrorCode::E4024, innerName, lhsName, innerName);
        }
    }
    // Phase 3d.1：Heap<T>? 顶层重赋同样走白名单（同上）。
    else if (rit != _rootType.end() && rit->second->isNullable() && as->expr()) {
        auto inner = rit->second->nullableInnerType();
        if (inner && inner->isHeap()) {
            bool ok = isFlexibleNullExpr(as->expr());
            if (!ok) {
                if (auto call = dynamic_cast<ExprCallNode*>(as->expr())) {
                    if (call->getType() == *rit->second) ok = true;
                }
            }
            if (!ok) {
                auto heapInner = inner->heapElementType();
                std::string innerName = heapInner ? heapInner->name : std::string("?");
                auto exprType = as->expr()->getType();
                if (exprType.isHeap()) {
                    throw RiuError(line, ErrorCode::E4027, innerName, innerName);
                }
                throw RiuError(line, ErrorCode::E4024, innerName, lhsName, innerName);
            }
        }
    }
}

void BorrowChecker::afterRet(StatementRetNode* ret, int line) {
    if (!ret || !ret->expr()) return;
    if (_returnsRef) {
        auto root = rootFromRetExpr(ret->expr(), line);
        if (_returnAllowedSources.find(root) == _returnAllowedSources.end()) {
            throw RiuError(line, ErrorCode::E4020, _returnAllowedDesc, root);
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
                if (it != _rootType.end() && it->second->isHeap()) {
                    auto inner = it->second->heapElementType();
                    if (inner && inner->name == _returnHeapInnerName) {
                        nrvoEligible = true;
                    }
                }
            }
        }
        if (!nrvoEligible) {
            throw RiuError(line, ErrorCode::E4023, _returnHeapInnerName, name);
        }
    }
}

void BorrowChecker::onCall(ExprCallNode* call) {
    if (!call) return;
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
    if (tit == _rootType.end() || !tit->second->isArrayGeneric()) return;

    // 该根对象必须有活跃借用
    auto bit = _activeBorrows.find(rootName);
    if (bit == _activeBorrows.end() || bit->second <= 0) return;

    throw RiuError(call->getLineNumber(), call->getColumn(), ErrorCode::E2010, methodName, recvName)
        .withHint("Array<T>& 借用存活区间内禁止 push / pop / clear / set_len 等修改操作（spec "
                  "§8.4.2.5）；先让借用结束再修改");
}

void BorrowChecker::enterLambda(LambdaExprNode* lam) {
    if (!lam) return;
    pushScope();

    LambdaFrame saved;
    saved.returnsRef = _returnsRef;
    saved.allowedSources = _returnAllowedSources;
    saved.allowedDesc = _returnAllowedDesc;
    _returnsRef = false;
    _returnAllowedSources.clear();
    _returnAllowedDesc.clear();

    for (auto& slot : lam->params()) {
        auto pname = slot.name.getText();
        declare(pname);
        if (slot.type) {
            const TypeInfo& ty = slot.type->getType();
            if (ty.isRef()) {
                _refToRoot[pname] = pname;
                saved.addedRefs.push_back(pname);
            } else {
                _rootType[pname] = &ty;
            }
        }
    }

    bool lamReturnsRef = lam->retType() && lam->retType()->getType().isRef();
    if (lamReturnsRef) {
        _returnsRef = true;
        _returnAllowedDesc = "global/static reference";
        _returnAllowedSources.insert("$rodata");
    }
    _lambdaStack.push_back(std::move(saved));
}

void BorrowChecker::afterLambdaBodyExpr(LambdaExprNode* lam) {
    if (!lam || !_returnsRef || !lam->bodyExpr()) return;
    auto root = rootFromRetExpr(lam->bodyExpr(), lam->getLineNumber());
    if (_returnAllowedSources.find(root) == _returnAllowedSources.end()) {
        throw RiuError(lam->getLineNumber(), ErrorCode::E4020, _returnAllowedDesc, root);
    }
}

void BorrowChecker::leaveLambda() {
    if (_lambdaStack.empty()) return;
    auto saved = std::move(_lambdaStack.back());
    _lambdaStack.pop_back();
    for (auto& n : saved.addedRefs) {
        _refToRoot.erase(n);
    }
    _returnsRef = saved.returnsRef;
    _returnAllowedSources = std::move(saved.allowedSources);
    _returnAllowedDesc = std::move(saved.allowedDesc);
    popScope();
}
