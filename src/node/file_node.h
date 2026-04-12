// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#ifndef YUX_LANG_FILE_NODE_H
#define YUX_LANG_FILE_NODE_H

#include "fn_node.h"
#include "node.h"
#include "struct_node.h"

class FileNode : public ScopeNode {
    vector<p<FnNode>> _functions;
    vector<p<StructDeclNode>> _structDecls;
    vector<p<StructImplNode>> _structImpls;
    string _moduleName;

public:
    explicit FileNode(string moduleName = "");
    
    void addFunction(const p<FnNode>& function);
    void addStructDecl(const p<StructDeclNode>& structDecl);
    void addStructImpl(const p<StructImplNode>& structImpl);

    const vector<p<FnNode>>& getFunctions() const;
    const vector<p<StructDeclNode>>& getStructDecls() const { return _structDecls; }
    const vector<p<StructImplNode>>& getStructImpls() const { return _structImpls; }
    
    StructDeclNode* getStructDecl(const string& name) const;
    StructImplNode* getStructImpl(const string& name) const;
    
    void setModuleName(const string& name) { _moduleName = name; }
    const string& moduleName() const { return _moduleName; }
    
    string getMangledName(const string& symbolName) const;
    string getMangledName(const string& symbolName, const vector<TypeInfo>& paramTypes) const;
};

#endif //YUX_LANG_FILE_NODE_H
