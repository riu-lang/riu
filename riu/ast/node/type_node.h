// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_TYPE_NODE_H
#define RIU_LANG_TYPE_NODE_H

#include "node.h"

#include <memory>
#include <optional>

class TypeNode : public Node {
protected:
    mutable const TypeInfo* _cachedType = nullptr;

    const TypeInfo& cacheType(TypeInfo t) const {
        _cachedType = &internTypeAt(this, std::move(t));
        return *_cachedType;
    }

public:
    explicit TypeNode(Node* parent) : Node(parent) {}

    [[nodiscard]] virtual const TypeInfo& getType() const = 0;
    [[nodiscard]] virtual bool isArray() const { return false; }

    // Sema 补齐尾部默认实参后写回 intern 槽；后续 getType() 带齐实参。
    void recacheType(TypeInfo t) const { cacheType(std::move(t)); }
};

// Self 类型字面量 (构造模型重构 Phase 2b)
// 出现于 structImpl 体内 (含 #Static fn / 实例 fn 的形参 / 返回 / 局部 / turbofish)
// 构造时由 ast_builder 把 enclosing StructImplNode 的 structName 直接灌入,
// 避免 getType 走 parent 链 (impl 方法 retType 父指针是 FileNode, 走不到 impl).
// 体外出现的 Self 由 sema 单独拒收 (Phase 2d 接入), 此时 structName 为空.
class TypeSelfNode : public TypeNode {
    Token _selfTok;
    string _structName;
    // 空 = 用 enclosingFile 的模块。spec 默认体 fall-through 时 spec 与 impl
    // 不在同一文件，必须改写成 impl 模块，否则 Self& 带上 spec 的 owner，
    // 与 impl 上登记的 `eq(other NEq&)` 对不上（T2 类型身份）。
    string _ownerModule;

public:
    TypeSelfNode(Node* parent, Token selfTok, string structName)
        : TypeNode(parent), _selfTok(selfTok), _structName(std::move(structName)) {}

    [[nodiscard]] const TypeInfo& getType() const override;

    [[nodiscard]] const Token& selfToken() const { return _selfTok; }
    [[nodiscard]] const string& structName() const { return _structName; }
    [[nodiscard]] const string& ownerModule() const { return _ownerModule; }

    // DRAFT-spec-default-body Phase 3：spec 默认体 fall-through 编译时, 把
    // spec 体内"无归属"的 TypeSelfNode 临时改写到具体实现类型, 编完再还原.
    // 不要在常规路径使用 — 仅供 compiler 的 fall-through 临时 patch.
    void setStructName(string s) {
        _structName = std::move(s);
        _cachedType = nullptr;
    }
    void setOwnerModule(string o) {
        _ownerModule = std::move(o);
        _cachedType = nullptr;
    }
};

class TypeNormalNode : public TypeNode {
    TypePath _path;

public:
    TypeNormalNode(Node* parent, Token typeName) : TypeNormalNode(parent, TypePath(typeName)) {}
    TypeNormalNode(Node* parent, TypePath path) : TypeNode(parent), _path(std::move(path)) {}

    [[nodiscard]] const TypeInfo& getType() const override;

    [[nodiscard]] Token typeNameToken() const { return _path.last(); }
    [[nodiscard]] const TypePath& path() const { return _path; }
};

class TypeArrayNode : public TypeNode {
    TypeNode* _elementType;
    Token _count;

public:
    TypeArrayNode(Node* parent, TypeNode* elementType, Token count)
        : TypeNode(parent), _elementType(elementType), _count(count) {}

    [[nodiscard]] const TypeInfo& getType() const override {
        if (_cachedType) return *_cachedType;
        u64 size = stoull(_count.getText());
        return cacheType({internTypeSpAt(this, _elementType->getType()), size});
    }

    [[nodiscard]] TypeNode* elementType() const { return _elementType; }

    [[nodiscard]] Token count() const { return _count; }

    [[nodiscard]] bool isArray() const override { return true; }
};

