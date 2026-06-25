// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "file_node.h"

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

    // Array 内建方法符号（供泛型方法调用返回类型推导，codegen 由 compileArrayMethodCall 接管）。
    // 返回值中的 T 是类型参数占位符，与 Array struct 声明的 typeParams[0] 同名，
    // ExprCallNode::getType() 的泛型替换逻辑（subst 表）会自动将 T 替换为具体元素类型。
    {
        TypeInfo tpT("T");                                    // 类型参数占位符
        TypeInfo tpRefT("Ref", {make_shared<TypeInfo>(tpT)}); // T&
        TypeInfo tpusize("usize");
        TypeInfo tpVoid; // void（空 TypeInfo）
        TypeInfo tpBool("bool");

        // Array.get(i usize) → T&
        registerSymbol("Array.get", {SymbolKind::Function, "get", tpRefT});
        registerFnSymbol("Array.get", {"get", "", {tpusize}, tpRefT});

        // Array.first() → T&
        registerSymbol("Array.first", {SymbolKind::Function, "first", tpRefT});
        registerFnSymbol("Array.first", {"first", "", {}, tpRefT});

        // Array.last() → T&
        registerSymbol("Array.last", {SymbolKind::Function, "last", tpRefT});
        registerFnSymbol("Array.last", {"last", "", {}, tpRefT});

        // Array.pop() → T
        registerSymbol("Array.pop", {SymbolKind::Function, "pop", tpT});
        registerFnSymbol("Array.pop", {"pop", "", {}, tpT});

        // Array.push(x T) → void
        registerSymbol("Array.push", {SymbolKind::Function, "push", tpVoid});
        registerFnSymbol("Array.push", {"push", "", {tpT}, tpVoid});

        // Array.len() → usize
        registerSymbol("Array.len", {SymbolKind::Function, "len", tpusize});
        registerFnSymbol("Array.len", {"len", "", {}, tpusize});

        // Array.cap() → usize
        registerSymbol("Array.cap", {SymbolKind::Function, "cap", tpusize});
        registerFnSymbol("Array.cap", {"cap", "", {}, tpusize});

        // Array.is_empty() → bool
        registerSymbol("Array.is_empty", {SymbolKind::Function, "is_empty", tpBool});
        registerFnSymbol("Array.is_empty", {"is_empty", "", {}, tpBool});

        // Array.clear() → void
        registerSymbol("Array.clear", {SymbolKind::Function, "clear", tpVoid});
        registerFnSymbol("Array.clear", {"clear", "", {}, tpVoid});

        // Array.set_len(n usize) → void
        registerSymbol("Array.set_len", {SymbolKind::Function, "set_len", tpVoid});
        registerFnSymbol("Array.set_len", {"set_len", "", {tpusize}, tpVoid});
    }
}

void FileNode::addFunction(const p<FnNode>& function) {
    _functions.push_back(function);
}

void FileNode::addStructDecl(const p<StructDeclNode>& structDecl) {
    _structDecls.push_back(structDecl);
    registerSymbol(structDecl->name().getText(),
                   {SymbolKind::Struct, structDecl->name().getText(), TypeInfo(structDecl->name().getText())});
}

void FileNode::addStructImpl(const p<StructImplNode>& structImpl) {
    _structImpls.push_back(structImpl);
}

void FileNode::addSpecDecl(const p<SpecDeclNode>& specDecl) {
    _specDecls.push_back(specDecl);
    // draft 名按 §10 共享顶层符号命名空间
    string n = specDecl->name().getText();
    if (!lookupSymbol(n)) {
        SymbolInfo sym(SymbolKind::Struct, n, TypeInfo(n));
        sym.moduleName = _moduleName;
        registerSymbol(n, sym);
    }
}

void FileNode::addAliasDecl(const p<AliasDeclNode>& aliasDecl) {
    _aliasDecls.push_back(aliasDecl);
    // 注意：aliasDecl->name() 返回 Token 值类型，需复制为 string，避免 .getText() 引用绑定到临时对象悬空
    string key = aliasDecl->name().getText();
    _aliasMap[key] = aliasDecl;
}

