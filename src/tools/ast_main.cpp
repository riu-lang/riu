// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// yux-ast: 仅运行 ANTLR4 词法 + 语法分析, 输出 parse tree
//
// 与 yux 主二进制不同, 本工具:
// - 不构造 AST (不调用 ASTBuilder)
// - 不解析 import / 不做语义分析 / 不生成 LLVM IR
// - 不安装错误监听器, 仅打印 ANTLR 默认的 parse tree
//
// 用法:
//   yux-ast <input.yux>             ; 输出到 stdout
//   yux-ast <input.yux> -o <file>   ; 写入文件
//   yux-ast <input.yux> --oneline   ; 单行输出 (默认多行 pretty)

#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include "antlr4-runtime.h"
#include <CLI/CLI.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) { // NOLINT(bugprone-exception-escape)
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    CLI::App app{"yux-ast: dump ANTLR parse tree of a .yux file"};

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file")->required();

    std::string outputFile;
    app.add_option("-o,--output", outputFile, "Output file (default: stdout)");

    bool oneline = false;
    app.add_flag("--oneline", oneline, "Print tree on a single line (default: pretty multi-line)");

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: input file not found: " << inputFile << '\n';
        return 1;
    }

    // 加载源文件 (UTF-8). ANTLRFileStream 自带 BOM 处理.
    antlr4::ANTLRFileStream stream;
    try {
        stream.loadFromFile(inputFile);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot load file " << inputFile << ": " << e.what() << '\n';
        return 1;
    }

    // 仅做词法 + 语法分析. 故意不安装 SyntaxErrorListener:
    // 任务约定 "ast 仅 antlr, 无错误输出树", 即只输出 ANTLR 自身的 parse tree,
    // 哪怕源文件存在语法错误也照样把 (可能含 <error> 节点的) 树打印出来.
    yux::yuxLexer lexer(&stream);
    antlr4::CommonTokenStream tokenStream(&lexer);
    yux::yuxParser parser(&tokenStream);

    auto* tree = parser.program();
    std::string out = tree->toStringTree(&parser, !oneline);

    if (outputFile.empty()) {
        std::cout << out << "\n";
    } else {
        std::ofstream of(outputFile, std::ios::binary);
        if (!of) {
            std::cerr << "Error: cannot open output file: " << outputFile << '\n';
            return 1;
        }
        of << out << "\n";
        if (!of) {
            std::cerr << "Error: failed to write output file: " << outputFile << '\n';
            return 1;
        }
    }
    return 0;
}
