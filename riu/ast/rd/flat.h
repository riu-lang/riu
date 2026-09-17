// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0
//
// 结构参考 V flat（MIT）。NodeKind 按 riuParser.g4 构造，不是 V 的 key_go / payload 表。
// rd.8 起 FileNode 主路走本栈；format / LSP / skeleton 仍 ANTLR。

#ifndef RIU_LANG_RD_FLAT_H
#define RIU_LANG_RD_FLAT_H

#include "ast/rd/token.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace rd {

using NodeId = i32;

constexpr NodeId kEmptyNode = -1;

// g4 构造名 → NodeKind。dump 印枚举名。
#define RD_NODE_KIND_LIST(X)                                                                                           \
    X(Empty)                                                                                                           \
    X(Program)                                                                                                         \
    X(Use)                                                                                                             \
    X(Extern)                                                                                                          \
    X(Fn)                                                                                                              \
    X(FnClean)                                                                                                         \
    X(Param)                                                                                                           \
    X(Anno)                                                                                                            \
    X(Struct)                                                                                                          \
    X(Field)                                                                                                           \
    X(Enum)                                                                                                            \
    X(EnumVariant)                                                                                                     \
    X(Alias)                                                                                                           \
    X(Let)                                                                                                             \
    X(LetTuple)                                                                                                        \
    X(Generic)                                                                                                         \
    X(TypePath)                                                                                                        \
    X(TypeGeneric)                                                                                                     \
    X(TypeNullable)                                                                                                    \
    X(TypeFallible)                                                                                                    \
    X(TypeSelf)                                                                                                        \
    X(TypeArray)                                                                                                       \
    X(TypeUnit)                                                                                                        \
    X(TypeTuple)                                                                                                       \
    X(Ident)                                                                                                           \
    X(This)                                                                                                            \
    X(IntLit)                                                                                                          \
    X(FloatLit)                                                                                                        \
    X(BoolLit)                                                                                                         \
    X(StringLit)                                                                                                       \
    X(StringInterp)                                                                                                    \
    X(TplText)                                                                                                         \
    X(TplDollar)                                                                                                       \
    X(TplInterp)                                                                                                       \
    X(CodePoint)                                                                                                       \
    X(NullLit)                                                                                                         \
    X(UnitLit)                                                                                                         \
    X(Paren)                                                                                                           \
    X(Infix)                                                                                                           \
    X(Prefix)                                                                                                          \
    X(Call)                                                                                                            \
    X(EnumCtor)                                                                                                        \
    X(Dot)                                                                                                             \
    X(TupleMember)                                                                                                     \
    X(Get)                                                                                                             \
    X(GetRef)                                                                                                          \
    X(If)                                                                                                              \
    X(IfLine)                                                                                                          \
    X(Elif)                                                                                                            \
    X(Match)                                                                                                           \
    X(MatchArm)                                                                                                        \
    X(Pattern)                                                                                                         \
    X(TryCatch)                                                                                                        \
    X(Catch)                                                                                                           \
    X(NullElse)                                                                                                        \
    X(MoveAssign)                                                                                                      \
    X(Lambda)                                                                                                          \
    X(StructLit)                                                                                                       \
    X(FieldInit)                                                                                                       \
    X(Array)                                                                                                           \
    X(ArrayInit)                                                                                                       \
    X(Tuple)                                                                                                           \
    X(Block)                                                                                                           \
    X(ExprStmt)                                                                                                        \
    X(Assign)                                                                                                          \
    X(Set)                                                                                                             \
    X(StaticFieldSet)                                                                                                  \
    X(Ret)                                                                                                             \
    X(RetVoid)                                                                                                         \
    X(Loop)                                                                                                            \
    X(ForIn)                                                                                                           \
    X(Break)                                                                                                           \
    X(Continue)

enum class NodeKind : std::uint8_t {
#define RD_NODE_KIND_ENUM(name) name,
    RD_NODE_KIND_LIST(RD_NODE_KIND_ENUM)
#undef RD_NODE_KIND_ENUM
};

struct Node {
    NodeKind kind = NodeKind::Empty;
    Kind op = Kind::Invalid; // Infix / Prefix / Assign；无则 Invalid
    Pos pos;
    std::string_view value; // 指向源或字面量文本；可空
    i32 children_start = 0;
    i32 children_count = 0;
};

class FlatAst {
public:
    [[nodiscard]] NodeId addNode(Node n);
    [[nodiscard]] i32 beginChildren();
    void addChild(NodeId id);

    // 先挂已有子节点 id，再写入父节点。
    [[nodiscard]] NodeId add(NodeKind kind, Pos pos = {}, std::string_view value = {},
                             std::initializer_list<NodeId> kids = {}, Kind op = Kind::Invalid);
    [[nodiscard]] NodeId add(NodeKind kind, Pos pos, std::string_view value, const std::vector<NodeId>& kids,
                             Kind op = Kind::Invalid);

    [[nodiscard]] NodeId child(NodeId parent, i32 index) const;
    [[nodiscard]] const Node& at(NodeId id) const;
    void setOp(NodeId id, Kind op);
    void setValue(NodeId id, std::string_view value);
    [[nodiscard]] NodeId root() const { return root_; }
    void setRoot(NodeId id) { root_ = id; }

private:
    [[nodiscard]] NodeId addWithKids(NodeKind kind, Pos pos, std::string_view value, const NodeId* kids, i32 n,
                                     Kind op);

    std::vector<Node> nodes_;
    std::vector<NodeId> children_;
    NodeId root_ = kEmptyNode;
};

[[nodiscard]] std::string_view nodeKindName(NodeKind k);

// 无子节点的 Program（空文件 / 仅空行）。
[[nodiscard]] FlatAst emptyProgram();

// 缩进树。空 root 输出空串。
[[nodiscard]] std::string dumpTree(const FlatAst& ast);

} // namespace rd

#endif // RIU_LANG_RD_FLAT_H