void FileNode::addEnumDecl(const p<EnumDeclNode>& enumDecl) {
    _enumDecls.push_back(enumDecl);
    string name = enumDecl->name().getText();
    _enumMap[name] = enumDecl;
    // enum 名进类型命名空间（与 struct 同等地位）；variant 名不进顶层
    if (!lookupSymbol(name)) {
        SymbolInfo sym(SymbolKind::Struct, name, TypeInfo(name));
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

void FileNode::addGlobalConst(const p<GlobalConstNode>& globalConst) {
    _globalConsts.push_back(globalConst);
    string name = globalConst->name().getText();
    if (!lookupSymbol(name)) {
        registerSymbol(name, {SymbolKind::Variable, name, globalConst->getType(), false});
    }
}

// DRAFT-static-vars Phase 1: 运行期初始化全局变量
void FileNode::addGlobalVar(const p<GlobalVarNode>& globalVar) {
    _globalVars.push_back(globalVar);
    string name = globalVar->name().getText();
    if (!lookupSymbol(name)) {
        bool isMutable = globalVar->isMutable();
        registerSymbol(name, {SymbolKind::Variable, name, globalVar->getType(), isMutable});
    }
}

const vector<p<FnNode>>& FileNode::getFunctions() const {
    return _functions;
}

StructDeclNode* FileNode::getStructDecl(const string& name, bool includeBuiltin) const {
    // `#Builtin` 声明仅作语言层占位（如 Rc/Ref/Ptr/Array 及 i8..f64），
    // 它们的布局与方法由编译器合成，对用户结构体逻辑不可见。默认过滤掉它们 ——
    // Compiler 端用户结构体查找不应命中。SemaPass 走 arity / 形态校验时需要看到
    // 这些占位 (否则 Rc/Ref 查不到), 显式传 includeBuiltin=true。
    auto matches = [&](const p<StructDeclNode>& decl) {
        if (decl->name().getText() != name) return false;
        return includeBuiltin || !decl->hasAnno("Builtin");
    };
    for (auto& decl : _structDecls) {
        if (matches(decl)) return decl;
    }
    for (auto* imp : _wildcardImports) {
        for (auto& decl : imp->_structDecls) {
            if (matches(decl)) return decl;
        }
    }
    return nullptr;
}

StructImplNode* FileNode::getStructImpl(const string& name) const {
    for (auto& impl : _structImpls) {
        if (impl->structName() == name) {
            return impl;
        }
    }
    for (auto* imp : _wildcardImports) {
        for (auto& impl : imp->_structImpls) {
            if (impl->structName() == name) return impl;
        }
    }
    return nullptr;
}

FnNode* FileNode::getFunction(const string& name) const {
    for (auto& fn : _functions) {
        if (fn->header()->name().getText() == name) {
            return fn;
        }
    }
    return nullptr;
}

// 递归沿 parentScope 链（仅 FileNode 层）查找函数。SDK 默认挂为 parent scope，
// 但 `use yux.core.*` 被 visitImports 跳过（ast_builder_decl.cpp:268），因此
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
} // namespace

// 同 getFunction，同时返回所属 FileNode；搜索范围：本地 + wildcardImports + parent scope 链
pair<FnNode*, FileNode*> FileNode::getFunctionWithOwner(const string& name) const {
    for (auto& fn : _functions) {
        if (fn->header()->name().getText() == name) {
            return {fn, const_cast<FileNode*>(this)};
        }
    }
    for (auto* imp : _wildcardImports) {
        for (auto& fn : imp->_functions) {
            if (fn->header()->name().getText() == name) {
                return {fn, imp};
            }
        }
    }
    // 沿 parent scope 链搜索（含各 parent 的 wildcardImports——SDK 平铺文件拆分后，
    // _sdkFile 空壳不再直接持有函数，泛型函数定义在各子文件的 wildcardImport 里）
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        for (auto& fn : pf->_functions) {
            if (fn->header()->name().getText() == name) {
                return {fn, pf};
            }
        }
        for (auto* imp : pf->_wildcardImports) {
            for (auto& fn : imp->_functions) {
                if (fn->header()->name().getText() == name) {
                    return {fn, imp};
                }
            }
        }
    }
    return {nullptr, nullptr};
}

