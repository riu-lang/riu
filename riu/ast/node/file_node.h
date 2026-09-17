// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_FILE_NODE_H
#define RIU_LANG_FILE_NODE_H

#include "alias_node.h"
#include "enum_node.h"
#include "fn_node.h"
#include "global_const_node.h"
#include "global_var_node.h"
#include "node.h"
#include "spec_node.h"
#include "struct_node.h"

class Riu;

class FileNode : public ScopeNode {
    vector<FnNode*> _functions;
    vector<StructDeclNode*> _structDecls;
    vector<StructImplNode*> _structImpls;
    // 与 _enumMap / _aliasMap 同：按名索引。Sema 每个类型注解都会 lookupStruct，
    // 线性扫 _structDecls / _functions 在大文件上是平方。
    map<string, StructDeclNode*> _structMap;
    map<string, StructImplNode*> _implMap;
    map<string, vector<FnNode*>> _fnsByName;
    vector<GlobalConstNode*> _globalConsts;
    vector<GlobalVarNode*> _globalVars; // DRAFT-static-vars Phase 1: 运行期初始化全局变量
    vector<SpecDeclNode*> _specDecls;
    vector<AliasDeclNode*> _aliasDecls;
    map<string, AliasDeclNode*> _aliasMap;
    vector<EnumDeclNode*> _enumDecls;
    map<string, EnumDeclNode*> _enumMap;
    string _moduleName;
    string _sourcePath; // 该 FileNode 对应的源文件绝对路径；空壳 SDK 父作用域可空
    bool _fromDecl = false;
    Riu* _riu = nullptr; // 非持有；类型路径的包边界检查使用所属 Riu。

public:
    explicit FileNode(string moduleName = "");
    void setRiu(Riu* riu) { _riu = riu; }
    [[nodiscard]] Riu* riu() const { return _riu; }

    // 覆写：搜索范围扩展到 wildcardImports（与 getStructDecl/getEnumDecl/getAliasDecl 一致）
    SymbolInfo* lookupSymbol(const string& name) override;
    FnSymbolInfo* lookupFnSymbol(const string& name) override;
    FnSymbolInfo* lookupFnSymbolWithParams(const string& name, const vector<TypeInfo>& paramTypes) override;
    void collectFnOverloads(const string& name, vector<FnSymbolInfo*>& out) override;

    void addFunction(FnNode* function);
    void addStructDecl(StructDeclNode* structDecl);
    void addStructImpl(StructImplNode* structImpl);
    void addGlobalConst(GlobalConstNode* globalConst);
    void addGlobalVar(GlobalVarNode* globalVar); // DRAFT-static-vars Phase 1
    void addSpecDecl(SpecDeclNode* specDecl);
    void addAliasDecl(AliasDeclNode* aliasDecl);
    void addEnumDecl(EnumDeclNode* enumDecl);

    [[nodiscard]] const vector<FnNode*>& getFunctions() const;
    [[nodiscard]] const vector<StructDeclNode*>& getStructDecls() const { return _structDecls; }
    [[nodiscard]] const vector<StructImplNode*>& getStructImpls() const { return _structImpls; }
    [[nodiscard]] const vector<GlobalConstNode*>& getGlobalConsts() const { return _globalConsts; }
    [[nodiscard]] const vector<GlobalVarNode*>& getGlobalVars() const {
        return _globalVars;
    } // DRAFT-static-vars Phase 1
    [[nodiscard]] const vector<SpecDeclNode*>& getSpecDecls() const { return _specDecls; }
    [[nodiscard]] const vector<AliasDeclNode*>& getAliasDecls() const { return _aliasDecls; }
    [[nodiscard]] const vector<EnumDeclNode*>& getEnumDecls() const { return _enumDecls; }
    [[nodiscard]] SpecDeclNode* getSpecDecl(const string& name) const;
    [[nodiscard]] AliasDeclNode* getAliasDecl(const string& name) const;
    [[nodiscard]] EnumDeclNode* getEnumDecl(const string& name) const;

    // includeBuiltin: 是否把 `#Builtin` 占位 (Rc/Ref/Ptr/Array 等内建容器
    // + i8..f64 等基本类型) 也算上。默认 false —— Compiler 端只关心用户结构体, 内建
    // 占位由编译器合成不需要走 decl 查找。SemaPass 做泛型 arity 校验 (E6011) 等仅需
    // 看到声明形态的场景要显式传 true, 否则 Rc<T> 查不到, arity 校验静默漏报。
    [[nodiscard]] StructDeclNode* getStructDecl(const string& name, bool includeBuiltin = false) const;
    // 仅本文件声明（不含 wildcard）。resolveTypePath 分层查找用。
    [[nodiscard]] StructDeclNode* localStructDecl(const string& name, bool includeBuiltin = false) const;
    [[nodiscard]] EnumDeclNode* localEnumDecl(const string& name) const;
    [[nodiscard]] AliasDeclNode* localAliasDecl(const string& name) const;
    [[nodiscard]] StructImplNode* getStructImpl(const string& name) const;
    // 仅本文件 impl（不含 wildcard）。ownerModule 消歧用。
    [[nodiscard]] StructImplNode* localStructImpl(const string& name) const;
    [[nodiscard]] FnNode* getFunction(const string& name) const;
    // 声明齐备后用 header.getType() 回填 fn/method 符号的 owner（预登记时 enum/struct 可能还没进表）
    void syncFnSymbolsFromAst();
    // 同 getFunction，同时返回所属 FileNode；搜索范围扩展到 wildcardImports
    [[nodiscard]] pair<FnNode*, FileNode*> getFunctionWithOwner(const string& name) const;
    // 仅返回 generic 重载（用于 dispatcher：与 lookupFnSymbolWithParams 命中的非泛型重载竞争优先级时用到）
    // 搜索范围：本地 _functions + wildcardImports
    [[nodiscard]] pair<FnNode*, FileNode*> getGenericFunction(const string& name) const;
    // 收集所有同名泛型函数（支持多个泛型重载消歧，如 print<T>(x T) + print<T>(x T&)）
    // 搜索范围：本地 _functions + wildcardImports
    void collectGenericFunctions(const string& name, vector<pair<FnNode*, FileNode*>>& out, FileNode* owner) const;

