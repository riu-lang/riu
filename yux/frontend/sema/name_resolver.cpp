// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "sema/name_resolver.h"

#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/struct_node.h"
#include "ast/yux.h"
#include "types.h"
#include <map>
#include <set>

namespace sema {

namespace {

template <typename T, typename Getter>
T* lookup3(FileNode* file, FileNode* sdkFile, Getter get, FileNode** outOwner) {
    auto tryFile = [&](FileNode* f) -> T* {
        if (!f) return nullptr;
        if (auto* d = get(f)) {
            if (outOwner) *outOwner = f;
            return d;
        }
        return nullptr;
    };
    if (auto* d = tryFile(file)) return d;
    if (sdkFile && sdkFile != file) {
        if (auto* d = tryFile(sdkFile)) return d;
    }
    if (file) {
        for (auto* imp : file->wildcardImports()) {
            if (auto* d = tryFile(imp)) return d;
        }
    }
    if (outOwner) *outOwner = nullptr;
    return nullptr;
}

TypeInfo resolveAliasImpl(const TypeInfo& t, const NameResolver& nr, std::set<std::string>& visited) {
    if (!nr.file) return t;
    if (t.kind == TypeKind::Normal) {
        auto* alias = nr.lookupAlias(t.name);
        if (!alias) return t;
        if (alias->isGeneric()) return t;
        if (visited.count(t.name)) {
            throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
        }
        visited.insert(t.name);
        if (!alias->target()) return t;
        TypeInfo target = alias->target()->getType();
        return resolveAliasImpl(target, nr, visited);
    }
    if (t.hasGenericArgs() && !t.genericArgs.empty()) {
        auto* alias = nr.lookupAlias(t.name);
        if (alias && alias->isGeneric() && alias->typeParams().size() == t.genericArgs.size() && alias->target()) {
            if (visited.count(t.name)) {
                throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
            }
            visited.insert(t.name);
            std::map<std::string, TypeInfo> subst;
            for (size_t i = 0; i < alias->typeParams().size(); ++i) {
                subst[alias->typeParams()[i]] = t.genericArgs[i] ? *t.genericArgs[i] : TypeInfo();
            }
            TypeInfo inst = alias->target()->getType().substitute(subst);
            return resolveAliasImpl(inst, nr, visited);
        }
        vector<sp<TypeInfo>> newArgs;
        newArgs.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newArgs.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, nr, sub)));
            } else {
                newArgs.push_back(nullptr);
            }
        }
        TypeInfo out{t.name, std::move(newArgs)};
        out.ownerModule = t.ownerModule;
        return out;
    }
    if (t.kind == TypeKind::Array && t.elementType) {
        std::set<std::string> sub = visited;
        TypeInfo inner = resolveAliasImpl(*t.elementType, nr, sub);
        return {std::make_shared<TypeInfo>(std::move(inner)), t.arraySize};
    }
    if (t.kind == TypeKind::Tuple) {
        vector<sp<TypeInfo>> newElems;
        newElems.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newElems.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, nr, sub)));
            } else {
                newElems.push_back(nullptr);
            }
        }
        return TypeInfo(TupleTag{}, std::move(newElems));
    }
    if (t.kind == TypeKind::Fn) {
        vector<sp<TypeInfo>> newParams;
        newParams.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newParams.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, nr, sub)));
            } else {
                newParams.push_back(nullptr);
            }
        }
        sp<TypeInfo> newRet = nullptr;
        if (t.elementType) {
            std::set<std::string> sub = visited;
            newRet = std::make_shared<TypeInfo>(resolveAliasImpl(*t.elementType, nr, sub));
        }
        return TypeInfo(FnTag{}, std::move(newParams), newRet, t.fnNullable);
    }
    return t;
}

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

StructDeclNode* NameResolver::lookupStruct(const string& name, bool includeBuiltin, FileNode** outOwner) const {
    return lookup3<StructDeclNode>(
        file, sdkFile, [&](FileNode* f) { return f->getStructDecl(name, includeBuiltin); }, outOwner);
}

EnumDeclNode* NameResolver::lookupEnum(const string& name, FileNode** outOwner) const {
    return lookup3<EnumDeclNode>(file, sdkFile, [&](FileNode* f) { return f->getEnumDecl(name); }, outOwner);
}

