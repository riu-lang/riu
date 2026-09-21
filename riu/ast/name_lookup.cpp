// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/name_lookup.h"

#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/global_const_node.h"
#include "ast/node/global_var_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"
#include "ast/riu.h"
#include "error_code.h"
#include "types.h"
#include <format>
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

thread_local vector<const AliasDeclNode*> gScopedAliasExpandStack;

AliasDeclNode* lookupScopedAlias(const Node* from, const string& name) {
    if (!from || name.empty()) return nullptr;
    for (const Node* n = from; n; n = n->parent()) {
        if (dynamic_cast<const FileNode*>(n)) break;
        if (auto* scope = dynamic_cast<const ScopeNode*>(n)) {
            if (auto* a = scope->localAlias(name)) return a;
        }
        if (auto* impl = dynamic_cast<const StructImplNode*>(n)) {
            if (auto* file = n->enclosingFile()) {
                if (auto* decl = file->localStructDecl(impl->structName(), /*includeBuiltin=*/true)) {
                    if (auto* a = decl->localAlias(name)) return a;
                }
            }
        }
    }
    return nullptr;
}

TypeInfo expandScopedAlias(const AliasDeclNode* alias) {
    if (!alias || !alias->target()) return {};
    for (auto* x : gScopedAliasExpandStack) {
        if (x == alias) {
            throw RiuError(static_cast<int>(alias->name().getLine()),
                           static_cast<int>(alias->name().getCharPositionInLine()) + 1, ErrorCode::E2016,
                           alias->name().getText());
        }
    }
    gScopedAliasExpandStack.push_back(alias);
    TypeInfo t;
    try {
        t = alias->target()->getType();
    } catch (...) {
        gScopedAliasExpandStack.pop_back();
        throw;
    }
    gScopedAliasExpandStack.pop_back();
    return t;
}

TypePathResult resolveExprTypeLhs(const Node* from, FileNode* file, Riu* riu, const TypePath& path, int line, int col) {
    if (from && path.isBare()) {
        if (auto* a = lookupScopedAlias(from, path.lastName())) {
            TypeInfo t = expandScopedAlias(a);
            TypePathResult r;
            r.type = t;
            r.aliasDecl = const_cast<AliasDeclNode*>(a);
            r.resolved = true;
            FileNode* sdk = riu ? riu->sdkFile() : parentFileOf(file);
            NameResolver nr(file, sdk);
            FileNode* owner = nullptr;
            r.structDecl = nr.lookupStruct(t, true, &owner);
            if (owner) r.owner = owner;
            if (!r.structDecl) {
                owner = nullptr;
                r.enumDecl = nr.lookupEnum(t, &owner);
                if (owner) r.owner = owner;
            }
            return r;
        }
    }
    return resolveExprTypeLhs(file, riu, path, line, col);
}

map<string, TypeInfo> enumInstSubst(EnumDeclNode* enumDecl, const TypeInfo& enumType) {
    map<string, TypeInfo> subst;
    if (!enumDecl) return subst;
    const auto& tps = enumDecl->typeParams();
    if (tps.empty() || enumType.genericArgs.size() != tps.size()) return subst;
    for (size_t i = 0; i < tps.size(); ++i) {
        if (!enumType.genericArgs[i] || enumType.genericArgs[i]->empty()) {
            subst.clear();
            return subst;
        }
        subst[tps[i]] = *enumType.genericArgs[i];
    }
    return subst;
}

bool tryFillTypeArgsWithDefaults(const vector<string>& names, const vector<TypeNode*>& defaults,
                                 const vector<TypeInfo>& written, vector<TypeInfo>& out) {
    if (names.empty() || written.size() > names.size()) return false;
    out.clear();
    out.reserve(names.size());
    map<string, TypeInfo> subst;
    for (size_t i = 0; i < written.size(); ++i) {
        out.push_back(written[i]);
        if (i < names.size()) subst[names[i]] = written[i];
    }
    if (written.size() == names.size()) return true;
    for (size_t i = written.size(); i < names.size(); ++i) {
        if (i >= defaults.size() || !defaults[i]) return false;
        TypeInfo d = defaults[i]->getType();
        if (!subst.empty()) d = d.substitute(subst);
        out.push_back(d);
        subst[names[i]] = d;
    }
    return true;
}

