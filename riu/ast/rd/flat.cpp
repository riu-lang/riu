// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast/rd/flat.h"

#include <array>

namespace rd {
namespace {

const Node kEmptyNodeValue{};

constexpr std::array kNodeKindNames{
#define RD_NODE_KIND_NAME(name) std::string_view{#name},
    RD_NODE_KIND_LIST(RD_NODE_KIND_NAME)
#undef RD_NODE_KIND_NAME
};

} // namespace

std::string_view nodeKindName(NodeKind k) {
    const auto i = static_cast<std::size_t>(k);
    if (i >= kNodeKindNames.size()) return "Empty";
    return kNodeKindNames[i];
}

NodeId FlatAst::addNode(Node n) {
    const auto id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(n);
    return id;
}

i32 FlatAst::beginChildren() {
    return static_cast<i32>(children_.size());
}

void FlatAst::addChild(NodeId id) {
    children_.push_back(id);
}

NodeId FlatAst::addWithKids(NodeKind kind, Pos pos, std::string_view value, const NodeId* kids, i32 n, Kind op) {
    const i32 start = beginChildren();
    for (i32 i = 0; i < n; ++i)
        addChild(kids[i]);
    Node node;
    node.kind = kind;
    node.op = op;
    node.pos = pos;
    node.value = value;
    node.children_start = start;
    node.children_count = n;
    return addNode(node);
}

NodeId FlatAst::add(NodeKind kind, Pos pos, std::string_view value, std::initializer_list<NodeId> kids, Kind op) {
    return addWithKids(kind, pos, value, kids.begin(), static_cast<i32>(kids.size()), op);
}

NodeId FlatAst::add(NodeKind kind, Pos pos, std::string_view value, const std::vector<NodeId>& kids, Kind op) {
    return addWithKids(kind, pos, value, kids.data(), static_cast<i32>(kids.size()), op);
}

NodeId FlatAst::child(NodeId parent, i32 index) const {
    const Node& n = at(parent);
    if (index < 0 || index >= n.children_count) return kEmptyNode;
    const i32 slot = n.children_start + index;
    if (slot < 0 || static_cast<size_t>(slot) >= children_.size()) return kEmptyNode;
    return children_[static_cast<size_t>(slot)];
}

const Node& FlatAst::at(NodeId id) const {
    if (id < 0 || static_cast<size_t>(id) >= nodes_.size()) return kEmptyNodeValue;
    return nodes_[static_cast<size_t>(id)];
}

FlatAst emptyProgram() {
    FlatAst ast;
    ast.setRoot(ast.add(NodeKind::Program));
    return ast;
}

} // namespace rd
