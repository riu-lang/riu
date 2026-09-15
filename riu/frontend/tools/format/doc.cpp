// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Doc IR 构造工具实现

#include "tools/format/doc.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace riu::format {

Doc text(std::string s) {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::Text;
    node->text = std::move(s);
    return node;
}

Doc concat(std::vector<Doc> parts) {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::Concat;
    node->children = std::move(parts);
    return node;
}

Doc indent(int n, Doc child) {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::Indent;
    node->indent = n;
    node->children.push_back(std::move(child));
    return node;
}

Doc group(Doc child) {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::Group;
    node->children.push_back(std::move(child));
    return node;
}

Doc line(std::string flatText) {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::Line;
    node->text = std::move(flatText);
    return node;
}

Doc hardline() {
    auto node = std::make_shared<DocNode>();
    node->kind = DocKind::HardLine;
    return node;
}

} // namespace riu::format
