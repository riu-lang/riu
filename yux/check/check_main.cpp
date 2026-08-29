// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// yux-check: 单文件快速语义检查 (阶段 0) + 批量诊断测试 (阶段 1)
//
// 与 yux 主二进制不同, 本工具:
// - 0 LLVM 依赖, 只链 yux_frontend
// - 不加载 SDK, 不解析 import 链, 不做 codegen
// - 仅 parse → ASTBuilder → SemaPass.run() → 打印诊断
//
// 设计意图: 日常写 demo / 改代码时快速跑诊断, 避免每次 xmake build 编 LLVM.
// **报错不与 yux build 等价**: 仅检出 SemaPass 当前能接管的错误码; 漏的部分
// (方法点 E3095 / 泛型实例化) 由 yux build 兜底.
//
// 详见 CURRENT.md "yux-check 最小可用 exe" 一节.
//
// 用法:
//   yux-check <input.yux>           ; 退出码: 0 = 无错, 1 = 文件 / 语法 / 语义错
//   yux-check test <dir>            ; 批量测试目录下所有 .yux (非递归)
//   yux-check test <dir> -r         ; 递归子目录

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
#include "tools/sdk_loader.h"
#include "tools/syntax_error_listener.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace yux;
using namespace std;

// ============================================================================
// "; check:" 注解解析 —— 读源文件文本行做字符串匹配, 不走 ANTLR token 通道
// ============================================================================

// 从源文件文本中解析 "; check:" 行尾注解。
// 返回 map: 行号 → 期望错误码集合 (按行号排序)。
// "; check: none" 表示显式标记该行无期望错误, 不计入。
static map<size_t, vector<string>> parseCheckAnnotations(const string& filePath) {
    map<size_t, vector<string>> result;
    ifstream in(filePath);
    if (!in.is_open()) return result;

    string line;
    size_t lineNum = 0;
    while (getline(in, line)) {
        ++lineNum;
        size_t pos = line.find("; check:");
        if (pos == string::npos) continue;

        // 提取空格分隔的错误码 (例: "E4024" 或 "E2030 E4022")
        string codesStr = line.substr(pos + 9); // skip "; check:"
        istringstream iss(codesStr);
        vector<string> codes;
        string code;
        while (iss >> code) {
            if (code == "none") continue; // "; check: none" → 显式标记无期望
            codes.push_back(code);
        }
        if (!codes.empty()) {
            result[lineNum] = std::move(codes);
        }
    }
    return result;
}

// 将错误码 vector 格式化为空格分隔的字符串 ("E4024" 或 "E2030 E4022")
static string formatCodes(const vector<string>& codes) {
    if (codes.empty()) return "(none)";
    ostringstream oss;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i > 0) oss << ' ';
        oss << codes[i];
    }
    return oss.str();
}

// 将错误码 vector 格式化为逗号分隔的字符串 ("E4024" 或 "E2030, E4022")
static string formatCodesComma(const vector<string>& codes) {
    if (codes.empty()) return "(none)";
    ostringstream oss;
    for (size_t i = 0; i < codes.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << codes[i];
    }
    return oss.str();
}

// ============================================================================
// 目录扫描
// ============================================================================

// 扫描目录下的 .yux 文件。
// recursive=false: 仅当前目录; recursive=true: 递归子目录。
// 结果按文件名排序, 保证输出确定性。
static vector<string> scanYuxFiles(const string& dir, bool recursive) {
    vector<string> result;
    error_code ec;

    if (recursive) {
        for (auto it = filesystem::recursive_directory_iterator(dir, ec);
             it != filesystem::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (it->is_regular_file() && it->path().extension() == ".yux") {
                result.push_back(filesystem::absolute(it->path()).string());
            }
        }
    } else {
        for (auto it = filesystem::directory_iterator(dir, ec); it != filesystem::directory_iterator(); ++it) {
            if (ec) break;
            if (it->is_regular_file() && it->path().extension() == ".yux") {
                result.push_back(filesystem::absolute(it->path()).string());
            }
        }
    }

    ranges::sort(result);
    return result;
}

// ============================================================================
// 单文件 sema 执行 —— parse → AST → SemaPass, 收集抛出的错误
// ============================================================================

