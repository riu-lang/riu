// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// riu-ast: dump ANTLR parse tree，或词法 token（ANTLR / rd 手写栈）
//
// 与 riu 主二进制不同, 本工具:
// - 不构造 AST (不调用 ASTBuilder)
// - 不解析 import / 不做语义分析 / 不生成 LLVM IR
// - 默认不安装错误监听器, 仅打印 ANTLR 默认的 parse tree
//
// 用法:
//   riu-ast <input.ut>             ; 输出到 stdout
//   riu-ast <input.ut> -o <file>   ; 写入文件
//   riu-ast <input.ut> --oneline   ; 单行输出 (默认多行 pretty)
//   riu-ast <input.ut> --tokens    ; ANTLR default 通道 token
//   riu-ast <input.ut> --rd-tokens ; rd Scanner default 通道 token

#include "riu/riuLexer.h"
#include "riu/riuParser.h"

#include "ast/rd/scanner.h"
#include "ast/rd/token.h"

#include "antlr4-runtime.h"
#include <CLI/CLI.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int writeOut(const std::string& outputFile, const std::string& body) {
    if (outputFile.empty()) {
        std::cout << body;
        if (body.empty() || body.back() != '\n') std::cout << '\n';
        return 0;
    }
    std::ofstream of(outputFile, std::ios::binary);
    if (!of) {
        std::cerr << "Error: cannot open output file: " << outputFile << '\n';
        return 1;
    }
    of << body;
    if (!body.empty() && body.back() != '\n') of << '\n';
    if (!of) {
        std::cerr << "Error: failed to write output file: " << outputFile << '\n';
        return 1;
    }
    return 0;
}

std::string readUtf8File(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open file");
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF && static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        src.erase(0, 3);
    }
    return src;
}

rd::Pos posFromAntlr(antlr4::Token* t) {
    rd::Pos p;
    p.line = static_cast<rd::i32>(t->getLine());
    p.column = static_cast<rd::i32>(t->getCharPositionInLine());
    const auto start = static_cast<rd::i32>(t->getStartIndex());
    const auto stop = static_cast<rd::i32>(t->getStopIndex());
    p.offset = start;
    p.end = (stop >= start) ? stop + 1 : start;
    return p;
}

std::string dumpAntlrTokens(riu::riuLexer& lexer, antlr4::CommonTokenStream& tokens) {
    tokens.fill();
    const auto& vocab = lexer.getVocabulary();
    std::string out;
    for (auto* t : tokens.getTokens()) {
        if (t->getChannel() != antlr4::Token::DEFAULT_CHANNEL) continue;
        std::string_view name = vocab.getSymbolicName(t->getType());
        if (name.empty()) name = "INVALID";
        std::string text = t->getText();
        if (t->getType() == antlr4::Token::EOF) text.clear();
        out += rd::formatTokenLine(name, posFromAntlr(t), text);
    }
    return out;
}

std::string dumpRdTokens(std::string_view src) {
    rd::Scanner scanner(src);
    std::string out;
    for (;;) {
        rd::Token t = scanner.next();
        out += rd::formatTokenLine(t);
        if (t.kind == rd::Kind::Eof) break;
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) { // NOLINT(bugprone-exception-escape)
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --version：在 CLI11 解析之前手动处理，避免 required option 冲突。
    // CLI11 仍注册同名 flag 以确保 -h 显示 --version。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "riu-ast " RIU_VERSION "\n";
            return 0;
        }
    }

    CLI::App app{"riu-ast: dump ANTLR parse tree of a .ut file"};
    bool versionFlag = false;
    app.add_flag("--version", versionFlag, "Print version and exit");

    std::string inputFile;
    app.add_option("input", inputFile, "Input .ut file")->required();

    std::string outputFile;
    app.add_option("-o,--output", outputFile, "Output file (default: stdout)");

    bool oneline = false;
    app.add_flag("--oneline", oneline, "Print tree on a single line (default: pretty multi-line)");

    bool dumpTokens = false;
    bool dumpRd = false;
    auto* tokensFlag = app.add_flag("--tokens", dumpTokens, "Dump ANTLR default-channel tokens");
    auto* rdTokensFlag = app.add_flag("--rd-tokens", dumpRd, "Dump rd scanner tokens");
    tokensFlag->excludes(rdTokensFlag);
    rdTokensFlag->excludes(tokensFlag);

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: input file not found: " << inputFile << '\n';
        return 1;
    }

    if (dumpRd) {
        std::string src;
        try {
            src = readUtf8File(inputFile);
        } catch (const std::exception& e) {
            std::cerr << "Error: cannot load file " << inputFile << ": " << e.what() << '\n';
            return 1;
        }
        return writeOut(outputFile, dumpRdTokens(src));
    }

    // 加载源文件 (UTF-8). ANTLRFileStream 自带 BOM 处理.
    antlr4::ANTLRFileStream stream;
    try {
        stream.loadFromFile(inputFile);
    } catch (const std::exception& e) {
        std::cerr << "Error: cannot load file " << inputFile << ": " << e.what() << '\n';
        return 1;
    }

    riu::riuLexer lexer(&stream);
    antlr4::CommonTokenStream tokenStream(&lexer);

    if (dumpTokens) {
        return writeOut(outputFile, dumpAntlrTokens(lexer, tokenStream));
    }

    // 仅做词法 + 语法分析. 故意不安装 SyntaxErrorListener:
    // 任务约定 "ast 仅 antlr, 无错误输出树", 即只输出 ANTLR 自身的 parse tree,
    // 哪怕源文件存在语法错误也照样把 (可能含 <error> 节点的) 树打印出来.
    riu::riuParser parser(&tokenStream);

    auto* tree = parser.program();
    std::string out = tree->toStringTree(&parser, !oneline);
    if (!out.empty() && out.back() != '\n') out += '\n';
    return writeOut(outputFile, out);
}
