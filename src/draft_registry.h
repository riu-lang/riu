// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// draft 注册表（spec §12 / 草案 docs/spec/draft/DRAFT-draft.md）
//
// 本文件提供跨文件的 draft 索引与按可见性的名字解析:
// - buildFromAllFiles: 扫描 Yux 持有的所有 FileNode（含 sdk）, 按
//   `<moduleName>.<D>` 完全限定名建立 (qualifiedName -> Resolved) 索引.
// - resolve(bareName, visibleFrom): 在 visibleFrom 文件的可见性范围内
//   按裸名解析 D, 走 owner._draftDecls -> owner.wildcardImports
//   -> 父作用域(sdkFile) 的链, 命中即返回完全限定名 + decl + ownerFile.
// - byQualified: 暴露全部已注册的 draft, 用于后续显式实现登记 / 显隐
//   冲突诊断 / `#DraftLike` 结构匹配 (Phase 3.2.b/c/d).
//
// 注: v0.5 类型/结构体/draft 引用语法均为单段 ID, 解析依赖
// `use a.b.*` 通配 + 父作用域链. 暂不需要解析 `a.b.D` 字面量.

#ifndef YUX_LANG_DRAFT_REGISTRY_H
#define YUX_LANG_DRAFT_REGISTRY_H

#include "node/draft_node.h"
#include "node/file_node.h"

#include <map>
#include <optional>

class Yux;

class DraftRegistry {
public:
    // draft 解析结果: 完全限定名 + 声明节点 + 所属文件
    struct Resolved {
        string qualifiedName;       // "<moduleName>.<D>"; 模块名为空时退化为裸名
        DraftDeclNode* decl;
        FileNode* ownerFile;
    };

    explicit DraftRegistry(Yux* yux);

    // 索引 Yux 持有的所有 FileNode（含 _sdkFile）下的 _draftDecls.
    // 重复构建时会清空再重建 (后续如新增动态加载模块可再次调用).
    void buildFromAllFiles();

    // 在 visibleFrom 文件的可见性范围内按裸名解析 D.
    // 链: visibleFrom._draftDecls -> visibleFrom.wildcardImports
    //   -> visibleFrom 的父作用域链中各 FileNode (典型为 _sdkFile).
    // 命中返回完全限定 Resolved; 未命中返回 nullopt.
    std::optional<Resolved> resolve(const string& bareName, FileNode* visibleFrom) const;

    // 已注册的全部 draft (qualified name -> Resolved).
    const std::map<string, Resolved>& byQualified() const { return _byQualified; }

private:
    Yux* _yux;
    std::map<string, Resolved> _byQualified;

    // 在单个 FileNode 内按裸名查 _draftDecls (不含 wildcards / parent).
    static DraftDeclNode* lookupLocal(FileNode* file, const string& name);

    static string makeQualified(const string& moduleName, const string& draftName);
};

#endif //YUX_LANG_DRAFT_REGISTRY_H