struct CheckResult {
    bool ok = true;               // false = 有错误 (semaError 或 otherError)
    optional<YuxError> semaError; // SemaPass 抛出的 YuxError
    string otherError;            // 非 YuxError 的错误信息 (parse / AST 阶段失败)
};

// 对单个 .yux 文件执行完整检查流水线。
// sdkPath 为空时跳过 SDK 加载 (退化为 builtin 范围检查)。
static CheckResult runSemaOnFile(const string& absPath, const string& sdkPath) {
    CheckResult cr;

    // 1. 词法 + 语法 (ANTLR)
    antlr4::ANTLRFileStream stream;
    try {
        stream.loadFromFile(absPath);
    } catch (const exception& e) {
        cr.ok = false;
        cr.otherError = string("cannot load file: ") + e.what();
        return cr;
    }

    ostringstream syntaxErrStream;
    SyntaxErrorListener errListener(absPath, syntaxErrStream);

    yux::yuxLexer lexer(&stream);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);

    antlr4::CommonTokenStream tokens(&lexer);
    yux::yuxParser parser(&tokens);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);

    auto* program = parser.program();
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        cr.ok = false;
        cr.otherError = syntaxErrStream.str();
        // 去除末尾换行, 保持输出整洁
        if (!cr.otherError.empty() && cr.otherError.back() == '\n') {
            cr.otherError.pop_back();
        }
        return cr;
    }

    // 2. AST 构建 + SemaPass
    string moduleName = filesystem::path(absPath).stem().string();

    Yux yux;
    yux.initFileRoot(absPath);

    try {
        // 加载 SDK (找不到不致命)
        if (!sdkPath.empty()) {
            sdk_loader::parseSdkDir(sdkPath, yux);
        }

        auto file = yux.loadMainFile(absPath, moduleName);
        yux.validateSpecImpls();
        SemaPass(file, &yux).run();
    } catch (const YuxError& e) {
        cr.ok = false;
        cr.semaError = e;
    } catch (const runtime_error& e) {
        cr.ok = false;
        cr.otherError = e.what();
    }

    return cr;
}

// ============================================================================
// test 子命令: 批量诊断测试
// ============================================================================

// 单文件测试结果
// NOLINTNEXTLINE(bugprone-exception-escape) — map 成员可能导致移动/拷贝抛异常, 但本 struct 仅作本地数据容器
struct TestFileResult {
    string filename; // 仅文件名 (用于显示)
    bool passed = false;
    string failReason;                       // 失败原因 (可能多行)
    map<size_t, vector<string>> annotations; // 解析出的注解
};

// 对单个 .yux 文件执行 sema 检查, 复用已有 Yux 实例 (SDK 已预加载).
// 与 runSemaOnFile 的区别: 不创建新 Yux、不加载 SDK、不重复做 ANTLR 解析
// (loadMainFile → _parseFile 内部已包含 lex/parse/syntax check).
static CheckResult runSemaOnFileWithYux(const string& absPath, Yux& yux) {
    CheckResult cr;
    string moduleName = filesystem::path(absPath).stem().string();

    try {
        auto file = yux.loadMainFile(absPath, moduleName);
        yux.validateSpecImpls();
        SemaPass(file, &yux).run();
    } catch (const YuxError& e) {
        cr.ok = false;
        cr.semaError = e;
    } catch (const runtime_error& e) {
        cr.ok = false;
        cr.otherError = e.what();
    }

    return cr;
}

