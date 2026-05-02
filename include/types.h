// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <utility>
#include <vector>
#include <map>
#include <algorithm>
#include <xstring>
#include <exception>
#include <stdexcept>
#include <cassert>
#include <iostream>
#include "antlr4-runtime.h"
#include "error_code.h"

using namespace std;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
// typedef __int128 i128;

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
// typedef unsigned __int128 u128;

typedef float f32;
typedef double f64;

template <typename T>
using sp = shared_ptr<T>;

template <typename T>
using p = T*;

#ifdef _DEBUG

extern bool debug;

#define DEBUG_LOG(msg) if(debug) { std::cerr << "[DEBUG] " << msg << '\n'; }
#define DEBUG_LOG_VAL(msg, val) if(debug) { std::cerr << "[DEBUG] " << msg << ": " << val << '\n'; }

#else

#define DEBUG_LOG(msg)
#define DEBUG_LOG_VAL(msg, val)

#endif

class TokenInfo {
    string _text;
    size_t _line = 0;
    size_t _charPositionInLine = 0;
    size_t _tokenIndex = 0;
    size_t _startIndex = 0;
    size_t _stopIndex = 0;

public:
    TokenInfo() = default;

    TokenInfo(antlr4::Token* token) {
        if (token) {
            _text = token->getText();
            _line = token->getLine();
            _charPositionInLine = token->getCharPositionInLine();
            _tokenIndex = token->getTokenIndex();
            _startIndex = token->getStartIndex();
            _stopIndex = token->getStopIndex();
        }
    }

    // 合成 Token：用于编译器解糖时构造没有真实 antlr token 的节点
    // 例如 T? -> Nullable<T> 时，"Nullable" 这个名字没有源文件来源
    TokenInfo(string text, size_t line)
        : _text(std::move(text)), _line(line) {
    }

    TokenInfo(const TokenInfo& other) = default;
    TokenInfo(TokenInfo&& other) noexcept = default;
    TokenInfo& operator=(const TokenInfo& other) = default;
    TokenInfo& operator=(TokenInfo&& other) noexcept = default;

    [[nodiscard]] const string& getText() const { return _text; }
    [[nodiscard]] size_t getLine() const { return _line; }
    [[nodiscard]] size_t getCharPositionInLine() const { return _charPositionInLine; }
    [[nodiscard]] size_t getTokenIndex() const { return _tokenIndex; }
    [[nodiscard]] size_t getStartIndex() const { return _startIndex; }
    [[nodiscard]] size_t getStopIndex() const { return _stopIndex; }

    [[nodiscard]] bool empty() const { return _text.empty(); }
    [[nodiscard]] bool valid() const { return !_text.empty() || _line > 0; }

    explicit operator bool() const { return valid(); }

    bool operator==(const TokenInfo& other) const { return _text == other._text && _line == other._line; }
    bool operator!=(const TokenInfo& other) const { return !(*this == other); }
};

using Token = TokenInfo;

// 源码位置：line 为 1-based 行号，col 为 1-based 列号。col == 0 表示未知（合成节点 / 旧路径）。
struct SourceLocation {
    int line = 0;
    int col = 0;

    SourceLocation() = default;
    SourceLocation(int l, int c) : line(l), col(c) {
    }

    [[nodiscard]] bool valid() const { return line > 0; }
};

class YuxError : public std::runtime_error {
    int _line = 0;
    int _col = 0; // 0 表示列未知
    const char* _code = "E0000"; // 指向 ErrorCode 表中的静态字面量

public:
    explicit YuxError(const string& msg, int line) : runtime_error(msg), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(int line, const format_string<_Types...> format, _Types&&... args) : runtime_error(
        std::vformat(format.get(), std::make_format_args(args...))), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(int line, int col, const format_string<_Types...> format, _Types&&... args) : runtime_error(
        std::vformat(format.get(), std::make_format_args(args...))), _line(line), _col(col) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(SourceLocation loc, const format_string<_Types...> format, _Types&&... args) : runtime_error(
        std::vformat(format.get(), std::make_format_args(args...))), _line(loc.line), _col(loc.col) {
        assert(loc.line > 0 && "YuxError line must be > 0");
    }

