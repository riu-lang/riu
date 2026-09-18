// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "file_node.h"
#include "builtin_methods.h"

FileNode::FileNode(string moduleName) : ScopeNode(nullptr), _moduleName(std::move(moduleName)) {
    // 注册基本类型为 struct 占位符，并预声明方法符号
    const initializer_list<string> TYPES = {"bool", "i8",  "i16", "i32", "i64",   "u8",   "u16",
                                            "u32",  "u64", "f32", "f64", "isize", "usize"};
    const initializer_list<string> INT_TYPES = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "isize", "usize"};
    const initializer_list<string> FLOAT_TYPES = {"f32", "f64"};

    for (const auto& t : TYPES) {
        registerSymbol(t, {SymbolKind::Struct, t, TypeInfo(t)});

        // 类型转换方法
        for (const auto& f : TYPES) {
            string fnName = "to_";
            fnName += f;
            string fullName = t;
            fullName += '.';
            fullName += fnName;
            registerSymbol(fullName, {SymbolKind::Function, fnName, TypeInfo(f)});
            registerFnSymbol(fullName, {fnName, "", {}, TypeInfo(f)});
        }
    }

    // 整数类型运算符方法
    for (const auto& t : INT_TYPES) {
        // 算术运算符: plus, minus, mul, div, mod
        for (auto op : {"plus", "minus", "mul", "div", "mod"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo(t)});
            registerFnSymbol(fullName, {op, "", {TypeInfo(t)}, TypeInfo(t)});
        }
        // 比较运算符: eq, ne, lt, le, gt, ge
        for (auto op : {"eq", "ne", "lt", "le", "gt", "ge"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo("bool")});
            registerFnSymbol(fullName, {op, "", {TypeInfo(t)}, TypeInfo("bool")});
        }
        // 位运算符: and, or, xor, shl, shr
        for (auto op : {"and", "or", "xor", "shl", "shr"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo(t)});
            registerFnSymbol(fullName, {op, "", {TypeInfo(t)}, TypeInfo(t)});
        }
        // 一元运算符: neg, inv
        for (auto op : {"neg", "inv"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo(t)});
            registerFnSymbol(fullName, {op, "", {}, TypeInfo(t)});
        }
    }

    // 浮点类型运算符方法
    for (const auto& t : FLOAT_TYPES) {
        // 算术运算符: plus, minus, mul, div, mod
        for (auto op : {"plus", "minus", "mul", "div", "mod"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo(t)});
            registerFnSymbol(fullName, {op, "", {TypeInfo(t)}, TypeInfo(t)});
        }
        // 比较运算符: eq, ne, lt, le, gt, ge
        for (auto op : {"eq", "ne", "lt", "le", "gt", "ge"}) {
            string fullName = string(t) + "." + op;
            registerSymbol(fullName, {SymbolKind::Function, op, TypeInfo("bool")});
            registerFnSymbol(fullName, {op, "", {TypeInfo(t)}, TypeInfo("bool")});
        }
        // 一元运算符: neg
        {
            string fullName = string(t) + ".neg";
            registerSymbol(fullName, {SymbolKind::Function, "neg", TypeInfo(t)});
            registerFnSymbol(fullName, {"neg", "", {}, TypeInfo(t)});
        }
    }

    // bool 类型运算符方法
    {
        string fullName = "bool.not";
        registerSymbol(fullName, {SymbolKind::Function, "not", TypeInfo("bool")});
        registerFnSymbol(fullName, {"not", "", {}, TypeInfo("bool")});
    }

    // Array 内建方法符号：与 kBuiltinMethods 同一张表（codegen 由 compileArrayMethodCall 接管）。
    {
        TypeInfo tpT("T");
        TypeInfo tpRefT("Ref", {make_shared<TypeInfo>(tpT)});
        TypeInfo tpusize("usize");
        TypeInfo tpVoid;
        TypeInfo tpBool("bool");
        TypeInfo tpArrayT("Array", {make_shared<TypeInfo>(tpT)});
        TypeInfo tpArrayU("Array", {make_shared<TypeInfo>("U")});
        TypeInfo tpElemNull("Nullable", {make_shared<TypeInfo>(tpT)});
        TypeInfo tpUsizeNull("Nullable", {make_shared<TypeInfo>("usize")});

        for (const auto& spec : sema::kBuiltinMethods) {
            if (spec.recv != sema::BuiltinRecv::Array || spec.isStatic) continue;
            TypeInfo ret;
            switch (spec.ret) {
            case sema::BuiltinRet::Void:
                ret = tpVoid;
                break;
            case sema::BuiltinRet::Usize:
                ret = tpusize;
                break;
            case sema::BuiltinRet::Bool:
                ret = tpBool;
                break;
            case sema::BuiltinRet::Elem:
                ret = tpT;
                break;
            case sema::BuiltinRet::ElemRef:
                ret = tpRefT;
                break;
            case sema::BuiltinRet::Self:
                ret = tpArrayT;
                break;
            case sema::BuiltinRet::ElemNullable:
                ret = tpElemNull;
                break;
            case sema::BuiltinRet::UsizeNullable:
                ret = tpUsizeNull;
                break;
            case sema::BuiltinRet::TypeArg0Array:
                ret = tpArrayU;
                break;
            }
            vector<TypeInfo> params;
            if (spec.arity >= 1) {
                if (spec.arg0Type && string(spec.arg0Type) == "Array") {
                    params.push_back(TypeInfo("Ref", {make_shared<TypeInfo>(tpArrayT)}));
                } else if (spec.arg0Type && string(spec.arg0Type) == "Predicate") {
                    params.push_back(TypeInfo(FnTag{}, {make_shared<TypeInfo>(tpRefT)}, make_shared<TypeInfo>(tpBool)));
                } else if (spec.arg0Type && string(spec.arg0Type) == "Transform") {
                    params.push_back(TypeInfo(FnTag{}, {make_shared<TypeInfo>(tpRefT)}, make_shared<TypeInfo>("U")));
                } else {
                    params.push_back(spec.arg0Type ? TypeInfo(spec.arg0Type) : tpT);
                }
            }
            if (spec.arity >= 2) {
                params.push_back(spec.arg1Type ? TypeInfo(spec.arg1Type) : tpT);
            }
            string full = string("Array.") + spec.name;
            registerSymbol(full, {SymbolKind::Function, spec.name, ret});
            registerFnSymbol(full, {spec.name, "", std::move(params), ret});
        }
    }
}

