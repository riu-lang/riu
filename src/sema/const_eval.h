// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_CONST_EVAL_H
#define YUX_LANG_CONST_EVAL_H

#include "ast/node/expr_node.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

class FileNode;

// DRAFT-const-eval Phase 1 —— sema 期常量求值器骨架（0 LLVM 依赖）。
//
// 设计依据：DRAFT-const-eval.md §2 全景模型 + §4.8 实现替代方案。
//
// 范围（Phase 1）：
//   - 叶：LiteralIntNode / LiteralBoolNode / LiteralNullNode / LiteralFloatNode
//   - 名字引用：LiteralObjNode —— 仅查 caller 注入的 externalEnv（Phase 1 standalone
//     不接 GlobalConstNode 缓存；待 Phase 2 接 visitLetGlobal 时建表）
//   - ExprParenNode
//   - ExprUnaryNode（Neg / Rev / Not）
//   - ExprAddSubNode / ExprMulDivModNode / ExprBinOpNode（位 + 移位）
//   - ExprCompareNode（含 AndAnd / OrOr）
//
// 不在范围（Phase 1）：
//   - ExprCallNode → Phase 4
//   - struct 字面量 → Phase 5
//   - 控制流（ExprIfElse / ExprOneLineIfElse / ExprIfElsePreValue）→ Phase 3
//
// 错误处理：本 Phase **不抛错码**；失败统一返回 nullopt。Phase 2 caller 在
// visitLetGlobal 内据 nullopt 抛 E3140（非 const 子表达式）。
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

struct ConstantValue {
    enum class Kind : u8 {
        Int,    // 整数：bits 持 zero-extended 表示, type 给出 width / signedness
        Float,  // 浮点：floatVal 持 host double, type 决定 f32 / f64
        Bool,   // bool: boolVal
        Null,   // null: 无 payload
        Struct  // 结构体字段值: structFields（Phase 5 启用）
    };

    Kind kind = Kind::Null;
    TypeInfo type;

    // 互斥 payload（Kind = Int / Float / Bool 时有效）。
    // Phase 1 不引入 union 以保留 ConstantValue 可默认 copy / assign；用独立成员牺牲一点
    // 空间换简化, 不影响正确性。
    u64    intBits  = 0;
    f64    floatVal = 0.0;
    bool   boolVal  = false;

    // Kind = Struct 时持有字段值（顺序与声明序一致）。Phase 1 留空。
    vector<ConstantValue> structFields;

    static ConstantValue makeInt(u64 bits, TypeInfo t) {
        ConstantValue v;
        v.kind = Kind::Int;
        v.type = std::move(t);
        v.intBits = bits;
        return v;
    }
    static ConstantValue makeFloat(f64 val, TypeInfo t) {
        ConstantValue v;
        v.kind = Kind::Float;
        v.type = std::move(t);
        v.floatVal = val;
        return v;
    }
    static ConstantValue makeBool(bool b) {
        ConstantValue v;
        v.kind = Kind::Bool;
        v.type = TypeInfo("bool");
        v.boolVal = b;
        return v;
    }
    static ConstantValue makeNull() {
        ConstantValue v;
        v.kind = Kind::Null;
        v.type = TypeInfo("Ptr");
        return v;
    }

    [[nodiscard]] bool isInt()    const { return kind == Kind::Int;    }
    [[nodiscard]] bool isFloat()  const { return kind == Kind::Float;  }
    [[nodiscard]] bool isBool()   const { return kind == Kind::Bool;   }
    [[nodiscard]] bool isNull()   const { return kind == Kind::Null;   }
    [[nodiscard]] bool isStruct() const { return kind == Kind::Struct; }

    // 整数按 type 中的符号位解释为 i64（仅 isInt() 时有意义）。
    [[nodiscard]] i64 asSigned() const;
};

// ConstEvaluator —— 单次求值会话。
//
// 用法（Phase 2 typical caller）：
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
    std::optional<ConstantValue> eval(const p<ExprNode>& expr);

private:
    std::map<string, ConstantValue> _env;
    FileNode* _file = nullptr;
    // Phase 4: 递归调用栈防自环；同名 #Const fn 进入即返 nullopt。
    std::set<string> _callStack;

    // 分支调度（按 expr_node.h 中的具体类型）。
    std::optional<ConstantValue> evalLiteral(const p<LiteralNode>& lit);
    std::optional<ConstantValue> evalLiteralObj(const p<LiteralObjNode>& obj);
    std::optional<ConstantValue> evalUnary(const p<ExprUnaryNode>& node);
    std::optional<ConstantValue> evalAddSub(const p<ExprAddSubNode>& node);
    std::optional<ConstantValue> evalMulDivMod(const p<ExprMulDivModNode>& node);
    std::optional<ConstantValue> evalBinOp(const p<ExprBinOpNode>& node);
    std::optional<ConstantValue> evalCompare(const p<ExprCompareNode>& node);
    std::optional<ConstantValue> evalCall(const p<ExprCallNode>& call);
};

#endif //YUX_LANG_CONST_EVAL_H
