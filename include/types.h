// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include <algorithm>
#include "antlr4-runtime.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include "error_code.h"
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <xstring>

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

// Debug 构建中校验 ErrorCode 消息模板的 {} 占位符数量与实际参数一致
// std::vformat 参数不足时会抛 format_error，此断言让问题在 throw 点立刻暴露
#ifndef NDEBUG
inline constexpr size_t countFmtPlaceholders(std::string_view fmt) {
    size_t count = 0;
    for (size_t i = 0; i + 1 < fmt.size(); ++i) {
        if (fmt[i] == '{' && fmt[i + 1] == '}') {
            ++count;
            ++i;
        }
    }
    return count;
}
#endif

class YuxError : public std::runtime_error {
    size_t _line = 0;
    int _col = 0; // 0 表示列未知
    const char* _code = "E0000"; // 指向 ErrorCode 表中的静态字面量
    DiagSeverity _sev = DiagSeverity::Error; // 默认严重等级（来源于 ErrorCodeDef.defaultSev）
    vector<string> _hints; // 修复建议（"= help: ..."），可链式 withHint 追加
    vector<string> _notes; // 附加说明（"= note: ..."），可链式 withNote 追加

public:
    explicit YuxError(const string& msg, size_t line) : runtime_error(msg), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(size_t line, const format_string<_Types...> format, _Types&&... args) : runtime_error(
        std::vformat(format.get(), std::make_format_args(args...))), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... _Types>
    explicit YuxError(size_t line, int col, const format_string<_Types...> format, _Types&&... args) : runtime_error(
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
    explicit YuxError(size_t line, int col, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(line), _col(col), _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "YuxError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    // 列未知场景的便利重载（驱动层 / 模块层 errorLine）
    template <class... _Types>
    explicit YuxError(size_t line, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(line), _col(0), _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "YuxError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    template <class... _Types>
    explicit YuxError(SourceLocation loc, const ErrorCodeDef& ec, _Types&&... args) : runtime_error(
        std::vformat(std::string_view(ec.message), std::make_format_args(args...))),
        _line(loc.line), _col(loc.col), _code(ec.code), _sev(ec.defaultSev) {
        assert(loc.line > 0 && "YuxError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    void setLineNumber(size_t line) {
        assert(line > 0 && "YuxError line must be > 0");
        _line = line;
    }

    void setColumn(int col) { _col = col; }

    [[nodiscard]] size_t getLineNumber() const {
        return _line;
    }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {static_cast<int>(_line), _col}; }

    [[nodiscard]] const char* getCode() const { return _code; }

    [[nodiscard]] DiagSeverity getSeverity() const { return _sev; }

    // 链式追加 help / note：支持 `throw YuxError(...).withHint("...")` 形态
    YuxError& withHint(string h) & { _hints.push_back(std::move(h)); return *this; }
    YuxError&& withHint(string h) && { _hints.push_back(std::move(h)); return std::move(*this); }
    YuxError& withNote(string n) & { _notes.push_back(std::move(n)); return *this; }
    YuxError&& withNote(string n) && { _notes.push_back(std::move(n)); return std::move(*this); }

    [[nodiscard]] const vector<string>& hints() const { return _hints; }
    [[nodiscard]] const vector<string>& notes() const { return _notes; }

    // 显式声明拷贝 / 移动构造 noexcept：throw YuxError 在抛出栈展开期间不允许再次抛异常；
    // 真正的 OOM 走 std::terminate（语义上等价于 runtime_error 自身的承诺）
    YuxError(const YuxError&) noexcept = default;
    YuxError(YuxError&&) noexcept = default;
    YuxError& operator=(const YuxError&) noexcept = default;
    YuxError& operator=(YuxError&&) noexcept = default;
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
    Array,
    Tuple,
    Fn      // 函数类型字面量 fn(P1, ..., Pn) R（结构等同；参数名不参与判等）
};

// 元组类型构造时使用的 tag，用来与 Generic 构造区分
struct TupleTag {};

// 函数类型构造 tag；fnRet 为返回类型（void 时传 nullptr 或空 TypeInfo）
struct FnTag {};

struct TypeInfo {
    TypeKind kind = TypeKind::Normal;
    string name;
    u64 arraySize = 0;
    sp<TypeInfo> elementType = nullptr;     // Array 元素类型 / Fn 返回类型（void 时为 nullptr）
    vector<sp<TypeInfo>> genericArgs;       // Generic 实参 / Tuple 元素 / Fn 形参类型列表
    bool fnNullable = false;                // Fn: fn?(...)R 紧凑形 nullable [#24]

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

    // 元组类型 (T1, T2, ...)
    // 元素列表复用 genericArgs 存储；name 合成为 "(T1,T2,...)" 形式
    TypeInfo(TupleTag, vector<sp<TypeInfo>> elements) :
        kind(TypeKind::Tuple),
        genericArgs(std::move(elements)) {
        name = "(";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) name += ",";
            name += genericArgs[i] ? genericArgs[i]->name : "?";
        }
        name += ")";
    }

    // 函数类型 fn(P1,...,Pn) R / fn?(...)R nullable 紧凑形 [#24]
    // 形参类型列表存 genericArgs；返回类型存 elementType（void 时为 nullptr）
    // name 合成为 "fn(P1,P2,...)R" / "fn?(P1,P2,...)R"，参数名不参与（§3.4）
    TypeInfo(FnTag, vector<sp<TypeInfo>> paramTypes, sp<TypeInfo> retType, bool nullable = false) :
        kind(TypeKind::Fn),
        elementType(std::move(retType)),
        genericArgs(std::move(paramTypes)),
        fnNullable(nullable) {
        name = nullable ? "fn?(" : "fn(";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) name += ",";
            name += genericArgs[i] ? genericArgs[i]->name : "?";
        }
        name += ")";
        if (elementType) name += elementType->name;
    }

    [[nodiscard]] bool isArray() const { return kind == TypeKind::Array; }

    [[nodiscard]] bool isNormal() const { return kind == TypeKind::Normal; }

    [[nodiscard]] bool isGeneric() const { return kind == TypeKind::Generic; }

    [[nodiscard]] bool isTuple() const { return kind == TypeKind::Tuple; }

    [[nodiscard]] bool isFn() const { return kind == TypeKind::Fn; }

    // 元组元素类型列表（仅 isTuple() 时有意义）
    [[nodiscard]] const vector<sp<TypeInfo>>& tupleElements() const { return genericArgs; }

    // 函数类型形参列表（仅 isFn() 时有意义）
    [[nodiscard]] const vector<sp<TypeInfo>>& fnParamTypes() const { return genericArgs; }

    // 函数类型返回值（void 时为 nullptr）
    [[nodiscard]] sp<TypeInfo> fnReturnType() const { return elementType; }

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

    [[nodiscard]] bool isRc() const {
        return kind == TypeKind::Generic && name == "Rc" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> rcElementType() const {
        if (isRc() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Heap<T>：堆作用域句柄（DRAFT-heap-types §8.3a），layout = 裸 T*
    // 与 Rc<T> 不同：无 RC 头、单所有权、作用域绑定析构、不可装入 Rc/Weak（§8.3a.5.1）
    [[nodiscard]] bool isHeap() const {
        return kind == TypeKind::Generic && name == "Heap" && genericArgs.size() == 1;
    }

    [[nodiscard]] sp<TypeInfo> heapElementType() const {
        if (isHeap() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Weak<T>：弱引用，layout 与 Rc<T> 同形 { ptr handle }
    // handle 指向 Rc 的 Block；weak 计数维护 block 存活，不维护 payload 存活
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

    // Dyn<D> / Dyn<D&>：draft 运行时多态形态（DRAFT-dyn-draft / 拟 §12.9）
    // layout = { vtable_ptr, data_ptr } 16 字节 fat pointer。
    // 内层若为 Ref<D> 则是借用形态 (Dyn<D&>)，否则 owned。
    [[nodiscard]] bool isDyn() const {
        return kind == TypeKind::Generic && name == "Dyn" && genericArgs.size() == 1;
    }

    [[nodiscard]] bool isDynBorrow() const {
        return isDyn() && genericArgs[0] && genericArgs[0]->isRef();
    }

    [[nodiscard]] bool isDynOwned() const {
        return isDyn() && genericArgs[0] && !genericArgs[0]->isRef();
    }

    // 拿 D（剥掉借用形态外层的 Ref）。
    [[nodiscard]] sp<TypeInfo> dynSpecType() const {
        if (!isDyn() || !genericArgs[0]) return nullptr;
        if (genericArgs[0]->isRef()) return genericArgs[0]->refElementType();
        return genericArgs[0];
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
        if (kind == TypeKind::Fn) {
            string result = fnNullable ? "fnQ" : "fn";
            for (auto& a : genericArgs) {
                result += "_" + (a ? a->getFullName() : string("?"));
            }
            result += "__" + (elementType ? elementType->getFullName() : string("void"));
            return result;
        }
        return name;
    }

    // LLVM 符号用 mangle 名（与 yux 源码写法一致）：
    // 泛型 Base<Arg1,Arg2>，元组 (T1,T2)，函数 fn(P1,...,Pn)R，数组 [E*N]
    string getMangleName() const {
        if (kind == TypeKind::Generic && !genericArgs.empty()) {
            string result = name + "<";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ",";
                result += genericArgs[i]->getMangleName();
            }
            result += ">";
            return result;
        }
        if (kind == TypeKind::Array && elementType) {
            return "[" + elementType->getMangleName() + "*" + std::to_string(arraySize) + "]";
        }
        // 元组：(T1,T2)
        if (kind == TypeKind::Tuple) {
            string result = "(";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ",";
                result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
            }
            result += ")";
            return result;
        }
        // 函数：fn(P1,...,Pn)R / fn?(...)R
        if (kind == TypeKind::Fn) {
            string result = fnNullable ? "fn?(" : "fn(";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ",";
                result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
            }
            result += ")";
            result += elementType ? elementType->getMangleName() : string("void");
            return result;
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
        if (kind == TypeKind::Tuple) {
            vector<sp<TypeInfo>> newElems;
            newElems.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newElems.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            return TypeInfo(TupleTag{}, std::move(newElems));
        }
        if (kind == TypeKind::Fn) {
            vector<sp<TypeInfo>> newParams;
            newParams.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newParams.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            sp<TypeInfo> newRet = nullptr;
            if (elementType) newRet = std::make_shared<TypeInfo>(elementType->substitute(subst));
            return TypeInfo(FnTag{}, std::move(newParams), newRet, fnNullable);
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
        if (kind == TypeKind::Generic || kind == TypeKind::Tuple) {
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (!genericArgs[i] && !other.genericArgs[i]) continue;
                if (!genericArgs[i] || !other.genericArgs[i]) return false;
                if (*genericArgs[i] != *other.genericArgs[i]) return false;
            }
        }
        if (kind == TypeKind::Fn) {
            if (fnNullable != other.fnNullable) return false;
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (!genericArgs[i] && !other.genericArgs[i]) continue;
                if (!genericArgs[i] || !other.genericArgs[i]) return false;
                if (*genericArgs[i] != *other.genericArgs[i]) return false;
            }
            // 返回类型：void（nullptr）也参与判等
            if (!elementType && !other.elementType) return true;
            if (!elementType || !other.elementType) return false;
            return *elementType == *other.elementType;
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
