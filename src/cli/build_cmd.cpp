// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "build_cmd.h"

#include "jit/lljit_runner.h"
#include "sdk_compile.h"

#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/yux.h"
#include "compiler/compiler.h"
#include "tools/diagnostic.h"
#include "tools/pkg_cache.h"
#include "tools/sdk_loader.h"
#include "types.h"

#include <lld/Common/Driver.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

LLD_HAS_DRIVER(coff)

namespace yux::cli {

int runBuildCommand(const BuildCmdOptions& opts) {
    bool projectMode = opts.projectMode;
    bool emitIr = opts.emitIr;
    bool jitRun = opts.jitRun;
    const std::string& buildNameArg = opts.buildNameArg;
    const std::string& emitIrDir = opts.emitIrDir;
    std::string inputFile = opts.inputFile;

    Yux yux;
    string projectName;
    string projectBuildDir;
    string buildDir;

    if (projectMode) {
        if (!inputFile.empty()) {
            std::cerr << "Error: cannot combine `build` subcommand with an input file positional" << '\n';
            return 1;
        }
        string cwd = std::filesystem::current_path().string();
        try {
            yux.initProjectFromDir(cwd);
        } catch (runtime_error& e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
        if (!buildNameArg.empty() && yux.projectName() != buildNameArg) {
            std::cerr << "Error: build target `" << buildNameArg << "` does not match yux.toml name `"
                      << yux.projectName() << "`" << '\n';
            return 1;
        }
        if (!yux.isLibProject()) {
            if (yux.projectEntry().empty()) {
                std::cerr << "Error: yux.toml is missing `entry`" << '\n';
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
                    std::cerr << "Error: entry file not found: " << inputFile << '\n';
                    return 1;
                }
                inputFile = std::filesystem::absolute(inputFile).string();
                // 校验 canonical 解析后仍在 sourceRoot/ 子树
                std::error_code _cec;
                auto canonEntry = std::filesystem::canonical(inputFile, _cec);
                auto canonRoot = std::filesystem::canonical(yux.sourceRoot(), _cec);
                if (!_cec) {
                    auto rel = std::filesystem::relative(canonEntry, canonRoot, _cec);
                    bool escapes =
                        _cec || rel.empty() || rel.native().starts_with(L"..") || rel.string().starts_with("..");
                    if (escapes) {
                        DiagnosticEngine::emit(tomlPath, YuxError(1, ErrorCode::E5014, yux.projectEntry()));
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
            std::cerr << opts.helpText << '\n';
            return 1;
        }
        if (!std::filesystem::exists(inputFile)) {
            std::cerr << "Error: Input file not found: " << inputFile << '\n';
            return 1;
        }
        inputFile = std::filesystem::absolute(inputFile).string();
        yux.initSingleFileRoot(inputFile);
        // 单文件模式：产物扁平放在 `<srcDir>/build/`，不使用项目名子目录。
        projectName = "";
        buildDir = getBuildDir(yux.projectRoot());
        projectBuildDir = buildDir;
    }

    std::cout << "Project root: " << yux.projectRoot() << '\n';
    ensureBuildDir(buildDir);
    ensureBuildDir(projectBuildDir);

    // IR 输出目录：默认 build/，可通过 --emit-ir-dir 覆盖
    std::string irDir = emitIrDir.empty() ? buildDir : emitIrDir;
    if (emitIr) ensureBuildDir(irDir);

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

    // SDK 符号表加载（所有项目都需要，仅解析不编译）
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
    string sdkLibPath;
    bool isSdkSelfBuild = projectMode && yux.projectName() == "yux" && !sdkPath.empty();

    if (!sdkPath.empty()) {
        namespace fs = std::filesystem;
        sdkPath = fs::absolute(sdkPath).string();
        // SDK 静态库路径推导：sdkRoot = sdkPath 向上到含 yux.toml 的目录
        fs::path sdkRoot = fs::path(sdkPath).parent_path().parent_path().parent_path(); // src/yux/core → sdk/yux
        sdkLibPath = (sdkRoot / "build" / "yux.lib").string();

        // 解析 SDK 源码获取符号表（_sdkFile + 各模块 AST）
        try {
            sdk_loader::parseSdkDir(sdkPath, yux);
        } catch (std::runtime_error& e) {
            reportRuntimeError(sdkPath, e, "Error in SDK: ");
            return 1;
        }
    }

    bool compiled = false;

    auto codegenTo = [&](p<FileNode> file, const std::string& moduleName, const std::string& objOut,
                         const std::string& irOut, bool isSdk = false) -> bool {
        std::cout << "Compile IR... (module: " << moduleName << ")" << '\n';
        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
        llvm::IRBuilder<> builder(*ctx);
        try {
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, isSdk);
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
                std::cerr << "Error opening IR file: " << ec.message() << '\n';
            } else {
                mod->print(irFile, nullptr);
                irFile.flush();
                std::cout << "Write IR ok: " << irOut << '\n';
            }
        }

        if (!compileIRToObj(mod.get(), objOut)) {
            std::cerr << "Failed to compile IR to object file: " << objOut << '\n';
            return false;
        }
        std::cout << "Write obj: " << objOut << '\n';
        return true;
    };

    // ====== lib 模式：递归扫描 src/ + 静态库 ======
    if (yux.isLibProject()) {
        namespace fs = std::filesystem;
        fs::path srcDir(yux.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: lib project missing `src/` directory at " << srcDir.string() << '\n';
            return 1;
        }
        // 递归扫 src/ 下 .yux；模块名 = src 下相对路径，点分（不加项目名前缀）
        vector<std::pair<std::string, std::string>> libFiles; // {abs, modName}
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(srcDir, walkEc); it != fs::recursive_directory_iterator();
             ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            auto& p = it->path();
            if (p.extension() != ".yux") continue;
            // 跳过 *.test.yux —— 测试文件仅由 `yux test` 子命令处理（spec §11.3.3.2）
            {
                auto fname = p.filename().string();
                if (fname.size() >= 9 && fname.ends_with(".test.yux")) {
                    continue;
                }
            }
            auto rel = fs::relative(p, srcDir);
            string modName = rel.generic_string();
            modName = modName.substr(0, modName.size() - 4); // strip .yux
            for (auto& c : modName)
                if (c == '/' || c == '\\') c = '.';
            libFiles.emplace_back(fs::absolute(p).string(), modName);
        }
        std::ranges::sort(libFiles);

        // 加载所有 AST（SDK 自构建时 parseSdkDir 已加载，跳过重复解析）
        if (!isSdkSelfBuild) {
            for (auto& [abs, mn] : libFiles) {
                try {
                    yux.loadMainFile(abs, mn);
                } catch (runtime_error& e) {
                    reportRuntimeError(abs, e, mn + ": ");
                    return 1;
                }
            }
        }

        // 各模块 codegen — 文件级聚合：单个文件失败不立即退出，继续编译其余文件，最终再决定是否链接
        vector<std::string> libObjs;
        bool anyCodegenError = false;
        PkgCacheRegistry libCaches(yux.projectRoot(), buildDir);

        // SDK 自构建：编译列表来自 parseSdkDir 已加载的模块，读 pkg 文件确定 runtime base
        std::map<std::string, SdkPkgEntry> sdkPkgMap;
        if (isSdkSelfBuild) {
            sdkPkgMap = sdk_loader::readSdkPkg(sdkPath);
        }

        // 构建编译列表：SDK 自构建用 yux.files()，普通 lib 用 libFiles
        vector<std::pair<std::string, std::string>> compileList; // {abs, modName}
        if (isSdkSelfBuild) {
            for (auto& file : yux.files()) {
                if (file == yux.sdkFile()) continue;
                string mn = file->moduleName();
                string abs = yux.modulePath(mn);
                if (abs.empty()) continue;
                compileList.emplace_back(abs, mn);
            }
            std::ranges::sort(compileList);
        } else {
            compileList = libFiles;
        }

        for (auto& [abs, mn] : compileList) {
            auto file = yux.module(mn);
            if (!file) continue;
            string base = mirroredOutputBase(yux.projectRoot(), buildDir, abs);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = mirroredOutputBase(yux.projectRoot(), irDir, abs) + ".ll";
            if (emitIr) {
                fs::create_directories(fs::path(ir).parent_path());
            }
            // SDK 自构建：runtime base 模块需发射运行时辅助
            bool isSdkRuntime = false;
            if (isSdkSelfBuild) {
                auto stem = fs::path(abs).stem().string();
                auto it = sdkPkgMap.find(stem);
                bool isFlatDep = (it == sdkPkgMap.end()) || it->second.isFlat;
                isSdkRuntime = isFlatDep && (stem == "base");
            }

            if (!libCaches.isFresh(abs, obj)) {
                if (!codegenTo(file, mn, obj, ir, isSdkRuntime)) {
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
                    if (fs::last_write_time(o) > t) {
                        needLib = true;
                        break;
                    }
                }
            } catch (...) {
                needLib = true;
            }
        }
        if (needLib) {
            string outArg = "/out:" + libPath;
            vector<const char*> args = {"lld-link", "/lib", outArg.c_str()};
            for (auto& o : libObjs)
                args.push_back(o.c_str());

            std::string outStr, errStr;
            llvm::raw_string_ostream oOS(outStr), eOS(errStr);
            std::cout << "Static lib: " << libPath << '\n';
            lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
            lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
            if (r.retCode) {
                llvm::errs() << errStr;
                return 1;
            }
            compiled = true;
        }

        if (!compiled) std::cout << "no work to do." << '\n';

        // ====== test 模式（lib 项目：每个 .test.yux → 独立 test exe） ======
        if (opts.testMode) {
            // 构建 test 专用产物目录
            std::string testsDir = buildDir + "/tests";
            std::string testsObjDir = testsDir + "/obj";
            ensureBuildDir(testsDir);
            ensureBuildDir(testsObjDir);

            // 递归扫 src/ 下 *.test.yux（test 模式不再跳过 .test.yux）
            std::vector<std::pair<std::string, std::string>> testFiles; // {abs, modName}
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(srcDir, ec); it != fs::recursive_directory_iterator();
                 ++it) {
                if (ec) break;
                if (!it->is_regular_file()) continue;
                auto& p = it->path();
                if (p.extension() != ".yux") continue;
                auto fname = p.filename().string();
                if (!(fname.size() >= 9 && fname.ends_with(".test.yux"))) continue;
                auto rel = fs::relative(p, srcDir);
                std::string modName = rel.generic_string();
                modName = modName.substr(0, modName.size() - 4); // strip .yux
                for (auto& c : modName)
                    if (c == '/' || c == '\\') c = '.';
                testFiles.emplace_back(fs::absolute(p).string(), modName);
            }
            std::ranges::sort(testFiles);

            // --test-mod 过滤：只编译指定模块的测试
            // 模块名如 yux.core.array.test，去掉末尾 .test 后与 --test-mod 值比对
            if (!opts.testMod.empty()) {
                std::vector<std::pair<std::string, std::string>> filtered;
                for (auto& [abs, mod] : testFiles) {
                    std::string base = mod;
                    // 去掉 trailing ".test"
                    if (base.size() > 5 && base.substr(base.size() - 5) == ".test") {
                        base = base.substr(0, base.size() - 5);
                    }
                    if (base == opts.testMod || mod == opts.testMod) {
                        filtered.emplace_back(abs, mod);
                    }
                }
                if (filtered.empty()) {
                    std::cerr << "Error: no test file matches --test-mod " << opts.testMod << "\n";
                }
                testFiles = std::move(filtered);
            }

            // 扫描模块名用于依赖收集
            // （libFiles 已包含所有非 test 的 .yux）
            std::map<std::string, std::string> allObjMap;  // modName → obj path
            for (auto& [abs, mn] : libFiles) {
                std::string obj = mirroredOutputBase(yux.projectRoot(), buildDir, abs) + ".obj";
                allObjMap[mn] = obj;
            }

            // 测试 obj 缓存（与 lib 缓存隔离：testsObjDir 下独立 .cache 文件）
            PkgCacheRegistry testCaches(yux.projectRoot(), testsObjDir);

            size_t testExeCount = 0;
            for (auto& [testAbs, testMod] : testFiles) {
                // 解析测试文件 AST
                std::string testModName = testMod;
                try {
                    yux.loadMainFile(testAbs, testModName);
                } catch (std::runtime_error& e) {
                    reportRuntimeError(testAbs, e, testModName + ": ");
                    continue;
                }

                // Codegen 测试模块（isTestDll=true）
                auto testFile = yux.module(testModName);
                if (!testFile) continue;

                std::string testObj = mirroredOutputBase(yux.projectRoot(), testsObjDir, testAbs) + ".obj";
                fs::create_directories(fs::path(testObj).parent_path());

                // 缓存检查：test obj 新鲜则跳过 codegen
                if (!testCaches.isFresh(testAbs, testObj)) {
                    auto tCtx = std::make_unique<llvm::LLVMContext>();
                    auto tMod = std::make_unique<llvm::Module>(testModName, *tCtx);
                    llvm::IRBuilder<> tBuilder(*tCtx);
                    try {
                        Compiler compiler(*tCtx, tBuilder, tMod.get(), testFile, &yux, false, true); // isTestDll=true
                        compiler.compile(testFile);
                    } catch (std::runtime_error& e) {
                        reportRuntimeError(testAbs, e, testModName + ": ");
                        continue;
                    }

                    if (emitIr) {
                        std::string testIr = mirroredOutputBase(yux.projectRoot(), irDir, testAbs) + ".ll";
                        fs::create_directories(fs::path(testIr).parent_path());
                        std::error_code ec;
                        llvm::raw_fd_ostream irFile(testIr, ec);
                        if (ec) {
                            std::cerr << "Error opening test IR file: " << ec.message() << '\n';
                        } else {
                            tMod->print(irFile, nullptr);
                            irFile.flush();
                            std::cout << "Write test IR: " << testIr << '\n';
                        }
                    }

                    if (!compileIRToObj(tMod.get(), testObj)) {
                        std::cerr << "Error: failed to compile test IR: " << testObj << '\n';
                        continue;
                    }
                    std::cout << "Write test obj: " << testObj << '\n';
                    testCaches.mark(testAbs);
                }

                // 收集依赖 obj（从 yux.loadOrder — 测试模块引用的依赖）
                std::vector<std::string> linkObjs = {testObj};
                std::set<std::string> seenMods;
                for (auto& depMod : yux.loadOrder()) {
                    if (seenMods.count(depMod)) continue;
                    seenMods.insert(depMod);
                    auto it = allObjMap.find(depMod);
                    if (it != allObjMap.end()) {
                        linkObjs.push_back(it->second);
                    }
                }
                // SDK lib
                if (!sdkLibPath.empty()) linkObjs.push_back(sdkLibPath);

                // LLD 链接 test dll
                // 替换模块名中的 '.' 为 '_'（Windows DLL 路径）
                std::string dllFileName = testMod;
                for (auto& c : dllFileName)
                    if (c == '.') c = '_';
                std::string testDllPath = testsDir;
                testDllPath += "/";
                testDllPath += dllFileName;
                testDllPath += ".test.dll";

                // 检查 DLL 是否需要重新链接（DLL 比所有输入 obj 都新则跳过）
                bool needLink = true;
                if (fs::exists(testDllPath)) {
                    needLink = false;
                    try {
                        auto dllTime = fs::last_write_time(testDllPath);
                        for (auto& o : linkObjs) {
                            if (fs::exists(o) && fs::last_write_time(o) > dllTime) {
                                needLink = true;
                                break;
                            }
                        }
                    } catch (...) {
                        needLink = true;
                    }
                }

                if (needLink) {
                    std::string dllOut = "/out:" + testDllPath;
                    // DLL 模式：/dll，无需 /entry /subsystem /kernel32.lib
                    std::vector<const char*> linkArgs = {"lld-link", dllOut.c_str(), "/dll", "/noentry", "kernel32.lib"};
                    for (auto& o : linkObjs)
                        linkArgs.insert(linkArgs.begin() + 1, o.c_str());

                    std::string outStr, errStr;
                    llvm::raw_string_ostream oOS(outStr), eOS(errStr);
                    std::cout << "Link test dll: " << testDllPath << '\n';
                    lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
                    lld::Result r = lldMain(linkArgs, oOS, eOS, llvm::ArrayRef{dd});
                    if (r.retCode) {
                        std::cerr << "Error: test dll link failed for " << testMod << "\n" << errStr;
                        continue;
                    }
                }
                ++testExeCount;
            }
            testCaches.flushAll();
            if (testExeCount > 0) {
                std::cout << "Built " << testExeCount << " test dll(s) into " << testsDir << '\n';
            } else if (!testFiles.empty()) {
                std::cout << "no test dll built (all failed)\n";
            } else if (opts.testMod.empty()) {
                std::cout << "no *.test.yux files found\n";
            }
            // --test-mod 无匹配时已在上面输出 stderr，不再重复 stdout
        }

        std::cout.flush();
        std::cerr.flush();
        _exit(0);
    }

