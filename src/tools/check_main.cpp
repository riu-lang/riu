// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// yux-check: 单文件快速语义检查 (阶段 0)
//
// 与 yux 主二进制不同, 本工具:
// - 0 LLVM 依赖, 只链 yux_frontend
// - 不加载 SDK, 不解析 import 链, 不做 codegen
// - 仅 parse → ASTBuilder → SemaPass.run() → 打印诊断
//
// 设计意图: 日常写 demo / 改代码时快速跑诊断, 避免每次 xmake build 编 LLVM.
// **报错不与 yux build 等价**: 仅检出 SemaPass 当前能接管的错误码; 漏的部分
// (泛型 / lambda / target-type / statement walk 等) 由 yux build 兜底.
//
// 详见 CURRENT.md "yux-check 最小可用 exe" 一节.
//
// 用法:
//   yux-check <input.yux>   ; 退出码: 0 = 无错, 1 = 文件 / 语法 / 语义错

// windows.h 必须在拉入 yux frontend (经由 include/types.h 做了 `using namespace
// std`) 之前 #include, 否则 std::byte 与 winapi byte 冲突 (rpcndr.h).
// NOGDI 跳过 wingdi.h, 避免其 ERROR 宏与 antlr4 的 ERROR 标识符冲突.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include "ast/yux.h"
#include "sema/sema_pass.h"
#include "tools/diagnostic.h"
#include "tools/syntax_error_listener.h"

#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    CLI::App app{"yux-check: fast standalone semantic check (no LLVM)"};

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file")->required();

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: input file not found: " << inputFile << std::endl;
        return 1;
    }

    std::string absPath = std::filesystem::absolute(inputFile).string();

    // 1. 词法 + 语法
    antlr4::ANTLRFileStream stream;
    try {
        stream.loadFromFile(absPath);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot load file " << absPath << ": " << e.what() << std::endl;
        return 1;
    }

    yux::yuxLexer lexer(&stream);
    SyntaxErrorListener errListener(absPath, std::cerr);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);

    antlr4::CommonTokenStream tokens(&lexer);
    yux::yuxParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);

    auto* program = parser.program();
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        return 1;
    }

    // 2. AST + SemaPass. 单文件模式, 不加载 SDK / 不解析 import 链.
    //    使用文件名 stem 作为 module name. 与 yux 主二进制行为不一致, 阶段 0
    //    可接受 —— 后续阶段补 SDK / 模块依赖时再对齐.
    std::string moduleName = std::filesystem::path(absPath).stem().string();

    Yux yux;
    yux.initSingleFileRoot(absPath);

    try {
        // loadMainFile 会触发 ASTBuilder.build, 含 import 解析.
        // 若 import 失败 (找不到 SDK / 模块), 这里抛 YuxError, 直接报.
        auto file = yux.loadMainFile(absPath, moduleName);
        yux.validateSpecImpls();
        SemaPass(file, yux.sdkFile()).run();
    } catch (const std::runtime_error& e) {
        if (auto* yuxErr = dynamic_cast<const YuxError*>(&e)) {
            DiagnosticEngine::renderYuxError(std::cerr, absPath, *yuxErr);
        } else {
            std::cerr << e.what() << std::endl;
        }
        return 1;
    }

    return 0;
}
