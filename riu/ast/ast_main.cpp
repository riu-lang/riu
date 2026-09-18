// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// riu-ast: dump rd 词法 token 或 FlatAst 树。
//
// 与 riu 主二进制不同, 本工具:
// - 不构造 FileNode（不调用 RdBuilder）
// - 不解析 import / 不做语义分析 / 不生成 LLVM IR
//
// 用法:
//   riu-ast <input.ut>             ; 默认 --rd，输出 FlatAst
//   riu-ast <input.ut> -o <file>   ; 写入文件
//   riu-ast <input.ut> --rd-tokens ; rd Scanner default 通道 token
//   riu-ast <input.ut> --rd        ; rd FlatAst 缩进树
//   riu-ast <input.ut> --quiet     ; 只词法/语法分析，不 dump（可与上列 flag 组合，便于计时）

#include "ast/rd/parser.h"
#include "ast/rd/scanner.h"
#include "ast/rd/token.h"

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

#include "ast/syntax_diag.h"

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

void drainRdTokens(std::string_view src) {
    rd::Scanner scanner(src);
    for (;;) {
        if (scanner.next().kind == rd::Kind::Eof) break;
    }
}

void writeRdErrors(const std::vector<rd::ParseError>& errs) {
    for (const auto& e : errs) {
        SyntaxDiag d;
        d.is_lexer = e.is_lexer;
        d.line = e.pos.line;
        d.col = e.pos.column + 1;
        d.message = e.message;
        d.offending = e.offending;
        d.prev_text = e.prev_text;
        std::cerr << formatSyntaxDiagCompact(d);
    }
}

std::string loadSrc(const std::string& inputFile) {
    try {
        return readUtf8File(inputFile);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("cannot load file ") + inputFile + ": " + e.what());
    }
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

    CLI::App app{"riu-ast: dump rd tokens / FlatAst of a .ut file"};
    bool versionFlag = false;
    app.add_flag("--version", versionFlag, "Print version and exit");

    std::string inputFile;
    app.add_option("input", inputFile, "Input .ut file")->required();

    std::string outputFile;
    app.add_option("-o,--output", outputFile, "Output file (default: stdout)");

    bool dumpRdTokens = false;
    bool dumpRdAst = false;
    bool quiet = false;
    auto* rdTokensFlag = app.add_flag("--rd-tokens", dumpRdTokens, "Dump rd scanner tokens");
    auto* rdAstFlag = app.add_flag("--rd", dumpRdAst, "Dump rd FlatAst tree (default)");
    app.add_flag("--quiet", quiet, "Parse/tokenize without dumping (for timing)");
    rdTokensFlag->excludes(rdAstFlag);
    rdAstFlag->excludes(rdTokensFlag);

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: input file not found: " << inputFile << '\n';
        return 1;
    }

    std::string src;
    try {
        src = loadSrc(inputFile);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    if (dumpRdTokens) {
        if (quiet) {
            drainRdTokens(src);
            return 0;
        }
        rd::Scanner scanner(src);
        std::string out;
        for (;;) {
            rd::Token t = scanner.next();
            out += rd::formatTokenLine(t);
            if (t.kind == rd::Kind::Eof) break;
        }
        writeRdErrors(scanner.errors());
        return writeOut(outputFile, out);
    }

    rd::ParseResult parsed = rd::parseProgram(src);
    if (!quiet) writeRdErrors(parsed.errors);
    if (quiet) return 0;
    return writeOut(outputFile, rd::dumpTree(parsed.ast));
}