namespace {

void throwGenericNamedArity(const string& name, size_t want, size_t got, int line, int col) {
    throw RiuError(line, col, ErrorCode::E6011, name, want, got)
        .withHint(std::format("实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", name,
                              std::string(want == 1 ? "T" : "T1, T2, ..."), want));
}

bool remainingHaveDefaults(const vector<TypeNode*>& defaults, size_t got, size_t want) {
    if (got >= want) return false;
    for (size_t i = got; i < want; ++i) {
        if (i >= defaults.size() || !defaults[i]) return false;
    }
    return true;
}

TypeInfo rebuildNamedType(const TypeInfo& t0, vector<sp<TypeInfo>> args) {
    if (t0.isTuple()) return TypeInfo(TupleTag{}, std::move(args));
    TypeInfo r = t0;
    r.genericArgs = std::move(args);
    if (!r.genericArgs.empty() && r.kind == TypeKind::Normal) r.kind = TypeKind::Generic;
    return r;
}

} // namespace

TypeInfo fillGenericNamedTypeArity(const TypeInfo& raw, const NameResolver& nr, int line, int col,
                                   const string& currentStructName, bool throwOnArityError) {
    std::set<string> filling;
    auto rec = [&](auto&& self, const TypeInfo& t) -> TypeInfo {
        TypeInfo t0 = t;
        try {
            t0 = resolveAlias(t, nr.file, nr.sdkFile);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return t;
        }
        if (t0.empty()) return t0;

        if (t0.isFn()) {
            vector<sp<TypeInfo>> params;
            params.reserve(t0.fnParamTypes().size());
            for (auto& p : t0.fnParamTypes()) {
                params.push_back(p ? internTypeSp(self(self, *p)) : p);
            }
            sp<TypeInfo> ret = nullptr;
            if (auto r = t0.fnReturnType()) ret = internTypeSp(self(self, *r));
            TypeInfo fn(FnTag{}, std::move(params), std::move(ret), t0.fnNullable);
            if (!t0.fallibleErr.empty()) fn.attachFallibleErr(t0.fallibleErr);
            return fn;
        }
        if (t0.isArray() && t0.elementType) {
            TypeInfo elem = self(self, *t0.elementType);
            TypeInfo arr{internTypeSp(std::move(elem)), t0.arraySize};
            if (!t0.fallibleErr.empty()) arr.attachFallibleErr(t0.fallibleErr);
            return arr;
        }

        vector<sp<TypeInfo>> newArgs;
        newArgs.reserve(t0.genericArgs.size());
        bool changed = false;
        for (auto& a : t0.genericArgs) {
            if (!a) {
                newArgs.push_back(a);
                continue;
            }
            TypeInfo fa = self(self, *a);
            if (fa.getFullName() != a->getFullName()) changed = true;
            newArgs.push_back(internTypeSp(std::move(fa)));
        }

        // Ref / Fn / 元组的 genericArgs 不是用户类型实参。
        // 泛型 impl 里 `Self` / 裸名即当前单态，不要求写出实参。
        const bool currentInst =
            t0.isSelf() || (!currentStructName.empty() && t0.name == currentStructName && t0.genericArgs.empty());
        const string fillKey = t0.ownerModule + "::" + t0.name;
        const bool skipNamed = currentInst || t0.isRef() || t0.isRc() || t0.isWeak() || t0.isHeap() || t0.isDyn() ||
                               t0.isPtr() || t0.isArray() || t0.isArrayGeneric() || t0.isTuple() || t0.name.empty() ||
                               isBuiltinType(t0.name) || filling.contains(fillKey);
        if (!skipNamed) {
            auto appendDefaults = [&](const vector<string>& names, const vector<TypeNode*>& defaults) {
                const size_t want = names.size();
                const size_t got = newArgs.size();
                if (want == 0 || want == got) return;
                if (got < want && remainingHaveDefaults(defaults, got, want)) {
                    filling.insert(fillKey);
                    map<string, TypeInfo> subst;
                    for (size_t i = 0; i < got && i < names.size(); ++i) {
                        if (newArgs[i]) subst[names[i]] = *newArgs[i];
                    }
                    for (size_t i = got; i < want; ++i) {
                        TypeInfo d = defaults[i]->getType();
                        if (!subst.empty()) d = d.substitute(subst);
                        d = self(self, d);
                        auto sp = internTypeSp(d);
                        subst[names[i]] = d;
                        newArgs.push_back(std::move(sp));
                    }
                    filling.erase(fillKey);
                    changed = true;
                    return;
                }
                if (throwOnArityError) throwGenericNamedArity(t0.name, want, got, line, col);
            };
            if (auto* sd = nr.lookupStruct(t0, true)) {
                appendDefaults(sd->typeParams(), sd->typeParamDefaults());
            } else if (auto* ed = nr.lookupEnum(t0)) {
                appendDefaults(ed->typeParams(), ed->typeParamDefaults());
            }
        }

        if (!changed) return t0;
        return rebuildNamedType(t0, std::move(newArgs));
    };
    return rec(rec, raw);
}

