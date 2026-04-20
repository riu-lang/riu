// Copyright (c) 2026. Yin-Jinlong@github

//
// Created by yjl_1 on 2026/4/4.
//

#ifndef YUX_LANG_FILE_NODE_H
#define YUX_LANG_FILE_NODE_H

#include "fn_node.h"
#include "global_const_node.h"
#include "node.h"
#include "struct_node.h"

class FileNode : public ScopeNode {
    vector<p<FnNode>> _functions;
    vector<p<StructDeclNode>> _structDecls;
    vector<p<StructImplNode>> _structImpls;
    vector<p<GlobalConstNode>> _globalConsts;
    string _moduleName;

public:
    explicit FileNode(string moduleName = "");
    
    void addFunction(const p<FnNode>& function);
    void addStructDecl(const p<StructDeclNode>& structDecl);
    void addStructImpl(const p<StructImplNode>& structImpl);
    void addGlobalConst(const p<GlobalConstNode>& globalConst);

    const vector<p<FnNode>>& getFunctions() const;
    const vector<p<StructDeclNode>>& getStructDecls() const { return _structDecls; }
    const vector<p<StructImplNode>>& getStructImpls() const { return _structImpls; }
    const vector<p<GlobalConstNode>>& getGlobalConsts() const { return _globalConsts; }
    
    StructDeclNode* getStructDecl(const string& name) const;
    StructImplNode* getStructImpl(const string& name) const;
    
    void setModuleName(const string& name) { _moduleName = name; }
    const string& moduleName() const { return _moduleName; }

    // 隐式/显式导入的模块名列表。yux 模块默认导入。
    // 预留扩展点以便后续支持 import/use 语法。
    void addImport(const string& mod);
    const vector<string>& imports() const { return _imports; }

private:
    vector<string> _imports;
};

#endif //YUX_LANG_FILE_NODE_H
