// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_NODE_H
#define YUX_LANG_TYPE_NODE_H

#include "node.h"

class TypeNode : public Node {
public:
    explicit TypeNode(const p<Node>& parent) : Node(parent) {}

    [[nodiscard]] virtual TypeInfo getType() const = 0;
    [[nodiscard]] virtual bool isArray() const { return false; }
};

// Self 类型字面量 (构造模型重构 Phase 2b)
// 出现于 structImpl 体内 (含 #Static fn / 实例 fn 的形参 / 返回 / 局部 / turbofish)
// 构造时由 ast_builder 把 enclosing StructImplNode 的 structName 直接灌入,
// 避免 getType 走 parent 链 (impl 方法 retType 父指针是 FileNode, 走不到 impl).
// 体外出现的 Self 由 sema 单独拒收 (Phase 2d 接入), 此时 structName 为空.
class TypeSelfNode : public TypeNode {
    Token _selfTok;
    string _structName;

public:
    TypeSelfNode(const p<Node>& parent, Token selfTok, string structName)
        : TypeNode(parent), _selfTok(std::move(selfTok)), _structName(std::move(structName)) {}

    [[nodiscard]] TypeInfo getType() const override {
        // structName 为空 (typically spec 体内): 返回名为 "Self" 的占位 TypeInfo,
        // 由 SpecImplChecker::sigEquivalent 通过 subst["Self"] 替换为 impl 具体类型;
        // 默认体 fall-through 编译时由 Compiler::compileInheritedDefaults 临时
        // setStructName 走真实 codegen.
        return _structName.empty() ? TypeInfo("Self") : TypeInfo(_structName);
    }

    [[nodiscard]] const Token& selfToken() const { return _selfTok; }
    [[nodiscard]] const string& structName() const { return _structName; }

    // DRAFT-spec-default-body Phase 3：spec 默认体 fall-through 编译时, 把
    // spec 体内"无归属"的 TypeSelfNode 临时改写到具体实现类型, 编完再还原.
    // 不要在常规路径使用 — 仅供 compiler 的 fall-through 临时 patch.
    void setStructName(string s) { _structName = std::move(s); }
};

class TypeNormalNode : public TypeNode {
    Token _typeName;

public:
    TypeNormalNode(const p<Node>& parent, Token typeName) : TypeNode(parent), _typeName(std::move(typeName)) {}

    [[nodiscard]] TypeInfo getType() const override { return TypeInfo(_typeName.getText()); }

    [[nodiscard]] Token typeNameToken() const { return _typeName; }
};

class TypeArrayNode : public TypeNode {
    p<TypeNode> _elementType;
    Token _count;

public:
    TypeArrayNode(const p<Node>& parent, p<TypeNode> elementType, Token count)
        : TypeNode(parent), _elementType(elementType), _count(std::move(count)) {}

    [[nodiscard]] TypeInfo getType() const override {
        auto elemTypeInfo = _elementType->getType();
        auto elemShared = make_shared<TypeInfo>(elemTypeInfo);
        u64 size = stoull(_count.getText());
        return {elemShared, size};
    }

    [[nodiscard]] p<TypeNode> elementType() const { return _elementType; }

    [[nodiscard]] Token count() const { return _count; }

    [[nodiscard]] bool isArray() const override { return true; }
};

class TypeGenericNode : public TypeNode {
    Token _baseName;
    vector<p<TypeNode>> _typeArgs;

public:
    TypeGenericNode(const p<Node>& parent, Token baseName, vector<p<TypeNode>> typeArgs)
        : TypeNode(parent), _baseName(std::move(baseName)), _typeArgs(std::move(typeArgs)) {}

    [[nodiscard]] TypeInfo getType() const override {
        vector<sp<TypeInfo>> args;
        args.reserve(_typeArgs.size());
        for (auto& typeArg : _typeArgs) {
            args.push_back(make_shared<TypeInfo>(typeArg->getType()));
        }
        return {_baseName.getText(), args};
    }

    [[nodiscard]] Token baseName() const { return _baseName; }

    [[nodiscard]] const vector<p<TypeNode>>& typeArgs() const { return _typeArgs; }
};

// 函数类型节点 Function<P..., Ret> / Function<...>?
// 形参类型列表 + 可选返回类型（unit 时为 nullptr）+ nullable 标志
// 由特殊泛型 Function<...> 解糖而来；参数名不参与判等
class TypeFnNode : public TypeNode {
    vector<p<TypeNode>> _paramTypes;
    p<TypeNode> _retType; // nullptr → unit
    bool _nullable;       // Function<...>?

public:
    TypeFnNode(const p<Node>& parent, vector<p<TypeNode>> paramTypes, p<TypeNode> retType, bool nullable)
        : TypeNode(parent), _paramTypes(std::move(paramTypes)), _retType(retType), _nullable(nullable) {}

    [[nodiscard]] TypeInfo getType() const override {
        vector<sp<TypeInfo>> params;
        params.reserve(_paramTypes.size());
        for (auto& pt : _paramTypes) {
            params.push_back(make_shared<TypeInfo>(pt->getType()));
        }
        sp<TypeInfo> ret = nullptr;
        if (_retType) ret = make_shared<TypeInfo>(_retType->getType());
        return TypeInfo(FnTag{}, std::move(params), ret, _nullable);
    }

    [[nodiscard]] const vector<p<TypeNode>>& paramTypes() const { return _paramTypes; }
    [[nodiscard]] p<TypeNode> retType() const { return _retType; }
    [[nodiscard]] bool nullable() const { return _nullable; }
    void setNullable(bool v) { _nullable = v; }
};

class TypeFallibleNode : public TypeNode {
    p<TypeNode> _base;
    p<TypeNode> _errType;

public:
    TypeFallibleNode(const p<Node>& parent, p<TypeNode> base, p<TypeNode> errType)
        : TypeNode(parent), _base(std::move(base)), _errType(std::move(errType)) {}

    [[nodiscard]] TypeInfo getType() const override {
        auto base = _base->getType();
        auto err = _errType->getType();
        TypeInfo ti = base;
        ti.name = base.getFullName() + "!" + err.name;
        return ti;
    }

    [[nodiscard]] p<TypeNode> baseType() const { return _base; }
    [[nodiscard]] p<TypeNode> errType() const { return _errType; }
};

// 元组类型节点 (T1, T2, ...)
class TypeTupleNode : public TypeNode {
    vector<p<TypeNode>> _elementTypes;

public:
    TypeTupleNode(const p<Node>& parent, vector<p<TypeNode>> elementTypes)
        : TypeNode(parent), _elementTypes(std::move(elementTypes)) {}

    [[nodiscard]] TypeInfo getType() const override {
        vector<sp<TypeInfo>> elems;
        elems.reserve(_elementTypes.size());
        for (auto& e : _elementTypes) {
            elems.push_back(make_shared<TypeInfo>(e->getType()));
        }
        return TypeInfo(TupleTag{}, std::move(elems));
    }

    [[nodiscard]] const vector<p<TypeNode>>& elementTypes() const { return _elementTypes; }
};

#endif // YUX_LANG_TYPE_NODE_H