// 仅返回 generic 重载：用于 dispatcher 让非泛型 fnSymbol 优先于同名泛型函数
// （例：`assert_eq:<T>` 与 `assert_eq(String&, String&)` 共存时）
// 搜索范围：本地 + wildcardImports + parent scope 链（含各 parent 的 wildcardImports）
pair<FnNode*, FileNode*> FileNode::getGenericFunction(const string& name) const {
    for (auto& fn : _functions) {
        if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
            return {fn, const_cast<FileNode*>(this)};
        }
    }
    for (auto* imp : _wildcardImports) {
        for (auto& fn : imp->_functions) {
            if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                return {fn, imp};
            }
        }
    }
    // 沿 parent scope 链搜索（含各 parent 的 wildcardImports）
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        for (auto& fn : pf->_functions) {
            if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                return {fn, pf};
            }
        }
        for (auto* imp : pf->_wildcardImports) {
            for (auto& fn : imp->_functions) {
                if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                    return {fn, imp};
                }
            }
        }
    }
    return {nullptr, nullptr};
}

// 收集所有同名泛型函数（支持多个泛型重载消歧，如 print<T>(x T) + print<T>(x T&)）
// 搜索范围：本地 + wildcardImports + parent scope 链（含各 parent 的 wildcardImports）
void FileNode::collectGenericFunctions(const string& name, vector<pair<FnNode*, FileNode*>>& out,
                                       FileNode* owner) const {
    for (auto& fn : _functions) {
        if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
            out.emplace_back(fn, owner);
        }
    }
    for (auto* imp : _wildcardImports) {
        if (imp == owner) continue; // 避免重复收集（调用方可能已用本地 owner 收集过该 imp）
        for (auto& fn : imp->_functions) {
            if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                out.emplace_back(fn, imp);
            }
        }
    }
    // 沿 parent scope 链搜索（含各 parent 的 wildcardImports）
    for (auto* pf = parentFileNode(this); pf; pf = parentFileNode(pf)) {
        if (pf == owner) continue;
        for (auto& fn : pf->_functions) {
            if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                out.emplace_back(fn, pf);
            }
        }
        for (auto* imp : pf->_wildcardImports) {
            if (imp == owner) continue;
            for (auto& fn : imp->_functions) {
                if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
                    out.emplace_back(fn, imp);
                }
            }
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
    return nullptr;
}

void FileNode::addPackageChild(const string& alias, const string& child, FileNode* file) {
    _packageChildren[alias][child] = file;
}

FileNode* FileNode::packageChild(const string& alias, const string& child) const {
    auto it = _packageChildren.find(alias);
    if (it == _packageChildren.end()) return nullptr;
    auto jt = it->second.find(child);
    if (jt == it->second.end()) return nullptr;
    return jt->second;
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
    throw YuxError(line, ErrorCode::E2008, alias, sources);
}

void FileNode::addWildcardImport(FileNode* file) {
    if (!file || file == this) return;
    for (auto* f : _wildcardImports)
        if (f == file) return;
    _wildcardImports.push_back(file);
}

FileNode* FileNode::getStructOwner(const string& name) {
    for (auto& decl : _structDecls) {
        if (decl->name().getText() == name) return this;
    }
    for (auto* imp : _wildcardImports) {
        if (imp->getStructDecl(name) || imp->getStructImpl(name)) return imp;
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
        if (jt != imp->_symbols.end()) {
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
        if (jt != imp->_fnSymbols.end() && !jt->second.empty()) {
            return &jt->second[0];
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
                if (matchFnParams(fnInfo, paramTypes)) return &fnInfo;
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
    auto hasSemanticDup = [&](FnSymbolInfo* target) -> bool {
        for (auto* existing : out) {
            if (existing->moduleName != target->moduleName) continue;
            if (existing->params.size() != target->params.size()) continue;
            bool same = true;
            for (size_t i = 0; i < target->params.size(); ++i) {
                if (!(existing->params[i] == target->params[i])) {
                    same = false;
                    break;
                }
            }
            if (same) return true;
        }
        return false;
    };

    // 1) 本地 _fnSymbols（含 visitImportDecl 注入的拷贝）
    auto it = _fnSymbols.find(name);
    if (it != _fnSymbols.end()) {
        for (auto& fn : it->second)
            addIfNew(&fn);
    }

    // 2) wildcardImports 的直接 _fnSymbols（仅浅层，避免递归回到自己）
    for (auto* imp : _wildcardImports) {
        auto jt = imp->_fnSymbols.find(name);
        if (jt != imp->_fnSymbols.end()) {
            for (auto& fn : jt->second)
                if (!hasSemanticDup(&fn)) addIfNew(&fn);
        }
    }

    // 3) parentScope 链（虚调用——若 parent 是 FileNode 则继续扩展 wildcardImports）
    if (_parentScope) {
        _parentScope->collectFnOverloads(name, out);
    }
}