void FileNode::addFunction(FnNode* function) {
    _functions.push_back(function);
    if (!function || !function->header()) return;
    string n = function->header()->name().getText();
    _fnsByName[n].push_back(function);
}

void FileNode::syncFnSymbolsFromAst() {
    auto namesMatch = [](const vector<TypeInfo>& a, const vector<TypeInfo>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].getFullName() != b[i].getFullName()) return false;
        }
        return true;
    };
    auto apply = [&](const string& key, vector<TypeInfo> params, TypeInfo ret, const string& fallible) {
        auto it = _fnSymbols.find(key);
        if (it == _fnSymbols.end()) return;
        FnSymbolInfo* target = nullptr;
        for (auto& cand : it->second) {
            if (namesMatch(cand.params, params)) {
                target = &cand;
                break;
            }
        }
        if (!target) {
            FnSymbolInfo* only = nullptr;
            int n = 0;
            for (auto& cand : it->second) {
                if (cand.params.size() == params.size()) {
                    only = &cand;
                    ++n;
                }
            }
            if (n == 1) target = only;
        }
        if (!target) return;
        target->params = std::move(params);
        target->retType = std::move(ret);
        target->fallibleErrType = fallible;
    };

    for (auto* fn : _functions) {
        auto* h = fn->header();
        vector<TypeInfo> params;
        for (auto p : h->params()) {
            params.push_back(p->type() ? p->type()->getType() : TypeInfo());
        }
        TypeInfo ret = h->retType() ? h->retType()->getType() : TypeInfo();
        apply(h->name().getText(), std::move(params), std::move(ret), h->resolvedFallibleErr());
    }
    for (auto* impl : _structImpls) {
        for (auto* method : impl->methods()) {
            auto* h = method->header();
            vector<TypeInfo> params;
            TypeInfo recv(impl->structName(), _moduleName);
            params.push_back(std::move(recv));
            for (auto p : h->params()) {
                params.push_back(p->type() ? p->type()->getType() : TypeInfo());
            }
            TypeInfo ret = h->retType() ? h->retType()->getType() : TypeInfo();
            apply(impl->structName() + "." + h->name().getText(), std::move(params), std::move(ret),
                  h->resolvedFallibleErr());
        }
    }
}

