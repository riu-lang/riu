// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "error_code.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
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

// Token 文本 intern：空串进程单例；其余挂当前 StringIntern（Riu 构造 push）。
[[nodiscard]] inline const string& emptyTokenText() {
    static const string kEmpty;
    return kEmpty;
}
[[nodiscard]] const string& internTokenText(string_view s);

// 恰好 `_`：丢弃槽（[#20]）。`_foo` 仍是普通私有名。
[[nodiscard]] inline bool isDiscardName(string_view name) noexcept {
    return name.size() == 1 && name.front() == '_';
}

[[nodiscard]] inline bool lastPathSegIsDiscard(string_view dotted) noexcept {
    auto pos = dotted.rfind('.');
    auto last = pos == string_view::npos ? dotted : dotted.substr(pos + 1);
    return isDiscardName(last);
}

class TokenInfo {
    const string* _text = nullptr;
    size_t _line = 0;
    size_t _charPositionInLine = 0;
    size_t _tokenIndex = 0;
    size_t _startIndex = 0;
    size_t _stopIndex = 0;

public:
    TokenInfo() = default;

    // 合成 Token：解糖时没有源 token 的节点（如 T? → Nullable<T> 的 "Nullable"）
    TokenInfo(string_view text, size_t line) : _text(&internTokenText(text)), _line(line) {}

    // 从 rd::Pos 填行列与字节区间。charPositionInLine 是 0-based 行内 UTF-8 字节
    // （rd::Pos.column）；start/stop 为 UTF-8 字节，stop 是闭区间（半开 end-1）。
    TokenInfo(string_view text, size_t line, size_t charPositionInLine, size_t tokenIndex, size_t startIndex,
              size_t stopIndex)
        : _text(&internTokenText(text)), _line(line), _charPositionInLine(charPositionInLine), _tokenIndex(tokenIndex),
          _startIndex(startIndex), _stopIndex(stopIndex) {}

    TokenInfo(const TokenInfo& other) = default;
    TokenInfo(TokenInfo&& other) noexcept = default;
    TokenInfo& operator=(const TokenInfo& other) = default;
    TokenInfo& operator=(TokenInfo&& other) noexcept = default;

    [[nodiscard]] const string& getText() const { return _text ? *_text : emptyTokenText(); }
    [[nodiscard]] size_t getLine() const { return _line; }
    [[nodiscard]] size_t getCharPositionInLine() const { return _charPositionInLine; } // 0-based 行内 UTF-8 字节
    [[nodiscard]] size_t getTokenIndex() const { return _tokenIndex; }
    [[nodiscard]] size_t getStartIndex() const { return _startIndex; }
    [[nodiscard]] size_t getStopIndex() const { return _stopIndex; }

    [[nodiscard]] bool empty() const { return !_text || _text->empty(); }
    [[nodiscard]] bool valid() const { return !empty() || _line > 0; }

    explicit operator bool() const { return valid(); }

    bool operator==(const TokenInfo& other) const { return getText() == other.getText() && _line == other._line; }
    bool operator!=(const TokenInfo& other) const { return !(*this == other); }
};

using Token = TokenInfo;

// 源码限定类型路径 `a.b.T`（riu.bnf typePath）。身份是 TypeInfo.ownerModule + 短名；
// TypeInfo.name 只用末段短名，路径不进 name。
// 一段名（`i32`）不进 heap：混测几乎全是裸名，vector<Token> 在 debug CRT 上按节点一份头。
struct TypePath {
    Token first;
    vector<Token> rest; // segs[1..]

    TypePath() = default;
    explicit TypePath(Token bare) : first(bare) {}
    explicit TypePath(vector<Token> s) {
        if (s.empty()) return;
        first = s[0];
        if (s.size() > 1) {
            rest.assign(s.begin() + 1, s.end());
        }
    }

    [[nodiscard]] bool empty() const { return first.empty() && rest.empty(); }
    [[nodiscard]] size_t size() const { return empty() ? 0 : 1 + rest.size(); }
    [[nodiscard]] bool isBare() const { return rest.empty() && !first.empty(); }
    [[nodiscard]] const Token& last() const { return rest.empty() ? first : rest.back(); }
    [[nodiscard]] const Token& operator[](size_t i) const { return i == 0 ? first : rest[i - 1]; }
    [[nodiscard]] const string& lastName() const {
        static const string kEmpty;
        return empty() ? kEmpty : last().getText();
    }
    [[nodiscard]] string dotted() const {
        if (empty()) return {};
        string s = first.getText();
        for (const auto& t : rest) {
            s += '.';
            s += t.getText();
        }
        return s;
    }
    void push_back(Token t) {
        if (empty())
            first = t;
        else
            rest.push_back(t);
    }
    void pop_back() {
        if (!rest.empty())
            rest.pop_back();
        else
            first = {};
    }
};

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

