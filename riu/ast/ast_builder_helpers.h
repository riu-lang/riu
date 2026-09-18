// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// RdBuilder 共享的注解白名单 / 头部校验 / 表达式辅助。
//
// 每个 .cpp 单独包含本头会各得到一份 anonymous-namespace 副本，
// 等价于原来「同一个 .cpp 内 file-local static」，不破坏链接。
//
// 内容：
//   - knownAnnos / argAnnos / nonFnAllowedAnnos / externFnAllowedAnnos —— 注解白名单
//   - checkNoReturnHeader / checkFallibleRetMismatch —— fn 头部校验
//   - peelFallibleRetType —— `T ! E` 拆回 base + err
//   - exprContainsTryCatch —— 全局 init 禁 try/catch（E3155）

#ifndef RIU_LANG_AST_BUILDER_HELPERS_H
#define RIU_LANG_AST_BUILDER_HELPERS_H

#include "node/expr_node.h"
#include "node/fn_node.h"
#include "node/statement_node.h"
#include "node/type_node.h"
#include "types.h"

#include <set>
#include <utility>

namespace {

// 已知的构建注解名字白名单；未知注解在 AST 构建期报错
// #NoReturn 由 DRAFT-错误.md 引入（spec §11.5.1）：
//   #NoReturn        零参；标在 fn / structImpl 内方法上
// 可失败签名 `T ! E` 走 fnHeader / lambda 后缀，不再使用 `#Fallible` 注解（F6 删除）。
// spec-unify v1（[#1.AD]）新增：
//   #Spec            零参；标在 struct 上 — 把声明转为 spec（仅签名）
//   #Impl(SpecName)  单参；标在 struct 上 — 实现关系，替代旧 `: D1 + D2` 头部槽
inline const set<string>& knownAnnos() {
    static const set<string> s = {"Builtin", "Test", "DraftLike", "NoReturn", "Const", "Static",
                                  "Spec",    "Impl", "Reflect",   "NoCopy",   "CName"};
    return s;
}

// 单参注解白名单（spec §11.1.1.1）。其它注解出现 (arg) 形式视为非法（E2005 形式错配）。
inline const set<string>& argAnnos() {
    static const set<string> s = {"Impl", "CName"};
    return s;
}

// 注解可附着位置的限定集合
// fn 之外的位置（structDecl / extern / globalConst）只接受 #Builtin / #Spec / #Impl，
// 不接受 #Test（spec §11.3.1.2）
inline const set<string>& nonFnAllowedAnnos() {
    static const set<string> s = {"Builtin", "Spec", "Impl", "Reflect", "NoCopy"};
    return s;
}

// 用于 extern 块内 fnHeader：允许 `Builtin` 与 `#NoReturn`（DRAFT-错误.md §8.3）。
// 可失败签名 `T ! E` 在 extern 上仍被推迟（[#7]），不在白名单。
inline const set<string>& externFnAllowedAnnos() {
    static const set<string> s = {"Builtin", "NoReturn", "CName"};
    return s;
}

// Phase 10d-1：`#NoReturn` 头部级语义校验（E7012 / E7013）
// 不依赖 fn body，仅看 header 注解 + retType。E7014（流终止）与调用点流终止注册推 10d-2。
//   E7012 — `#NoReturn` 函数声明带返回类型
//   E7013 — `#NoReturn` 与 `T ! E` 互斥
static void checkNoReturnHeader(FnHeaderNode* header) {
    if (!header->hasAnno("NoReturn")) return;
    int line = header->getLineNumber();
    int col = header->getColumn();
    if (header->retType()) {
        throw RiuError(line, col, ErrorCode::E7012, header->name().getText());
    }
    if (!header->resolvedFallibleErr().empty()) {
        throw RiuError(line, col, ErrorCode::E7013);
    }
}

static void checkFallibleRetMismatch(FnHeaderNode* header) {
    const string err = header->resolvedFallibleErr();
    if (err.empty() || !header->retType()) return;
    const TypeInfo retType = header->retType()->getType().withoutFallible();
    if (retType.getFullName() == err) {
        throw RiuError(header->getLineNumber(), header->getColumn(), ErrorCode::E7008, retType.getFullName(), err);
    }
}

// typeFallibleWithRef 在 fn / lambda 返回位会把 `T ! E` 合成单节点；拆回 base + err 槽。
inline std::pair<TypeNode*, TypeNode*> peelFallibleRetType(TypeNode* retType) {
    if (!retType) return {nullptr, nullptr};
    if (auto* f = dynamic_cast<TypeFallibleNode*>(retType)) {
        TypeNode* base = f->baseType();
        // `! E` / `() ! E`：unit 不是成功通道类型。
        if (auto* tup = dynamic_cast<TypeTupleNode*>(base)) {
            if (tup->elementTypes().empty()) base = nullptr;
        }
        return {base, f->errType()};
    }
    return {retType, nullptr};
}

inline bool exprContainsTryCatch(ExprNode* expr) {
    if (!expr) return false;

    if (dynamic_cast<ExprTryCatchNode*>(expr)) return true;

    if (auto* bin = dynamic_cast<ExprBinOpNode*>(expr)) {
        return exprContainsTryCatch(bin->left()) || exprContainsTryCatch(bin->right());
    }

    if (auto* ne = dynamic_cast<ExprNullElseNode*>(expr)) {
        return exprContainsTryCatch(ne->left()) || exprContainsTryCatch(ne->right());
    }

    if (auto* un = dynamic_cast<ExprUnaryNode*>(expr)) {
        return exprContainsTryCatch(un->right());
    }

    if (auto* call = dynamic_cast<ExprCallNode*>(expr)) {
        if (exprContainsTryCatch(call->getCalleeExpr())) return true;
        for (auto& a : call->getArgs()) {
            if (exprContainsTryCatch(a)) return true;
        }
        return false;
    }

    if (auto* dot = dynamic_cast<ExprDotNode*>(expr)) {
        return exprContainsTryCatch(dot->baseExpr());
    }

    if (auto* ol = dynamic_cast<ExprOneLineIfElseNode*>(expr)) {
        if (exprContainsTryCatch(ol->condition())) return true;
        if (exprContainsTryCatch(ol->trueValue())) return true;
        if (exprContainsTryCatch(ol->falseValue())) return true;
        return false;
    }

    if (auto* paren = dynamic_cast<ExprParenNode*>(expr)) {
        return exprContainsTryCatch(paren->expr());
    }

    if (auto* arr = dynamic_cast<ExprArrayNode*>(expr)) {
        for (auto& e : arr->elements()) {
            if (exprContainsTryCatch(e)) return true;
        }
        return false;
    }

    if (auto* m = dynamic_cast<ExprMatchNode*>(expr)) {
        if (exprContainsTryCatch(m->scrutinee())) return true;
        for (auto& arm : m->arms()) {
            if (arm->hasBlock()) {
                auto* blk = arm->block();
                if (blk->hasResult() && exprContainsTryCatch(blk->resultExpr())) return true;
                for (auto& s : blk->statements()) {
                    if (auto se = dynamic_cast<StatementExprNode*>(s)) {
                        if (exprContainsTryCatch(se->expr())) return true;
                    }
                }
            } else if (exprContainsTryCatch(arm->body())) {
                return true;
            }
        }
        return false;
    }

    if (auto* sl = dynamic_cast<ExprStructLitNode*>(expr)) {
        if (sl->positional() && exprContainsTryCatch(sl->positional())) return true;
        for (auto& fi : sl->fields()) {
            if (exprContainsTryCatch(fi->value())) return true;
        }
        return false;
    }
    if (auto* dynCtor = dynamic_cast<ExprDynCtorNode*>(expr)) {
        return exprContainsTryCatch(dynCtor->arg());
    }
    if (auto* pc = dynamic_cast<ExprPathCallNode*>(expr)) {
        for (auto& a : pc->args()) {
            if (exprContainsTryCatch(a)) return true;
        }
        return false;
    }

    if (auto* g = dynamic_cast<ExprGetNode*>(expr)) {
        if (exprContainsTryCatch(g->arrayExpr())) return true;
        for (auto& idx : g->indices()) {
            if (exprContainsTryCatch(idx)) return true;
        }
        return false;
    }

    if (auto* tup = dynamic_cast<ExprTupleNode*>(expr)) {
        for (auto& e : tup->elements()) {
            if (exprContainsTryCatch(e)) return true;
        }
        return false;
    }

    return false;
}

} // namespace

#endif // RIU_LANG_AST_BUILDER_HELPERS_H
