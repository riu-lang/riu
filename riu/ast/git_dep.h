// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#pragma once

#include <string>

// 检出到 `<rootDir>/build/dependences/<name>/`。HEAD 已是 `rev` 则跳过网络。
// 工作区有改动 → E5043。clone / fetch / checkout 非零 → GitCommandFailed。
[[nodiscard]] std::string checkoutGitDep(const std::string& rootDir, const std::string& name, const std::string& url,
                                         const std::string& rev);
