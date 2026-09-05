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

namespace {
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

TypePathResult resolveExprTypeLhs(FileNode* file, Yux* yux, const TypePath& path, int line, int col) {
    auto r = resolveTypePath(file, yux, path, line, col);
    FileNode* sdk = yux ? yux->sdkFile() : parentFileOf(file);
    return realizeTypePathResult(std::move(r), file, sdk);
}

} // namespace sema
