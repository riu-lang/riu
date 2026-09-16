// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 泛型单态化（`riu_generic`）。0 LLVM，不 include SemaPass 实现。
// 2.5：Sema 与 codegen 共用实例表 + subst 栈；发射 IR 仍在 Compiler。

#ifndef RIU_LANG_GENERIC_H
#define RIU_LANG_GENERIC_H

#include "types.h"

#include <deque>

class FileNode;
class FnNode;
class StructDeclNode;
class StructImplNode;

namespace generic {

struct StructInstance {
    StructDeclNode* baseDecl = nullptr; // 泛型结构体声明
    StructImplNode* baseImpl = nullptr; // 泛型结构体实现（含方法）
    FileNode* ownerFile = nullptr;      // 定义该结构体的文件
    vector<TypeInfo> args;              // 类型参数实例化参数
    string mangledName;                 // mangle 后的实例名
    // 定义该泛型的模块名（与 LLVM 符号前缀一致）。多 TU 各发一份 IR 时
    // 同名符号靠 linkonce_odr + COMDAT 合并，不再用消费方模块当分隔。
    string consumerModule;
    bool methodsEmitted = false; // codegen 是否已发射方法 IR
    string sourceFile;           // 实例化发生的源文件（用于错误报告）
    int sourceLine = 0;          // 实例化发生的行号（用于错误报告）

    [[nodiscard]] string ownerModule() const;
    [[nodiscard]] TypeInfo typeInfo() const;
    [[nodiscard]] map<string, TypeInfo> substMap() const;
};

// 从声明 + 实参填一份实例记录（不入表）。`ownerFile` 空则用 `currentFile`。
[[nodiscard]] StructInstance makeStructInstance(StructDeclNode* baseDecl, vector<TypeInfo> args, FileNode* ownerFile,
                                                FileNode* currentFile, FileNode* sdkFile, string mangledName,
                                                int sourceLine);

// 泛型 struct 单态表：key = 定义模块全限定实例名（如 "riu.core.map.Map<i32,i32>"）。
class StructTable {
    map<string, StructInstance> _instances;

public:
    [[nodiscard]] StructInstance* find(const string& mangledName);
    [[nodiscard]] const StructInstance* find(const string& mangledName) const;
    [[nodiscard]] bool contains(const string& mangledName) const { return _instances.contains(mangledName); }

    StructInstance& operator[](const string& mangledName) { return _instances[mangledName]; }

    void insert(StructInstance inst);

    [[nodiscard]] size_t size() const { return _instances.size(); }

    auto begin() { return _instances.begin(); }
    auto end() { return _instances.end(); }
    [[nodiscard]] auto begin() const { return _instances.begin(); }
    [[nodiscard]] auto end() const { return _instances.end(); }
};

struct FnInstance {
    FnNode* baseFn = nullptr; // 泛型函数 / 方法定义
    FileNode* ownerFile = nullptr;
    vector<TypeInfo> typeArgs; // 类型参数实例化参数
    string mangledName;        // `name<Args>`（不含形参表；表 key 另算）
    string methodStructName;   // 非空表示方法实例；值为 receiver 的实际 struct 名
    bool methodIsStatic = false;
    bool emitted = false; // codegen 是否已发射 IR
    // 定义该泛型函数的模块名（与 LLVM 符号前缀一致）。多 TU 靠 linkonce_odr 合并。
    string consumerModule;

    [[nodiscard]] string ownerModule() const;
    [[nodiscard]] map<string, TypeInfo> substMap() const;
};

// 从定义 + 实参填一份实例记录（不入表）。`ownerFile` 空则用 `currentFile`。
[[nodiscard]] FnInstance makeFnInstance(FnNode* baseFn, vector<TypeInfo> typeArgs, FileNode* ownerFile,
                                        FileNode* currentFile, string mangledName);
[[nodiscard]] FnInstance makeMethodInstance(FnNode* baseMethod, string structName, vector<TypeInfo> typeArgs,
                                            FileNode* ownerFile, FileNode* currentFile, string mangledName);

// 泛型 fn / method 单态表：key 含形参 mangle（如 "println<i32>(i32)" / "method:mod:S.foo<T>(...)"）。
class FnTable {
    map<string, FnInstance> _instances;

public:
    [[nodiscard]] FnInstance* find(const string& key);
    [[nodiscard]] const FnInstance* find(const string& key) const;
    [[nodiscard]] bool contains(const string& key) const { return _instances.contains(key); }

