// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// AST 驱动格式化器入口
//
// formatAst() 是当前唯一的格式化引擎（Phase 6 起取代旧 token 流 Formatter）。
// 接管词法 / 语法分析、trivia 扫描、AST accept 分派（4.4）、render 渲染。

#ifndef RIU_LANG_FORMAT_PRINTER_H
#define RIU_LANG_FORMAT_PRINTER_H

#include <string>

#include "tools/formatter.h"

namespace riu::format {

// 用 AST 引擎格式化 source。filePath 给 #Test 识别（`.test.ut`）；可空。
std::string formatAst(const std::string& source, const FormatConfig& config, const std::string& filePath = {});

} // namespace riu::format

#endif // RIU_LANG_FORMAT_PRINTER_H
