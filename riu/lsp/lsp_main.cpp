// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// riu-lsp 可执行入口
//
// 本文件是独立 LSP 服务进程的主入口:
// - 直接进入 stdio JSON-RPC 主循环（riu::lsp::runServer）
// - 不解析任何命令行参数；调用方（VSCode / IntelliJ LSP4IJ）按 LSP 规范驱动
// - 仅处理 --version（打印版本号后退出）

#include "lsp/lsp_server.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) { // NOLINT(bugprone-exception-escape) — iostream 可能抛 ios_base::failure
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "riu-lsp " RIU_VERSION "\n";
            return 0;
        }
    }
    return riu::lsp::runServer();
}
