// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_LITERAL_NODE_H
#define YUX_LANG_LITERAL_NODE_H

#include "node.h"

#include <utility>

class ExprNode;

class LiteralNode : public Node, public Typed {
protected:
    Token _value;

public:
    explicit LiteralNode(const Token& value);

    [[nodiscard]] Token getValue() const;

    [[nodiscard]] string getLocation() const override;
};

class LiteralNumberNode : public LiteralNode {
public:
    explicit LiteralNumberNode(const Token& value);
};

class LiteralIntNode : public LiteralNumberNode {
protected:
    TypeInfo _type;
    bool _hasSuffix = false;
    // 外层 ExprUnaryNode::Neg 次数为奇数。token 不含 '-'，i32/i64 最小值
    // 的绝对值超出正范围，范围检查与 codegen 必须看此标记。
    bool _unaryNegated = false;

public:
    explicit LiteralIntNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] bool hasSuffix() const { return _hasSuffix; }
    void setType(TypeInfo t) { _type = std::move(t); }
    void setUnaryNegated(bool v) { _unaryNegated = v; }
    [[nodiscard]] bool isUnaryNegated() const { return _unaryNegated; }
};

class LiteralFloatNode : public LiteralNumberNode {
protected:
    TypeInfo _type;
    bool _hasSuffix = false;

public:
    explicit LiteralFloatNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] bool hasSuffix() const { return _hasSuffix; }
    void setType(TypeInfo t) { _type = std::move(t); }
};

class LiteralBoolNode : public LiteralNode {
public:
    explicit LiteralBoolNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
};

class LiteralObjNode : public LiteralNode {
public:
    explicit LiteralObjNode(Node* parent, const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] string getLocation() const override;
};

class LiteralNullNode : public LiteralNode {
protected:
    TypeInfo _type; // 推断的目标类型（初始为空，此时 getType() 返回 Ptr）
public:
    explicit LiteralNullNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    void setType(TypeInfo t) { _type = std::move(t); }
    [[nodiscard]] bool hasInferredType() const { return !_type.empty(); }
};

class LiteralCodePointNode : public LiteralNode {
    u32 _codePoint = 0;

public:
    explicit LiteralCodePointNode(const Token& value);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] u32 codePoint() const { return _codePoint; }
};

class LiteralStringNode : public LiteralNode {
    vector<u32> _codePoints;

public:
    explicit LiteralStringNode(const Token& value, bool raw = false);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] const vector<u32>& codePoints() const { return _codePoints; }
};

// 字符串模板（Kotlin 风 "$x" / "${expr}"）。
// 不变量：parts.size() == interps.size() + 1；交替序列为
// parts[0], interps[0], parts[1], interps[1], ..., parts[N]。
// parts 中存储的是已解码的 UTF-8 文本片段（单个或多个 \\... 转义已展开）。
// 空模板 / 无插值在 ast_builder 处直接降级为 LiteralStringNode，故 interps 至少 1 个。
// TODO: codegen 在 Phase 2 lower 为 StringBuilder 链式 append。
class StringTemplateNode : public LiteralNode {
    vector<string> _parts;
    vector<ExprNode*> _interps;

public:
    StringTemplateNode(const Token& openTok, vector<string> parts, vector<ExprNode*> interps);
    [[nodiscard]] TypeInfo getType() const override;
    [[nodiscard]] const vector<string>& parts() const { return _parts; }
    [[nodiscard]] const vector<ExprNode*>& interps() const { return _interps; }
};

#endif // YUX_LANG_LITERAL_NODE_H
