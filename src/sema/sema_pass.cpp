// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 实现 —— 详见 sema_pass.h
//
// Phase 3.2a：visitExpr 在每个表达式节点上写入 `setResolvedType(getType())`,
// 覆盖范围扩到 file 顶层 fn body + struct impl 的方法/析构 body。
// 泛型模板 / #CompilerInner 仍跳过 —— 它们的 codegen 路径会自行写 resolvedType,
// 留作本步 known-issue (lambda 反推 / 泛型 applySubst 的"运行时再写")。
//
// Phase 3.2b 前置 (本步)：将 `getType()` 抛出的"已迁移诊断"从静默吞掉改为
// 向外抛, 让 SemaPass 实际接管该错误码。当前已迁移清单 (kMigratedCodes)：
//   C1 算术 / 比较 / 分支结果类型不匹配:
//     - E3001 / E3002 / E3003: 算术 / 乘除模 / 二元位运算左右类型不匹配
//     - E3004:                 比较运算左右类型不匹配
//     - E3005 / E3006:         if-elif / if-else 分支结果类型不匹配
//     - E3007:                 one-line if-else 真假分支类型不匹配
//     - E3008:                 if-else 预值表达式真假分支类型不匹配
//   C1 字段 / 元组访问 / 引用 / 索引 形态:
//     - E3025: `.?` safe-dot base 不是 Nullable
//     - E3040: 字段不存在 (struct / nullable struct payload)
//     - E3041: `&` getRef 时 struct 未找到
//     - E3043: `&` getRef 时找不到 file 作用域
//     - E3044: `.?` safe-dot 在 inner struct 上找不到 struct decl
//     - E3050: Box<T>? 取 inner 时 Box 元素类型缺失
//     - E3051: `.?` safe-dot 的 nullable inner 类型缺失
//     - E3057: 数组索引时元素类型缺失 (含 Array 泛型 / 普通数组)
//     - E3062: 索引目标非数组
//     - E3097: `&` getRef 时找不到 nearestScope
//     - E3100: 元组下标越界
// 其余错误码 (lambda 形参未推断、泛型 arity E3095 等) 仍走原 codegen 路径
// 报错; 等后续 batch 一并迁过来再扩 kMigratedCodes。

#include "sema/sema_pass.h"

#include <array>
#include <string_view>

#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "sema/call_resolve.h"

namespace {
// Phase 3.2b 已由 SemaPass 接管的错误码白名单。SemaPass 在 visitExpr 中
// 捕获 YuxError 时, 命中此清单的直接 rethrow, 让 SemaPass 成为该诊断的
// 实际抛出点。新增迁移码追加到此处即可。
constexpr std::array<std::string_view, 19> kMigratedCodes = {
    // 算术 / 比较 / 分支结果
    "E3001", "E3002", "E3003", "E3004",
    "E3005", "E3006", "E3007", "E3008",
    // 数组 / 字段 / 元组 / 引用
    // E3011 (数组元素类型不一致) 暂不迁移: 嵌套数组字面量 / 目标类型上下文
    // (`var rows Array<Array<i32>> = [[1,2],[3,4,5]]`) 在 codegen 走 target-type
    // 驱动路径, 不调用 `ExprArrayNode::getType()`; 但 SemaPass 下钻 visitExpr
    // 时会触发 E3011, 是假阳性。需把"目标类型上下文"协议建到 SemaPass 里才能
    // 安全迁; 留作下一批。
    "E3025",
    "E3040", "E3041", "E3043", "E3044",
    "E3050", "E3051", "E3057", "E3062",
    "E3097",
    "E3100",
};

bool isMigratedCode(const char* code) {
    if (!code) return false;
    std::string_view sv(code);
    for (auto c : kMigratedCodes) {
        if (sv == c) return true;
    }
    return false;
}
}

SemaPass::SemaPass(p<FileNode> file, p<FileNode> sdkFile)
    : _file(file), _sdkFile(sdkFile) {
}

