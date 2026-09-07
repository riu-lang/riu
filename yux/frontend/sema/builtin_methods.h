// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_BUILTIN_METHODS_H
#define YUX_LANG_SEMA_BUILTIN_METHODS_H

#include "error_code.h"
#include "types.h"

#include <array>
#include <string_view>

// Array（及预留 String）`#Builtin` 方法表：类型谓词 × 方法名 → arity + lowering id。
// 0 LLVM。SemaPass / getType / 符号表 / codegen 共用一份。
// String 方法当前是 yux 实现，表里占 BuiltinRecv::String，无行。
namespace sema {

enum class BuiltinRecv : u8 { Array, String };

enum class BuiltinRet : u8 { Void, Usize, Bool, Elem, ElemRef, Self };

enum class BuiltinLower : u8 {
    None,
    ArrayLen,
    ArrayCap,
    ArrayIsEmpty,
    ArrayGet,
    ArrayFirst,
    ArrayLast,
    ArrayPop,
    ArrayPush,
    ArraySetLen,
    ArrayClear,
    ArrayReserve,
    ArrayClone,
    ArrayWithCapacity,
    ArraySlice,
    ArrayConcat,
    ArrayContains,
};

struct BuiltinMethodSpec {
    BuiltinRecv recv;
    const char* name;
    u8 arity;             // 精确实参个数
    bool needsLvalue;     // 修改 receiver（push / pop / clear …）
    bool isStatic;        // Type::name 工厂
    bool needsElemType;   // Array 需 T；len/cap/with_capacity 否
    const char* arg0Type; // 非空则第 0 实参须为此名（如 "usize"）；"Array" = Array<T>&；空 = T / 不查
    const char* arg1Type; // 第 1 实参（slice 的 end）；空则无或与 arg0 相同规则
    BuiltinRet ret;
    BuiltinLower lower;
};

inline bool recvMatches(BuiltinRecv r, const TypeInfo& t) {
    switch (r) {
    case BuiltinRecv::Array:
        return t.isArrayGeneric() || t.isArray();
    case BuiltinRecv::String:
        return t.isString();
    }
    return false;
}

inline const char* recvTypeName(BuiltinRecv r) {
    switch (r) {
    case BuiltinRecv::Array:
        return "Array";
    case BuiltinRecv::String:
        return "String";
    }
    return "";
}

inline constexpr std::array kBuiltinMethods = {
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "len",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = false,
                      .arg0Type = nullptr,
                      .arg1Type = nullptr,
                      .ret = BuiltinRet::Usize,
                      .lower = BuiltinLower::ArrayLen},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "cap",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = false,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Usize,
                      .lower = BuiltinLower::ArrayCap},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "is_empty",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Bool,
                      .lower = BuiltinLower::ArrayIsEmpty},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "get",
                      .arity = 1,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = "usize",
                      .ret = BuiltinRet::ElemRef,
                      .lower = BuiltinLower::ArrayGet},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "first",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::ElemRef,
                      .lower = BuiltinLower::ArrayFirst},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "last",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::ElemRef,
                      .lower = BuiltinLower::ArrayLast},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "pop",
                      .arity = 0,
                      .needsLvalue = true,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Elem,
                      .lower = BuiltinLower::ArrayPop},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "push",
                      .arity = 1,
                      .needsLvalue = true,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Void,
                      .lower = BuiltinLower::ArrayPush},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "set_len",
                      .arity = 1,
                      .needsLvalue = true,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = "usize",
                      .ret = BuiltinRet::Void,
                      .lower = BuiltinLower::ArraySetLen},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "clear",
                      .arity = 0,
                      .needsLvalue = true,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Void,
                      .lower = BuiltinLower::ArrayClear},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "reserve",
                      .arity = 1,
                      .needsLvalue = true,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = "usize",
                      .ret = BuiltinRet::Void,
                      .lower = BuiltinLower::ArrayReserve},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "clone",
                      .arity = 0,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Self,
                      .lower = BuiltinLower::ArrayClone},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "slice",
                      .arity = 2,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = "usize",
                      .arg1Type = "usize",
                      .ret = BuiltinRet::Self,
                      .lower = BuiltinLower::ArraySlice},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "concat",
                      .arity = 1,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = "Array",
                      .ret = BuiltinRet::Self,
                      .lower = BuiltinLower::ArrayConcat},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "contains",
                      .arity = 1,
                      .needsLvalue = false,
                      .isStatic = false,
                      .needsElemType = true,
                      .arg0Type = nullptr,
                      .ret = BuiltinRet::Bool,
                      .lower = BuiltinLower::ArrayContains},
    BuiltinMethodSpec{.recv = BuiltinRecv::Array,
                      .name = "with_capacity",
                      .arity = 1,
                      .needsLvalue = false,
                      .isStatic = true,
                      .needsElemType = false,
                      .arg0Type = "usize",
                      .ret = BuiltinRet::Self,
                      .lower = BuiltinLower::ArrayWithCapacity},
};

inline const BuiltinMethodSpec* lookupInstanceBuiltin(const TypeInfo& recv, string_view name) {
    for (const auto& s : kBuiltinMethods) {
        if (s.isStatic) continue;
        if (name != s.name) continue;
        if (recvMatches(s.recv, recv)) return &s;
    }
    return nullptr;
}

inline const BuiltinMethodSpec* lookupStaticBuiltin(string_view typeName, string_view name) {
    for (const auto& s : kBuiltinMethods) {
        if (!s.isStatic) continue;
        if (name != s.name) continue;
        if (typeName == recvTypeName(s.recv)) return &s;
    }
    return nullptr;
}

inline bool isKnownArrayMethod(string_view name) {
    for (const auto& s : kBuiltinMethods) {
        if (s.recv == BuiltinRecv::Array && !s.isStatic && name == s.name) return true;
    }
    return false;
}

inline TypeInfo builtinMethodReturnType(const BuiltinMethodSpec& spec, const TypeInfo& recv) {
    switch (spec.ret) {
    case BuiltinRet::Void:
        return {};
    case BuiltinRet::Usize:
        return TypeInfo("usize");
    case BuiltinRet::Bool:
        return TypeInfo("bool");
    case BuiltinRet::Self:
        return recv;
    case BuiltinRet::Elem: {
        auto e = recv.isArrayGeneric() ? recv.arrayGenericElementType() : recv.elementType;
        return e ? *e : TypeInfo();
    }
    case BuiltinRet::ElemRef: {
        auto e = recv.isArrayGeneric() ? recv.arrayGenericElementType() : recv.elementType;
        if (!e) return {};
        return TypeInfo("Ref", {e});
    }
    }
    return {};
}

inline void validateBuiltinMethodCall(const BuiltinMethodSpec& spec, const TypeInfo& recv, size_t argsCount,
                                      bool baseIsLvalue, int line, int col) {
    if (spec.needsElemType) {
        if (!recv.arrayGenericElementType() && !recv.elementType) {
            throw YuxError(line, col, ErrorCode::E3050);
        }
    }
    if (spec.needsLvalue && !baseIsLvalue) {
        throw YuxError(line, col, ErrorCode::E6042, spec.name);
    }
    if (argsCount != static_cast<size_t>(spec.arity)) {
        throw YuxError(line, col, ErrorCode::E6027, spec.name, static_cast<size_t>(spec.arity));
    }
}

} // namespace sema

#endif // YUX_LANG_SEMA_BUILTIN_METHODS_H
