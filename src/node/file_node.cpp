// Copyright (c) 2026. Yin-Jinlong@github

#include "file_node.h"

FileNode::FileNode(string moduleName) : ScopeNode(nullptr), _moduleName(std::move(moduleName)) {
    // 注册基本类型为 struct 占位符，并预声明 to_<type>() 方法符号
    const initializer_list<string> TYPES = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    for (auto t : TYPES) {
        registerSymbol(t, {SymbolKind::Struct, t, TypeInfo(t)});

        for (auto f : TYPES) {
            string fnName = "to_" + f;
            string fullName = t + "." + fnName;
            registerSymbol(fullName, {SymbolKind::Function, fnName, TypeInfo(f)});
            registerFnSymbol(fullName, {fnName, "", {}, TypeInfo(f)});
        }
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
    for (auto& decl : _structDecls) {
        if (decl->name().getText() == name) {
            return decl;
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
