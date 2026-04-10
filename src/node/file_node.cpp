// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#include "file_node.h"

FileNode::FileNode() : ScopeNode(nullptr) {
    const initializer_list<string> TYPES = {"bool", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64"};
    for (auto t : TYPES) {
        registerSymbol(t, {SymbolKind::Struct, t, TypeInfo(t)});
        
        for (auto f : TYPES) {
            string fnName = "to_" + f;
            string fullName = t + "." + fnName;
            registerSymbol(fullName, {SymbolKind::Function, fnName, TypeInfo(f)});
            registerFnSymbol(fullName, {fnName, {}, TypeInfo(f)});
            _innerFnNames.insert(fullName);
        }
    }
    
    registerSymbol("print", {SymbolKind::Function, "print", TypeInfo()});
    registerSymbol("println", {SymbolKind::Function, "println", TypeInfo()});
    registerFnSymbol("print", {"print", {}, TypeInfo()});
    registerFnSymbol("println", {"println", {}, TypeInfo()});
    _innerFnNames.insert("print");
    _innerFnNames.insert("println");
}

void FileNode::addFunction(const p<FnNode>& function) {
    _functions.push_back(function);
}

const vector<p<FnNode>>& FileNode::getFunctions() const {
    return _functions;
}
