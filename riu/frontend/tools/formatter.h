// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 格式化配置
//
// 历史上本文件还导出过 token-流式 Formatter 类（启发式重排），Phase 6
// 把它整体替换为 AST 驱动的 riu::format::formatAst (见 tools/format/printer.h)，
// 此处仅保留共享的 FormatConfig 结构

#ifndef RIU_LANG_FORMATTER_H
#define RIU_LANG_FORMATTER_H

#include <cstddef>

namespace riu {

struct FormatConfig {
    std::size_t indentSize = 2;            // 缩进空格数（固定为 2）
    std::size_t lineWidth = 120;           // 列宽阈值
    bool insertSpaceAfterKeyword = true;   // 关键字后插入空格
    bool insertSpaceAroundOperator = true; // 运算符两边插入空格
    bool insertSpaceAfterComma = true;     // 逗号后插入空格
};

} // namespace riu

#endif // RIU_LANG_FORMATTER_H
