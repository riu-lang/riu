// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "types.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
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

#ifdef _DEBUG
#include <crtdbg.h>
#endif

using namespace riu;
using namespace riu::cli;

std::string wstr2str(const std::wstring& wstr) {
    std::u16string u16(reinterpret_cast<const char16_t*>(wstr.c_str()));
    auto u8 = utf8::utf16tou8(u16);
    return {u8.begin(), u8.end()};
}

// findSdkPath() 已抠到 sdk_loader::findSdkPath (riu/frontend/tools/sdk_loader.{h,cpp})。
// 历史 TODO(phase-C): SDK 改用 lib 链路后, 该函数应返回 SDK 项目根 (含 riu.toml),
// 而非直接给 core 目录。

void handleCrash(int signal) {
    std::cerr << "\nProgram crashed! Signal: " << signal << '\n';
    _exit(1);
}

int wmain(int argc, wchar_t* argv[]) { // NOLINT(modernize-avoid-c-arrays) Windows wmain signature
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

#ifdef _DEBUG
    // Debug CRT 默认 assert/abort 弹框；`riu test` 并行 job 会挂起且父进程仍可能当成功。
    // 改写 stderr 后走 SIGABRT → handleCrash → _exit(1)。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif

    // --version：在 CLI11 解析之前手动处理，避免 require_subcommand 冲突。
    // CLI11 仍注册同名 flag 以确保 -h 显示 --version。
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--version") {
            std::cout << "riu " RIU_VERSION "\n";
            return 0;
        }
    }

    signal(SIGSEGV, handleCrash);
    signal(SIGABRT, handleCrash);
    signal(SIGFPE, handleCrash);

    CLI::App app{"riu compiler"};
    app.require_subcommand(1);
    bool versionFlag = false;
    app.add_flag("--version", versionFlag, "Print version and exit");

    bool emitIr = false;
    app.add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

    std::string emitIrDir;
    app.add_option("--emit-ir-dir", emitIrDir, "Output directory for .ll files (default: build/)");

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

    auto* buildCmd = app.add_subcommand("build", "Build project (must run at project root containing riu.toml)");
    std::string buildNameArg;
    // name 可省略：当前每个 riu.toml 仅声明一个目标，省略时直接取 toml 的 name；显式给出则必须与之一致。
    buildCmd->add_option("name", buildNameArg, "Project name (optional; must match `name` in riu.toml when given)");
    buildCmd->add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");
    buildCmd->add_option("--emit-ir-dir", emitIrDir, "Output directory for .ll files (default: build/)");
    bool testMode = false;
    buildCmd->add_flag("--test", testMode, "Build test executables for *.test.ut into build/tests/");
    std::vector<std::string> buildTestMods;
    buildCmd->add_option("--test-mod", buildTestMods,
                         "Build only specified test module(s); repeatable (e.g. riu.core.array)");
    int buildThreads = 0;
    buildCmd->add_option("--threads", buildThreads, "Parallel test compile jobs (default: CPU cores; 1 = serial)");
    buildCmd->fallthrough(); // 允许 --warn / --allow / --deny / -Werror 在 build 子命令上使用

#ifdef _DEBUG
    buildCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    // `riu test` 子命令：riu build --test → riu-test-runner 加载 DLL 执行
    auto* testCmd =
        app.add_subcommand("test", "Run #Test functions via DLL test runner (riu-test-runner must be in PATH)");
    bool testVerbose = false;
    testCmd->add_flag("-v,--verbose", testVerbose,
                      "Print captured stdout/stderr for every test (default: only on failure)");
    int testThreads = 0;
    testCmd->add_option("--threads", testThreads, "Parallel compile and runner jobs (default: CPU cores; 1 = serial)");
    std::string testTestMod;
    testCmd->add_option("--test-mod", testTestMod,
                        "Build and run only the specified test module (e.g. riu.core.array)");
#ifdef _DEBUG
    testCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    auto* formatCmd = app.add_subcommand("format", "Format a .ut source file");
    std::string formatFile;
    formatCmd->add_option("file", formatFile, "Input .ut file to format");
    bool formatInPlace = false;
    formatCmd->add_flag("-i,--in-place", formatInPlace, "Edit file in place");
    bool formatStdin = false;
    formatCmd->add_flag("--stdin", formatStdin, "Read from stdin instead of file");
    int formatLineWidth = 0; // 0 表示使用默认值或从 riu.toml 读取
    formatCmd->add_option("--line-width", formatLineWidth, "Line width threshold (default: 120)");
    // 只剩 ast 引擎；旧 token 流 Formatter 已删除

    CLI11_PARSE(app, argc, argv);

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

    // 始终打印工作目录
    std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << '\n';
    std::cout.flush();

    // `riu test` 子命令：riu build --test → riu-test-runner 加载 DLL 执行
    if (testCmd->parsed()) {
        TestCmdOptions opts;
        opts.verbose = testVerbose;
        opts.threads = testThreads;
        opts.testMod = testTestMod;
        runTestCommand(opts);
    }

    BuildCmdOptions bopts;
    bopts.projectMode = buildCmd->parsed();
    bopts.testMode = testMode;
    bopts.testMods = std::move(buildTestMods);
    bopts.threads = buildThreads;
    bopts.emitIr = emitIr;
    bopts.emitIrDir = emitIrDir;
    bopts.buildNameArg = buildNameArg;
    int rc = runBuildCommand(bopts);
    if (rc != 0) return rc;
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
