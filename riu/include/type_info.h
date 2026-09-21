// Copyright (c) 2025-2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "primitives.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    TypeInfo(sp<TypeInfo> elemType, u64 size);

    // 元组类型 (T1, T2, ...)
    // 元素列表复用 genericArgs 存储；name 合成为 "(T1,T2,...)" 形式
    TypeInfo(TupleTag, vector<sp<TypeInfo>> elements);

    // 函数类型 Function<P..., Ret> / Function<...>?
    // 形参类型列表存 genericArgs；返回类型存 elementType（unit 时为 nullptr）
    // 末位永远是返回类型；unit 返回写 ()
    TypeInfo(FnTag, vector<sp<TypeInfo>> paramTypes, sp<TypeInfo> retType, bool nullable = false);

    void rebuildFnName();

    // T ! E 签名位：fallibleErr 非空即 fallible 类型（ABI 仍按成功类型 T）
    [[nodiscard]] bool isFallible() const { return !fallibleErr.empty(); }

    // 剥掉 fallible 后缀，保留成功类型 T（intern 后的引用；供 LLVM / 值类型判等）
    [[nodiscard]] const TypeInfo& withoutFallible() const;

    // 在成功类型上附加 fallible 后缀，刷新 name。`err` 须是 E 的 getFullName。
    void attachFallibleErr(string err);

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

    // 裸 Ptr ≡ Ptr<()>（#21）；带具体 payload 的 Ptr<T> 为 false
    [[nodiscard]] bool isErasedPtr() const {
        if (!isPtr()) return false;
        auto elem = ptrElementType();
        return !elem || elem->isUnit();
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
    [[nodiscard]] bool hasGenericArgs() const;

    // 完整类型名（与 riu 源码写法一致，用于报错/调试输出）：
    // 泛型 Base<Arg1,Arg2>，元组 (T1,T2)，函数 Function<P...,Ret>，数组 [E*N]
    [[nodiscard]] string getFullName() const;

    // LLVM 符号用 mangle 名（与 riu 声明/调用写法同形）：
    // 有 owner → `mod.Name` / `mod.Name<Arg1,Arg2>`；内建无 owner → 短名。
    // 元组 (T1,T2)，函数 Function<P...,Ret>，数组 [E*N]。分隔只用 `.` `::` `<>` `()` `@`。
    [[nodiscard]] string getMangleName() const;

    // struct decl 查找用基名：剥 `Foo<Arg>` / `Foo$Arg` 修饰。
    // TypeGenericNode 的 name 已是基名；此处防御完整名或 LLVM 修饰名被塞进 name 的路径。
    [[nodiscard]] string baseStructName() const;

    [[nodiscard]] string formatFnGeneric(bool mangle) const;

    // 应用类型形参替换。无类型实参的具名类型（Normal / 空 Generic）匹配 subst 键则整体替换。
    // Ptr<T> 走下方 genericArgs 替换（#21）。T ! E 的 E 按 getFullName 还原后再 subst。
    [[nodiscard]] TypeInfo substitute(const std::map<std::string, TypeInfo>& subst) const;

    bool operator==(const TypeInfo& other) const;

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }
};
// ptr_of:<T> 的所指类型（#21 切片 4）
[[nodiscard]] inline TypeInfo ptrOfPointeeType(TypeInfo t) {
    if (t.isRef()) {
        if (auto e = t.refElementType()) t = *e;
    }
    if (t.isRc()) {
        if (auto e = t.rcElementType()) return *e;
    }
    if (t.isHeap()) {
        if (auto e = t.heapElementType()) return *e;
    }
    if (t.isArrayGeneric()) {
        if (auto e = t.arrayGenericElementType()) return *e;
    }
    if (t.isString()) return TypeInfo("u32");
    // U& 已剥壳：标量 / struct 等 payload 即 U 自身
    if (!t.isRcHandle() && !t.isPtr() && !t.isFn() && !t.empty()) return t;
    return {};
}
// T ! E 错误通道的比较 / 存储键：完整写法（`Box<String>` 而非裸名 `Box`）
inline string fallibleErrKey(const TypeInfo& t) {
    return t.getFullName();
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

[[nodiscard]] const TypeInfo& internNamedType(string_view name);

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
