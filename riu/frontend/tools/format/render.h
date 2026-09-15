// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Doc IR 渲染器
//
// 把 Doc 渲染为最终字符串：
// - Group 在 flat 模式下若总宽超过 lineWidth 则切换到 break 模式；
// - HardLine 始终换行并把外层 Group 标记为 break；
// - Indent 在 break 时影响下一行的缩进字符数。
//
// 当前实现是简化版 (类 Wadler / Prettier 的 best layout)，
// 满足 Phase 1 骨架，复杂决策留到后续阶段细化。

#ifndef RIU_LANG_FORMAT_RENDER_H
#define RIU_LANG_FORMAT_RENDER_H

#include <string>

#include "tools/format/doc.h"

namespace riu::format {

struct RenderOptions {
    std::size_t lineWidth = 120;
    std::size_t indentSize = 2;
};

std::string render(const Doc& doc, const RenderOptions& opt);

} // namespace riu::format

#endif // RIU_LANG_FORMAT_RENDER_H