    FnInstance& operator[](const string& key) { return _instances[key]; }

    void insert(string key, FnInstance inst);

    [[nodiscard]] size_t size() const { return _instances.size(); }

    auto begin() { return _instances.begin(); }
    auto end() { return _instances.end(); }
    [[nodiscard]] auto begin() const { return _instances.begin(); }
    [[nodiscard]] auto end() const { return _instances.end(); }
};

// map/string 分配可抛；与迁出前 Compiler::SubstFrame 相同。
struct SubstFrame {              // NOLINT(bugprone-exception-escape)
    map<string, TypeInfo> subst; // 类型参数 -> 实际类型
    string baseStructName;       // 泛型原名，如 "Foo2"
    string effStructName;        // 实例名，如 "riu.core.map.Map<i32,i32>"
    string sourceFile;
    int sourceLine = 0;
};

class SubstStack {
    // deque：压栈不使已有帧失效。Sema 会把 currentInstSubst() 指针传入同步 helper。
    deque<SubstFrame> _frames;

public:
    void push_back(SubstFrame frame) { _frames.push_back(std::move(frame)); }
    void pop_back() { _frames.pop_back(); }
    [[nodiscard]] bool empty() const { return _frames.empty(); }
    [[nodiscard]] SubstFrame& back() { return _frames.back(); }
    [[nodiscard]] const SubstFrame& back() const { return _frames.back(); }

    using const_iterator = deque<SubstFrame>::const_iterator;
    using const_reverse_iterator = deque<SubstFrame>::const_reverse_iterator;
    [[nodiscard]] const_iterator begin() const { return _frames.begin(); }
    [[nodiscard]] const_iterator end() const { return _frames.end(); }
    [[nodiscard]] const_reverse_iterator rbegin() const { return _frames.rbegin(); }
    [[nodiscard]] const_reverse_iterator rend() const { return _frames.rend(); }

    [[nodiscard]] string formatInstantiationContext() const;
};

// 压入 / 弹出替换帧。Sema 复查模板体与 codegen 发射共用。
class SubstScope {
    SubstStack* _stack = nullptr;

public:
    SubstScope(SubstStack& stack, SubstFrame frame);
    ~SubstScope();
    SubstScope(const SubstScope&) = delete;
    SubstScope& operator=(const SubstScope&) = delete;
    SubstScope(SubstScope&& o) noexcept : _stack(o._stack) { o._stack = nullptr; }
    SubstScope& operator=(SubstScope&&) = delete;
};

// 实例身份：节点指针 + subst 内容。Sema 登记 / 去重用；codegen 仍用 mangle 名。
[[nodiscard]] string instanceKey(const void* p, const map<string, TypeInfo>& subst);

// 按形参顺序从 subst 取实参；缺的位置填空 TypeInfo。
[[nodiscard]] vector<TypeInfo> argsFromSubst(const vector<string>& typeParams, const map<string, TypeInfo>& subst);

// `Self` / 泛型原名 → 用 `effType` 替换。`effName` 空则原样返回。
[[nodiscard]] bool bindsStructSelf(const TypeInfo& t, const string& baseName, const string& effName);
[[nodiscard]] TypeInfo bindStructSelfType(const TypeInfo& t, const string& baseName, const string& effName,
                                          const TypeInfo& effType);

using NamedStructFn = TypeInfo (*)(const void* ctx, const string& name);

// 自底向上 substitute，再从栈顶绑定 Self / 裸 generic 原名，最后透明别名。
// `namedStruct` 把实例名（或非泛型 `_currentStructName`）还原成 TypeInfo。
[[nodiscard]] TypeInfo applySubst(const TypeInfo& t, const SubstStack& stack, const string& currentStructName,
                                  FileNode* file, FileNode* sdkFile, NamedStructFn namedStruct, const void* namedCtx);

[[nodiscard]] TypeInfo applySubstMap(const TypeInfo& t, const map<string, TypeInfo>* subst);

} // namespace generic

#endif // RIU_LANG_GENERIC_H