void SemaPass::run() {
    if (!_file) return;
    for (auto& fn : _file->getFunctions()) {
        // 泛型模板 / #CompilerInner 不走常规 codegen, 在 Compiler::compile 里也
        // 是被跳过的; SemaPass 这里同步跳过, 保持与 codegen 覆盖一致。
        if (fn->header()->isGeneric()) continue;
        if (fn->header()->hasAnno("CompilerInner")) continue;
        visitFn(fn);
    }
    // struct impl 内的方法 / 析构 body 同样要走 SemaPass —— 它们的 codegen
    // 入口也是 compile<Foo>Expr, 不覆盖会导致后续 3.2b 把 set 改 assert 时
    // 方法体内表达式全部 assert 失败。
    for (auto& impl : _file->getStructImpls()) {
        if (impl->isGeneric()) continue;
        for (auto& m : impl->methods()) {
            if (m->header()->isGeneric()) continue;
            if (m->header()->hasAnno("CompilerInner")) continue;
            visitFn(m);
        }
        if (impl->hasDestructor()) {
            visitFn(impl->destructor());
        }
    }
}

void SemaPass::visitFn(p<FnNode> fn) {
    if (!fn) return;
    for (auto& stmt : fn->body()) {
        visitStmt(stmt);
    }
}

void SemaPass::visitBlock(p<StatementBlockNode> block) {
    if (!block) return;
    for (auto& s : block->statements()) {
        visitStmt(s);
    }
    if (block->hasResult()) {
        visitExpr(block->resultExpr());
    }
}

void SemaPass::visitStmt(p<StatementNode> stmt) {
    if (!stmt) return;
    if (auto loop = dynamic_cast<p<StatementLoopNode>>(stmt)) {
        visitBlock(loop->block());
        return;
    }
    if (auto set = dynamic_cast<p<StatementSetNode>>(stmt)) {
        visitExpr(set->arrayExpr());
        for (auto& idx : set->indices()) visitExpr(idx);
        visitExpr(set->valueExpr());
        return;
    }
    if (dynamic_cast<p<StatementBreakNode>>(stmt)) return;
    if (dynamic_cast<p<StatementRetVoidNode>>(stmt)) return;
    if (dynamic_cast<p<StatementDeclareNode>>(stmt)) return; // 无表达式
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
        if (se->expr()) visitExpr(se->expr());
        return;
    }
    // 兜底：未识别的 stmt 直接跳过, 不抛错 —— SemaPass 当前是 no-op, 漏处理
    // 不应阻塞 codegen; 3.2 起开始有实际写入后再改成 assert(false)。
}

