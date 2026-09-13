// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "type_path.h"

#include "node/file_node.h"
#include "yux.h"

#include <set>
#include <vector>

namespace {

bool isLanguageNamedType(const string& n) {
    if (n.empty()) return false;
    if (isBuiltinType(n) || n == "Ptr" || n == "Self" || n == "Function") return true;
    return kindForBuiltinWrapper(n) != TypeKind::Generic;
}

FileNode* parentFileOf(FileNode* file) {
    if (!file) return nullptr;
    auto* ps = file->parentScope();
    while (ps) {
        if (auto* pf = dynamic_cast<FileNode*>(ps)) return pf;
        ps = ps->parentScope();
    }
    return nullptr;
}

string typeIdentity(FileNode* owner, const string& typeName) {
    if (!owner) return typeName;
    return owner->moduleName() + "." + typeName;
}

[[noreturn]] void throwAmbiguousBareType(const string& name, const vector<TypePathResult>& cands, int line, int col) {
    string sources;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (i) sources += " and ";
        sources += typeIdentity(cands[i].owner, name);
    }
    int ln = line > 0 ? line : 1;
    throw YuxError(static_cast<size_t>(ln), col, ErrorCode::E5015, name, sources);
}

void addUniqueTypeCand(vector<TypePathResult>& cands, std::set<string>& seen, TypePathResult hit,
                       const string& typeName) {
    if (!hit.resolved) return;
    string id = typeIdentity(hit.owner, typeName);
    if (!seen.insert(id).second) return;
    cands.push_back(std::move(hit));
}

TypePathResult pickUniqueOrAmbiguous(const string& name, vector<TypePathResult> cands, int line, int col) {
    if (cands.size() > 1) throwAmbiguousBareType(name, cands, line, col);
    if (cands.size() == 1) return cands[0];
    return {};
}

TypePathResult bindTypeInFile(FileNode* target, const string& typeName) {
    TypePathResult r;
    r.type = TypeInfo(typeName);
    if (!target || typeName.empty()) return r;
    if (isLanguageNamedType(typeName)) {
        r.resolved = true;
        return r;
    }
    if (auto* s = target->localStructDecl(typeName, /*includeBuiltin=*/true)) {
        r.structDecl = s;
        r.owner = target;
        r.resolved = true;
        if (!s->hasAnno("Builtin")) r.type.ownerModule = target->moduleName();
        return r;
    }
    if (auto* e = target->localEnumDecl(typeName)) {
        r.enumDecl = e;
        r.owner = target;
        r.resolved = true;
        r.type.ownerModule = target->moduleName();
        return r;
    }
    if (auto* a = target->localAliasDecl(typeName)) {
        r.aliasDecl = a;
        r.owner = target;
        r.resolved = true;
        r.type.ownerModule = target->moduleName();
        return r;
    }
    return r;
}

} // namespace

TypePathResult resolveTypePath(FileNode* file, Yux* yux, const TypePath& path, int line, int col) {
    if (!yux && file) yux = file->yux();
    TypePathResult r;
    if (path.empty()) return r;
    const string last = path.lastName();
    r.type = TypeInfo(last);

    if (path.isBare() && isLanguageNamedType(last)) {
        r.resolved = true;
        return r;
    }

    if (path.isBare()) {
        if (file) {
            auto local = bindTypeInFile(file, last);
            if (local.resolved) return local;
            auto* named = file->namedTypeImports(last);
            if (named && !named->empty()) {
                vector<TypePathResult> cands;
                std::set<string> seen;
                for (auto* owner : *named) {
                    addUniqueTypeCand(cands, seen, bindTypeInFile(owner, last), last);
                }
                auto picked = pickUniqueOrAmbiguous(last, std::move(cands), line, col);
                if (picked.resolved) return picked;
            }
            {
                vector<TypePathResult> cands;
                std::set<string> seen;
                for (auto* imp : file->wildcardImports()) {
                    addUniqueTypeCand(cands, seen, bindTypeInFile(imp, last), last);
                }
                FileNode* sdk = parentFileOf(file);
                if (!sdk && yux) sdk = yux->sdkFile();
                if (sdk && sdk != file) {
                    for (auto* imp : sdk->wildcardImports()) {
                        addUniqueTypeCand(cands, seen, bindTypeInFile(imp, last), last);
                    }
                    addUniqueTypeCand(cands, seen, bindTypeInFile(sdk, last), last);
                }
                auto picked = pickUniqueOrAmbiguous(last, std::move(cands), line, col);
                if (picked.resolved) return picked;
            }
            return r;
        }
        FileNode* sdk = yux ? yux->sdkFile() : nullptr;
        if (sdk) {
            vector<TypePathResult> cands;
            std::set<string> seen;
            for (auto* imp : sdk->wildcardImports()) {
                addUniqueTypeCand(cands, seen, bindTypeInFile(imp, last), last);
            }
            addUniqueTypeCand(cands, seen, bindTypeInFile(sdk, last), last);
            auto picked = pickUniqueOrAmbiguous(last, std::move(cands), line, col);
            if (picked.resolved) return picked;
        }
        return r;
    }

    // 限定路径：只走已登记前缀，禁止 loadModule
    if (!file) return r;
    const string first = path.segs[0].getText();
    auto* aliasSym = file->lookupSymbol(first);
    if (aliasSym && (aliasSym->kind == SymbolKind::Package || aliasSym->kind == SymbolKind::Module) &&
        file->isAmbiguousAlias(first)) {
        file->throwAmbiguousAlias(first, line > 0 ? line : 1);
    }

    FileNode* target = nullptr;
    if (aliasSym && aliasSym->kind == SymbolKind::Package) {
        if (path.segs.size() >= 3) {
            string childKey;
            for (size_t i = 1; i + 1 < path.segs.size(); ++i) {
                if (!childKey.empty()) childKey += '.';
                childKey += path.segs[i].getText();
            }
            if (yux) (void)yux->resolvePkgPath(file, childKey, line, aliasSym->moduleName);
            target = file->packageChild(first, childKey);
        }
    } else if (aliasSym && aliasSym->kind == SymbolKind::Module) {
        if (path.segs.size() == 2) {
            target = file->moduleAlias(first);
            if (!target && yux) target = yux->module(aliasSym->moduleName);
        } else if (path.segs.size() > 2) {
            string childKey;
            for (size_t i = 1; i + 1 < path.segs.size(); ++i) {
                if (!childKey.empty()) childKey += '.';
                childKey += path.segs[i].getText();
            }
            target = file->packageChild(first, childKey);
        }
    }

    if (!target) return r;
    return bindTypeInFile(target, last);
}
