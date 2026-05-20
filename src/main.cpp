// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "types.h"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "cli/build_cmd.h"
#include "cli/format_cmd.h"
#include "cli/test_cmd.h"

#include "tools/diagnostic.h"
#include "utf8.h"

#include "CLI/CLI.hpp"

using namespace yux;
using namespace yux::cli;

std::string wstr2str(const std::wstring& wstr) {
    std::u16string u16(reinterpret_cast<const char16_t*>(wstr.c_str()));
    auto u8 = utf8::utf16tou8(u16);
    return {u8.begin(), u8.end()};
}

// findSdkPath() 已抠到 sdk_loader::findSdkPath (src/tools/sdk_loader.{h,cpp})。
// 历史 TODO(phase-C): SDK 改用 lib 链路后, 该函数应返回 SDK 项目根 (含 yux.toml),
// 而非直接给 core 目录。

void handleCrash(int signal) {
    std::cerr << "\nProgram crashed! Signal: " << signal << '\n';
    _exit(1);
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    signal(SIGSEGV, handleCrash);
    signal(SIGABRT, handleCrash);
    signal(SIGFPE, handleCrash);

    CLI::App app{"yux compiler"};
    app.require_subcommand(0, 1);

    bool emitIr = false;
    app.add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

    // [Phase 1 spike] 在进程内 JIT 跑入口模块，绕开 obj 写盘 + LLD 链接。
    // 仅单文件模式生效；正式 `yux test` 子命令会替代它。
    bool jitRun = false;
    app.add_flag("--jit-run", jitRun, "[spike] Run input via in-process JIT (single-file only; skips obj/exe)");

    // 诊断严重度开关：允许在主命令和 build 子命令上都使用
    // --warn=<code>  把指定 code 视为 warning（仅对默认 sev <= Warning 的码生效；Error 码拒绝降级）
    // --allow=<code> 把指定 code 视为 note（同上规则）
    // --deny=<code>  把指定 code 视为 error
    // -Werror        把所有 warning 视为 error
    std::vector<std::string> warnCodes, allowCodes, denyCodes;
    bool werror = false;
    // 仅在根 app 注册一次；buildCmd 通过 fallthrough() 继承
    // expected(1) + allow_extra_args(false)：每次出现只吞 1 个值，不吃后续 positional
    app.add_option("--warn", warnCodes, "Treat code as warning (Exxxx; can repeat)")
        ->expected(1)
        ->allow_extra_args(false);
    app.add_option("--allow", allowCodes, "Treat code as note (Exxxx; can repeat)")
        ->expected(1)
        ->allow_extra_args(false);
    app.add_option("--deny", denyCodes, "Treat code as error (Exxxx; can repeat)")
        ->expected(1)
        ->allow_extra_args(false);
    app.add_flag("--Werror", werror, "Treat all warnings as errors");

#ifdef _DEBUG
    app.add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file (single-file mode)");

    auto* buildCmd = app.add_subcommand("build", "Build project (must run at project root containing yux.toml)");
    std::string buildNameArg;
    // name 可省略：当前每个 yux.toml 仅声明一个目标，省略时直接取 toml 的 name；显式给出则必须与之一致。
    buildCmd->add_option("name", buildNameArg, "Project name (optional; must match `name` in yux.toml when given)");
    buildCmd->add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");
    buildCmd->fallthrough(); // 允许 --warn / --allow / --deny / -Werror 在 build 子命令上使用

#ifdef _DEBUG
    buildCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    // `yux test [selector]` 子命令（仅项目模式；spec §11.3.4）
    // selector 形态：
    //   <prefix>             模块名前缀匹配（例：yux.core 命中 yux.core.*.test）
    //   <module>#<fnName>    精确匹配模块名 + 函数名
    auto* testCmd = app.add_subcommand("test", "Run #Test functions in *.test.yux files (project mode only)");
    std::vector<std::string> testSelectors;
    bool testVerbose = false;
    testCmd->add_option("selector", testSelectors,
                        "One or more module prefixes or `<module>#<fnName>` selectors (test matches any)");
    testCmd->add_flag("-v,--verbose", testVerbose,
                      "Print captured stdout/stderr for every test (default: only on failure)");
    // Phase 5：进程隔离开关
    std::string testIsolate = "none";
    testCmd->add_option("--isolate", testIsolate, "Isolation mode: none|process (default: none)")
        ->check(CLI::IsMember({"none", "process"}));
    bool testIsolateChild = false;
    auto* childOpt =
        testCmd->add_flag("--isolate-child", testIsolateChild, "(internal) child runner for --isolate=process");
    childOpt->group(""); // 隐藏
    std::string testCaptureFile;
    auto* capOpt = testCmd->add_option("--capture", testCaptureFile, "(internal) child capture file path");
    capOpt->group("");
    testCmd->fallthrough();
#ifdef _DEBUG
    testCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    auto* formatCmd = app.add_subcommand("format", "Format a .yux source file");
    std::string formatFile;
    formatCmd->add_option("file", formatFile, "Input .yux file to format");
    bool formatInPlace = false;
    formatCmd->add_flag("-i,--in-place", formatInPlace, "Edit file in place");
    bool formatStdin = false;
    formatCmd->add_flag("--stdin", formatStdin, "Read from stdin instead of file");
    int formatLineWidth = 0; // 0 表示使用默认值或从 yux.toml 读取
    formatCmd->add_option("--line-width", formatLineWidth, "Line width threshold (default: 120)");
    // 只剩 ast 引擎；旧 token 流 Formatter 已删除

    CLI11_PARSE(app, argc, argv);

    // Phase 5：子进程模式 — 在任何输出前把 stdout/stderr 重定向到 capture 文件
    bool isChildIsolated = maybeApplyChildRedirect(testCmd->parsed(), testIsolateChild, testCaptureFile);

    // 把诊断 severity 开关下发到 DiagPolicy
    // applyOverride: 校验 code 已知 + 允许策略；不可降级时打印拒绝信息
    auto applyOverride = [](const std::vector<std::string>& codes, DiagSeverity newSev, const char* flagName) {
        for (const auto& code : codes) {
            const auto* def = ErrorCode::lookupDefaultSeverity(code);
            if (!def) {
                std::cerr << "warning: unknown error code '" << code << "' for " << flagName << " (ignored)" << '\n';
                continue;
            }
            if (!DiagPolicy::setSeverityOverride(code, *def, newSev)) {
                // 默认 Error 的码不允许降级
                std::cerr << "warning: cannot downgrade error code '" << code << "' (default severity is error); "
                          << flagName << " ignored" << '\n';
            }
        }
    };
    applyOverride(warnCodes, DiagSeverity::Warning, "--warn");
    applyOverride(allowCodes, DiagSeverity::Note, "--allow");
    applyOverride(denyCodes, DiagSeverity::Error, "--deny");
    DiagPolicy::setWerror(werror);

    // 处理格式化命令
    if (formatCmd->parsed()) {
        FormatCmdOptions fopts;
        fopts.file = formatFile;
        fopts.inPlace = formatInPlace;
        fopts.fromStdin = formatStdin;
        fopts.lineWidth = formatLineWidth;
        return runFormatCommand(fopts);
    }

    if (!isChildIsolated) {
        std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << '\n';
    }

    // `yux test` 子命令：项目模式 #Test 收集 + LLJIT 装载 + SEH 包裹 + 子进程隔离。
    // 实现拆到 src/cli/test_cmd.cpp；runTestCommand 内部 _exit，永不返回。
    if (testCmd->parsed()) {
        TestCmdOptions opts;
        opts.selectors = testSelectors;
        opts.verbose = testVerbose;
        opts.isolate = testIsolate;
        opts.isolateChild = testIsolateChild;
        opts.captureFile = testCaptureFile;
        opts.hasPositionalInput = !inputFile.empty();
        runTestCommand(opts, isChildIsolated);
    }

    BuildCmdOptions bopts;
    bopts.projectMode = buildCmd->parsed();
    bopts.emitIr = emitIr;
    bopts.jitRun = jitRun;
    bopts.buildNameArg = buildNameArg;
    bopts.inputFile = inputFile;
    bopts.helpText = app.help();
    int rc = runBuildCommand(bopts);
    if (rc != 0) return rc;
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
