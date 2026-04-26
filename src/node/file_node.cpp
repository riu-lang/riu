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

StructDeclNode* FileNode::getStructDecl(const string& name) const {
    // `#CompilerInner` 声明仅作语言层占位（如 Box/Ref/Ptr/Array 及 i8..f64），
    // 它们的布局与方法由编译器合成，对用户结构体逻辑不可见。
    for (auto& decl : _structDecls) {
        if (decl->name().getText() == name && !decl->hasAnno("CompilerInner")) {
            return decl;
        }
    }
    for (auto* imp : _wildcardImports) {
        for (auto& decl : imp->_structDecls) {
            if (decl->name().getText() == name && !decl->hasAnno("CompilerInner")) return decl;
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
    string msg = "`" + alias + "` is ambiguous, matched";
    if (it != _wildcardAliasSources.end()) {
        for (size_t i = 0; i < it->second.size(); ++i) {
            msg += (i == 0 ? " " : " and ");
            msg += it->second[i];
        }
    }
    throw YuxError(msg, line);
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
