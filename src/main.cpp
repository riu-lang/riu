// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "types.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <lld/Common/Driver.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <regex>
#include <sstream>

#include "cli/format_cmd.h"
#include "cli/sdk_compile.h"
#include "cli/test_cmd.h"
#include "jit/lljit_runner.h"

#include "ast/ast_builder.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/yux.h"
#include "compiler/compiler.h"
#include "compiler/compiler_test_intrinsics.h"
#include "tools/build_cache.h"
#include "tools/pkg_cache.h"
#include "tools/diagnostic.h"
#include "tools/formatter.h"
#include "tools/format/printer.h"
#include "tools/sdk_loader.h"
#include "tools/syntax_error_listener.h"
#include "utf8.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include "CLI/CLI.hpp"
#include <toml.hpp>

LLD_HAS_DRIVER(coff)

LLD_HAS_DRIVER(elf)

LLD_HAS_DRIVER(macho)

LLD_HAS_DRIVER(wasm)

using namespace yux;
using namespace yux::cli;

std::string wstr2str(const std::wstring& wstr) {
    std::u16string u16((char16_t*)wstr.c_str());
    auto u8 = utf8::utf16tou8(u16);
    return {u8.begin(), u8.end()};
}


// findSdkPath() 已抠到 sdk_loader::findSdkPath (src/tools/sdk_loader.{h,cpp})。
// 历史 TODO(phase-C): SDK 改用 lib 链路后, 该函数应返回 SDK 项目根 (含 yux.toml),
// 而非直接给 core 目录。