void FileNode::addStructDecl(StructDeclNode* structDecl) {
    _structDecls.push_back(structDecl);
    string n = structDecl->name().getText();
    if (!_structMap.contains(n)) _structMap[n] = structDecl;
    SymbolInfo sym(SymbolKind::Struct, n, TypeInfo(n, _moduleName));
    sym.moduleName = _moduleName;
    registerSymbol(n, std::move(sym));
}

void FileNode::addStructImpl(StructImplNode* structImpl) {
    _structImpls.push_back(structImpl);
    if (!structImpl) return;
    const string& n = structImpl->structName();
    if (!_implMap.contains(n)) _implMap[n] = structImpl;
}

void FileNode::addSpecDecl(SpecDeclNode* specDecl) {
    _specDecls.push_back(specDecl);
    // draft 名按 §10 共享顶层符号命名空间
    string n = specDecl->name().getText();
    if (!lookupSymbol(n)) {
        SymbolInfo sym(SymbolKind::Struct, n, TypeInfo(n, _moduleName));
        sym.moduleName = _moduleName;
        registerSymbol(n, sym);
    }
}

void FileNode::addAliasDecl(AliasDeclNode* aliasDecl) {
    _aliasDecls.push_back(aliasDecl);
    // aliasDecl->name() 是成员 Token 引用；map 键仍要自有 string。
    string key = aliasDecl->name().getText();
    _aliasMap[key] = aliasDecl;
}

void FileNode::addEnumDecl(EnumDeclNode* enumDecl) {
    _enumDecls.push_back(enumDecl);
    string name = enumDecl->name().getText();
    _enumMap[name] = enumDecl;
    // enum 名进类型命名空间（与 struct 同等地位）；variant 名不进顶层
    if (!lookupSymbol(name)) {
        SymbolInfo sym(SymbolKind::Struct, name, TypeInfo(name, _moduleName));
        sym.moduleName = _moduleName;
        registerSymbol(name, sym);
    }
}

EnumDeclNode* FileNode::getEnumDecl(const string& name) const {
    auto it = _enumMap.find(name);
    if (it != _enumMap.end()) return it->second;
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_enumMap.find(name);
        if (jt != imp->_enumMap.end()) return jt->second;
    }
    return nullptr;
}

AliasDeclNode* FileNode::getAliasDecl(const string& name) const {
    auto it = _aliasMap.find(name);
    if (it != _aliasMap.end()) return it->second;
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_aliasMap.find(name);
        if (jt != imp->_aliasMap.end()) return jt->second;
    }
    return nullptr;
}

SpecDeclNode* FileNode::getSpecDecl(const string& name) const {
    for (auto& d : _specDecls) {
        if (d->name().getText() == name) return d;
    }
    for (auto* imp : _wildcardImports) {
        for (auto& d : imp->_specDecls) {
            if (d->name().getText() == name) return d;
        }
    }
    return nullptr;
}

void FileNode::addGlobalConst(GlobalConstNode* globalConst) {
    _globalConsts.push_back(globalConst);
    string name = globalConst->name().getText();
    if (!lookupSymbol(name)) {
        SymbolInfo sym(SymbolKind::Variable, name, globalConst->getType(), false);
        sym.moduleName = _moduleName;
        registerSymbol(name, sym);
    }
}

// DRAFT-static-vars Phase 1: 运行期初始化全局变量
void FileNode::addGlobalVar(GlobalVarNode* globalVar) {
    _globalVars.push_back(globalVar);
    string name = globalVar->name().getText();
    if (!lookupSymbol(name)) {
        bool isMutable = globalVar->isMutable();
        SymbolInfo sym(SymbolKind::Variable, name, globalVar->getType(), isMutable);
        sym.moduleName = _moduleName;
        registerSymbol(name, sym);
    }
}

const vector<FnNode*>& FileNode::getFunctions() const {
    return _functions;
}

namespace {
template <typename Map>
StructDeclNode* structFromMap(const Map& m, const string& name, bool includeBuiltin) {
    auto it = m.find(name);
    if (it == m.end()) return nullptr;
    StructDeclNode* decl = it->second;
    // `#Builtin` 声明仅作语言层占位（Rc/Ref/Ptr/Array 及 i8..f64）。
    // 默认过滤；Sema arity 等要看见占位时传 includeBuiltin=true。
    if (!includeBuiltin && decl && decl->hasAnno("Builtin")) return nullptr;
    return decl;
}

template <typename Map>
StructImplNode* implFromMap(const Map& m, const string& name) {
    auto it = m.find(name);
    return it != m.end() ? it->second : nullptr;
}

template <typename Map>
FnNode* firstFnByName(const Map& m, const string& name) {
    auto it = m.find(name);
    if (it == m.end() || it->second.empty()) return nullptr;
    return it->second[0];
}
} // namespace

