// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// spec 注册表实现 (spec §12).
//
// 本文件实现:
// - SpecRegistry 构造与全量索引;
// - resolve(bareName, visibleFrom): 按 §10 可见性链解析裸 D 名;
// - 内部辅助: 单文件内查 spec / 拼完全限定名.

#include "spec_registry.h"

#include "ast/yux.h"

SpecRegistry::SpecRegistry(Yux* yux) : _yux(yux) {}

void SpecRegistry::buildFromAllFiles() {
    _byQualified.clear();

    auto indexFile = [&](FileNode* file) {
        if (!file) return;
        for (auto& d : file->getSpecDecls()) {
            string qn = makeQualified(file->moduleName(), d->name().getText());
            // 同一 qualified name 在多次 build 或 sdk 与 _files 重叠时可能重复; 取首次登记.
            if (_byQualified.find(qn) == _byQualified.end()) {
                _byQualified.emplace(qn, Resolved{.qualifiedName = qn, .decl = d, .ownerFile = file});
            }
        }
    };

    if (_yux) {
        if (auto sdk = _yux->sdkFile()) indexFile(sdk);
        for (auto& f : _yux->files())
            indexFile(f);
    }
}

std::optional<SpecRegistry::Resolved> SpecRegistry::resolve(const string& bareName, FileNode* visibleFrom) const {
    if (!visibleFrom) return std::nullopt;

    // 1. 当前文件本地
    if (auto* d = lookupLocal(visibleFrom, bareName)) {
        return Resolved{.qualifiedName = makeQualified(visibleFrom->moduleName(), d->name().getText()),
                        .decl = d,
                        .ownerFile = visibleFrom};
    }

    // 2. wildcard 导入 (use a.b.*)
    for (auto* imp : visibleFrom->wildcardImports()) {
        if (auto* d = lookupLocal(imp, bareName)) {
            return Resolved{
                .qualifiedName = makeQualified(imp->moduleName(), d->name().getText()), .decl = d, .ownerFile = imp};
        }
    }

    // 3. 父作用域链 (用户文件 -> _sdkFile)，含各 parent 的 wildcardImports
    ScopeNode* parent = visibleFrom->parentScope();
    while (parent) {
        if (auto* pf = dynamic_cast<FileNode*>(parent)) {
            if (auto* d = lookupLocal(pf, bareName)) {
                return Resolved{
                    .qualifiedName = makeQualified(pf->moduleName(), d->name().getText()), .decl = d, .ownerFile = pf};
            }
            // SDK 平铺文件拆分后，_sdkFile 空壳通过 wildcardImports 指向各子文件
            for (auto* imp : pf->wildcardImports()) {
                if (auto* d = lookupLocal(imp, bareName)) {
                    return Resolved{.qualifiedName = makeQualified(imp->moduleName(), d->name().getText()),
                                    .decl = d,
                                    .ownerFile = imp};
                }
            }
        }
        parent = parent->parentScope();
    }

    return std::nullopt;
}

SpecDeclNode* SpecRegistry::lookupLocal(FileNode* file, const string& name) {
    if (!file) return nullptr;
    for (auto& d : file->getSpecDecls()) {
        if (d->name().getText() == name) return d;
    }
    return nullptr;
}

string SpecRegistry::makeQualified(const string& moduleName, const string& specName) {
    if (moduleName.empty()) return specName;
    return moduleName + "." + specName;
}
