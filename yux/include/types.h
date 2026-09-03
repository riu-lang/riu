// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "antlr4-runtime.h"
#include "error_code.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

using f32 = float;
using f64 = double;

template <typename T>
using sp = shared_ptr<T>;

template <typename T>
using p = T*;

#ifdef _DEBUG

extern bool debug;

// NOLINTBEGIN(bugprone-macro-parentheses)
#define DEBUG_LOG(msg)                                                                                                 \
    if (debug) {                                                                                                       \
        std::cerr << "[DEBUG] " << msg << '\n';                                                                        \
    }
#define DEBUG_LOG_VAL(msg, val)                                                                                        \
    if (debug) {                                                                                                       \
        std::cerr << "[DEBUG] " << msg << ": " << val << '\n';                                                         \
    }
// NOLINTEND(bugprone-macro-parentheses)

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
    TokenInfo(string text, size_t line) : _text(std::move(text)), _line(line) {}

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
    SourceLocation(int l, int c) : line(l), col(c) {}

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
    int _col = 0;                            // 0 表示列未知
    const char* _code = "E0000";             // 指向 ErrorCode 表中的静态字面量
    DiagSeverity _sev = DiagSeverity::Error; // 默认严重等级（来源于 ErrorCodeDef.defaultSev）
    vector<string> _hints;                   // 修复建议（"= help: ..."），可链式 withHint 追加
    vector<string> _notes;                   // 附加说明（"= note: ..."），可链式 withNote 追加