class TypeGenericNode : public TypeNode {
    TypePath _path;
    vector<TypeNode*> _typeArgs;

public:
    TypeGenericNode(Node* parent, Token baseName, vector<TypeNode*> typeArgs)
        : TypeGenericNode(parent, TypePath(baseName), std::move(typeArgs)) {}
    TypeGenericNode(Node* parent, TypePath path, vector<TypeNode*> typeArgs)
        : TypeNode(parent), _path(std::move(path)), _typeArgs(std::move(typeArgs)) {}

    [[nodiscard]] const TypeInfo& getType() const override;

    [[nodiscard]] Token baseName() const { return _path.last(); }
    [[nodiscard]] const TypePath& path() const { return _path; }

    [[nodiscard]] const vector<TypeNode*>& typeArgs() const { return _typeArgs; }
};

// 函数类型节点 Function<P..., Ret> / Function<...>?
// 形参类型列表 + 可选返回类型（unit 时为 nullptr）+ nullable 标志
// 由特殊泛型 Function<...> 解糖而来；参数名不参与判等
class TypeFnNode : public TypeNode {
    vector<TypeNode*> _paramTypes;
    TypeNode* _retType; // nullptr → unit
    bool _nullable;     // Function<...>?

public:
    TypeFnNode(Node* parent, vector<TypeNode*> paramTypes, TypeNode* retType, bool nullable)
        : TypeNode(parent), _paramTypes(std::move(paramTypes)), _retType(retType), _nullable(nullable) {}

    [[nodiscard]] const TypeInfo& getType() const override {
        if (_cachedType) return *_cachedType;
        vector<sp<TypeInfo>> params;
        params.reserve(_paramTypes.size());
        for (auto& pt : _paramTypes) {
            params.push_back(internTypeSpAt(this, pt->getType()));
        }
        sp<TypeInfo> ret = nullptr;
        if (_retType) ret = internTypeSpAt(this, _retType->getType());
        return cacheType(TypeInfo(FnTag{}, std::move(params), ret, _nullable));
    }

    [[nodiscard]] const vector<TypeNode*>& paramTypes() const { return _paramTypes; }
    [[nodiscard]] TypeNode* retType() const { return _retType; }
    [[nodiscard]] bool nullable() const { return _nullable; }
    void setNullable(bool v) {
        _nullable = v;
        _cachedType = nullptr;
    }
};

class TypeFallibleNode : public TypeNode {
    TypeNode* _base;
    TypeNode* _errType;

public:
    TypeFallibleNode(Node* parent, TypeNode* base, TypeNode* errType)
        : TypeNode(parent), _base(base), _errType(errType) {}

    [[nodiscard]] const TypeInfo& getType() const override {
        if (_cachedType) return *_cachedType;
        TypeInfo ti = _base->getType();
        ti.attachFallibleErr(fallibleErrKey(_errType->getType()));
        return cacheType(std::move(ti));
    }

    [[nodiscard]] TypeNode* baseType() const { return _base; }
    [[nodiscard]] TypeNode* errType() const { return _errType; }
};

// 元组类型节点 (T1, T2, ...)
class TypeTupleNode : public TypeNode {
    vector<TypeNode*> _elementTypes;

public:
    TypeTupleNode(Node* parent, vector<TypeNode*> elementTypes)
        : TypeNode(parent), _elementTypes(std::move(elementTypes)) {}

    [[nodiscard]] const TypeInfo& getType() const override {
        if (_cachedType) return *_cachedType;
        vector<sp<TypeInfo>> elems;
        elems.reserve(_elementTypes.size());
        for (auto& e : _elementTypes) {
            elems.push_back(internTypeSpAt(this, e->getType()));
        }
        return cacheType(TypeInfo(TupleTag{}, std::move(elems)));
    }

    [[nodiscard]] const vector<TypeNode*>& elementTypes() const { return _elementTypes; }
};

#endif // RIU_LANG_TYPE_NODE_H
