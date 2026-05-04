// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// yux-lsp 可执行入口
//
// 本文件是独立 LSP 服务进程的主入口:
// - 直接进入 stdio JSON-RPC 主循环（yux::lsp::runServer）
// - 不解析任何命令行参数；调用方（VSCode / IntelliJ LSP4IJ）按 LSP 规范驱动

#include "lsp/lsp_server.h"

int main(int /*argc*/, char** /*argv*/) {
    return yux::lsp::runServer();
}
