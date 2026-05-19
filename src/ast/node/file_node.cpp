// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "file_node.h"

FileNode::FileNode(string moduleName) : ScopeNode(nullptr), _moduleName(std::move(moduleName)) {
    // 注册基本类型为 struct 占位符，并预声明方法符号
    const initializer_list<string> TYPES = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    const initializer_list<string> INT_TYPES = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"};
    const initializer_list<string> FLOAT_TYPES = {"f32", "f64"};
    
    for (auto t : TYPES) {
        registerSymbol(t, {SymbolKind::Struct, t, TypeInfo(t)});

        // 类型转换方法
        for (auto f : TYPES) {
            string fnName = "to_" + f;
            string fullName = t + "." + fnName;
            registerSymbol(fullName, {SymbolKind::Function, fnName, TypeInfo(f)});
            registerFnSymbol(fullName, {fnName, "", {}, TypeInfo(f)});
        }
    }
    
    // 整数类型运算符方法
    for (auto t : INT_TYPES) {
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
    for (auto t : FLOAT_TYPES) {
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
}

void FileNode::addFunction(const p<FnNode>& function) {
    _functions.push_back(function);
}

void FileNode::addStructDecl(const p<StructDeclNode>& structDecl) {
    _structDecls.push_back(structDecl);
    registerSymbol(structDecl->name().getText(), {SymbolKind::Struct, structDecl->name().getText(), TypeInfo(structDecl->name().getText())});
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

const vector<p<FnNode>>& FileNode::getFunctions() const {
    return _functions;
}

StructDeclNode* FileNode::getStructDecl(const string& name, bool includeCompilerInner) const {
    // `#CompilerInner` 声明仅作语言层占位（如 Rc/Ref/Ptr/Array 及 i8..f64），
    // 它们的布局与方法由编译器合成，对用户结构体逻辑不可见。默认过滤掉它们 ——
    // Compiler 端用户结构体查找不应命中。SemaPass 走 arity / 形态校验时需要看到
    // 这些占位 (否则 Rc/Ref 查不到), 显式传 includeCompilerInner=true。
    auto matches = [&](const p<StructDeclNode>& decl) {
        if (decl->name().getText() != name) return false;
        return includeCompilerInner || !decl->hasAnno("CompilerInner");
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

// 仅返回 generic 重载：用于 dispatcher 让非泛型 fnSymbol 优先于同名泛型函数
// （例：`assert_eq:<T>` 与 `assert_eq(String&, String&)` 共存时）
FnNode* FileNode::getGenericFunction(const string& name) const {
    for (auto& fn : _functions) {
        if (fn->header()->name().getText() == name && fn->header()->isGeneric()) {
            return fn;
        }
    }
    return nullptr;
}

void FileNode::addImport(const string& mod) {
    if (mod.empty() || mod == _moduleName) return;
    for (auto& m : _imports) if (m == mod) return;
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
    for (auto* f : _wildcardImports) if (f == file) return;
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
