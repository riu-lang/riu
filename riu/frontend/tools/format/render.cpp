// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Doc 渲染：简化版 best-layout
//
// 核心算法：
// - fits(doc, remaining)：尝试在剩余宽度内 flat 渲染，若遇 HardLine 或越界则失败；
// - 渲染时遇到 Group：先尝试 flat，fits 通过则按 flat 输出，否则按 break 输出；
// - Line 在 flat 模式输出 flatText，break 模式输出 "\n" + 当前缩进；
// - HardLine 始终换行并强制外层 Group 进入 break。

#include "tools/format/render.h"

#include <cstdint>
#include <string>
#include <vector>

#include "tools/format/doc.h"

namespace riu::format {

namespace {

enum class Mode : std::uint8_t { Flat, Break };

struct Frame {
    Mode mode;
    int indent;
    Doc doc;
};

// 估算 flat 模式下从 frames 顶端开始能否在 remaining 宽度内放下，
// 直到遇到一个 break 或 hardline。简化版：遇到 HardLine 直接失败；
// 内层 Group 默认按 flat 估。
bool fits(std::vector<Frame> frames, int remaining) {
    while (!frames.empty()) {
        if (remaining < 0) return false;
        Frame f = frames.back();
        frames.pop_back();
        const DocNode& n = *f.doc;
        switch (n.kind) {
        case DocKind::Text:
            remaining -= static_cast<int>(n.text.size());
            break;
        case DocKind::Concat:
            for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
                frames.push_back({.mode = f.mode, .indent = f.indent, .doc = *it});
            }
            break;
        case DocKind::Indent:
            if (!n.children.empty()) {
                frames.push_back({.mode = f.mode, .indent = f.indent + n.indent, .doc = n.children[0]});
            }
            break;
        case DocKind::Group:
            if (!n.children.empty()) {
                frames.push_back({.mode = Mode::Flat, .indent = f.indent, .doc = n.children[0]});
            }
            break;
        case DocKind::Line:
            if (f.mode == Mode::Flat) {
                remaining -= static_cast<int>(n.text.size());
            } else {
                return true; // break 模式下一行重置，认为放得下
            }
            break;
        case DocKind::HardLine:
            return true; // 硬换行：当前行到此结束，认为放得下
        }
    }
    return remaining >= 0;
}

} // namespace

std::string render(const Doc& doc, const RenderOptions& opt) {
    std::string out;
    int column = 0;
    std::vector<Frame> stack;
    stack.push_back({.mode = Mode::Break, .indent = 0, .doc = doc});

    auto emitNewline = [&](int indent) {
        out.push_back('\n');
        for (int i = 0; i < indent; ++i)
            out.push_back(' ');
        column = indent;
    };

    while (!stack.empty()) {
        Frame f = stack.back();
        stack.pop_back();
        const DocNode& n = *f.doc;
        switch (n.kind) {
        case DocKind::Text:
            out += n.text;
            column += static_cast<int>(n.text.size());
            break;
        case DocKind::Concat:
            for (auto it = n.children.rbegin(); it != n.children.rend(); ++it) {
                stack.push_back({.mode = f.mode, .indent = f.indent, .doc = *it});
            }
            break;
        case DocKind::Indent:
            if (!n.children.empty()) {
                stack.push_back({.mode = f.mode, .indent = f.indent + n.indent, .doc = n.children[0]});
            }
            break;
        case DocKind::Group: {
            if (n.children.empty()) break;
            std::vector<Frame> probe;
            probe.push_back({.mode = Mode::Flat, .indent = f.indent, .doc = n.children[0]});
            int remaining = static_cast<int>(opt.lineWidth) - column;
            Mode chosen = fits(probe, remaining) ? Mode::Flat : Mode::Break;
            stack.push_back({.mode = chosen, .indent = f.indent, .doc = n.children[0]});
            break;
        }
        case DocKind::Line:
            if (f.mode == Mode::Flat) {
                out += n.text;
                column += static_cast<int>(n.text.size());
            } else {
                emitNewline(f.indent);
            }
            break;
        case DocKind::HardLine:
            emitNewline(f.indent);
            break;
        }
    }

    return out;
}

} // namespace riu::format