void SemaPass::visitExpr(p<ExprNode> expr) {
    if (!expr) return;
    // Phase 3.2a：在每个 expr 节点上写 resolvedType, 与 compile<Foo>Expr 入口
    // 的同款 set 并存 (值相同, 后者随后变成 no-op)。getType() 是各子类的纯查询,
    // 不读 resolvedType, 此处先写后递归都安全; 选先写, 让下游若有早读路径也能命中。
    //
    // 已知问题: 部分 getType() 在错误形态下会抛 YuxError —— 例如算术节点遇到
    // lambda 形参未推断 (E3001), 或调用节点遇到泛型 arity 错配 (E3095)。
    //
    // Phase 3.2b 起开始按错误码白名单 (kMigratedCodes) 接管诊断: 命中清单的
    // 重新抛出, 由 SemaPass 实际报错; 其余仍吞掉, 留给 compile<Foo>Expr 的
    // 原有路径继续报。这样可以一码一码迁, 不必一次性把整个 getType 路径搬空。
    try {
        expr->setResolvedType(expr->getType());
    } catch (const YuxError& e) {
        if (isMigratedCode(e.getCode())) {
            throw;
        }
        // 未迁移码: 暂留给原 codegen 路径
    } catch (...) {
        // 非 YuxError (内部异常) 不该出现; 防御性吞掉以免影响 codegen
    }

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps()) visitExpr(e);
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr()); return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr());
        for (auto& a : n->getArgs()) visitExpr(a);

        // Phase 3.3 前置.2：SemaPass 主动驱动重载解析 + 灵活整数推断.
        // 只接管"纯 ID callee + 无显式类型实参 + 非泛型"的情形, 与
        // compiler_call.cpp 中 `else if (structDecl)` / `else` 分支的进入条件保持一致;
        // 其余 (Fn 类型 callee / 泛型 fn 或 ctor / 方法调用) 仍由 codegen 自行处理.
        //
        // 副作用幂等性: tryInferIntType 仅在 isFlexibleIntExpr 为真时改写; SemaPass
        // 跑完后字面量已带类型, Compiler 端再次调用 resolve* 时 isFlexibleIntExpr 返回 false,
        // 不会重复推断 (见 call_resolve.h 的契约说明).
        if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                int line = n->getLineNumber();
                int col = n->getColumn();
                bool hasTypeArgs = !n->getTypeArgs().empty();

                auto* structDecl = _file->getStructDecl(fnName);
                if (!structDecl && _sdkFile) structDecl = _sdkFile->getStructDecl(fnName);

                if (structDecl) {
                    // 形态校验对泛型 / 非泛型 ctor 都适用 (E6008 私有 / E6009 缺 typeArgs)
                    sema::validateCtorCallShape(structDecl, fnName, hasTypeArgs, line, col);
                    // 泛型 ctor + 显式 typeArgs 的 arity 校验 (E6010)
                    if (structDecl->isGeneric() && hasTypeArgs) {
                        sema::validateGenericTypeArgsArity(fnName,
                            structDecl->typeParams().size(),
                            n->getTypeArgs().size(), line, col);
                    }
                }

                // 泛型 fn + 显式 typeArgs 的 arity 校验 (E6010, 与 compileCallExpr 入口一致)
                if (!structDecl && hasTypeArgs) {
                    auto* genFn2 = _file->getFunction(fnName);
                    if (!genFn2 && _sdkFile) genFn2 = _sdkFile->getFunction(fnName);
                    if (genFn2 && genFn2->header()->isGeneric()) {
                        sema::validateGenericTypeArgsArity(fnName,
                            genFn2->header()->typeParams().size(),
                            n->getTypeArgs().size(), line, col);
                    }
                }

                // 仅在无显式 typeArgs + 非泛型路径上才驱动重载解析:
                // 泛型 fn/ctor 走 Compiler 的 substitute 推断, 灵活整数推断由
                // 那条路径自行完成; SemaPass 暂不接入泛型实例化.
                if (!hasTypeArgs) {
                    auto* genFn = _file->getFunction(fnName);
                    if (!genFn && _sdkFile) genFn = _sdkFile->getFunction(fnName);
                    if (!genFn || !genFn->header()->isGeneric()) {
                        if (structDecl && !structDecl->isGeneric()) {
                            sema::resolveCtorOverload(_file, fnName, n->getArgs(), line);
                            if (_sdkFile && _sdkFile != _file) {
                                sema::resolveCtorOverload(_sdkFile, fnName, n->getArgs(), line);
                            }
                        } else if (!structDecl) {
                            sema::resolveFnOverload(_file, _sdkFile, fnName, n->getArgs(), line);
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        visitExpr(n->baseExpr()); return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitBlock(n->thenBlock());
        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            visitBlock(el->block());
        }
        if (n->elseBlock()) visitBlock(n->elseBlock());
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElsePreValueNode>>(expr)) {
        visitExpr(n->condition()); visitExpr(n->trueValue()); visitExpr(n->falseValue());
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices()) visitExpr(i);
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements()) visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right()); return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // lambda 体内表达式的类型依赖调用点对形参的反推 / 上下文回填
        // (典型: `x => x + 1`, 在 `apply(it, 20)` 处才知道 `x : i32`)。
        // 3.2a 时这条路径靠 catch(...) 吞掉所有错误才没炸;
        // 3.2b 起 SemaPass 接管已迁移码 (E3001 等), 必须不再下钻 lambda 体,
        // 留给 codegen 在 compileCallExpr 回填形参类型后再走 compile<Foo>Expr
        // 入口的 setResolvedType 兜底写入。
        (void)n;
        return;
    }
    if (auto n = dynamic_cast<p<ExprEnumCtorNode>>(expr)) {
        for (auto& a : n->args()) visitExpr(a);
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms()) visitExpr(arm->body());
        return;
    }
    if (auto n = dynamic_cast<p<ExprTryCatchNode>>(expr)) {
        visitBlock(n->tryBlock());
        for (auto& c : n->catches()) visitBlock(c->body());
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg()); return;
    }
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left()); visitExpr(n->right()); return;
    }
    // ExprGetRefNode / ExprArrayInitNode 无子表达式 (ArrayInit 的 value 是
    // LiteralNode, 不递归)。其余未识别节点 3.2 起补 assert。
}