void validateGenericNamedTypeArity(const TypeInfo& raw, const NameResolver& nr, int line, int col,
                                   const string& currentStructName) {
    (void)fillGenericNamedTypeArity(raw, nr, line, col, currentStructName);
}

void fillFileDeclTypes(FileNode* file) {
    if (!file) return;
    auto anyDefault = [](const vector<TypeNode*>& defs) {
        for (auto* d : defs) {
            if (d) return true;
        }
        return false;
    };
    bool hasDefaults = false;
    for (auto* sd : file->getStructDecls()) {
        if (sd && anyDefault(sd->typeParamDefaults())) {
            hasDefaults = true;
            break;
        }
    }
    if (!hasDefaults) {
        for (auto* ed : file->getEnumDecls()) {
            if (ed && anyDefault(ed->typeParamDefaults())) {
                hasDefaults = true;
                break;
            }
        }
    }
    if (!hasDefaults) {
        for (auto* spec : file->getSpecDecls()) {
            if (spec && anyDefault(spec->typeParamDefaults())) {
                hasDefaults = true;
                break;
            }
        }
    }
    if (!hasDefaults) {
        auto hdrHas = [&](FnHeaderNode* h) { return h && anyDefault(h->typeParamDefaults()); };
        for (auto* fn : file->getFunctions()) {
            if (fn && hdrHas(fn->header())) {
                hasDefaults = true;
                break;
            }
        }
        if (!hasDefaults) {
            for (auto* impl : file->getStructImpls()) {
                if (!impl) continue;
                for (auto* m : impl->methods()) {
                    if (m && hdrHas(m->header())) {
                        hasDefaults = true;
                        break;
                    }
                }
                if (hasDefaults) break;
            }
        }
    }
    if (hasDefaults) {
        NameResolver nr(file, file->riu() ? file->riu()->sdkFile() : nullptr);
        auto recache = [&](TypeNode* tn) {
            if (!tn || dynamic_cast<TypeSelfNode*>(tn)) return;
            try {
                const int line = tn->getLineNumber() > 0 ? tn->getLineNumber() : 1;
                const int col = tn->getColumn() > 0 ? tn->getColumn() : 1;
                const TypeInfo& orig = tn->getType();
                TypeInfo filled = fillGenericNamedTypeArity(orig, nr, line, col, {}, false);
                if (filled.getFullName() != orig.getFullName()) tn->recacheType(std::move(filled));
            } catch (const RiuError&) { // NOLINT(bugprone-empty-catch)
                // 非法 arity（如 Rc 少写）留给 Sema / 使用点报 E6011
            }
        };
        auto fillHeader = [&](FnHeaderNode* h) {
            if (!h) return;
            for (auto* p : h->params()) {
                if (p) recache(p->type());
            }
            recache(h->retType());
            recache(h->fallibleErrTypeNode());
            for (auto* d : h->typeParamDefaults())
                recache(d);
        };
        for (auto* fn : file->getFunctions()) {
            if (fn) fillHeader(fn->header());
        }
        for (auto* sd : file->getStructDecls()) {
            if (!sd) continue;
            for (auto* f : sd->fields()) {
                if (f) recache(f->type());
            }
            for (auto* d : sd->typeParamDefaults())
                recache(d);
        }
        for (auto* impl : file->getStructImpls()) {
            if (!impl) continue;
            for (auto* m : impl->methods()) {
                if (m) fillHeader(m->header());
            }
        }
        for (auto* ed : file->getEnumDecls()) {
            if (!ed) continue;
            for (auto* v : ed->variants()) {
                if (!v) continue;
                for (auto* pt : v->payloadTypes())
                    recache(pt);
            }
            for (auto* d : ed->typeParamDefaults())
                recache(d);
        }
        for (auto* spec : file->getSpecDecls()) {
            if (!spec) continue;
            for (auto* s : spec->signatures())
                fillHeader(s);
            for (auto* d : spec->typeParamDefaults())
                recache(d);
        }
        for (auto* al : file->getAliasDecls()) {
            if (al) recache(al->target());
        }
        for (auto* g : file->getGlobalConsts()) {
            if (g) recache(g->typeNode());
        }
        for (auto* g : file->getGlobalVars()) {
            if (g) recache(g->typeNode());
        }
    }
    file->syncFnSymbolsFromAst();
}

} // namespace sema
