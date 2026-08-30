// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 子进程工具：取自身 exe、UTF-8→UTF-16、spawn 等待 / 重定向到日志。
// `yux test` runner 与 `yux build --test` 并行编译 job 共用。

#pragma once

#include <cstdint>
#include <string>

namespace yux::cli {

// spawn 失败时的退出码哨兵（与 Win32 MAXDWORD 同值）。
inline constexpr std::uint32_t kSpawnFailed = 0xFFFFFFFFu;

// 当前进程 exe 全路径（UTF-8）。失败返回空串。
std::string getSelfExePath();

std::wstring toWide(const std::string& s);

// 0 → hardware_concurrency（至少 1）。
int resolveThreadCount(int threads);

// spawn 子进程，等待完成，返回退出码。失败返回 kSpawnFailed。
std::uint32_t spawnAndWait(const std::wstring& cmdLine, const std::wstring& workingDir = {});

// spawn 子进程，stdout/stderr 重定向到 logPath（覆盖）。失败返回 kSpawnFailed。
std::uint32_t spawnToLog(const std::wstring& cmdLine, const std::wstring& logPath, const std::wstring& workingDir = {});

} // namespace yux::cli