// 单文件结果评估，复用已有 Yux (SDK 已预加载)
static TestFileResult evaluateOneFileWithYux(const string& absPath, Yux& yux) {
    namespace fs = filesystem;

    TestFileResult tfr;
    tfr.filename = fs::path(absPath).filename().string();
    tfr.annotations = parseCheckAnnotations(absPath);

    auto cr = runSemaOnFileWithYux(absPath, yux);

    if (!cr.otherError.empty()) {
        tfr.passed = false;
        tfr.failReason = "  parse/AST error: " + cr.otherError + "\n";
        return tfr;
    }

    bool hasAnnotations = !tfr.annotations.empty();
    bool hasSemaError = cr.semaError.has_value();

    if (!hasAnnotations && !hasSemaError) {
        tfr.passed = true;
    } else if (!hasAnnotations && hasSemaError) {
        tfr.passed = false;
        ostringstream oss;
        oss << "  unexpected " << cr.semaError->getCode() << " at line " << cr.semaError->getLineNumber() << ": "
            << cr.semaError->what() << '\n';
        tfr.failReason = oss.str();
    } else if (hasAnnotations && !hasSemaError) {
        tfr.passed = false;
        ostringstream oss;
        for (auto& [line, codes] : tfr.annotations) {
            oss << "  line " << line << ": expected " << formatCodesComma(codes) << ", got no error\n";
        }
        tfr.failReason = oss.str();
    } else {
        auto& err = *cr.semaError;
        size_t errLine = err.getLineNumber();
        string errCode = err.getCode();

        auto it = tfr.annotations.find(errLine);
        if (it != tfr.annotations.end()) {
            auto& expected = it->second;
            bool matched = false;
            for (auto& ec : expected) {
                if (ec == errCode) {
                    matched = true;
                    break;
                }
            }

            if (matched) {
                tfr.passed = true;
            } else {
                tfr.passed = false;
                ostringstream oss;
                oss << "  line " << errLine << ": expected " << formatCodesComma(expected) << ", got " << errCode
                    << '\n';
                tfr.failReason = oss.str();
            }
        } else {
            tfr.passed = false;
            ostringstream oss;
            oss << "  line " << errLine << ": unexpected " << errCode << " (" << err.what() << ")";
            if (tfr.annotations.size() == 1) {
                auto& [line, codes] = *tfr.annotations.begin();
                oss << ", expected " << formatCodesComma(codes) << " at line " << line;
            }
            oss << '\n';
            tfr.failReason = oss.str();
        }
    }

    return tfr;
}

// 检查文件是否包含 "; require-sdk" 注解（文件头几行）。
// 有此注解的文件依赖 SDK 类型（Heap/Rc/Weak/String 等），需要在 test 模式下加载 SDK。
static bool fileRequiresSdk(const string& filePath) {
    ifstream in(filePath);
    if (!in.is_open()) return false;

    string line;
    // 仅检查前 10 行，避免扫描整个文件
    for (int i = 0; i < 10 && getline(in, line); ++i) {
        if (line.find("; require-sdk") != string::npos) {
            return true;
        }
    }
    return false;
}

static int runCheckTest(const string& dir, bool recursive) {
    namespace fs = filesystem;
    auto t0 = chrono::steady_clock::now();

    if (!fs::is_directory(dir)) {
        cerr << "Error: not a directory: " << dir << '\n';
        return 1;
    }

    // diag 文件不依赖 SDK 类型，跳过 SDK 加载消除 ~4s/线程 启动开销。
    // 对确实需要 SDK 的用例，后续可通过文件头注解（如 ; require-sdk）按需加载。

    auto files = scanYuxFiles(dir, recursive);
    if (files.empty()) {
        cout << "No .yux files found in " << fs::absolute(dir).string() << '\n';
        return 0;
    }

    // 单线程处理，每文件独立 Yux 实例，避免跨文件状态累积（BUG#3）。
    // 对标注了 "; require-sdk" 的文件，SDK 只加载一次到模板 Yux，
    // 各文件通过共享 _sdkFile 指针获得 SDK 符号可见性。
    size_t fileCount = files.size();
    vector<TestFileResult> results(fileCount);

    // 预加载 SDK（一次性），后续各文件共享其 _sdkFile
    Yux sdkYux;
    bool sdkLoaded = false;
    {
        string sdkPath;
        for (size_t i = 0; i < fileCount && !sdkLoaded; ++i) {
            if (fileRequiresSdk(files[i])) {
                sdkPath = sdk_loader::findSdkPath();
                if (!sdkPath.empty()) {
                    sdkYux.initFileRoot(files[0]);
                    sdk_loader::parseSdkDir(sdkPath, sdkYux);
                    sdkLoaded = true;
                }
                break;
            }
        }
    }

    for (size_t i = 0; i < fileCount; ++i) {
        Yux yux;
        yux.initFileRoot(files[i]);

        if (fileRequiresSdk(files[i]) && sdkLoaded) {
            yux.setSdkFile(sdkYux.sdkFile());
        }

        results[i] = evaluateOneFileWithYux(files[i], yux);

        // 解除共享引用，避免 Yux 析构时 delete 不属于它的 SDK FileNode
        if (fileRequiresSdk(files[i]) && sdkLoaded) {
            yux.setSdkFile(nullptr);
        }
    }

    // 统计 (results 已按 files 的序号排列, 即按文件名排序)
    size_t passed = 0;
    size_t failed = 0;
    for (auto& r : results) {
        if (r.passed)
            ++passed;
        else
            ++failed;
    }

    // 输出结果
    for (auto& r : results) {
        if (r.passed) {
            cout << "  [PASS] " << r.filename << '\n';
        } else {
            cout << "  [FAIL] " << r.filename << '\n';
            cout << r.failReason;
        }
    }

    // 总耗时
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();
    auto sec = elapsed / 1000;
    auto ms = elapsed % 1000;

    cout << '\n';
    cout << "  Summary: " << passed << " passed, " << failed << " failed, " << results.size() << " total"
         << " [" << sec << '.' << (ms / 100) % 10 << (ms / 10) % 10 << ms % 10 << "s]\n";

    return failed == 0 ? 0 : 1;
}

