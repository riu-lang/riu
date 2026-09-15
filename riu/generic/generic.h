// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 泛型单态化（`riu_generic`）。0 LLVM，不 include SemaPass 实现。
// 2.2：替换栈 + applySubst / bindStructSelfType。Struct/Fn 实例表仍在 Compiler。

#ifndef RIU_LANG_GENERIC_H
#define RIU_LANG_GENERIC_H

#include "types.h"

class FileNode;

namespace generic {

struct StructInstance {};
struct FnInstance {};

// map/string 分配可抛；与迁出前 Compiler::SubstFrame 相同。
struct SubstFrame { // NOLINT(bugprone-exception-escape)
    map<string, TypeInfo> subst; // 类型参数 -> 实际类型
    string baseStructName;       // 泛型原名，如 "Foo2"
    string effStructName;        // 实例名，如 "riu.core.map.Map<i32,i32>"
    string sourceFile;
    int sourceLine = 0;
};

class SubstStack {
    vector<SubstFrame> _frames;

public:
    void push_back(SubstFrame frame) { _frames.push_back(std::move(frame)); }
    void pop_back() { _frames.pop_back(); }
    [[nodiscard]] bool empty() const { return _frames.empty(); }

    using const_iterator = vector<SubstFrame>::const_iterator;
    using const_reverse_iterator = vector<SubstFrame>::const_reverse_iterator;
    [[nodiscard]] const_iterator begin() const { return _frames.begin(); }
    [[nodiscard]] const_iterator end() const { return _frames.end(); }
    [[nodiscard]] const_reverse_iterator rbegin() const { return _frames.rbegin(); }
    [[nodiscard]] const_reverse_iterator rend() const { return _frames.rend(); }

    [[nodiscard]] string formatInstantiationContext() const;
};

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
