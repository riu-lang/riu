// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#include "file_node.h"

FileNode::FileNode(string moduleName) : ScopeNode(nullptr), _moduleName(std::move(moduleName)) {
    const initializer_list<string> TYPES = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    for (auto t : TYPES) {
        registerSymbol(t, {SymbolKind::Struct, t, TypeInfo(t)});
        
        for (auto f : TYPES) {
            string fnName = "to_" + f;
            string fullName = t + "." + fnName;
            registerSymbol(fullName, {SymbolKind::Function, fnName, TypeInfo(f)});
            registerFnSymbol(fullName, {fnName, "", {}, TypeInfo(f)});
            _innerFnNames.insert(fullName);
        }
    }
    
    registerSymbol("print", {SymbolKind::Function, "print", TypeInfo()});
    registerSymbol("println", {SymbolKind::Function, "println", TypeInfo()});
    registerFnSymbol("print", {"print", "", {}, TypeInfo()});
    registerFnSymbol("println", {"println", "", {}, TypeInfo()});
    _innerFnNames.insert("print");
    _innerFnNames.insert("println");
}

void FileNode::addFunction(const p<FnNode>& function) {
    _functions.push_back(function);
}

void FileNode::addStructDecl(const p<StructDeclNode>& structDecl) {
    _structDecls.push_back(structDecl);
    registerSymbol(structDecl->name()->getText(), {SymbolKind::Struct, structDecl->name()->getText(), TypeInfo(structDecl->name()->getText())});
}

void FileNode::addStructImpl(const p<StructImplNode>& structImpl) {
    _structImpls.push_back(structImpl);
}

const vector<p<FnNode>>& FileNode::getFunctions() const {
    return _functions;
}

StructDeclNode* FileNode::getStructDecl(const string& name) const {
    for (auto& decl : _structDecls) {
        if (decl->name()->getText() == name) {
            return decl;
        }
    }
    return nullptr;
}

string FileNode::getMangledName(const string& symbolName) const {
    if (_moduleName.empty()) {
        return symbolName;
    }
    return _moduleName + "_" + symbolName;
}
