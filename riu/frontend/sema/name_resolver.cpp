// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "sema/name_resolver.h"

#include "types.h"

namespace sema {

void validateAliases(FileNode* file, FileNode* sdkFile) {
    if (!file) return;
    auto& aliases = file->getAliasDecls();

    for (auto& a : aliases) {
        string name = a->name().getText();
        if (auto* s = file->getStructDecl(name)) {
            (void)s;
            throw RiuError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("struct"), name);
        }
        if (auto* d = file->getSpecDecl(name)) {
            (void)d;
            throw RiuError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("draft"), name);
        }
        size_t cnt = 0;
        for (auto& b : aliases) {
            if (b->name().getText() == name) ++cnt;
        }
        if (cnt > 1) {
            throw RiuError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("type alias"), name);
        }
    }

    for (auto& a : aliases) {
        if (!a->target()) continue;
        (void)resolveAlias(a->target()->getType(), file, sdkFile, a->name().getText());
        int aline = static_cast<int>(a->name().getLine());
        int acol = static_cast<int>(a->name().getCharPositionInLine()) + 1;
        validateTypeArgRefPolicy(a->target()->getType(), aline, acol, false);
    }

    // 函数符号表 params / retType 透明别名归一化（原 Compiler::validateAliases 副作用）
    file->normalizeFnSymbolTypes([&](const TypeInfo& t) { return resolveAlias(t, file, sdkFile); });
}

} // namespace sema