StructDeclNode* FileNode::getStructDecl(const string& name, bool includeBuiltin) const {
    if (auto* d = structFromMap(_structMap, name, includeBuiltin)) return d;
    for (auto* imp : _wildcardImports) {
        if (auto* d = structFromMap(imp->_structMap, name, includeBuiltin)) return d;
    }
    return nullptr;
}

StructDeclNode* FileNode::localStructDecl(const string& name, bool includeBuiltin) const {
    return structFromMap(_structMap, name, includeBuiltin);
}

EnumDeclNode* FileNode::localEnumDecl(const string& name) const {
    auto it = _enumMap.find(name);
    return it != _enumMap.end() ? it->second : nullptr;
}

AliasDeclNode* FileNode::localAliasDecl(const string& name) const {
    auto it = _aliasMap.find(name);
    return it != _aliasMap.end() ? it->second : nullptr;
}

StructImplNode* FileNode::getStructImpl(const string& name) const {
    if (auto* impl = implFromMap(_implMap, name)) return impl;
    for (auto* imp : _wildcardImports) {
        if (auto* impl = implFromMap(imp->_implMap, name)) return impl;
    }
    return nullptr;
}

StructImplNode* FileNode::localStructImpl(const string& name) const {
    return implFromMap(_implMap, name);
}

FnNode* FileNode::getFunction(const string& name) const {
    return firstFnByName(_fnsByName, name);
}

// 递归沿 parentScope 链（仅 FileNode 层）查找函数。SDK 默认挂为 parent scope，
// 但 `use riu.core.*` 被 visitImports 跳过（ast_builder_decl.cpp:268），因此
// SDK 不进入 _wildcardImports，需额外沿 parent scope 链搜索。
namespace {
FileNode* parentFileNode(const FileNode* f) {
    if (!f) return nullptr;
    auto* ps = const_cast<FileNode*>(f)->parentScope();
    while (ps) {
        if (auto* pf = dynamic_cast<FileNode*>(ps)) return pf;
        ps = ps->parentScope();
    }
    return nullptr;
}

// `use foo.*` 会把源模块符号拷进 foo 的表；通配查找只暴露 foo 自己的声明，
// 不把 foo 的 use 再导出。与 injectFileWildcard 的 moduleName 过滤对齐。
bool isOwnModuleName(const FileNode* file, const string& moduleName) {
    return file && moduleName == file->moduleName();
}

template <typename Map>
FnNode* firstGenericFn(const Map& m, const string& name) {
    auto it = m.find(name);
    if (it == m.end()) return nullptr;
    for (auto* fn : it->second) {
        if (fn && fn->header() && fn->header()->isGeneric()) return fn;
    }
    return nullptr;
}

template <typename Map>
void collectGenericFnsIn(const Map& m, FileNode* owner, const string& name, vector<pair<FnNode*, FileNode*>>& out) {
    auto it = m.find(name);
    if (it == m.end()) return;
    for (auto* fn : it->second) {
        if (fn && fn->header() && fn->header()->isGeneric()) {
            out.emplace_back(fn, owner);
        }
    }
}
} // namespace

// 同 getFunction，同时返回所属 FileNode；搜索范围：本地 + wildcardImports + parent scope 链
pair<FnNode*, FileNode*> FileNode::getFunctionWithOwner(const string& name) const {
    if (auto* fn = firstFnByName(_fnsByName, name)) return {fn, const_cast<FileNode*>(this)};
    for (auto* imp : _wildcardImports) {
        if (auto* fn = firstFnByName(imp->_fnsByName, name)) return {fn, imp};
    }
    // 沿 parent scope 链搜索（含各 parent 的 wildcardImports——SDK 平铺文件拆分后，
    // _sdkFile 空壳不再直接持有函数，泛型函数定义在各子文件的 wildcardImport 里）
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        if (auto* fn = firstFnByName(pf->_fnsByName, name)) return {fn, pf};
        for (auto* imp : pf->_wildcardImports) {
            if (auto* fn = firstFnByName(imp->_fnsByName, name)) return {fn, imp};
        }
    }
    return {nullptr, nullptr};
}

