// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 服务主循环
//
// 本文件包含 LSP server 的入口与分发逻辑:
// - 在 stdin 上轮询读取消息，按 method 分发到对应 handler
// - 维护打开文档表（uri -> 文本 + version）
// - P0 阶段仅实现握手与文档同步，不做解析与诊断（留给 P1）

#pragma once

namespace riu::lsp {

// 进入 LSP 主循环；返回进程应当使用的退出码。
int runServer();

} // namespace riu::lsp
