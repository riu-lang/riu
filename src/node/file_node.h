// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

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
    FnNode* getFunction(const string& name) const;
    
    void setModuleName(const string& name) { _moduleName = name; }
    const string& moduleName() const { return _moduleName; }

    // 隐式/显式导入的模块名列表。yux 模块默认导入。
    // 预留扩展点以便后续支持 import/use 语法。
    void addImport(const string& mod);
    const vector<string>& imports() const { return _imports; }

    // `use` 指令（源码显式导入）。wildcard=true 表示 `use a.b.*`，
    // 语义为"导入模块 a.b 的所有非私有成员"。wildcard=false 表示
    // `use a.b.c`，语义为"引入模块别名 c 指向 a.b.c"（暂未实现，见 BUGS.md）。
    struct UseSpec {
        string moduleName;          // 点分，如 "yux.net"
        string alias;               // 最后一段，如 "net"；wildcard 时未使用
        bool wildcard = false;      // 是否 `.*`
        int line = 0;               // 源码行号，用于报错
    };
    void addUseSpec(UseSpec spec);
    const vector<UseSpec>& useSpecs() const { return _useSpecs; }

    // 通配 `use a.b.*` 导入的源 FileNode。结构体/方法查找会回退到这里。
    void addWildcardImport(FileNode* file);
    const vector<FileNode*>& wildcardImports() const { return _wildcardImports; }

    // `use a.b.c` 引入的模块别名 → 目标 FileNode。用于类型推断 / 调用分发。
    void addModuleAlias(const string& alias, FileNode* file);
    FileNode* moduleAlias(const string& alias) const;

    // `use a.b` 中 a.b 是目录 → 包别名 `b` 指向点分路径 "a.b"，并记录直接 .yux 子项
    // 对应的 FileNode。支持 `b.child.fn()` 形式调用（单层子模块）。
    void addPackageAlias(const string& alias, const string& dottedPath);
    const string* packageAlias(const string& alias) const;
    void addPackageChild(const string& alias, const string& child, FileNode* file);
    FileNode* packageChild(const string& alias, const string& child) const;

    // 通配导入注入的别名来源追踪。同名别名被多个 `use X.*` 注入时，两个源模块
    // 都会被记录；查找时再判定为歧义。
    void addWildcardAliasSource(const string& alias, const string& sourceModule);
    const vector<string>* wildcardAliasSources(const string& alias) const;
    bool isAmbiguousAlias(const string& alias) const;
    // 查找点若发现 alias 歧义，调用此方法抛出带候选列表的错误。
    [[noreturn]] void throwAmbiguousAlias(const string& alias, int line) const;

    // 给定结构体名，返回其所属的 FileNode；本地优先，其次按 wildcardImports
    // 顺序查找。未找到返回 nullptr。
    FileNode* getStructOwner(const string& name);

private:
    vector<string> _imports;
    vector<UseSpec> _useSpecs;
    vector<FileNode*> _wildcardImports;
    map<string, FileNode*> _moduleAliases;
    map<string, string> _packageAliases;                    // alias → dotted path
    map<string, map<string, FileNode*>> _packageChildren;   // alias → { child file name → FileNode }
    map<string, vector<string>> _wildcardAliasSources;      // alias → 注入过该别名的源模块点分路径列表
};

#endif //YUX_LANG_FILE_NODE_H