// ============================================================================
// main
// ============================================================================

int main(int argc,
         char* argv[]) { // NOLINT(bugprone-exception-escape) — main 入口点, filesystem API 可能抛 system_error
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --version：在 CLI11 解析之前手动处理，避免 required option 冲突。
    // CLI11 仍注册同名 flag 以确保 -h 显示 --version。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "yux-check " YUX_VERSION "\n";
            return 0;
        }
    }

    CLI::App app{"yux-check: fast standalone semantic check (no LLVM)"};
    bool versionFlag = false;
    app.add_flag("--version", versionFlag, "Print version and exit");
    app.require_subcommand(0, 1);

    // ---- 单文件模式 (保持现有行为) ----
    string inputFile;
    app.add_option("input", inputFile, "Input .yux file");

    // ---- test 子命令 ----
    auto* testCmd = app.add_subcommand("test", "Batch test .yux files with ; check: annotations");
    string testDir;
    testCmd->add_option("dir", testDir, "Directory containing .yux test files")->required();
    bool testRecursive = false;
    testCmd->add_flag("-r,--recursive", testRecursive, "Scan subdirectories recursively");

    CLI11_PARSE(app, argc, argv);

    // ---- test 子命令分支 ----
    if (testCmd->parsed()) {
        return runCheckTest(testDir, testRecursive);
    }

    // ==== 单文件模式 (原逻辑) ====

    if (!filesystem::exists(inputFile)) {
        cerr << "Error: input file not found: " << inputFile << '\n';
        return 1;
    }

    string absPath = filesystem::absolute(inputFile).string();

    // 1. 词法 + 语法
    antlr4::ANTLRFileStream stream;
    try {
        stream.loadFromFile(absPath);
    } catch (const exception& e) {
        cerr << "Error: cannot load file " << absPath << ": " << e.what() << '\n';
        return 1;
    }

    yux::yuxLexer lexer(&stream);
    SyntaxErrorListener errListener(absPath, cerr);
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
    string moduleName = filesystem::path(absPath).stem().string();

    Yux yux;
    yux.initFileRoot(absPath);

    try {
        // 先加载 SDK (yux.core), 让用户文件经父作用域看到 String / ToString /
        // StringBuilder 等; sdk_loader::parseSdkDir 0 LLVM, 失败抛 YuxError.
        // SDK 找不到时不致命 —— 仅打印警告并继续 (退化为 yux-check 阶段 0 行为).
        string sdkPath = sdk_loader::findSdkPath();
        if (sdkPath.empty()) {
            cerr << "warning: SDK not found (yux.core 未加载); 仅做 builtin 范围内的 sema 检查" << '\n';
        } else {
            sdk_loader::parseSdkDir(sdkPath, yux);
        }

        // loadMainFile 会触发 ASTBuilder.build, 含 import 解析.
        // 若 import 失败 (找不到 SDK / 模块), 这里抛 YuxError, 直接报.
        auto file = yux.loadMainFile(absPath, moduleName);
        yux.validateSpecImpls();
        SemaPass(file, &yux).run();
    } catch (const runtime_error& e) {
        if (auto* yuxErr = dynamic_cast<const YuxError*>(&e)) {
            DiagnosticEngine::renderYuxError(cerr, absPath, *yuxErr);
        } else {
            cerr << e.what() << '\n';
        }
        return 1;
    }

    return 0;
}