// 仅返回 generic 重载：用于 dispatcher 让非泛型 fnSymbol 优先于同名泛型函数
// （例：`assert_eq:<T>` 与 `assert_eq(String&, String&)` 共存时）
// 搜索范围：本地 + wildcardImports + parent scope 链（含各 parent 的 wildcardImports）
pair<FnNode*, FileNode*> FileNode::getGenericFunction(const string& name) const {
    if (auto* fn = firstGenericFn(_fnsByName, name)) return {fn, const_cast<FileNode*>(this)};
    for (auto* imp : _wildcardImports) {
        if (auto* fn = firstGenericFn(imp->_fnsByName, name)) return {fn, imp};
    }
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        if (auto* fn = firstGenericFn(pf->_fnsByName, name)) return {fn, pf};
        for (auto* imp : pf->_wildcardImports) {
            if (auto* fn = firstGenericFn(imp->_fnsByName, name)) return {fn, imp};
        }
    }
    return {nullptr, nullptr};
}

// 收集所有同名泛型函数（支持多个泛型重载消歧，如 print<T>(x T) + print<T>(x T&)）
// 搜索范围：本地 + wildcardImports + parent scope 链（含各 parent 的 wildcardImports）
void FileNode::collectGenericFunctions(const string& name, vector<pair<FnNode*, FileNode*>>& out,
                                       FileNode* owner) const {
    collectGenericFnsIn(_fnsByName, owner, name, out);
    for (auto* imp : _wildcardImports) {
        if (imp == owner) continue; // 避免重复收集（调用方可能已用本地 owner 收集过该 imp）
        collectGenericFnsIn(imp->_fnsByName, imp, name, out);
    }
    // 沿 parent scope 链搜索（含各 parent 的 wildcardImports）
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        if (pf == owner) continue;
        collectGenericFnsIn(pf->_fnsByName, pf, name, out);
        for (auto* imp : pf->_wildcardImports) {
            if (imp == owner) continue;
            collectGenericFnsIn(imp->_fnsByName, imp, name, out);
        }
    }
}

void FileNode::addImport(const string& mod) {
    if (mod.empty() || mod == _moduleName) return;
    for (auto& m : _imports)
        if (m == mod) return;
    _imports.push_back(mod);
}

void FileNode::addModuleAlias(const string& alias, FileNode* file) {
    _moduleAliases[alias] = file;
}

FileNode* FileNode::moduleAlias(const string& alias) const {
    auto it = _moduleAliases.find(alias);
    if (it != _moduleAliases.end()) return it->second;
    // 沿父作用域查找（用户文件经父作用域继承 _sdkFile 的别名）
    ScopeNode* parent = const_cast<FileNode*>(this)->parentScope();
    while (parent) {
        if (auto* parentFile = dynamic_cast<FileNode*>(parent)) {
            auto pit = parentFile->_moduleAliases.find(alias);
            if (pit != parentFile->_moduleAliases.end()) return pit->second;
        }
        parent = parent->parentScope();
    }
    return nullptr;
}

void FileNode::addPackageAlias(const string& alias, const string& dottedPath) {
    _packageAliases[alias] = dottedPath;
}

const string* FileNode::packageAlias(const string& alias) const {
    auto it = _packageAliases.find(alias);
    if (it != _packageAliases.end()) return &it->second;
    ScopeNode* parent = const_cast<FileNode*>(this)->parentScope();
    while (parent) {
        if (auto* parentFile = dynamic_cast<FileNode*>(parent)) {
            auto pit = parentFile->_packageAliases.find(alias);
            if (pit != parentFile->_packageAliases.end()) return &pit->second;
        }
        parent = parent->parentScope();
    }
    return nullptr;
}

void FileNode::addPackageChild(const string& alias, const string& child, FileNode* file) {
    _packageChildren[alias][child] = file;
}

FileNode* FileNode::packageChild(const string& alias, const string& child) const {
    auto it = _packageChildren.find(alias);
    if (it != _packageChildren.end()) {
        auto jt = it->second.find(child);
        if (jt != it->second.end()) return jt->second;
    }
    ScopeNode* parent = const_cast<FileNode*>(this)->parentScope();
    while (parent) {
        if (auto* parentFile = dynamic_cast<FileNode*>(parent)) {
            auto pit = parentFile->_packageChildren.find(alias);
            if (pit != parentFile->_packageChildren.end()) {
                auto jt = pit->second.find(child);
                if (jt != pit->second.end()) return jt->second;
            }
        }
        parent = parent->parentScope();
    }
    return nullptr;
}

