// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_CONST_EVAL_H
#define YUX_LANG_CONST_EVAL_H

#include "ast/node/expr_node.h"
#include "constant_value.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

class FileNode;

// DRAFT-const-eval Phase 1 —— sema 期常量求值器骨架（0 LLVM 依赖）。
//
// 设计依据：DRAFT-const-eval.md §2 全景模型 + §4.8 实现替代方案。
//
// ConstantValue 类型定义已提取至 yux/include/constant_value.h（纯数据类型，与
// types.h 同级），供 AST 节点与 sema 层共享，消除 AST→sema 反向依赖。
//
// 范围（Phase 1）：
//   - 叶：LiteralIntNode / LiteralBoolNode / LiteralNullNode / LiteralFloatNode
//   - 名字引用：LiteralObjNode —— 仅查 caller 注入的 externalEnv（Phase 1 standalone
//     不接 GlobalConstNode 缓存；待 Phase 2 接 visitLetGlobal 时建表）
//   - ExprParenNode
//   - ExprUnaryNode（Neg / Not）
//   - ExprAddSubNode / ExprMulDivModNode
//   - 整数位方法：recv.and/or/xor/shl/shr(arg) / recv.inv()
//   - ExprCompareNode（含 AndAnd / OrOr）
//
// 不在范围（Phase 1）：
//   - ExprCallNode → Phase 4
//   - struct 字面量 → Phase 5
//   - 控制流（ExprIfElse / ExprOneLineIfElse）→ Phase 3
//
// 错误处理：本 Phase **不抛错码**；失败统一返回 nullopt。SemaPass 对全局
// `#Cval` 据 nullopt 抛 E3140（非 const 子表达式）。
//
// TODO: 区分 E3140（非 const）与 E3143（算术错：溢出 / 除 0）。当前所有失败统一
// 报 E3140；E3143 已在 error_code.h 注册，未触发；引入 lastError 字段或 result
// 类型分流后启用 diag_const_eval_E3143 测试。
//
// [#4.8.A] 浮点：v1 走 host double 简化（draft 写"严格对齐目标三元组"是长期目标；
// 实际依赖 llvm::APFloat 与 sema 0-LLVM 协议冲突，待真触发跨平台差异再改）。
// f32 通过 static_cast<float> 模拟收敛。
//
// [#4.8.B] 整数：uint64_t bits + TypeInfo（按 type 决定 sign / width）；溢出按
// DRAFT §4.7 决议 trap（Phase 2 抛 E3143），Phase 1 暂以 nullopt 返回。

// ConstEvaluator —— 单次求值会话。
//
// 用法（SemaPass / codegen typical caller）：
//   ConstEvaluator ev;
//   ev.setNamedConst("MAX", ConstantValue::makeInt(100, TypeInfo("i32")));
//   auto v = ev.eval(rhsExpr);
//   if (!v) throw E3140 ...
//
// externalEnv：caller 注入的"已知常量"表（Phase 2 = 全局 #Cval 池；Phase 4 = #Const fn
// 形参绑定 + 体内 const let）。Phase 1 standalone 场景留空。
class ConstEvaluator {
public:
    ConstEvaluator() = default;

    void setNamedConst(const string& name, ConstantValue value);

    // Phase 4: 注入文件上下文以解析 #Const fn 调用。未注入时 ExprCallNode 直接 nullopt。
    void setFile(FileNode* file) { _file = file; }

    // 主入口：对任意 ExprNode 试求值。失败返回 nullopt（含：不支持的节点 / 名字
    // 查不到 / 类型不匹配 / 溢出等运行期错形态）。本 Phase 不区分原因；Phase 2
    // 接入后由 caller 据失败点抛错码。
    std::optional<ConstantValue> eval(ExprNode* expr);

private:
    std::map<string, ConstantValue> _env;
    FileNode* _file = nullptr;
    // Phase 4: 递归调用栈防自环；同名 #Const fn 进入即返 nullopt。
    std::set<string> _callStack;

    // 分支调度（按 expr_node.h 中的具体类型）。
    std::optional<ConstantValue> evalLiteral(LiteralNode* lit);
    std::optional<ConstantValue> evalLiteralObj(LiteralObjNode* obj);
    std::optional<ConstantValue> evalUnary(ExprUnaryNode* node);
    std::optional<ConstantValue> evalAddSub(ExprAddSubNode* node);
    std::optional<ConstantValue> evalMulDivMod(ExprMulDivModNode* node);
    std::optional<ConstantValue> evalBinOp(ExprBinOpNode* node);
    std::optional<ConstantValue> evalIntBitMethod(ExprCallNode* call);
    std::optional<ConstantValue> evalCompare(ExprCompareNode* node);
    std::optional<ConstantValue> evalCall(ExprCallNode* call);
    // DRAFT-const-eval Phase 5: struct 字面量.
    std::optional<ConstantValue> evalStructLit(ExprStructLitNode* node);
};

#endif // YUX_LANG_CONST_EVAL_H