void handleCrash(int signal) {
    std::cerr << "\nProgram crashed! Signal: " << signal << std::endl;
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
    app.add_flag("--jit-run", jitRun,
                 "[spike] Run input via in-process JIT (single-file only; skips obj/exe)");

    // 诊断严重度开关：允许在主命令和 build 子命令上都使用
    // --warn=<code>  把指定 code 视为 warning（仅对默认 sev <= Warning 的码生效；Error 码拒绝降级）
    // --allow=<code> 把指定 code 视为 note（同上规则）
    // --deny=<code>  把指定 code 视为 error
    // -Werror        把所有 warning 视为 error
    std::vector<std::string> warnCodes, allowCodes, denyCodes;
    bool werror = false;
    // 仅在根 app 注册一次；buildCmd 通过 fallthrough() 继承
    // expected(1) + allow_extra_args(false)：每次出现只吞 1 个值，不吃后续 positional
    app.add_option("--warn", warnCodes, "Treat code as warning (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
    app.add_option("--allow", allowCodes, "Treat code as note (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
    app.add_option("--deny", denyCodes, "Treat code as error (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
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
    testCmd->add_flag("-v,--verbose", testVerbose, "Print captured stdout/stderr for every test (default: only on failure)");
    // Phase 5：进程隔离开关
    std::string testIsolate = "none";
    testCmd->add_option("--isolate", testIsolate, "Isolation mode: none|process (default: none)")
           ->check(CLI::IsMember({"none", "process"}));
    bool testIsolateChild = false;
    auto* childOpt = testCmd->add_flag("--isolate-child", testIsolateChild, "(internal) child runner for --isolate=process");
    childOpt->group("");  // 隐藏
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
    int formatLineWidth = 0;  // 0 表示使用默认值或从 yux.toml 读取
    formatCmd->add_option("--line-width", formatLineWidth, "Line width threshold (default: 120)");
    // 只剩 ast 引擎；旧 token 流 Formatter 已删除

    CLI11_PARSE(app, argc, argv);

    // Phase 5：子进程模式 — 在任何输出前把 stdout/stderr 重定向到 capture 文件
    bool isChildIsolated = maybeApplyChildRedirect(testCmd->parsed(), testIsolateChild, testCaptureFile);

    // 把诊断 severity 开关下发到 DiagPolicy
    // applyOverride: 校验 code 已知 + 允许策略；不可降级时打印拒绝信息
    auto applyOverride = [](const std::vector<std::string>& codes, DiagSeverity newSev,
                            const char* flagName) {
        for (const auto& code : codes) {
            const auto* def = ErrorCode::lookupDefaultSeverity(code);
            if (!def) {
                std::cerr << "warning: unknown error code '" << code << "' for " << flagName
                          << " (ignored)" << std::endl;
                continue;
            }
            if (!DiagPolicy::setSeverityOverride(code, *def, newSev)) {
                // 默认 Error 的码不允许降级
                std::cerr << "warning: cannot downgrade error code '" << code
                          << "' (default severity is error); " << flagName << " ignored"
                          << std::endl;
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
        std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << std::endl;
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

    bool projectMode = buildCmd->parsed();

    Yux yux;
    string projectName;
    string projectBuildDir;
    string buildDir;

    if (projectMode) {
        if (!inputFile.empty()) {
            std::cerr << "Error: cannot combine `build` subcommand with an input file positional" << std::endl;
            return 1;
        }
        string cwd = std::filesystem::current_path().string();
        try {
            yux.initProjectFromDir(cwd);
        } catch (runtime_error& e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
        if (!buildNameArg.empty() && yux.projectName() != buildNameArg) {
            std::cerr << "Error: build target `" << buildNameArg
                      << "` does not match yux.toml name `" << yux.projectName() << "`" << std::endl;
            return 1;
        }
        if (!yux.isLibProject()) {
            if (yux.projectEntry().empty()) {
                std::cerr << "Error: yux.toml is missing `entry`" << std::endl;
                return 1;
            }
            // spec §10：entry 必须是 src/ 下的相对路径
            // E5013：绝对路径 → 直接 error；E5014：解析后逃出 sourceRoot → warning
            string tomlPath = (std::filesystem::path(yux.projectRoot()) / "yux.toml").string();
            try {
                std::filesystem::path entryPath(yux.projectEntry());
                if (entryPath.is_absolute()) {
                    throw YuxError(1, ErrorCode::E5013, yux.projectEntry());
                }
                inputFile = (std::filesystem::path(yux.sourceRoot()) / yux.projectEntry()).string();
                if (!std::filesystem::exists(inputFile)) {
                    std::cerr << "Error: entry file not found: " << inputFile << std::endl;
                    return 1;
                }
                inputFile = std::filesystem::absolute(inputFile).string();
                // 校验 canonical 解析后仍在 sourceRoot/ 子树
                std::error_code _cec;
                auto canonEntry = std::filesystem::canonical(inputFile, _cec);
                auto canonRoot  = std::filesystem::canonical(yux.sourceRoot(), _cec);
                if (!_cec) {
                    auto rel = std::filesystem::relative(canonEntry, canonRoot, _cec);
                    bool escapes = _cec || rel.empty() || rel.native().substr(0, 2) == L".." ||
                                   rel.string().substr(0, 2) == "..";
                    if (escapes) {
                        DiagnosticEngine::emit(tomlPath,
                            YuxError(1, ErrorCode::E5014, yux.projectEntry()));
                    }
                }
            } catch (runtime_error& e) {
                reportRuntimeError(tomlPath, e);
                return 1;
            }
        }
        projectName = yux.projectName();
        buildDir = getBuildDir(yux.projectRoot());
        // 项目模式不再用 <projectName>/ 子层隔离；exe / lib 直接落在 build/ 下，
        // 单文件 obj 镜像 src 相对路径到 build/<rel>.obj。
        projectBuildDir = buildDir;
    } else {
        if (inputFile.empty()) {
            std::cerr << app.help() << std::endl;
            return 1;
        }
        if (!std::filesystem::exists(inputFile)) {
            std::cerr << "Error: Input file not found: " << inputFile << std::endl;
            return 1;
        }
        inputFile = std::filesystem::absolute(inputFile).string();
        yux.initSingleFileRoot(inputFile);
        // 单文件模式：产物扁平放在 `<srcDir>/build/`，不使用项目名子目录。
        projectName = "";
        buildDir = getBuildDir(yux.projectRoot());
        projectBuildDir = buildDir;
    }

    std::cout << "Project root: " << yux.projectRoot() << std::endl;
    ensureBuildDir(buildDir);
    ensureBuildDir(projectBuildDir);

    // 单文件模式：obj/ir 写到 pid 隔离的 tmp 目录，避免多进程同时编译同一被 use 的模块时
    // 互相覆盖中间产物。exe 仍落在 projectBuildDir。链接成功后清理。
    string intermediateDir = projectBuildDir;
    string intermediateRoot;
    bool cleanupIntermediate = false;
    if (!projectMode) {
        intermediateRoot = buildDir + "/.tmp";
        intermediateDir = intermediateRoot + "/yux-" + std::to_string(GetCurrentProcessId());
        std::error_code _ec;
        std::filesystem::remove_all(intermediateDir, _ec); // 防御性：清理同 pid 残留
        ensureBuildDir(intermediateDir);
        cleanupIntermediate = true;
    }
    auto cleanupTmp = [&]() {
        if (!cleanupIntermediate) return;
        std::error_code _ec;
        std::filesystem::remove_all(intermediateDir, _ec);
        // 若 .tmp 已空，顺手删掉
        std::filesystem::remove(intermediateRoot, _ec);
    };

    // SDK 自构建（cd sdk/yux && yux build [yux]）：sdkPath 必须指向项目源里的 SDK，
    // 否则会与 findSdkPath() 返回的安装拷贝走两条路径，最终把同一批文件编译两次。
    string sdkPath;
    {
        namespace fs = std::filesystem;
        if (projectMode && yux.projectName() == "yux") {
            fs::path candidate = fs::path(yux.projectRoot()) / "src" / "yux" / "core";
            if (fs::is_directory(candidate)) {
                sdkPath = candidate.string();
            }
        }
        if (sdkPath.empty()) sdkPath = sdk_loader::findSdkPath();
    }
    // SDK 静态库路径：放在 sdk 目录下的 build 中（不放用户项目）
    string sdkLibPath;
    bool compiled = false;

    if (!sdkPath.empty()) {
        namespace fs = std::filesystem;
        sdkPath = fs::absolute(sdkPath).string();
        SdkPaths sp = sdkBuildPaths(sdkPath);
        string sdkObjPath = sp.objPath;
        sdkLibPath = sp.libPath;

        bool libExists = fs::exists(sdkLibPath);
        bool needCompile = !libExists || needRecompileSdkDir(sdkPath, sdkObjPath);
        if (needCompile) {
            SdkLock sdkLock;
            sdkLock.tryLock();
            needCompile = !fs::exists(sdkLibPath) || needRecompileSdkDir(sdkPath, sdkObjPath);
            if (needCompile) {
                auto sdkIrr = compileSdkDir(sdkPath, yux);
                auto sdkModule = sdkIrr.module.get();

                if (emitIr) {
                    string sdkIrPath = sp.irPath;
                    std::error_code ec;
                    llvm::raw_fd_ostream irFile(sdkIrPath, ec);
                    if (!ec) {
                        sdkModule->print(irFile, nullptr);
                        irFile.flush();
                        std::cout << "Write SDK IR: " << sdkIrPath << std::endl;
                    }
                }

                if (!compileIRToObj(sdkModule, sdkObjPath)) {
                    std::cerr << "Failed to compile SDK to object file" << std::endl;
                    return 1;
                }
                std::cout << "Write SDK obj: " << sdkObjPath << std::endl;

                // 用 lld-link /lib 打包 obj 为静态库
                string libOutArg = "/out:" + sdkLibPath;
                std::vector<const char*> libArgs = {
                    "lld-link", "/lib", sdkObjPath.c_str(), libOutArg.c_str()
                };
                std::string libOutStr, libErrStr;
                llvm::raw_string_ostream libOOS(libOutStr), libEOS(libErrStr);
                lld::DriverDef libDD = {lld::WinLink, &lld::coff::link};
                auto libR = lldMain(libArgs, libOOS, libEOS, llvm::ArrayRef{libDD});
                if (libR.retCode) {
                    llvm::errs() << libErrStr;
                    std::cerr << "Failed to archive SDK lib" << std::endl;
                    return 1;
                }
                std::cout << "Write SDK lib: " << sdkLibPath << std::endl;
                compiled = true;
            } else {
                parseSdkDirOrExit(sdkPath, yux);
            }
        } else {
            parseSdkDirOrExit(sdkPath, yux);
        }
    }

    // SDK 自构建：上面 compileSdkDir → core.obj → yux.lib 的产物已经就是项目目标 lib，
    // 路径与 lib 模式下 `<projectRoot>/build/yux/yux.lib` 一致。再走 lib 走法会把同一批
    // 源文件以 isSdk=false 重新编译一次（且会与已注册到 _sdkFile 的模块名冲突），
    // 因此这里直接收尾退出。
    if (projectMode && yux.projectName() == "yux") {
        if (!compiled) std::cout << "no work to do." << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(0);
    }

    auto codegenTo = [&](p<FileNode> file, const std::string& moduleName,
                         const std::string& objOut, const std::string& irOut) -> bool {
        std::cout << "Compile IR... (module: " << moduleName << ")" << std::endl;
        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
        llvm::IRBuilder<> builder(*ctx);
        try {
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, false);
            compiler.compile(file);
        } catch (runtime_error& e) {
            // 通过模块名查回源文件路径（Yux::modulePath 维护映射）
            string srcPath = yux.modulePath(moduleName);
            reportRuntimeError(srcPath, e);
            return false;
        }

        if (emitIr) {
            std::error_code ec;
            llvm::raw_fd_ostream irFile(irOut, ec);
            if (ec) {
                std::cerr << "Error opening IR file: " << ec.message() << std::endl;
            } else {
                mod->print(irFile, nullptr);
                irFile.flush();
                std::cout << "Write IR ok: " << irOut << std::endl;
            }
        }

        if (!compileIRToObj(mod.get(), objOut)) {
            std::cerr << "Failed to compile IR to object file: " << objOut << std::endl;
            return false;
        }
        std::cout << "Write obj: " << objOut << std::endl;
        return true;
    };

    // ====== lib 模式：递归扫描 src/ + 静态库 ======
    if (yux.isLibProject()) {
        namespace fs = std::filesystem;
        fs::path srcDir(yux.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: lib project missing `src/` directory at " << srcDir.string() << std::endl;
            return 1;
        }
        // 递归扫 src/ 下 .yux；模块名 = src 下相对路径，点分（不加项目名前缀）
        vector<std::pair<std::string, std::string>> libFiles; // {abs, modName}
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(srcDir, walkEc);
             it != fs::recursive_directory_iterator(); ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            auto& p = it->path();
            if (p.extension() != ".yux") continue;
            // 跳过 *.test.yux —— 测试文件仅由 `yux test` 子命令处理（spec §11.3.3.2）
            {
                auto fname = p.filename().string();
                if (fname.size() >= 9 && fname.compare(fname.size() - 9, 9, ".test.yux") == 0) {
                    continue;
                }
            }
            auto rel = fs::relative(p, srcDir);
            string modName = rel.generic_string();
            modName = modName.substr(0, modName.size() - 4); // strip .yux
            for (auto& c : modName) if (c == '/' || c == '\\') c = '.';
            libFiles.emplace_back(fs::absolute(p).string(), modName);
        }
        std::sort(libFiles.begin(), libFiles.end());

        // 加载所有 AST
        for (auto& [abs, mn] : libFiles) {
            try {
                yux.loadMainFile(abs, mn);
            } catch (runtime_error& e) {
                reportRuntimeError(abs, e, mn + ": ");
                return 1;
            }
        }

        // 各模块 codegen — 文件级聚合：单个文件失败不立即退出，继续编译其余文件，最终再决定是否链接
        vector<std::string> libObjs;
        bool anyCodegenError = false;
        PkgCacheRegistry libCaches(yux.projectRoot(), buildDir);
        for (auto& [abs, mn] : libFiles) {
            auto file = yux.module(mn);
            if (!file) continue;
            string base = mirroredOutputBase(yux.projectRoot(), buildDir, abs);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = base + ".ll";
            if (!libCaches.isFresh(abs, obj)) {
                if (!codegenTo(file, mn, obj, ir)) {
                    anyCodegenError = true;
                    continue; // 跳过 cache 更新与 obj 收集；继续下一个模块
                }
                libCaches.mark(abs);
                compiled = true;
            }
            libObjs.push_back(obj);
        }
        libCaches.flushAll();
        if (anyCodegenError) {
            return 1;
        }

        // 链接为静态库
        string libPath = projectBuildDir + "/" + projectName + ".lib";
        bool needLib = !fs::exists(libPath);
        if (!needLib) {
            try {
                auto t = fs::last_write_time(libPath);
                for (auto& o : libObjs) {
                    if (fs::last_write_time(o) > t) { needLib = true; break; }
                }
            } catch (...) { needLib = true; }
        }
        if (needLib) {
            string outArg = "/out:" + libPath;
            vector<const char*> args = {"lld-link", "/lib", outArg.c_str()};
            for (auto& o : libObjs) args.push_back(o.c_str());

            std::string outStr, errStr;
            llvm::raw_string_ostream oOS(outStr), eOS(errStr);
            std::cout << "Static lib: " << libPath << std::endl;
            lld::DriverDef dd = {lld::WinLink, &lld::coff::link};
            lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
            if (r.retCode) {
                llvm::errs() << errStr;
                return 1;
            }
            compiled = true;
        }

        if (!compiled) std::cout << "no work to do." << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(0);
    }

    // ====== exe 模式：原流程 ======
    std::string baseName = llvm::sys::path::stem(inputFile).str();
    // 项目模式：obj 镜像 src 相对路径到 build/<rel>.obj；
    // 单文件模式：仍走 intermediateDir（pid 隔离的 .tmp，已跳过缓存）。
    std::string objPath = projectMode
        ? mirroredOutputBase(yux.projectRoot(), buildDir, std::filesystem::absolute(inputFile).string()) + ".obj"
        : intermediateDir + "/" + baseName + ".obj";
    if (projectMode) {
        std::filesystem::create_directories(std::filesystem::path(objPath).parent_path());
    }

    p<FileNode> mainFile = nullptr;
    try {
        mainFile = yux.loadMainFile(inputFile, baseName);
    } catch (runtime_error& e) {
        reportRuntimeError(inputFile, e);
        return 1;
    }

    // [Phase 1 spike] --jit-run：把主模块 + 用户导入模块 IR 直接送入 LLJIT 执行。
    // 不写 obj、不调 LLD；sdk 通过预编译的 core.obj 装载。
    if (jitRun) {
        if (projectMode) {
            std::cerr << "Error: --jit-run only supports single-file mode in Phase 1 spike\n";
            return 1;
        }
        auto buildIR = [&](p<FileNode> file, const std::string& moduleName)
            -> std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>> {
            auto ctx = std::make_unique<llvm::LLVMContext>();
            auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
            llvm::IRBuilder<> builder(*ctx);
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, false);
            compiler.compile(file);
            return {std::move(mod), std::move(ctx)};
        };

        // ctx 声明在 mod 之前：栈展开时 mod 先析构、ctx 后析构，
        // 满足 LLVM "Module 必须先于其 Context 销毁" 的约束（BUG#2 同因）。
        std::unique_ptr<llvm::LLVMContext> mainCtx;
        std::unique_ptr<llvm::Module> mainMod;
        try {
            auto pr = buildIR(mainFile, baseName);
            mainMod = std::move(pr.first);
            mainCtx = std::move(pr.second);
        } catch (runtime_error& e) {
            reportRuntimeError(inputFile, e);
            return 1;
        }

        std::vector<std::unique_ptr<llvm::LLVMContext>> extraCtxs;
        std::vector<std::unique_ptr<llvm::Module>> extraMods;
        for (auto& modName : yux.loadOrder()) {
            auto modFile = yux.module(modName);
            if (!modFile || modFile == yux.sdkFile()) continue;
            try {
                auto pr = buildIR(modFile, modName);
                extraMods.push_back(std::move(pr.first));
                extraCtxs.push_back(std::move(pr.second));
            } catch (runtime_error& e) {
                std::string mp = yux.modulePath(modName);
                reportRuntimeError(mp, e, modName + ": ");
                return 1;
            }
        }

        std::string sdkObjPath;
        if (!sdkPath.empty()) {
            sdkObjPath = sdkBuildPaths(sdkPath).objPath;
        }

        int rc = jit::runViaJIT(std::move(mainMod), std::move(mainCtx),
                           extraMods, extraCtxs, sdkObjPath);
        std::cout << "[jit-run] exit code = " << rc << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(rc);
    }

    // 文件级聚合：主模块与各导入模块逐个 codegen，单文件失败不立即退出，继续编译其余文件
    bool anyCodegenError = false;

    // 项目模式：包级缓存（每目录一份 <dirname>.cache，含编译器指纹）。
    // 单文件模式：不使用缓存（中间产物在 pid tmp 目录，每次重编）。
    PkgCacheRegistry exeCaches(yux.projectRoot(), buildDir);

    // 主模块。
    std::string mainAbs = std::filesystem::absolute(inputFile).string();
    bool needCompile = !projectMode || !exeCaches.isFresh(mainAbs, objPath);
    if (needCompile) {
        std::string irPath = projectMode
            ? mirroredOutputBase(yux.projectRoot(), buildDir, mainAbs) + ".ll"
            : intermediateDir + "/" + baseName + ".ll";
        if (!codegenTo(mainFile, baseName, objPath, irPath)) {
            anyCodegenError = true;
        } else {
            if (projectMode) exeCaches.mark(mainAbs);
            compiled = true;
        }
    }

    // 导入的用户模块
    std::vector<std::string> modObjPaths;
    for (auto& modName : yux.loadOrder()) {
        auto modFile = yux.module(modName);
        if (!modFile || modFile == yux.sdkFile()) continue;
        std::string modSrc = yux.modulePath(modName);
        std::string modBase = projectMode
            ? mirroredOutputBase(yux.projectRoot(), buildDir, modSrc)
            : moduleOutputBase(intermediateDir, projectName, modName);
        std::filesystem::create_directories(std::filesystem::path(modBase).parent_path());
        std::string modObj = modBase + ".obj";
        std::string modIr = modBase + ".ll";
        if (!projectMode || !exeCaches.isFresh(modSrc, modObj)) {
            if (!codegenTo(modFile, modName, modObj, modIr)) {
                anyCodegenError = true;
                continue; // 继续尝试下一个模块的 codegen
            }
            if (projectMode) exeCaches.mark(modSrc);
            compiled = true;
        }
        modObjPaths.push_back(modObj);
    }
    if (projectMode) exeCaches.flushAll();

    // 任一模块（含主模块）codegen 失败：跳过链接，统一非零退出
    if (anyCodegenError) {
        cleanupTmp();
        std::cout.flush();
        std::cerr.flush();
        return 1;
    }

    // 项目模式 exe 使用 yux.toml 的 name；单文件模式用源文件 basename。
    std::string exeStem = projectMode ? projectName : baseName;
    std::string exePath = projectBuildDir + "/" + exeStem + ".exe";

    bool needLink = !projectMode || !std::filesystem::exists(exePath);
    if (!needLink) {
        try {
            auto exeTime = std::filesystem::last_write_time(exePath);
            if (std::filesystem::last_write_time(objPath) > exeTime) {
                needLink = true;
            }
            if (!sdkLibPath.empty() &&
                std::filesystem::last_write_time(sdkLibPath) > exeTime) {
                needLink = true;
            }
            for (auto& mo : modObjPaths) {
                if (std::filesystem::last_write_time(mo) > exeTime) {
                    needLink = true;
                    break;
                }
            }
        } catch (const std::exception& e) {
            needLink = true;
        }
    }

    if (needLink) {
        auto exeOut = "/out:" + exePath;

        std::vector<const char*> args = {
            "lld-link",
            objPath.c_str(),
            exeOut.c_str(),
            "/subsystem:console",
            "/entry:mainStartup",
            "kernel32.lib"
        };

        if (!sdkLibPath.empty()) {
            args.insert(args.begin() + 2, sdkLibPath.c_str());
        }
        for (auto& mo : modObjPaths) {
            args.insert(args.begin() + 2, mo.c_str());
        }

        std::string stdoutStr, stderrStr;
        llvm::raw_string_ostream stdoutOS(stdoutStr), stderrOS(stderrStr);

        std::cout << "Link obj: " << exePath << std::endl;
        lld::DriverDef driverDef = {lld::WinLink, &lld::coff::link};
        lld::Result result = lldMain(args, stdoutOS, stderrOS, llvm::ArrayRef{driverDef});

        if (result.retCode) {
            llvm::errs() << stderrStr;
            cleanupTmp();
            return 1;
        }
        compiled = true;
    }

    if (!compiled) {
        std::cout << "no work to do." << std::endl;
    }

    cleanupTmp();
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
