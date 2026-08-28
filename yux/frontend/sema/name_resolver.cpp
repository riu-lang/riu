// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "sema/name_resolver.h"

#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/struct_node.h"
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
        return {t.name, std::move(newArgs)};
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
