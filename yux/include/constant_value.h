// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ConstantValue —— 编译期常量求值结果类型。
//
// 纯数据类型，0 LLVM 依赖，0 sema 依赖。供 AST 节点（如 GlobalVarNode）与 sema
// 层（ConstEvaluator）共享使用。与 types.h / error_code.h 同级置于 yux/include/。
//
// 设计依据：DRAFT-const-eval.md §2 全景模型。
//
// 注意：ConstEvaluator（求值器）本身依赖 AST 节点，留在 sema 层；ConstantValue
// 仅含数据 + 简单工厂方法与 asSigned() 工具，不牵涉任何求值逻辑。

#ifndef YUX_LANG_CONSTANT_VALUE_H
#define YUX_LANG_CONSTANT_VALUE_H

#include "types.h"

#include <vector>

struct ConstantValue {
    enum class Kind : u8 {
        Int,    // 整数：intBits 持 zero-extended 表示, type 给出 width / signedness
        Float,  // 浮点：floatVal 持 host double, type 决定 f32 / f64
        Bool,   // bool: boolVal
        Null,   // null: 无 payload
        Struct, // 结构体字段值: structFields
        String  // 编译期字符串：stringCodePoints 持 u32 码点向量
    };

    Kind kind = Kind::Null;
    TypeInfo type;

    // 互斥 payload（Kind = Int / Float / Bool 时有效）。
    u64 intBits = 0;
    f64 floatVal = 0.0;
    bool boolVal = false;

    // Kind = Struct 时持有字段值（顺序与声明序一致）。
    vector<ConstantValue> structFields;

    // Kind = String 时持有 u32 码点（不可变 rodata 字符串内容）。
    vector<u32> stringCodePoints;

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
    static ConstantValue makeStruct(vector<ConstantValue> fields, TypeInfo t) {
        ConstantValue v;
        v.kind = Kind::Struct;
        v.type = std::move(t);
        v.structFields = std::move(fields);
        return v;
    }
    static ConstantValue makeString(vector<u32> codePoints) {
        ConstantValue v;
        v.kind = Kind::String;
        v.type = TypeInfo("String");
        v.stringCodePoints = std::move(codePoints);
        return v;
    }

    [[nodiscard]] bool isInt() const { return kind == Kind::Int; }
    [[nodiscard]] bool isFloat() const { return kind == Kind::Float; }
    [[nodiscard]] bool isBool() const { return kind == Kind::Bool; }
    [[nodiscard]] bool isNull() const { return kind == Kind::Null; }
    [[nodiscard]] bool isStruct() const { return kind == Kind::Struct; }
    [[nodiscard]] bool isString() const { return kind == Kind::String; }

    // 整数按 type 中的符号位解释为 i64（仅 isInt() 时有意义）。
    // 实现内联以保持本头文件 0 .cpp 依赖，与 types.h 同级策略一致。
    [[nodiscard]] i64 asSigned() const {
        if (kind != Kind::Int) return 0;

        // 计算 type 的位宽
        auto w = 0u;
        const auto& n = type.name;
        if (n == "i8" || n == "u8")
            w = 8;
        else if (n == "i16" || n == "u16")
            w = 16;
        else if (n == "i32" || n == "u32")
            w = 32;
        else if (n == "i64" || n == "u64")
            w = 64;
        else if (n == "isize" || n == "usize")
            w = static_cast<int>(sizeof(void*) * 8);
        else
            return static_cast<i64>(intBits); // 未知类型，保守返回原始 bits

        if (w >= 64) return static_cast<i64>(intBits);

        u64 signBit = static_cast<u64>(1) << (w - 1u);
        u64 mask = (static_cast<u64>(1) << w) - 1u;
        if (intBits & signBit) {
            // 符号位为 1，做符号扩展
            return static_cast<i64>(intBits | ~mask);
        }
        return static_cast<i64>(intBits);
    }
};

#endif // YUX_LANG_CONSTANT_VALUE_H