    // ErrorCode 路径：模板取自 ec.message，code 取自 ec.code
    template <class... _Types>
    explicit YuxError(int line, int col, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(line), _col(col), _code(ec.code) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    // 列未知场景的便利重载（驱动层 / 模块层 errorLine）
    template <class... _Types>
    explicit YuxError(int line, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(line), _col(0), _code(ec.code) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(SourceLocation loc, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(loc.line), _col(loc.col), _code(ec.code) {
        assert(loc.line > 0 && "YuxError line must be > 0");
    }

    void setLineNumber(int line) {
        assert(line > 0 && "YuxError line must be > 0");
        _line = line;
    }

    void setColumn(int col) { _col = col; }

    [[nodiscard]] int getLineNumber() const {
        return _line;
    }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {_line, _col}; }

    [[nodiscard]] const char* getCode() const { return _code; }
};

template <typename T>
p<T> any_cast_p(const std::any& a) {
    return std::any_cast<p<T>>(a);
}

template <typename T>
T any_cast_v(const std::any& a) {
    return std::any_cast<T>(a);
}

enum class TypeKind : u8 {
    Normal,
    Generic,
    Array
};

struct TypeInfo {
    TypeKind kind = TypeKind::Normal;
    string name;
    u64 arraySize = 0;
    sp<TypeInfo> elementType = nullptr;
    vector<sp<TypeInfo>> genericArgs;

    TypeInfo() = default;

    explicit TypeInfo(string n) : name(std::move(n)) {
    }

    TypeInfo(string n, vector<sp<TypeInfo>> args) :
        kind(TypeKind::Generic),
        name(std::move(n)),
        genericArgs(std::move(args)) {
    }

    TypeInfo(sp<TypeInfo> elemType, u64 size) :
        kind(TypeKind::Array),
        arraySize(size),
        elementType(std::move(elemType)) {
        name = "[" + elementType->name + " * " + to_string(arraySize) + "]";
    }

    [[nodiscard]] bool isArray() const { return kind == TypeKind::Array; }

    [[nodiscard]] bool isNormal() const { return kind == TypeKind::Normal; }

    [[nodiscard]] bool isGeneric() const { return kind == TypeKind::Generic; }

    [[nodiscard]] bool empty() const { return name.empty(); }

    [[nodiscard]] bool startsWith(char c) const { return !name.empty() && name[0] == c; }

    [[nodiscard]] bool isRef() const {
        return kind == TypeKind::Generic && name == "Ref" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> refElementType() const {
        if (isRef() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isBox() const {
        return kind == TypeKind::Generic && name == "Box" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> boxElementType() const {
        if (isBox() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Weak<T>：弱引用，layout 与 Box<T> 同形 { ptr handle }
    // handle 指向 Box 的 Block；weak 计数维护 block 存活，不维护 payload 存活
    [[nodiscard]] bool isWeak() const {
        return kind == TypeKind::Generic && name == "Weak" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> weakElementType() const {
        if (isWeak() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isPtr() const {
        return name == "Ptr";
    }

    [[nodiscard]] sp<TypeInfo> ptrElementType() const {
        if (isPtr() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isArrayGeneric() const {
        return kind == TypeKind::Generic && name == "Array" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> arrayGenericElementType() const {
        if (isArrayGeneric() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Nullable<T>：T? 解糖后的类型；layout = { bool _has; T _value }
    [[nodiscard]] bool isNullable() const {
        return kind == TypeKind::Generic && name == "Nullable" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> nullableInnerType() const {
        if (isNullable() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    string getFullName() const {
        if (kind == TypeKind::Generic && !genericArgs.empty()) {
            string result = name;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                result += "_" + genericArgs[i]->getFullName();
            }
            return result;
        }
        return name;
    }

    // 单态化实例 mangle 名：Base$Arg1$Arg2，嵌套递归（e.g. A<B<i32>> → A$B$i32）
    string getGenericMangleName() const {
        if (kind == TypeKind::Generic && !genericArgs.empty()) {
            string result = name;
            for (auto& a : genericArgs) {
                result += "$" + a->getGenericMangleName();
            }
            return result;
        }
        if (kind == TypeKind::Array && elementType) {
            return "[" + elementType->getGenericMangleName() + "*" + std::to_string(arraySize) + "]";
        }
        return name;
    }

    // 应用类型形参替换。Normal 类型若匹配 subst 键则整体替换（可被替换为 Generic/Array）。
    TypeInfo substitute(const std::map<std::string, TypeInfo>& subst) const {
        if (kind == TypeKind::Normal) {
            auto it = subst.find(name);
            if (it != subst.end()) return it->second;
            return *this;
        }
        if (kind == TypeKind::Generic) {
            vector<sp<TypeInfo>> newArgs;
            newArgs.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newArgs.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            return TypeInfo(name, std::move(newArgs));
        }
        if (kind == TypeKind::Array && elementType) {
            auto sub = elementType->substitute(subst);
            return TypeInfo(std::make_shared<TypeInfo>(std::move(sub)), arraySize);
        }
        return *this;
    }

    bool operator==(const TypeInfo& other) const {
        if (kind != other.kind) return false;
        if (name != other.name) return false;
        if (kind == TypeKind::Array) {
            if (arraySize != other.arraySize) return false;
            if (!elementType && !other.elementType) return true;
            if (!elementType || !other.elementType) return false;
            return *elementType == *other.elementType;
        }
        if (kind == TypeKind::Generic) {
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (!genericArgs[i] && !other.genericArgs[i]) continue;
                if (!genericArgs[i] || !other.genericArgs[i]) return false;
                if (*genericArgs[i] != *other.genericArgs[i]) return false;
            }
        }
        return true;
    }

    bool operator!=(const TypeInfo& other) const {
        return !(*this == other);
    }
};

inline bool isBuiltinType(const string& typeName) {
    static const vector<string> builtinTypes = {
        "bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"
    };
    return std::find(builtinTypes.begin(), builtinTypes.end(), typeName) != builtinTypes.end();
}
