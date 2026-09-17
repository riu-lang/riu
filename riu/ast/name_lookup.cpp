// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/name_lookup.h"

#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/struct_node.h"
#include "ast/riu.h"
#include "types.h"
#include <map>
#include <set>
#include <vector>

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

// `use a.b.Name`：裸名命中声明模块的本地类型，outOwner 是声明方（不是消费方）。
template <typename T, typename Getter>
T* lookupNamedType(FileNode* file, const string& name, Getter localGet, FileNode** outOwner) {
    if (!file) return nullptr;
    auto* named = file->namedTypeImports(name);
    if (!named) return nullptr;
    for (auto* owner : *named) {
        if (!owner) continue;
        if (auto* d = localGet(owner)) {
            if (outOwner) *outOwner = owner;
            return d;
        }
    }
    return nullptr;
}

TypeInfo resolveAliasImpl(const TypeInfo& t, const NameResolver& nr, std::set<std::string>& visited) {
    if (!nr.file) return t;
    if (t.kind == TypeKind::Normal) {
        auto* alias = nr.lookupAlias(t.name);
        if (!alias) return t;
        if (alias->isGeneric()) return t;
        if (visited.count(t.name)) {
            throw RiuError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
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
                throw RiuError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
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

FileNode* parentFileOf(FileNode* file) {
    if (!file) return nullptr;
    auto* ps = file->parentScope();
    while (ps) {
        if (auto* pf = dynamic_cast<FileNode*>(ps)) return pf;
        ps = ps->parentScope();
    }
    return nullptr;
}

TypePathResult realizeTypePathResult(TypePathResult r, FileNode* file, FileNode* sdkFile) {
    if (!r.aliasDecl || !r.aliasDecl->target()) return r;
    TypeInfo t = r.aliasDecl->target()->getType();
    FileNode* search = r.owner ? r.owner : file;
    t = resolveAlias(t, search, sdkFile);
    NameResolver nr(file, sdkFile);
    TypePathResult out;
    out.type = t;
    FileNode* owner = nullptr;
    out.structDecl = nr.lookupStruct(t, true, &owner);
    if (owner) out.owner = owner;
    if (!out.structDecl) {
        owner = nullptr;
        out.enumDecl = nr.lookupEnum(t, &owner);
        if (owner) out.owner = owner;
    }
    out.resolved = out.structDecl || out.enumDecl || r.resolved;
    return out;
}

} // namespace

StructDeclNode* NameResolver::lookupStruct(const string& name, bool includeBuiltin, FileNode** outOwner) const {
    if (file) {
        if (auto* d = file->localStructDecl(name, includeBuiltin)) {
            if (outOwner) *outOwner = file;
            return d;
        }
        if (auto* d = lookupNamedType<StructDeclNode>(
                file, name, [&](FileNode* f) { return f->localStructDecl(name, includeBuiltin); }, outOwner)) {
            return d;
        }
    }
    return lookup3<StructDeclNode>(
        file, sdkFile, [&](FileNode* f) { return f->getStructDecl(name, includeBuiltin); }, outOwner);
}

EnumDeclNode* NameResolver::lookupEnum(const string& name, FileNode** outOwner) const {
    if (file) {
        if (auto* d = file->localEnumDecl(name)) {
            if (outOwner) *outOwner = file;
            return d;
        }
        if (auto* d = lookupNamedType<EnumDeclNode>(
                file, name, [&](FileNode* f) { return f->localEnumDecl(name); }, outOwner)) {
            return d;
        }
    }
    return lookup3<EnumDeclNode>(file, sdkFile, [&](FileNode* f) { return f->getEnumDecl(name); }, outOwner);
}

AliasDeclNode* NameResolver::lookupAlias(const string& name, FileNode** outOwner) const {
    if (file) {
        if (auto* d = file->localAliasDecl(name)) {
            if (outOwner) *outOwner = file;
            return d;
        }
        if (auto* d = lookupNamedType<AliasDeclNode>(
                file, name, [&](FileNode* f) { return f->localAliasDecl(name); }, outOwner)) {
            return d;
        }
    }
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
    if (file) {
        if (auto* f = file->relatedFile(ownerModule)) return f;
    }
    if (sdkFile) {
        if (auto* f = sdkFile->relatedFile(ownerModule)) return f;
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
    // 按裸名查找；genericArgs 不参与 lookup（IterErr<NetErr> 与 IterErr<ParseErr> 同一份 decl）。
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

StructImplNode* NameResolver::lookupStructImpl(const string& name, FileNode** outOwner) const {
    if (file) {
        if (auto* d = file->localStructImpl(name)) {
            if (outOwner) *outOwner = file;
            return d;
        }
        if (auto* d = lookupNamedType<StructImplNode>(
                file, name, [&](FileNode* f) { return f->localStructImpl(name); }, outOwner)) {
            return d;
        }
    }
    return lookup3<StructImplNode>(file, sdkFile, [&](FileNode* f) { return f->getStructImpl(name); }, outOwner);
}

StructImplNode* NameResolver::lookupStructImpl(const TypeInfo& t, FileNode** outOwner) const {
    if (!t.ownerModule.empty()) {
        if (auto* f = fileForOwner(t.ownerModule)) {
            if (auto* d = f->localStructImpl(t.baseStructName())) {
                if (outOwner) *outOwner = f;
                return d;
            }
        }
    }
    return lookupStructImpl(t.name, outOwner);
}

FnSymbolInfo* NameResolver::lookupMethod(const TypeInfo& recv, const string& methodName, FileNode** outOwner) const {
    string full = recv.baseStructName() + "." + methodName;
    if (!recv.ownerModule.empty()) {
        if (auto* f = fileForOwner(recv.ownerModule)) {
            if (auto* s = f->lookupFnSymbol(full)) {
                if (outOwner) *outOwner = f;
                return s;
            }
        }
    }
    return lookupFn(full, outOwner);
}

FnSymbolInfo* NameResolver::lookupMethodWithParams(const TypeInfo& recv, const string& methodName,
                                                   const vector<TypeInfo>& paramTypes, FileNode** outOwner) const {
    string full = recv.baseStructName() + "." + methodName;
    if (!recv.ownerModule.empty()) {
        if (auto* f = fileForOwner(recv.ownerModule)) {
            if (auto* s = f->lookupFnSymbolWithParams(full, paramTypes)) {
                if (outOwner) *outOwner = f;
                return s;
            }
        }
    }
    return lookupFnWithParams(full, paramTypes, outOwner);
}

TypeInfo resolveAlias(const TypeInfo& t, FileNode* file, FileNode* sdkFile, const string& cycleFrom) {
    std::set<std::string> visited;
    if (!cycleFrom.empty()) visited.insert(cycleFrom);
    return resolveAliasImpl(t, NameResolver(file, sdkFile), visited);
}

TypePathResult resolveExprTypeLhs(FileNode* file, Riu* riu, const TypePath& path, int line, int col) {
    auto r = ::resolveTypePath(file, riu, path, line, col);
    FileNode* sdk = riu ? riu->sdkFile() : parentFileOf(file);
    return realizeTypePathResult(std::move(r), file, sdk);
}

} // namespace sema
