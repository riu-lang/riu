// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include "riu/riuParser.h"

namespace antlr4 {
class ANTLRErrorListener;
class CommonTokenStream;
} // namespace antlr4

// 两段式语法分析：先 SLL + BailErrorStrategy（无逐决策 LL 回退），失败再整文件 LL。
// 合法输入几乎都走 SLL；语法错由第二段 LL + errListener 报出。
riu::riuParser::ProgramContext* parseRiuProgram(riu::riuParser& parser, antlr4::CommonTokenStream& tokens,
                                                antlr4::ANTLRErrorListener* errListener);