public:
    explicit YuxError(const string& msg, size_t line) : runtime_error(msg), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... Types>
    explicit YuxError(size_t line, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... Types>
    explicit YuxError(size_t line, int col, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line), _col(col) {
        assert(line > 0 && "YuxError line must be > 0");
    }

    template <class... Types>
    explicit YuxError(SourceLocation loc, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(loc.line), _col(loc.col) {
        assert(loc.line > 0 && "YuxError line must be > 0");
    }

    // ErrorCode 路径：模板取自 ec.message，code 取自 ec.code
    template <class... Types>
    explicit YuxError(size_t line, int col, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _col(col), _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "YuxError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    // 列未知场景的便利重载（驱动层 / 模块层 errorLine）
    template <class... Types>
    explicit YuxError(size_t line, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "YuxError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    template <class... Types>
    explicit YuxError(SourceLocation loc, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(loc.line),
          _col(loc.col), _code(ec.code), _sev(ec.defaultSev) {
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

    [[nodiscard]] size_t getLineNumber() const { return _line; }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {static_cast<int>(_line), _col}; }

    [[nodiscard]] const char* getCode() const { return _code; }

    [[nodiscard]] DiagSeverity getSeverity() const { return _sev; }

    // 链式追加 help / note：支持 `throw YuxError(...).withHint("...")` 形态
    YuxError& withHint(string h) & {
        _hints.push_back(std::move(h));
        return *this;
    }
    YuxError&& withHint(string h) && {
        _hints.push_back(std::move(h));
        return std::move(*this);
    }
    YuxError& withNote(string n) & {
        _notes.push_back(std::move(n));
        return *this;
    }
    YuxError&& withNote(string n) && {
        _notes.push_back(std::move(n));
        return std::move(*this);
    }

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
    Normal,       // 普通具名类型（内置标量 / 用户 struct 名 / Self）
    Generic,      // 用户定义泛型结构体实例化（如 MyVec<i32>）
    Rc,           // 内置 Rc<T> 智能指针
    Ref,          // 内置 Ref<T> / T& 引用
    Weak,         // 内置 Weak<T> 弱引用
    Heap,         // 内置 Heap<T> 堆作用域句柄
    Dyn,          // 内置 Dyn<D> 动态分发
    ArrayGeneric, // 内置 Array<T> 动态数组
    Nullable,     // 内置 Nullable<T> / T?
    Ptr,          // 内置原始指针（void*）
    Array,        // 固定大小数组 [T * N]
    Tuple,        // 元组 (T1, T2, ...)
    Fn            // 函数类型 Function<P1, ..., Pn, Ret>（结构等同；末位为返回类型）
};

// 元组类型构造时使用的 tag，用来与 Generic 构造区分
struct TupleTag {};

// 函数类型构造 tag；fnRet 为返回类型（unit 时传 nullptr 或空 TypeInfo）
struct FnTag {};

// 根据内置泛型包装名称返回对应 TypeKind；非内置名返回 TypeKind::Generic
// 用于 TypeInfo(string, vector<sp<TypeInfo>>) 构造函数自动分发，消除字符串比对
inline TypeKind kindForBuiltinWrapper(const string& name) {
    if (name == "Rc") return TypeKind::Rc;
    if (name == "Ref") return TypeKind::Ref;
    if (name == "Weak") return TypeKind::Weak;
    if (name == "Heap") return TypeKind::Heap;
    if (name == "Dyn") return TypeKind::Dyn;
    if (name == "Array") return TypeKind::ArrayGeneric;
    if (name == "Nullable") return TypeKind::Nullable;
    return TypeKind::Generic;
}

struct TypeInfo {
    TypeKind kind = TypeKind::Normal;
    string name;
    u64 arraySize = 0;
    sp<TypeInfo> elementType = nullptr; // Array 元素类型 / Fn 返回类型（unit 时为 nullptr）
    vector<sp<TypeInfo>> genericArgs;   // Generic 实参 / Tuple 元素 / Fn 形参类型列表
    bool fnNullable = false;            // Fn: Function<...>? 可空（仍 16 字节 fat-ptr，不套 Nullable）

    TypeInfo() = default;

    // 普通具名类型构造（内置标量 / 用户 struct 名 / Self）
    // "Ptr" 自动识别为 TypeKind::Ptr（null 字面量类型）
    explicit TypeInfo(string n) : name(std::move(n)) {
        if (name == "Ptr") kind = TypeKind::Ptr;
    }

    // 泛型实例化构造：根据 name 自动分发到正确的 TypeKind
    // 内置包装（Rc/Ref/Weak/Heap/Dyn/Array/Nullable）→ 对应专有 kind
    // Function<P..., Ret> → TypeKind::Fn（末位为返回类型）
    // Nullable<Function<...>> 折叠为 Fn + fnNullable（可空仍是 16 字节 fat-ptr）
    // 其他 → TypeKind::Generic（用户定义泛型结构体）
    TypeInfo(string n, vector<sp<TypeInfo>> args);

    TypeInfo(sp<TypeInfo> elemType, u64 size)
        : kind(TypeKind::Array), arraySize(size), elementType(std::move(elemType)) {
        name = "[" + elementType->name + " * " + to_string(arraySize) + "]";
    }

    // 元组类型 (T1, T2, ...)
    // 元素列表复用 genericArgs 存储；name 合成为 "(T1,T2,...)" 形式
    TypeInfo(TupleTag, vector<sp<TypeInfo>> elements) : kind(TypeKind::Tuple), genericArgs(std::move(elements)) {
        name = "(";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) name += ',';
            name += genericArgs[i] ? genericArgs[i]->name : "?";
        }
        name += ')';
    }

    // 函数类型 Function<P..., Ret> / Function<...>?
    // 形参类型列表存 genericArgs；返回类型存 elementType（unit 时为 nullptr）
    // 末位永远是返回类型；unit 返回写 ()
    TypeInfo(FnTag, vector<sp<TypeInfo>> paramTypes, sp<TypeInfo> retType, bool nullable = false)
        : kind(TypeKind::Fn), elementType(std::move(retType)), genericArgs(std::move(paramTypes)),
          fnNullable(nullable) {
        // 规范化：() 返回类型等价于省略 retType（皆为 unit）
        if (elementType && elementType->kind == TypeKind::Tuple && elementType->genericArgs.empty()) {
            elementType = nullptr;
        }
        rebuildFnName();
    }

    void rebuildFnName() {
        name = "Function<";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) name += ',';
            name += genericArgs[i] ? genericArgs[i]->name : "?";
        }
        if (!genericArgs.empty()) name += ',';
        name += elementType ? elementType->name : "()";
        name += '>';
        if (fnNullable) name += '?';
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

    // 函数类型返回值（unit 时为 nullptr）
    [[nodiscard]] sp<TypeInfo> fnReturnType() const { return elementType; }

    [[nodiscard]] bool empty() const { return name.empty(); }

    // 标量浮点 f32/f64（不靠 name[0]=='f'，避免用户类型名误命中）
    [[nodiscard]] bool isFloat() const { return kind == TypeKind::Normal && (name == "f32" || name == "f64"); }

    // 无符号整数：u8/u16/u32/u64/usize
    [[nodiscard]] bool isUnsigned() const {
        return kind == TypeKind::Normal &&
               (name == "u8" || name == "u16" || name == "u32" || name == "u64" || name == "usize");
    }

    // 方法上下文占位名；仍是 Normal，只是不许散落 "Self" 字符串
    [[nodiscard]] bool isSelf() const { return kind == TypeKind::Normal && name == "Self"; }

    [[nodiscard]] bool isRef() const { return kind == TypeKind::Ref && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> refElementType() const {
        if (isRef() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isRc() const { return kind == TypeKind::Rc && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> rcElementType() const {
        if (isRc() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Heap<T>：堆作用域句柄（DRAFT-heap-types §8.3a），layout = 裸 T*
    // 与 Rc<T> 不同：无 RC 头、单所有权、作用域绑定析构、不可装入 Rc/Weak（§8.3a.5.1）
    [[nodiscard]] bool isHeap() const { return kind == TypeKind::Heap && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> heapElementType() const {
        if (isHeap() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Weak<T>：弱引用，layout 与 Rc<T> 同形 { ptr handle }
    // handle 指向 Rc 的 Block；weak 计数维护 block 存活，不维护 payload 存活
    [[nodiscard]] bool isWeak() const { return kind == TypeKind::Weak && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> weakElementType() const {
        if (isWeak() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isPtr() const { return kind == TypeKind::Ptr; }

    [[nodiscard]] sp<TypeInfo> ptrElementType() const {
        if (isPtr() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    [[nodiscard]] bool isArrayGeneric() const { return kind == TypeKind::ArrayGeneric && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> arrayGenericElementType() const {
        if (isArrayGeneric() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // Dyn<D> / Dyn<D&>：draft 运行时多态形态（DRAFT-dyn-draft / 拟 §12.9）
    // layout = { vtable_ptr, data_ptr } 16 字节 fat pointer。
    // 内层若为 Ref<D> 则是借用形态 (Dyn<D&>)，否则 owned。
    [[nodiscard]] bool isDyn() const { return kind == TypeKind::Dyn && genericArgs.size() == 1; }

    [[nodiscard]] bool isDynBorrow() const { return isDyn() && genericArgs[0] && genericArgs[0]->isRef(); }

    [[nodiscard]] bool isDynOwned() const { return isDyn() && genericArgs[0] && !genericArgs[0]->isRef(); }

    // 拿 D（剥掉借用形态外层的 Ref）。
    [[nodiscard]] sp<TypeInfo> dynSpecType() const {
        if (!isDyn() || !genericArgs[0]) return nullptr;
        if (genericArgs[0]->isRef()) return genericArgs[0]->refElementType();
        return genericArgs[0];
    }

    // Nullable<T>：T? 解糖后的类型；layout = { bool _has; T _value }
    [[nodiscard]] bool isNullable() const { return kind == TypeKind::Nullable && genericArgs.size() == 1; }

    [[nodiscard]] sp<TypeInfo> nullableInnerType() const {
        if (isNullable() && genericArgs.size() == 1) {
            return genericArgs[0];
        }
        return nullptr;
    }

    // 堆句柄 String（B-4：`{ _buf: Rc<Array<u32>> }`），不是用户 struct 名碰巧叫 String
    [[nodiscard]] bool isString() const { return kind == TypeKind::Normal && name == "String"; }

    // 8 字节堆句柄：Rc / Weak / Array<T> / String。不含 Heap（单所有权裸指针）
    [[nodiscard]] bool isRcHandle() const { return isRc() || isWeak() || isArrayGeneric() || isString(); }

    // 剥一层 Ref<T> → T；非 Ref 原样返回
    [[nodiscard]] TypeInfo peelRef() const {
        if (isRef()) {
            if (auto inner = refElementType()) return *inner;
        }
        return *this;
    }

    // 运算符自动解引用：Ref → Heap → Rc，各一层（与 expr getType 历史行为一致）
    [[nodiscard]] TypeInfo peelAutoDeref() const {
        TypeInfo t = peelRef();
        if (t.isHeap()) {
            if (auto inner = t.heapElementType()) t = *inner;
        }
        if (t.isRc()) {
            if (auto inner = t.rcElementType()) t = *inner;
        }
        return t;
    }

    // 是否有类型实参：Generic（用户泛型）或内置包装类型
    [[nodiscard]] bool hasGenericArgs() const {
        switch (kind) {
        case TypeKind::Generic:
        case TypeKind::Rc:
        case TypeKind::Ref:
        case TypeKind::Weak:
        case TypeKind::Heap:
        case TypeKind::Dyn:
        case TypeKind::ArrayGeneric:
        case TypeKind::Nullable:
            return true;
        default:
            return false;
        }
    }

    // 完整类型名（与 yux 源码写法一致，用于报错/调试输出）：
    // 泛型 Base<Arg1,Arg2>，元组 (T1,T2)，函数 Function<P...,Ret>，数组 [E*N]
    [[nodiscard]] string getFullName() const {
        if (hasGenericArgs() && !genericArgs.empty()) {
            string result = name + "<";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ',';
                result += genericArgs[i]->getFullName();
            }
            result += '>';
            return result;
        }
        if (kind == TypeKind::Array && elementType) {
            return "[" + elementType->getFullName() + "*" + std::to_string(arraySize) + "]";
        }
        // 元组：(T1,T2)
        if (kind == TypeKind::Tuple) {
            string result = "(";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ',';
                result += genericArgs[i] ? genericArgs[i]->getFullName() : string("?");
            }
            result += ')';
            return result;
        }
        // 函数：Function<P...,Ret> / Function<...>?
        if (kind == TypeKind::Fn) {
            return formatFnGeneric(false);
        }
        return name;
    }

    // LLVM 符号用 mangle 名（与 yux 源码写法一致）：
    // 泛型 Base<Arg1,Arg2>，元组 (T1,T2)，函数 Function<P...,Ret>，数组 [E*N]
    [[nodiscard]] string getMangleName() const {
        if (hasGenericArgs() && !genericArgs.empty()) {
            string result = name + "<";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ',';
                result += genericArgs[i]->getMangleName();
            }
            result += '>';
            return result;
        }
        if (kind == TypeKind::Array && elementType) {
            return "[" + elementType->getMangleName() + "*" + std::to_string(arraySize) + "]";
        }
        // 元组：(T1,T2)
        if (kind == TypeKind::Tuple) {
            string result = "(";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ',';
                result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
            }
            result += ')';
            return result;
        }
        // 函数：Function<P...,Ret> / Function<...>?
        if (kind == TypeKind::Fn) {
            return formatFnGeneric(true);
        }
        return name;
    }

    // struct decl 查找用基名：剥 `Foo<Arg>` / `Foo$Arg` 修饰。
    // TypeGenericNode 的 name 已是基名；此处防御完整名或 LLVM 修饰名被塞进 name 的路径。
    [[nodiscard]] string baseStructName() const {
        string n = name;
        auto cut = n.find_first_of("$<");
        if (cut != string::npos) {
            n.resize(cut);
        }
        return n;
    }

    [[nodiscard]] string formatFnGeneric(bool mangle) const {
        string result = "Function<";
        for (size_t i = 0; i < genericArgs.size(); ++i) {
            if (i > 0) result += ',';
            if (!genericArgs[i]) {
                result += '?';
            } else {
                result += mangle ? genericArgs[i]->getMangleName() : genericArgs[i]->getFullName();
            }
        }
        if (!genericArgs.empty()) result += ',';
        if (!elementType) {
            result += "()";
        } else {
            result += mangle ? elementType->getMangleName() : elementType->getFullName();
        }
        result += '>';
        if (fnNullable) result += '?';
        return result;
    }

    // 应用类型形参替换。无类型实参的具名类型（Normal / Ptr / 空 Generic）匹配 subst 键则整体替换。
    [[nodiscard]] TypeInfo substitute(const std::map<std::string, TypeInfo>& subst) const {
        if (kind == TypeKind::Normal || kind == TypeKind::Ptr || (kind == TypeKind::Generic && genericArgs.empty())) {
            auto it = subst.find(name);
            if (it != subst.end()) return it->second;
            if (kind != TypeKind::Generic) return *this;
        }
        if (hasGenericArgs() && !genericArgs.empty()) {
            vector<sp<TypeInfo>> newArgs;
            newArgs.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newArgs.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            return {name, std::move(newArgs)};
        }
        if (kind == TypeKind::Array && elementType) {
            auto sub = elementType->substitute(subst);
            return {std::make_shared<TypeInfo>(std::move(sub)), arraySize};
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
        if (hasGenericArgs() || kind == TypeKind::Tuple) {
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
            // 返回类型：unit（nullptr）也参与判等
            if (!elementType && !other.elementType) return true;
            if (!elementType || !other.elementType) return false;
            return *elementType == *other.elementType;
        }
        return true;
    }

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }
};

inline TypeInfo::TypeInfo(string n, vector<sp<TypeInfo>> args) {
    // Function<P..., Ret>：末位是返回类型，其余是形参
    if (n == "Function") {
        kind = TypeKind::Fn;
        if (!args.empty()) {
            elementType = args.back();
            genericArgs.assign(args.begin(), args.end() - 1);
            if (elementType && elementType->kind == TypeKind::Tuple && elementType->genericArgs.empty()) {
                elementType = nullptr;
            }
        }
        rebuildFnName();
        return;
    }
    // Function<...>? 走标准 `?` 后缀，AST 会包一层 Nullable；折叠回 fat-ptr 可空
    if (n == "Nullable" && args.size() == 1 && args[0] && args[0]->isFn() && !args[0]->fnNullable) {
        *this = *args[0];
        fnNullable = true;
        rebuildFnName();
        return;
    }
    kind = kindForBuiltinWrapper(n);
    name = std::move(n);
    genericArgs = std::move(args);
}

inline bool isBuiltinType(const string& typeName) {
    static const vector<string> builtinTypes = {"bool", "i8",  "i16", "i32", "i64",   "u8",   "u16",
                                                "u32",  "u64", "f32", "f64", "isize", "usize"};
    return std::ranges::find(builtinTypes, typeName) != builtinTypes.end();
}

// E4025: Rc / Weak / Array 禁止直接内嵌 Heap（§8.3a.5.1）。递归下钻，覆盖
// `Rc<Rc<Heap<T>>>` / `Array<Rc<Heap<T>>>`。sema 与 getLLVMType 共用。
inline void validateNoNestedHeap(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Rc"), inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Weak"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw YuxError(line, col, ErrorCode::E4025, std::string("Array"),
                               inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) validateNoNestedHeap(*e, line, col);
        return;
    }
    for (const auto& g : t.genericArgs) {
        if (g) validateNoNestedHeap(*g, line, col);
    }
}

// E1132: Rc / Weak 禁止直接内嵌 Dyn（§12.9.3.2）。递归下钻，覆盖
// `Rc<Rc<Dyn<D>>>`。Array<Dyn<D>> 合法，不在此禁。sema 与 getLLVMType 共用。
inline void validateNoDynInRcWeak(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isDyn()) {
                throw YuxError(line, col, ErrorCode::E1132, std::string("Rc<") + e->getFullName() + ">");
            }
            validateNoDynInRcWeak(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isDyn()) {
                throw YuxError(line, col, ErrorCode::E1132, std::string("Weak<") + e->getFullName() + ">");
            }
            validateNoDynInRcWeak(*e, line, col);
        }
        return;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) validateNoDynInRcWeak(*e, line, col);
        return;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) validateNoDynInRcWeak(*e, line, col);
        return;
    }
    for (const auto& g : t.genericArgs) {
        if (g) validateNoDynInRcWeak(*g, line, col);
    }
}

inline void validateRcContainerBans(const TypeInfo& t, int line, int col) {
    validateNoNestedHeap(t, line, col);
    validateNoDynInRcWeak(t, line, col);
}
