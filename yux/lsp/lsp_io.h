// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 传输层
//
// 本文件包含 LSP stdio 传输层相关定义:
// - 按 LSP 规范读写带 Content-Length 头的 JSON-RPC 消息
// - Windows 下需要把 stdin/stdout 切到二进制模式以避免 \r\n 翻译
// - 消息体一律按 UTF-8 处理（LSP 协议强制要求）

#pragma once

#include <optional>
#include <string>

namespace yux::lsp {

// 把 stdin/stdout 设置为二进制模式（仅 Windows 必要，POSIX 是 no-op）
void setStdioBinary();

// 从 stdin 阻塞读取一条 LSP 消息（含 Content-Length 帧解析）。
// 返回消息 body（UTF-8 JSON 文本）；EOF 或 IO 错误返回 std::nullopt。
std::optional<std::string> readMessage();

// 把一条消息写到 stdout（自动加 Content-Length 头，立即 flush）。
// body 必须是 UTF-8。
void writeMessage(const std::string& body);

} // namespace yux::lsp