    void setModuleName(const string& name) { _moduleName = name; }
    [[nodiscard]] const string& moduleName() const { return _moduleName; }

    void setSourcePath(string p) { _sourcePath = std::move(p); }
    [[nodiscard]] const string& sourcePath() const { return _sourcePath; }

    // .ud 重建的接口树：无非泛型体，不能拿去 codegen；obj 过期时需 loadMainFile 重 parse
    void setFromDecl(bool v) { _fromDecl = v; }
    [[nodiscard]] bool isFromDecl() const { return _fromDecl; }

    // 隐式/显式导入的模块名列表。riu 模块默认导入。
    // 预留扩展点以便后续支持 import/use 语法。
    void addImport(const string& mod);
    [[nodiscard]] const vector<string>& imports() const { return _imports; }

    // `use` 指令。wildcard=true 表示 `use a.b.*`（扁平 + 末段别名）。
    // wildcard=false：`use a.b` 路径前缀，或 `use a.b.MyType` 具名类型（L2）。
    struct UseSpec {
        string moduleName;     // 点分，如 "riu.net"
        string alias;          // 最后一段；wildcard 时也填末段
        bool wildcard = false; // 是否 `.*`
        int line = 0;          // 源码行号，用于报错
    };
    void addUseSpec(UseSpec spec);
    [[nodiscard]] const vector<UseSpec>& useSpecs() const { return _useSpecs; }

    // 通配 `use a.b.*` 导入的源 FileNode。结构体/方法查找会回退到这里。
    void addWildcardImport(FileNode* file);
    [[nodiscard]] const vector<FileNode*>& wildcardImports() const { return _wildcardImports; }

    // `use a.b.c` 引入的模块别名 → 目标 FileNode。用于类型推断 / 调用分发。
    void addModuleAlias(const string& alias, FileNode* file);
    [[nodiscard]] FileNode* moduleAlias(const string& alias) const;

    // `use a.b` 中 a.b 是目录 → 包别名 `b` 指向点分路径 "a.b"，并记录子 .ut
    // （键可为 `child` 或 `sub.inner`）。查找沿 parent scope（SDK 上的 `riu` 包）。
    void addPackageAlias(const string& alias, const string& dottedPath);
    [[nodiscard]] const string* packageAlias(const string& alias) const;
    void addPackageChild(const string& alias, const string& child, FileNode* file);
    [[nodiscard]] FileNode* packageChild(const string& alias, const string& child) const;

    // 通配导入注入的别名来源追踪。同名别名被多个 `use X.*` 注入时，两个源模块
    // 都会被记录；查找时再判定为歧义。
    void addWildcardAliasSource(const string& alias, const string& sourceModule);
    [[nodiscard]] const vector<string>* wildcardAliasSources(const string& alias) const;
    [[nodiscard]] bool isAmbiguousAlias(const string& alias) const;
    // 查找点若发现 alias 歧义，调用此方法抛出带候选列表的错误。
    [[noreturn]] void throwAmbiguousAlias(const string& alias, int line) const;

    // `use a.b.MyType`：把公开类型注入裸名 L2。同名可有多个来源（使用点 E5015）。
    void addNamedTypeImport(const string& name, FileNode* owner);
    [[nodiscard]] const vector<FileNode*>* namedTypeImports(const string& name) const;

    // 给定结构体名，返回其所属的 FileNode；本地优先，其次具名导入，再 wildcardImports。
    // 未找到返回 nullptr。
    FileNode* getStructOwner(const string& name);

    // 已关联模块：本文件 / 通配导入 / 模块别名 / 包孩子 / 父作用域（SDK）。
    // 不 loadModule。fileForOwner 用：`use geom` 的 geom.Point 要能找到 geom。
    [[nodiscard]] FileNode* relatedFile(const string& moduleName) const;

private:
    [[nodiscard]] FileNode* relatedFileHere(const string& moduleName) const;

    vector<string> _imports;
    vector<UseSpec> _useSpecs;
    vector<FileNode*> _wildcardImports;
    map<string, FileNode*> _moduleAliases;
    map<string, string> _packageAliases;                  // alias → dotted path
    map<string, map<string, FileNode*>> _packageChildren; // alias → { child file name → FileNode }
    map<string, vector<string>> _wildcardAliasSources;    // alias → 注入过该别名的源模块点分路径列表
    map<string, vector<FileNode*>> _namedTypeImports;     // 裸名 → `use path.Name` 的声明模块
};

#endif // RIU_LANG_FILE_NODE_H
