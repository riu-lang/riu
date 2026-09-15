// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// `riu format` 子命令实现
//
// 从 riu/riu/main.cpp 抠出 (P1 Phase 1.b.iii): stdin / 文件输入, --in-place 覆盖,
// 向上查找 riu.toml 读取 fmt.line_width。仅 AST 引擎; 旧 token 流 Formatter 已删除。

#pragma once

#include <string>

namespace riu::cli {

struct FormatCmdOptions {
    std::string file;       // formatCmd file 形参 (与 --stdin 互斥)
    bool inPlace = false;   // -i / --in-place
    bool fromStdin = false; // --stdin
    int lineWidth = 0;      // --line-width; 0 = 默认 / 从 riu.toml 读
};

// 运行 `riu format`。返回 进程退出码 (0 成功, 非 0 错误)。
int runFormatCommand(const FormatCmdOptions& opts);

} // namespace riu::cli
