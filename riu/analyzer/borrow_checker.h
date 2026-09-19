// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_BORROW_CHECKER_H
#define RIU_LANG_BORROW_CHECKER_H

#include "ast/node/fn_node.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class StatementDeclareAssignNode;
class StatementAssignNode;
class StatementRetNode;
class ExprCallNode;
class ExprNode;
class LambdaExprNode;

// 借用寿命检查（spec §8.6.5 / §8.6.5.8）：块作用域栈，O(1) 局部规则；
// 借用期内根对象不可重赋（§8.6.5.5）。
// Sema 之后由 runFnCheckers 与 const-mut 同趟 walk 调用（见 addAnalysisPasses）。
// selfStructName 非空表示方法（注册 `$` 作为有效根对象名）。
class BorrowChecker {
    // §8.6.5.8 块作用域栈：每层记录本块新增的 declared 名 / 借用名，
    // 弹栈时把它们从 _declared / _refToRoot 摘除并减 _activeBorrows。
    struct Scope {
        std::vector<std::string> declared;
        std::vector<std::string> borrows;
    };
    std::vector<Scope> _scopes;

    std::set<std::string> _declared;
    std::map<std::string, int> _activeBorrows;
    std::map<std::string, std::string> _refToRoot;
    std::unordered_map<std::string, const TypeInfo*> _rootType;

    bool _returnsRef = false;
    std::set<std::string> _returnAllowedSources;
    std::string _returnAllowedDesc;

    bool _returnsHeap = false;
    std::string _returnHeapInnerName;

    FnNode* _fn = nullptr;

    struct LambdaFrame {
        bool returnsRef = false;
        std::set<std::string> allowedSources;
        std::string allowedDesc;
        std::vector<std::string> addedRefs;
    };
    std::vector<LambdaFrame> _lambdaStack;

    static const std::set<std::string>& arrayMutatingMethods();

    void registerBorrow(const std::string& refName, const std::string& rootName, int line);
    void checkRootReassign(const std::string& name, int line);
    [[nodiscard]] std::string resolveRoot(const std::string& name) const;
    std::string rootFromRetExpr(ExprNode* expr, int line);
    std::string rootFromDynBorrowInit(ExprNode* expr, int line);
    std::string rootFromRefInit(ExprNode* expr, int line);

public:
    void begin(FnNode* fn, const std::string& selfStructName);
    void finish();

    void pushScope();
    void popScope();
    void declare(const std::string& name);

    // 本节点检查，不递归。由 fused walk 在子节点之后 / 之前按原顺序调用。
    void afterDeclareAssign(StatementDeclareAssignNode* da, int line);
    void onAssign(StatementAssignNode* as, int line);
    void afterRet(StatementRetNode* ret, int line);
    void onCall(ExprCallNode* call);

    void enterLambda(LambdaExprNode* lam);
    void afterLambdaBodyExpr(LambdaExprNode* lam);
    void leaveLambda();
};

#endif // RIU_LANG_BORROW_CHECKER_H