void FileNode::addWildcardAliasSource(const string& alias, const string& sourceModule) {
    auto& sources = _wildcardAliasSources[alias];
    for (auto& s : sources) {
        if (s == sourceModule) return;
    }
    sources.push_back(sourceModule);
}

const vector<string>* FileNode::wildcardAliasSources(const string& alias) const {
    auto it = _wildcardAliasSources.find(alias);
    if (it == _wildcardAliasSources.end()) return nullptr;
    return &it->second;
}

bool FileNode::isAmbiguousAlias(const string& alias) const {
    auto it = _wildcardAliasSources.find(alias);
    if (it == _wildcardAliasSources.end()) return false;
    return it->second.size() >= 2;
}

void FileNode::throwAmbiguousAlias(const string& alias, int line) const {
    auto it = _wildcardAliasSources.find(alias);
    string sources;
    if (it != _wildcardAliasSources.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) {
            sources += (i == 0 ? "" : " and ");
            sources += it->second[i];
        }
    }
    throw RiuError(line, ErrorCode::E2008, alias, sources);
}

void FileNode::addNamedTypeImport(const string& name, FileNode* owner) {
    if (name.empty() || !owner) return;
    auto& owners = _namedTypeImports[name];
    for (auto* o : owners) {
        if (o == owner) return;
    }
    owners.push_back(owner);
}

const vector<FileNode*>* FileNode::namedTypeImports(const string& name) const {
    auto it = _namedTypeImports.find(name);
    if (it == _namedTypeImports.end()) return nullptr;
    return &it->second;
}

void FileNode::addWildcardImport(FileNode* file) {
    if (!file || file == this) return;
    for (auto* f : _wildcardImports)
        if (f == file) return;
    _wildcardImports.push_back(file);
}

FileNode* FileNode::getStructOwner(const string& name) {
    if (_structMap.contains(name)) return this;
    if (auto* named = namedTypeImports(name)) {
        for (auto* o : *named) {
            if (o && (o->localStructDecl(name) || o->localStructImpl(name))) return o;
        }
    }
    for (auto* imp : _wildcardImports) {
        if (imp->getStructDecl(name) || imp->getStructImpl(name)) return imp;
    }
    return nullptr;
}

FileNode* FileNode::relatedFileHere(const string& moduleName) const {
    if (moduleName.empty()) return nullptr;
    if (_moduleName == moduleName) return const_cast<FileNode*>(this);
    for (auto* imp : _wildcardImports) {
        if (imp && imp->moduleName() == moduleName) return imp;
    }
    for (auto& [_, target] : _moduleAliases) {
        if (target && target->moduleName() == moduleName) return target;
    }
    for (auto& [_, kids] : _packageChildren) {
        for (auto& [_, child] : kids) {
            if (child && child->moduleName() == moduleName) return child;
        }
    }
    for (auto& [_, owners] : _namedTypeImports) {
        for (auto* o : owners) {
            if (o && o->moduleName() == moduleName) return o;
        }
    }
    return nullptr;
}

FileNode* FileNode::relatedFile(const string& moduleName) const {
    if (auto* hit = relatedFileHere(moduleName)) return hit;
    ScopeNode* parent = const_cast<FileNode*>(this)->parentScope();
    while (parent) {
        if (auto* pf = dynamic_cast<FileNode*>(parent)) {
            if (auto* hit = pf->relatedFileHere(moduleName)) return hit;
        }
        parent = parent->parentScope();
    }
    return nullptr;
}

void FileNode::addUseSpec(UseSpec spec) {
    if (spec.moduleName.empty() || spec.moduleName == _moduleName) return;
    _useSpecs.push_back(std::move(spec));
}

// ==================== 符号查找（覆写，搜索范围扩展到 wildcardImports） ====================

SymbolInfo* FileNode::lookupSymbol(const string& name) {
    auto it = _symbols.find(name);
    if (it != _symbols.end()) {
        return &it->second;
    }
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_symbols.find(name);
        if (jt != imp->_symbols.end() && isOwnModuleName(imp, jt->second.moduleName)) {
            // 他模块 `_` 前缀全局变量 / `#Cval` 不可见（§10.3.2）。
            // 函数仍查：SDK `__riu_*` 与 call_resolve 的 E6006 都走这条。
            if (jt->second.isPrivate && jt->second.kind == SymbolKind::Variable) continue;
            return &jt->second;
        }
    }
    if (_parentScope) {
        return _parentScope->lookupSymbol(name);
    }
    return nullptr;
}