class RiuError : public std::runtime_error {
    size_t _line = 0;
    int _col = 0;                            // 0 表示列未知
    const char* _code = "E0000";             // 指向 ErrorCode 表中的静态字面量
    DiagSeverity _sev = DiagSeverity::Error; // 默认严重等级（来源于 ErrorCodeDef.defaultSev）
    vector<string> _hints;                   // 修复建议（"= help: ..."），可链式 withHint 追加
    vector<string> _notes;                   // 附加说明（"= note: ..."），可链式 withNote 追加
    string _file;                            // 出错节点所属源文件；空 = 由渲染入口 sourcePath 决定

public:
    explicit RiuError(const string& msg, size_t line) : runtime_error(msg), _line(line) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(size_t line, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(size_t line, int col, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(line), _col(col) {
        assert(line > 0 && "RiuError line must be > 0");
    }

    template <class... Types>
    explicit RiuError(SourceLocation loc, const format_string<Types...> format, Types&&... args)
        : runtime_error(std::vformat(format.get(), std::make_format_args(args...))), _line(loc.line), _col(loc.col) {
        assert(loc.line > 0 && "RiuError line must be > 0");
    }

    // ErrorCode 路径：模板取自 ec.message，code 取自 ec.code
    template <class... Types>
    explicit RiuError(size_t line, int col, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _col(col), _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    // 列未知场景的便利重载（驱动层 / 模块层 errorLine）
    template <class... Types>
    explicit RiuError(size_t line, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(line),
          _code(ec.code), _sev(ec.defaultSev) {
        assert(line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    template <class... Types>
    explicit RiuError(SourceLocation loc, const ErrorCodeDef& ec, Types&&... args)
        : runtime_error(std::vformat(std::string_view(ec.message), std::make_format_args(args...))), _line(loc.line),
          _col(loc.col), _code(ec.code), _sev(ec.defaultSev) {
        assert(loc.line > 0 && "RiuError line must be > 0");
#ifndef NDEBUG
        assert(sizeof...(args) == countFmtPlaceholders(ec.message) && "ErrorCode format arg count mismatch");
#endif
    }

    void setLineNumber(size_t line) {
        assert(line > 0 && "RiuError line must be > 0");
        _line = line;
    }

    void setColumn(int col) { _col = col; }

    void setFile(string f) { _file = std::move(f); }

    [[nodiscard]] size_t getLineNumber() const { return _line; }

    [[nodiscard]] int getColumn() const { return _col; }

    [[nodiscard]] SourceLocation location() const { return {static_cast<int>(_line), _col}; }

    [[nodiscard]] const char* getCode() const { return _code; }

    [[nodiscard]] DiagSeverity getSeverity() const { return _sev; }

    // 出错节点所属源文件。空 = 渲染时回退到入口传入的正在编译文件。
    [[nodiscard]] const string& file() const { return _file; }

    RiuError& withFile(string f) & {
        _file = std::move(f);
        return *this;
    }
    RiuError&& withFile(string f) && {
        _file = std::move(f);
        return std::move(*this);
    }

    // 链式追加 help / note：支持 `throw RiuError(...).withHint("...")` 形态
    RiuError& withHint(string h) & {
        _hints.push_back(std::move(h));
        return *this;
    }
    RiuError&& withHint(string h) && {
        _hints.push_back(std::move(h));
        return std::move(*this);
    }
    RiuError& withNote(string n) & {
        _notes.push_back(std::move(n));
        return *this;
    }
    RiuError&& withNote(string n) && {
        _notes.push_back(std::move(n));
        return std::move(*this);
    }

    [[nodiscard]] const vector<string>& hints() const { return _hints; }
    [[nodiscard]] const vector<string>& notes() const { return _notes; }

    // 显式声明拷贝 / 移动构造 noexcept：throw RiuError 在抛出栈展开期间不允许再次抛异常；
    // 真正的 OOM 走 std::terminate（语义上等价于 runtime_error 自身的承诺）
    // NOLINTBEGIN(bugprone-exception-escape)
    RiuError(const RiuError&) noexcept = default;
    RiuError(RiuError&&) noexcept = default;
    RiuError& operator=(const RiuError&) noexcept = default;
    RiuError& operator=(RiuError&&) noexcept = default;
    // NOLINTEND(bugprone-exception-escape)
};

inline void checkDiscardDeclName(string_view name, string_view kind, int line, int col) {
    if (isDiscardName(name)) {
        throw RiuError(line, col, ErrorCode::E3161, string(kind));
    }
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
    Ptr,          // 内置 Ptr<T=()>；ABI 永远一指针字
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
inline TypeKind kindForBuiltinWrapper(string_view name) {
    if (name == "Rc") return TypeKind::Rc;
    if (name == "Ref") return TypeKind::Ref;
    if (name == "Weak") return TypeKind::Weak;
    if (name == "Heap") return TypeKind::Heap;
    if (name == "Dyn") return TypeKind::Dyn;
    if (name == "Array") return TypeKind::ArrayGeneric;
    if (name == "Nullable") return TypeKind::Nullable;
    if (name == "Ptr") return TypeKind::Ptr;
    return TypeKind::Generic;
}

struct TypeInfo {
    TypeKind kind = TypeKind::Normal;
    string name;
    // 声明模块（点分）。空 = 内建 / 未解析。身份是 (ownerModule, 短名)；路径不进 name。
    string ownerModule;
    u64 arraySize = 0;
    sp<TypeInfo> elementType = nullptr; // Array 元素类型 / Fn 返回类型（unit 时为 nullptr）
    vector<sp<TypeInfo>> genericArgs;   // Generic 实参 / Tuple 元素 / Fn 形参类型列表
    bool fnNullable = false;            // Fn: Function<...>? 可空（仍 16 字节 fat-ptr，不套 Nullable）
    string fallibleErr; // T ! E 的错误类型 E 的 getFullName；空 = 非 fallible。须存完整写法（`Box<String>` 而非裸名
                        // `Box`）
    // intern 后的成功类型（剥 fallible）；空 = 尚未 intern 或本身非 fallible。
    mutable const TypeInfo* _withoutFallible = nullptr;

    TypeInfo() = default;

    // 普通具名类型构造（内置标量 / 用户 struct 名 / Self）
    // 裸 "Ptr" ≡ Ptr<()>（#21）；owner：声明模块；空 = 内建或尚未 resolveTypePath
    explicit TypeInfo(string n, string owner = {}) : name(std::move(n)), ownerModule(std::move(owner)) {
        if (name == "Ptr") {
            kind = TypeKind::Ptr;
            genericArgs.push_back(std::make_shared<TypeInfo>(TupleTag{}, vector<sp<TypeInfo>>{}));
        }
    }

    // 泛型实例化构造：根据 name 自动分发到正确的 TypeKind
    // 内置包装（Rc/Ref/Weak/Heap/Dyn/Array/Nullable/Ptr）→ 对应专有 kind
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
            name += genericArgs[i] ? genericArgs[i]->getFullName() : "?";
        }
        if (!genericArgs.empty()) name += ',';
        name += elementType ? elementType->getFullName() : "()";
        name += '>';
        if (fnNullable) name += '?';
    }

    // T ! E 签名位：fallibleErr 非空即 fallible 类型（ABI 仍按成功类型 T）
    [[nodiscard]] bool isFallible() const { return !fallibleErr.empty(); }

    // 剥掉 fallible 后缀，保留成功类型 T（intern 后的引用；供 LLVM / 值类型判等）
    [[nodiscard]] const TypeInfo& withoutFallible() const;

    // 在成功类型上附加 fallible 后缀，刷新 name。`err` 须是 E 的 getFullName。
    void attachFallibleErr(string err) {
        fallibleErr = std::move(err);
        if (fallibleErr.empty()) return;
        const string base = withoutFallible().getFullName();
        name = base + "!" + fallibleErr;
    }

    // 从 getFullName 写法还原 TypeInfo（`Name` / `Name<A,B>` / 嵌套）。
    // 用于 T ! E 字符串槽还原 LLVM 类型；对不上的输入当裸名。
    [[nodiscard]] static TypeInfo fromFullName(const string& s);

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

    // 两边都有 owner 才比模块；任一侧为空则只比短名（未解析 / 内建兼容）
    [[nodiscard]] bool sameOwner(const TypeInfo& other) const {
        return ownerModule.empty() || other.ownerModule.empty() || ownerModule == other.ownerModule;
    }

    // LLVM 缓存 / mangle 用：(owner, 短名)；owner 空则短名
    [[nodiscard]] string identityKey() const {
        if (ownerModule.empty()) return name;
        return ownerModule + "." + name;
    }

    // 零元素元组 `()`：与省略 retType 的 unit / void 等同（§3.8.1.0）
    [[nodiscard]] bool isUnit() const { return kind == TypeKind::Tuple && genericArgs.empty(); }

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

    [[nodiscard]] bool isStringBuilder() const { return kind == TypeKind::Normal && name == "StringBuilder"; }

    // 8 字节堆句柄：Rc / Weak / Array<T> / String。不含 Heap（单所有权裸指针）
    [[nodiscard]] bool isRcHandle() const { return isRc() || isWeak() || isArrayGeneric() || isString(); }

    // 剥一层 Ref<T> → T；非 Ref 原样返回。走已 intern 的 genericArgs[0]，不拷贝。
    [[nodiscard]] const TypeInfo& peelRef() const {
        if (isRef() && genericArgs.size() == 1 && genericArgs[0]) return *genericArgs[0];
        return *this;
    }

    // 运算符自动解引用：Ref → Heap → Rc，各一层（与 expr getType 历史行为一致）
    [[nodiscard]] const TypeInfo& peelAutoDeref() const {
        const TypeInfo* t = &peelRef();
        if (t->isHeap()) {
            if (auto inner = t->heapElementType()) t = inner.get();
        }
        if (t->isRc()) {
            if (auto inner = t->rcElementType()) t = inner.get();
        }
        return *t;
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
        case TypeKind::Ptr:
            return true;
        default:
            return false;
        }
    }

    // 完整类型名（与 riu 源码写法一致，用于报错/调试输出）：
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
        if (!fallibleErr.empty()) {
            const string suffix = "!" + fallibleErr;
            if (!name.ends_with(suffix)) return name + suffix;
        }
        return name;
    }

    // LLVM 符号用 mangle 名（与 riu 声明/调用写法同形）：
    // 有 owner → `mod.Name` / `mod.Name<Arg1,Arg2>`；内建无 owner → 短名。
    // 元组 (T1,T2)，函数 Function<P...,Ret>，数组 [E*N]。分隔只用 `.` `::` `<>` `()` `@`。
    [[nodiscard]] string getMangleName() const {
        auto head = [this]() -> string {
            string n = withoutFallible().name;
            if (kind == TypeKind::Array || kind == TypeKind::Tuple || kind == TypeKind::Fn) return n;
            if (ownerModule.empty()) return n;
            return ownerModule + "." + n;
        };
        if (hasGenericArgs() && !genericArgs.empty()) {
            string result = head() + "<";
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (i > 0) result += ',';
                result += genericArgs[i] ? genericArgs[i]->getMangleName() : string("?");
            }
            result += '>';
            if (!fallibleErr.empty()) result += "!" + fallibleErr;
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
        if (!fallibleErr.empty()) return head() + "!" + fallibleErr;
        return head();
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

    // 应用类型形参替换。无类型实参的具名类型（Normal / 空 Generic）匹配 subst 键则整体替换。
    // Ptr<T> 走下方 genericArgs 替换（#21）。T ! E 的 E 按 getFullName 还原后再 subst。
    [[nodiscard]] TypeInfo substitute(const std::map<std::string, TypeInfo>& subst) const {
        auto withErr = [&](TypeInfo r) -> TypeInfo {
            if (fallibleErr.empty()) return r;
            TypeInfo err = TypeInfo::fromFullName(fallibleErr).substitute(subst);
            r.attachFallibleErr(err.getFullName());
            return r;
        };
        if (kind == TypeKind::Normal || (kind == TypeKind::Generic && genericArgs.empty())) {
            auto it = subst.find(name);
            if (it != subst.end()) return withErr(it->second);
            if (kind != TypeKind::Generic) return withErr(*this);
        }
        if (hasGenericArgs() && !genericArgs.empty()) {
            vector<sp<TypeInfo>> newArgs;
            newArgs.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newArgs.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            TypeInfo r{name, std::move(newArgs)};
            r.ownerModule = ownerModule;
            return withErr(std::move(r));
        }
        if (kind == TypeKind::Array && elementType) {
            auto sub = elementType->substitute(subst);
            return withErr({std::make_shared<TypeInfo>(std::move(sub)), arraySize});
        }
        if (kind == TypeKind::Tuple) {
            vector<sp<TypeInfo>> newElems;
            newElems.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newElems.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            return withErr(TypeInfo(TupleTag{}, std::move(newElems)));
        }
        if (kind == TypeKind::Fn) {
            vector<sp<TypeInfo>> newParams;
            newParams.reserve(genericArgs.size());
            for (auto& a : genericArgs) {
                newParams.push_back(std::make_shared<TypeInfo>(a ? a->substitute(subst) : TypeInfo()));
            }
            sp<TypeInfo> newRet = nullptr;
            if (elementType) newRet = std::make_shared<TypeInfo>(elementType->substitute(subst));
            return withErr(TypeInfo(FnTag{}, std::move(newParams), newRet, fnNullable));
        }
        return withErr(*this);
    }

    bool operator==(const TypeInfo& other) const {
        if (kind != other.kind) return false;
        if (fallibleErr != other.fallibleErr) return false;
        if (kind == TypeKind::Array) {
            if (arraySize != other.arraySize) return false;
            if (!elementType && !other.elementType) return true;
            if (!elementType || !other.elementType) return false;
            return *elementType == *other.elementType;
        }
        // 裸 Ptr 与 Ptr<()> 同型（旧 .ud / 未 intern 的空 genericArgs）
        if (kind == TypeKind::Ptr) {
            auto payload = [](const TypeInfo& t) -> const TypeInfo* {
                return t.genericArgs.size() == 1 ? t.genericArgs[0].get() : nullptr;
            };
            const TypeInfo* a = payload(*this);
            const TypeInfo* b = payload(other);
            const bool aUnit = !a || a->isUnit();
            const bool bUnit = !b || b->isUnit();
            if (aUnit && bUnit) {
                return name == other.name && sameOwner(other);
            }
            if (!a || !b) return false;
            if (*a != *b) return false;
            return name == other.name && sameOwner(other);
        }
        if (hasGenericArgs() || kind == TypeKind::Tuple) {
            if (genericArgs.size() != other.genericArgs.size()) return false;
            for (size_t i = 0; i < genericArgs.size(); ++i) {
                if (!genericArgs[i] && !other.genericArgs[i]) continue;
                if (!genericArgs[i] || !other.genericArgs[i]) return false;
                if (*genericArgs[i] != *other.genericArgs[i]) return false;
            }
            if (kind == TypeKind::Generic) {
                if (name != other.name) return false;
                if (!sameOwner(other)) return false;
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
        if (kind == TypeKind::Normal || kind == TypeKind::Ptr) {
            // fallibleErr 已相等。热路径两边都是 `i32`：直接比 name，不要
            // withoutFallible() 整份拷 TypeInfo（含 vector/shared_ptr）。
            if (name != other.name) {
                auto strip = [](const TypeInfo& t) -> string_view {
                    if (t.fallibleErr.empty()) return t.name;
                    const size_t n = t.fallibleErr.size() + 1;
                    if (t.name.size() >= n && t.name[t.name.size() - n] == '!' &&
                        string_view(t.name).substr(t.name.size() - t.fallibleErr.size()) == t.fallibleErr) {
                        return string_view(t.name).substr(0, t.name.size() - n);
                    }
                    return t.name;
                };
                if (strip(*this) != strip(other)) return false;
            }
            if (!sameOwner(other)) return false;
        } else if (!hasGenericArgs() && kind != TypeKind::Tuple) {
            if (name != other.name) return false;
            if (!sameOwner(other)) return false;
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
    if (kind == TypeKind::Ptr && genericArgs.empty()) {
        genericArgs.push_back(std::make_shared<TypeInfo>(TupleTag{}, vector<sp<TypeInfo>>{}));
    }
}

// T ! E 错误通道的比较 / 存储键：完整写法（`Box<String>` 而非裸名 `Box`）
inline string fallibleErrKey(const TypeInfo& t) {
    return t.getFullName();
}

inline TypeInfo TypeInfo::fromFullName(const string& s) {
    if (s.empty()) return {};
    struct Parser {
        const string& s;
        size_t i = 0;
        TypeInfo parse() {
            const size_t start = i;
            while (i < s.size()) {
                const char c = s[i];
                if (c == '<' || c == '>' || c == ',') break;
                ++i;
            }
            string n = s.substr(start, i - start);
            if (i < s.size() && s[i] == '<') {
                ++i;
                vector<sp<TypeInfo>> args;
                if (i < s.size() && s[i] != '>') {
                    while (true) {
                        args.push_back(std::make_shared<TypeInfo>(parse()));
                        if (i < s.size() && s[i] == ',') {
                            ++i;
                            continue;
                        }
                        break;
                    }
                }
                if (i < s.size() && s[i] == '>') ++i;
                if (n.empty()) return {};
                return {std::move(n), std::move(args)};
            }
            if (n.empty()) return {};
            if (n == "()") return TypeInfo(TupleTag{}, vector<sp<TypeInfo>>{});
            return TypeInfo(std::move(n));
        }
    };
    Parser p{.s = s};
    TypeInfo t = p.parse();
    if (p.i != s.size() && t.empty()) return TypeInfo(s);
    return t;
}

inline bool isBuiltinType(string_view typeName) {
    switch (typeName.size()) {
    case 2:
        return typeName == "i8" || typeName == "u8";
    case 3:
        return typeName == "i16" || typeName == "u16" || typeName == "i32" || typeName == "u32" || typeName == "i64" ||
               typeName == "u64" || typeName == "f32" || typeName == "f64";
    case 4:
        return typeName == "bool";
    case 5:
        return typeName == "isize" || typeName == "usize";
    default:
        return false;
    }
}

// 语言内建具名类型：标量 / Ptr / Self / Function / Rc·Ref 等包装名（无实参时仍是这个名字）。
inline bool isLanguageNamedType(string_view n) {
    if (n.empty()) return false;
    if (isBuiltinType(n) || n == "Ptr" || n == "Self" || n == "Function") return true;
    return kindForBuiltinWrapper(n) != TypeKind::Generic;
}

// 语言具名类型的进程内单例。只对 isLanguageNamedType 有定义；指针可挂 AST 槽。
inline const TypeInfo& internNamedType(string_view name) {
    switch (name.size()) {
    case 2:
        if (name == "i8") {
            static const TypeInfo t{"i8"};
            return t;
        }
        if (name == "u8") {
            static const TypeInfo t{"u8"};
            return t;
        }
        if (name == "Rc") {
            static const TypeInfo t{"Rc"};
            return t;
        }
        break;
    case 3:
        if (name == "i16") {
            static const TypeInfo t{"i16"};
            return t;
        }
        if (name == "u16") {
            static const TypeInfo t{"u16"};
            return t;
        }
        if (name == "i32") {
            static const TypeInfo t{"i32"};
            return t;
        }
        if (name == "u32") {
            static const TypeInfo t{"u32"};
            return t;
        }
        if (name == "i64") {
            static const TypeInfo t{"i64"};
            return t;
        }
        if (name == "u64") {
            static const TypeInfo t{"u64"};
            return t;
        }
        if (name == "f32") {
            static const TypeInfo t{"f32"};
            return t;
        }
        if (name == "f64") {
            static const TypeInfo t{"f64"};
            return t;
        }
        if (name == "Ptr") {
            static const TypeInfo t{"Ptr"};
            return t;
        }
        if (name == "Dyn") {
            static const TypeInfo t{"Dyn"};
            return t;
        }
        if (name == "Ref") {
            static const TypeInfo t{"Ref"};
            return t;
        }
        break;
    case 4:
        if (name == "bool") {
            static const TypeInfo t{"bool"};
            return t;
        }
        if (name == "Self") {
            static const TypeInfo t{"Self"};
            return t;
        }
        if (name == "Heap") {
            static const TypeInfo t{"Heap"};
            return t;
        }
        if (name == "Weak") {
            static const TypeInfo t{"Weak"};
            return t;
        }
        break;
    case 5:
        if (name == "isize") {
            static const TypeInfo t{"isize"};
            return t;
        }
        if (name == "usize") {
            static const TypeInfo t{"usize"};
            return t;
        }
        if (name == "Array") {
            static const TypeInfo t{"Array"};
            return t;
        }
        break;
    case 8:
        if (name == "Function") {
            static const TypeInfo t{"Function"};
            return t;
        }
        if (name == "Nullable") {
            static const TypeInfo t{"Nullable"};
            return t;
        }
        break;
    default:
        break;
    }
    static const TypeInfo kEmpty;
    return kEmpty;
}

// 无实参 / 无 fallible / 无 owner 的语言具名类型 → intern 单例；其余 nullptr。
// 裸 Ptr 与 Ptr<()> 同一单例（#21）；空 genericArgs 的旧 Ptr 也归一。
inline void ensurePtrGenericArg(TypeInfo& t) {
    if (t.kind != TypeKind::Ptr || !t.genericArgs.empty()) return;
    t.genericArgs.push_back(std::make_shared<TypeInfo>(TupleTag{}, vector<sp<TypeInfo>>{}));
}

inline const TypeInfo* internTypePtr(const TypeInfo& t) {
    if (!t.fallibleErr.empty() || t.fnNullable || t.elementType || t.arraySize != 0) {
        return nullptr;
    }
    if (!t.ownerModule.empty()) return nullptr;
    if (t.kind == TypeKind::Ptr && t.name == "Ptr") {
        if (t.genericArgs.empty()) return &internNamedType("Ptr");
        if (t.genericArgs.size() == 1 && t.genericArgs[0] && t.genericArgs[0]->isUnit()) {
            return &internNamedType("Ptr");
        }
        return nullptr;
    }
    if (!t.genericArgs.empty()) return nullptr;
    if (t.kind != TypeKind::Normal) return nullptr;
    if (!isLanguageNamedType(t.name)) return nullptr;
    return &internNamedType(t.name);
}

// Token 文本 intern 表（挂在 Riu 上，与 RdBuilder / arena 同寿）。
class StringIntern {
    struct Impl;
    unique_ptr<Impl> _impl;

public:
    StringIntern();
    ~StringIntern();
    StringIntern(StringIntern&&) noexcept;
    StringIntern& operator=(StringIntern&&) noexcept;
    StringIntern(const StringIntern&) = delete;
    StringIntern& operator=(const StringIntern&) = delete;

    const string& intern(string_view s);
    void reserve(size_t n);
};

void bindStringIntern(StringIntern* intern);

// 编译期类型 intern 表（挂在 Riu 上）。子类型先 intern，按 kind / owner / name /
// 子指针 / fallible 哈希。语言标量仍走 internNamedType 进程单例。
class TypeIntern {
    struct Impl;
    unique_ptr<Impl> _impl;

public:
    TypeIntern();
    ~TypeIntern();
    TypeIntern(TypeIntern&&) noexcept;
    TypeIntern& operator=(TypeIntern&&) noexcept;
    TypeIntern(const TypeIntern&) = delete;
    TypeIntern& operator=(const TypeIntern&) = delete;

    const TypeInfo& intern(TypeInfo t);
    sp<TypeInfo> internSp(TypeInfo t);
};

// Riu 构造 push、析构 pop。无绑定则用进程 fallback（单文件 / 早期构造）。
void bindTypeIntern(TypeIntern* intern);

// 子类型先 intern；语言具名走进程单例。返回的引用在当前 intern 表存活期内有效。
[[nodiscard]] const TypeInfo& internType(TypeInfo t);
[[nodiscard]] sp<TypeInfo> internTypeSp(TypeInfo t);

inline void typeInfoStripFallible(TypeInfo& t) {
    if (t.fallibleErr.empty()) return;
    const string suffix = "!" + t.fallibleErr;
    if (t.name.size() >= suffix.size() && t.name.ends_with(suffix)) {
        t.name.resize(t.name.size() - suffix.size());
    }
    t.fallibleErr.clear();
    t._withoutFallible = nullptr;
}

inline const TypeInfo& TypeInfo::withoutFallible() const {
    if (fallibleErr.empty()) return *this;
    if (_withoutFallible) return *_withoutFallible;
    TypeInfo t = *this;
    typeInfoStripFallible(t);
    // 不把 TLS intern 指针写回 *this：SDK 节点上的 interned 对象可能活过用户 Riu。
    return internType(std::move(t));
}

// E4025: Rc / Weak / Array 禁止直接内嵌 Heap（§8.3a.5.1）。递归下钻，覆盖
// `Rc<Rc<Heap<T>>>` / `Array<Rc<Heap<T>>>`。sema 与 getLLVMType 共用。
inline void validateNoNestedHeap(const TypeInfo& t, int line, int col) {
    if (t.isRc()) {
        if (auto e = t.rcElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw RiuError(line, col, ErrorCode::E4025, std::string("Rc"), inner ? inner->name : std::string("?"));
            }
            validateNoNestedHeap(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isHeap()) {
                auto inner = e->heapElementType();
                throw RiuError(line, col, ErrorCode::E4025, std::string("Weak"),
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
                throw RiuError(line, col, ErrorCode::E4025, std::string("Array"),
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
                throw RiuError(line, col, ErrorCode::E1132, std::string("Rc<") + e->getFullName() + ">");
            }
            validateNoDynInRcWeak(*e, line, col);
        }
        return;
    }
    if (t.isWeak()) {
        if (auto e = t.weakElementType()) {
            if (e->isDyn()) {
                throw RiuError(line, col, ErrorCode::E1132, std::string("Weak<") + e->getFullName() + ">");
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

// 裸 T& / Array<T&> / [T& * N] / 含它们的元组或 Nullable：持有位（字段 / 别名 /
// 全局 / enum payload）报 E4039。Function 是 owned fat-ptr，即使槽里有 T& 也不算持有。
inline bool typeHoldsBorrowedValue(const TypeInfo& t) {
    if (t.isFn()) return false;
    if (t.isRef()) return true;
    if (t.isArray() && t.elementType) return typeHoldsBorrowedValue(*t.elementType);
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) return typeHoldsBorrowedValue(*e);
    }
    if (t.isTuple()) {
        for (const auto& e : t.tupleElements()) {
            if (e && typeHoldsBorrowedValue(*e)) return true;
        }
    }
    if (t.isNullable()) {
        if (auto inner = t.nullableInnerType()) return typeHoldsBorrowedValue(*inner);
    }
    return false;
}

// 返回类型：裸 T& 由 borrow checker 管；Array<T&> / [T& * N] / 含它们的元组不可作为返回值。
inline void validateReturnTypeBorrowPolicy(const TypeInfo& t, int line, int col) {
    if (t.isRef()) return;
    if (typeHoldsBorrowedValue(t)) {
        throw RiuError(line, col, ErrorCode::E4040)
            .withHint("返回单个 `T&`（方法 `$` 或静态 `$rodata`）；容器里的借用不能随返回值逃逸");
    }
}

// `<>` 内 T&：Function 形参 / 返回允许；Dyn<D&> 仅临时位；
// Array<T&> 仅临时位；Rc / Weak / Heap / 用户泛型的实参必须 owned（E4037）。
inline void validateTypeArgRefPolicy(const TypeInfo& t, int line, int col, bool allowDynBorrow) {
    if (t.isFn()) {
        for (const auto& p : t.fnParamTypes()) {
            if (p) validateTypeArgRefPolicy(*p, line, col, true);
        }
        if (auto ret = t.fnReturnType()) validateTypeArgRefPolicy(*ret, line, col, true);
        return;
    }
    if (!allowDynBorrow && typeHoldsBorrowedValue(t)) {
        throw RiuError(line, col, ErrorCode::E4039)
            .withHint("`T&` / `Array<T&>` / `[T& * N]` 只能出现在形参、局部 `let` 和返回类型；"
                      "`Function<…>` 里的 `T&` 槽是 owned fat-ptr，可作字段");
    }
    if (t.isDyn()) {
        if (t.isDynBorrow() && !allowDynBorrow) {
            throw RiuError(line, col, ErrorCode::E4038)
                .withHint("`Dyn<D&>` 只出现在形参 / 返回 / `let` 类型位；字段、别名和容器元素用 `Dyn<D>`");
        }
        if (auto spec = t.dynSpecType()) {
            TypeInfo inner = *spec;
            if (inner.isRef()) {
                if (auto peeled = inner.refElementType()) inner = *peeled;
            }
            validateTypeArgRefPolicy(inner, line, col, false);
        }
        return;
    }
    if (t.isRef()) {
        // 临时位的 `U&` 其 referent 仍按临时位查（`[Field& * N]&` 是局部 T&）。
        if (auto inner = t.refElementType()) validateTypeArgRefPolicy(*inner, line, col, allowDynBorrow);
        return;
    }
    if (t.isTuple()) {
        for (const auto& e : t.tupleElements()) {
            if (e) validateTypeArgRefPolicy(*e, line, col, allowDynBorrow);
        }
        return;
    }
    if (t.isArray()) {
        if (t.elementType) validateTypeArgRefPolicy(*t.elementType, line, col, allowDynBorrow);
        return;
    }

    const bool ownedSlots =
        t.isArrayGeneric() || t.isRc() || t.isWeak() || t.isHeap() || t.isNullable() || t.isGeneric() || t.isPtr();
    if (!ownedSlots) return;

    for (const auto& g : t.genericArgs) {
        if (!g) continue;
        if (g->isRef()) {
            // Array<T&> 临时位放行；持有位已由 typeHoldsBorrowedValue / E4039 拒。
            if (t.isArrayGeneric() && allowDynBorrow) {
                validateTypeArgRefPolicy(*g, line, col, true);
                continue;
            }
            throw RiuError(line, col, ErrorCode::E4037, t.name)
                .withHint("类型实参须为 owned（值类型 / 堆句柄 / Ptr）；借用写在形参上，如 `fn f<T>(x T&)`。"
                          "`Function` 形参和临时位的 `Dyn<D&>` / `Array<T&>` 可以写 `&`");
        }
        validateTypeArgRefPolicy(*g, line, col, false);
    }
}

// 用户泛型 fn / 泛型 struct turbofish：每个实参须 owned（Dyn:<D&> 走构造节点，不走这里）。
inline void validateOwnedTypeArgs(const string& host, const vector<TypeInfo>& typeArgs, int line, int col) {
    for (const auto& a : typeArgs) {
        if (a.isRef()) {
            throw RiuError(line, col, ErrorCode::E4037, host)
                .withHint("类型实参须为 owned（值类型 / 堆句柄 / Ptr）；借用写在形参上，如 `fn f<T>(x T&)`");
        }
        validateTypeArgRefPolicy(a, line, col, false);
    }
}
