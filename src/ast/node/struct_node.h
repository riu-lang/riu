// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_STRUCT_NODE_H
#define YUX_LANG_STRUCT_NODE_H

#include "fn_node.h"
#include "node.h"
#include "spec_node.h"
#include "type_node.h"

class StructFieldNode : public Node {
    Token _name;
    p<TypeNode> _type;
    bool _isPrivate;
    // P1-4 DRAFT-const-mut §6.1：字段三档 (default var / #Val 浅 / #Frozen 深)。
    // 互斥；同时出现 → ast_builder 抛 E3105。
    bool _isVal = false;
    bool _isFrozen = false;
    // DRAFT-spec-reflect Phase 1: `#Static` 字段段 (type-bound 契约;
    // spec body 内允许, 普通 struct 内待 data-struct 全集启用).
    bool _isStatic = false;

public:
    StructFieldNode(const p<Node>& parent, const Token& name, p<TypeNode> type)
        : Node(parent), _name(name), _type(type) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    [[nodiscard]] Token name() const { return _name; }
    [[nodiscard]] p<TypeNode> type() const { return _type; }
    [[nodiscard]] TypeInfo getType() const { return _type->getType(); }
    [[nodiscard]] bool isPrivate() const { return _isPrivate; }

    void setVal(bool v) { _isVal = v; }
    void setFrozen(bool v) { _isFrozen = v; }
    void setStatic(bool v) { _isStatic = v; }
    [[nodiscard]] bool isVal() const { return _isVal; }
    [[nodiscard]] bool isFrozen() const { return _isFrozen; }
    [[nodiscard]] bool isStatic() const { return _isStatic; }
};

class StructDeclNode : public ScopeNode, public Named, public Annotated {
    vector<p<StructFieldNode>> _fields;
    map<string, size_t> _fieldIndices;
    vector<string> _typeParams;
    bool _isPrivate;

public:
    StructDeclNode(const p<Node>& parent, const Token& name) : ScopeNode(parent), Named(name) {
        _isPrivate = !name.getText().empty() && name.getText()[0] == '_';
    }

    void addField(p<StructFieldNode> field) {
        _fieldIndices[field->name().getText()] = _fields.size();
        _fields.push_back(field);
    }

    [[nodiscard]] const vector<p<StructFieldNode>>& fields() const { return _fields; }
    [[nodiscard]] int fieldIndex(const string& name) const {
        auto it = _fieldIndices.find(name);
        return it != _fieldIndices.end() ? static_cast<int>(it->second) : -1;
    }
    [[nodiscard]] const StructFieldNode* field(const string& name) const {
        int idx = fieldIndex(name);
        return idx >= 0 ? _fields[idx] : nullptr;
    }
    [[nodiscard]] bool isPrivate() const { return _isPrivate; }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }
};

class StructImplNode : public ScopeNode, public Named, public Annotated {
    vector<p<FnNode>> _methods;
    p<FnNode> _destructor = nullptr;
    vector<string> _typeParams;
    string _structName;

public:
    StructImplNode(const p<Node>& parent, const Token& structName)
        : ScopeNode(parent), Named(structName), _structName(structName.getText()) {}

    void addMethod(p<FnNode> method) { _methods.push_back(method); }

    void setDestructor(p<FnNode> destructor) { _destructor = destructor; }

    [[nodiscard]] const vector<p<FnNode>>& methods() const { return _methods; }
    [[nodiscard]] const p<FnNode>& destructor() const { return _destructor; }
    [[nodiscard]] bool hasDestructor() const { return _destructor != nullptr; }
    [[nodiscard]] const string& structName() const { return _structName; }

    void setTypeParams(vector<string> params) { _typeParams = std::move(params); }
    [[nodiscard]] const vector<string>& typeParams() const { return _typeParams; }
    [[nodiscard]] bool isGeneric() const { return !_typeParams.empty(); }

    // spec §12.2 draft 实现约束：`Type : D1 + D2 { ... }`。
    // v0.5 简化记录：只存 D 的解析名 + 类型实参（文本）+ 源位置。
    void setSpecRefs(vector<SpecRef> refs) { _specRefs = std::move(refs); }
    [[nodiscard]] const vector<SpecRef>& specRefs() const { return _specRefs; }

    // DRAFT-spec-default-body Phase 3: 该 impl 通过 spec 默认体 fall-through 拿到的方法
    // (实现者未显式写, spec 提供了默认体). 由 SpecImplChecker::validateImpl 登记, 由
    // Compiler::compileStructImpls 在常规方法编完后再编译 (用 spec 默认体 FnNode +
    // Self patch 到本 impl 的 structName).
    // 隐式拷贝/移动经 map<string,TypeInfo> 可能抛 std::bad_alloc; 仅在分配失败下触发,
    // 本进程 OOM 即 panic, 无 unwind 需求.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    struct InheritedDefault {
        SpecDeclNode* spec = nullptr;
        size_t sigIdx = 0;
        // spec 自身泛型形参 → impl 块给出的类型实参替换表 (与 sigEquivalent 同源).
        // v1 codegen 暂未消费 (Phase 5 base.yux 5-set 全部非泛型 spec), 留位备用.
        map<string, TypeInfo> subst;
    };
    void addInheritedDefault(InheritedDefault d) { _inheritedDefaults.push_back(std::move(d)); }
    void clearInheritedDefaults() { _inheritedDefaults.clear(); }
    [[nodiscard]] const vector<InheritedDefault>& inheritedDefaults() const { return _inheritedDefaults; }

    // DRAFT-spec-disambig-at: 为 `$.m@SpecA()` 形态 (即便 S 覆盖了 m, 也走 spec 默认体)
    // 预登记的"@-tagged" 方法发射点. 每个 (spec, sigIdx with default body) 一条;
    // emitMethodName = origName + "__at__" + specShortName, 与 fall-through 的同名 fnSymbol
    // 共存. 由 SpecImplChecker::validateImpl 登记; 由 Compiler::compileSpecDisambigEmits
    // 在常规方法 + fall-through 编完后逐条 emit.
    // NOLINTNEXTLINE(bugprone-exception-escape) — 同 InheritedDefault, 经 map / string 可能 bad_alloc.
    struct SpecDisambigEmit {
        SpecDeclNode* spec = nullptr;
        size_t sigIdx = 0;
        map<string, TypeInfo> subst;
        string emitMethodName;
    };
    void addSpecDisambigEmit(SpecDisambigEmit d) { _specDisambigEmits.push_back(std::move(d)); }
    void clearSpecDisambigEmits() { _specDisambigEmits.clear(); }
    [[nodiscard]] const vector<SpecDisambigEmit>& specDisambigEmits() const { return _specDisambigEmits; }

private:
    vector<SpecRef> _specRefs;
    vector<InheritedDefault> _inheritedDefaults;
    vector<SpecDisambigEmit> _specDisambigEmits;
};

#endif // YUX_LANG_STRUCT_NODE_H