FnSymbolInfo* FileNode::lookupFnSymbol(const string& name) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end() && !it->second.empty()) {
        return &it->second[0];
    }
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_fnSymbols.find(name);
        if (jt != imp->_fnSymbols.end()) {
            for (auto& fn : jt->second) {
                if (isOwnModuleName(imp, fn.moduleName)) return &fn;
            }
        }
    }
    if (_parentScope) {
        return _parentScope->lookupFnSymbol(name);
    }
    return nullptr;
}

FnSymbolInfo* FileNode::lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes) {
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        for (auto& fnInfo : it->second) {
            if (matchFnParams(fnInfo, paramTypes)) return &fnInfo;
        }
    }
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_fnSymbols.find(name);
        if (jt != imp->_fnSymbols.end()) {
            for (auto& fnInfo : jt->second) {
                if (isOwnModuleName(imp, fnInfo.moduleName) && matchFnParams(fnInfo, paramTypes)) return &fnInfo;
            }
        }
    }
    if (_parentScope) {
        return _parentScope->lookupFnSymbolWithParams(name, paramTypes);
    }
    return nullptr;
}

void FileNode::collectFnOverloads(const string& name, vector<FnSymbolInfo*>& out) {
    // 指针去重辅助：线性扫描（实际集合很小，通常 < 5 条目）
    auto addIfNew = [&](FnSymbolInfo* p) {
        for (auto* existing : out) {
            if (existing == p) return;
        }
        out.push_back(p);
    };

    // 语义去重辅助：检查 fns 是否已包含与 target 相同模块名+形参列表的条目。
    // visitImportDecl 会将导入函数拷贝到本地 _fnSymbols，同时 addWildcardImport，
    // 导致同一函数以不同 FnSymbolInfo 副本存在于本地和 wildcardImport 两端。
    // 指针去重无法覆盖此场景（不同对象），需要按模块名+形参去重。
    auto sameParams = [](FnSymbolInfo* a, FnSymbolInfo* b) -> bool {
        if (a->params.size() != b->params.size()) return false;
        for (size_t i = 0; i < a->params.size(); ++i) {
            if (!(a->params[i] == b->params[i])) return false;
        }
        return true;
    };
    auto hasSemanticDup = [&](FnSymbolInfo* target) -> bool {
        for (auto* existing : out) {
            if (!sameParams(existing, target)) continue;
            if (existing->moduleName == target->moduleName) return true;
            // 多模块重复声明同一 C 函数：riu 名按模块分区，LLVM 只认链接名
            if (existing->isExternal && target->isExternal && existing->externLinkName() == target->externLinkName() &&
                existing->sameExternCSig(*target)) {
                return true;
            }
        }
        return false;
    };

    // 1) 本地 _fnSymbols：本模块声明优先于 `use` 注入的副本（同 C ABI 不构成重载）
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        vector<FnSymbolInfo*> own;
        vector<FnSymbolInfo*> injected;
        for (auto& fn : it->second) {
            if (fn.moduleName.empty() || fn.moduleName == _moduleName)
                own.push_back(&fn);
            else
                injected.push_back(&fn);
        }
        for (auto* fn : own)
            addIfNew(fn);
        for (auto* fn : injected) {
            if (!hasSemanticDup(fn)) addIfNew(fn);
        }
    }

    // 2) wildcardImports 的直接 _fnSymbols（仅浅层，避免递归回到自己）。
    // 只收该文件自己的声明，不把其对别人的 `use` 注入再导出。
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_fnSymbols.find(name);
        if (jt != imp->_fnSymbols.end()) {
            for (auto& fn : jt->second)
                if (isOwnModuleName(imp, fn.moduleName) && !hasSemanticDup(&fn)) addIfNew(&fn);
        }
    }

    // 3) parentScope 链：先收到独立列表再按 C ABI / 模块+形参去重，避免 SDK
    // 与本模块同签名 extern 被当成两个重载（E6014）。
    if (_parentScope) {
        vector<FnSymbolInfo*> fromParent;
        _parentScope->collectFnOverloads(name, fromParent);
        for (auto* fn : fromParent) {
            if (!hasSemanticDup(fn)) addIfNew(fn);
        }
    }
}