    // ====== exe 模式：原流程 ======
    std::string baseName = llvm::sys::path::stem(inputFile).str();
    // 项目模式：obj 镜像 src 相对路径到 build/<rel>.obj；
    // 单文件模式：仍走 intermediateDir（pid 隔离的 .tmp，已跳过缓存）。
    std::string objPath =
        projectMode
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

        std::string sdkObjDir;
        if (!sdkPath.empty()) {
            // sdkPath = <sdkRoot>/src/yux/core, obj 产物在 <sdkRoot>/build/src/yux/core/
            namespace fs = std::filesystem;
            auto sdkRootPath = fs::path(sdkPath).parent_path().parent_path().parent_path();
            sdkObjDir = (sdkRootPath / "build" / "src" / "yux" / "core").string();
        }

        int rc = jit::runViaJIT(std::move(mainMod), std::move(mainCtx), extraMods, extraCtxs, sdkObjDir);
        std::cout << "[jit-run] exit code = " << rc << '\n';
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
        std::string irPath = projectMode ? mirroredOutputBase(yux.projectRoot(), irDir, mainAbs) + ".ll"
                                         : irDir + "/" + baseName + ".ll";
        if (emitIr && projectMode) {
            std::filesystem::create_directories(std::filesystem::path(irPath).parent_path());
        }
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
        std::string modBase = projectMode ? mirroredOutputBase(yux.projectRoot(), buildDir, modSrc)
                                          : moduleOutputBase(intermediateDir, projectName, modName);
        std::filesystem::create_directories(std::filesystem::path(modBase).parent_path());
        std::string modObj = modBase + ".obj";
        std::string modIr = projectMode ? mirroredOutputBase(yux.projectRoot(), irDir, modSrc) + ".ll"
                                        : moduleOutputBase(irDir, projectName, modName) + ".ll";
        if (emitIr) {
            std::filesystem::create_directories(std::filesystem::path(modIr).parent_path());
        }
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
            if (!sdkLibPath.empty() && std::filesystem::last_write_time(sdkLibPath) > exeTime) {
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

        std::vector<const char*> args = {"lld-link",           objPath.c_str(),      exeOut.c_str(),
                                         "/subsystem:console", "/entry:mainStartup", "kernel32.lib"};

        if (!sdkLibPath.empty()) {
            args.insert(args.begin() + 2, sdkLibPath.c_str());
        }
        for (auto& mo : modObjPaths) {
            args.insert(args.begin() + 2, mo.c_str());
        }

        std::string stdoutStr, stderrStr;
        llvm::raw_string_ostream stdoutOS(stdoutStr), stderrOS(stderrStr);

        std::cout << "Link obj: " << exePath << '\n';
        lld::DriverDef driverDef = {.f = lld::WinLink, .d = &lld::coff::link};
        lld::Result result = lldMain(args, stdoutOS, stderrOS, llvm::ArrayRef{driverDef});

        if (result.retCode) {
            llvm::errs() << stderrStr;
            cleanupTmp();
            return 1;
        }
        compiled = true;
    }

    if (!compiled) {
        std::cout << "no work to do." << '\n';
    }

    cleanupTmp();
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}

} // namespace yux::cli