AliasDeclNode* NameResolver::lookupAlias(const string& name, FileNode** outOwner) const {
    return lookup3<AliasDeclNode>(file, sdkFile, [&](FileNode* f) { return f->getAliasDecl(name); }, outOwner);
}

FnSymbolInfo* NameResolver::lookupFn(const string& name, FileNode** outOwner) const {
    return lookup3<FnSymbolInfo>(file, sdkFile, [&](FileNode* f) { return f->lookupFnSymbol(name); }, outOwner);
}

FnSymbolInfo* NameResolver::lookupFnWithParams(const string& name, const vector<TypeInfo>& paramTypes,
                                               FileNode** outOwner) const {
    return lookup3<FnSymbolInfo>(
        file, sdkFile, [&](FileNode* f) { return f->lookupFnSymbolWithParams(name, paramTypes); }, outOwner);
}

FileNode* NameResolver::fileForOwner(const string& ownerModule) const {
    if (ownerModule.empty()) return nullptr;
    auto hit = [&](FileNode* f) { return f && f->moduleName() == ownerModule; };
    if (hit(file)) return file;
    if (file) {
        for (auto* imp : file->wildcardImports()) {
            if (hit(imp)) return imp;
        }
    }
    if (sdkFile) {
        if (hit(sdkFile)) return sdkFile;
        for (auto* imp : sdkFile->wildcardImports()) {
            if (hit(imp)) return imp;
        }
    }
    return nullptr;
}

StructDeclNode* NameResolver::lookupStruct(const TypeInfo& t, bool includeBuiltin, FileNode** outOwner) const {
    if (!t.ownerModule.empty()) {
        if (auto* f = fileForOwner(t.ownerModule)) {
            if (auto* d = f->localStructDecl(t.baseStructName(), includeBuiltin)) {
                if (outOwner) *outOwner = f;
                return d;
            }
        }
    }
    return lookupStruct(t.name, includeBuiltin, outOwner);
}

EnumDeclNode* NameResolver::lookupEnum(const TypeInfo& t, FileNode** outOwner) const {
    if (!t.ownerModule.empty()) {
        if (auto* f = fileForOwner(t.ownerModule)) {
            if (auto* d = f->localEnumDecl(t.baseStructName())) {
                if (outOwner) *outOwner = f;
                return d;
            }
        }
    }
    return lookupEnum(t.name, outOwner);
}

TypePathResult resolveTypePath(FileNode* file, Yux* yux, const TypePath& path, int line, int col) {
    TypePathResult r;
    (void)col;
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
            for (auto* imp : file->wildcardImports()) {
                auto hit = bindTypeInFile(imp, last);
                if (hit.resolved) return hit;
            }
        }
        FileNode* sdk = file ? parentFileOf(file) : nullptr;
        if (!sdk && yux) sdk = yux->sdkFile();
        if (sdk && sdk != file) {
            for (auto* imp : sdk->wildcardImports()) {
                auto hit = bindTypeInFile(imp, last);
                if (hit.resolved) return hit;
            }
            auto hit = bindTypeInFile(sdk, last);
            if (hit.resolved) return hit;
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

TypeInfo resolveAlias(const TypeInfo& t, FileNode* file, FileNode* sdkFile) {
    std::set<std::string> visited;
    return resolveAliasImpl(t, NameResolver(file, sdkFile), visited);
}

void validateAliases(p<FileNode> file, p<FileNode> sdkFile) {
    if (!file) return;
    auto& aliases = file->getAliasDecls();

    for (auto& a : aliases) {
        string name = a->name().getText();
        if (auto* s = file->getStructDecl(name)) {
            (void)s;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("struct"), name);
        }
        if (auto* d = file->getSpecDecl(name)) {
            (void)d;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("draft"), name);
        }
        size_t cnt = 0;
        for (auto& b : aliases) {
            if (b->name().getText() == name) ++cnt;
        }
        if (cnt > 1) {
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("type alias"), name);
        }
    }

    NameResolver nr(file, sdkFile);
    for (auto& a : aliases) {
        if (!a->target()) continue;
        std::set<std::string> visited;
        visited.insert(a->name().getText());
        (void)resolveAliasImpl(a->target()->getType(), nr, visited);
    }

    // 函数符号表 params / retType 透明别名归一化（原 Compiler::validateAliases 副作用）
    file->normalizeFnSymbolTypes([&](const TypeInfo& t) { return resolveAlias(t, file, sdkFile); });
}

} // namespace sema
