// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "test_cmd.h"

#include "jit/lljit_runner.h"
#include "sdk_compile.h"

#include "ast/mangler.h"
#include "ast/node/fn_node.h"
#include "ast/yux.h"
#include "compiler/compiler.h"
#include "compiler/compiler_test_intrinsics.h"
#include "tools/pkg_cache.h"
#include "tools/sdk_loader.h"
#include "types.h"

#include <array>

#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace yux::cli {

// SEH 包裹单次测试调用。返回 0 表示无异常; 非 0 为 GetExceptionCode()。
// 必须保持 extern "C" + 无 C++ 析构对象, 避免 clang 对 SEH + 局部对象的限制。
extern "C" unsigned long runTestSEH(void (*fn)()) noexcept {
    __try {
        fn();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

namespace {

// 把 Win32 SEH 异常码翻译成可读名字
const char* sehExceptionName(unsigned long code) {
    // yux test 自定义码: 测试断言失败 (spec §11.3.5)
    if (code == test_intrinsics::ASSERT_FAILED_CODE) return "ASSERT_FAILED";
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "ACCESS_VIOLATION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
        return "INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_OVERFLOW:
        return "FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW:
        return "FLT_UNDERFLOW";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "FLT_INVALID_OPERATION";
    case EXCEPTION_STACK_OVERFLOW:
        return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "PRIV_INSTRUCTION";
    case EXCEPTION_BREAKPOINT:
        return "BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "ARRAY_BOUNDS_EXCEEDED";
    default:
        return "UNKNOWN";
    }
}

// 把 stdout / stderr 的底层 fd 重定向到一个临时文件, stop() 时还原并读出内容。
// 用 tmpfile() (C 运行时, 自动删除) 避免 pipe 缓冲被填满后被测函数阻塞。
struct TestOutputCapture {
    int savedOut = -1;
    int savedErr = -1;
    FILE* tmp = nullptr;

    bool start() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        tmp = std::tmpfile();
        if (!tmp) return false;
        int fd = _fileno(tmp);
        savedOut = _dup(_fileno(stdout));
        savedErr = _dup(_fileno(stderr));
        if (savedOut < 0 || savedErr < 0) return false;
        if (_dup2(fd, _fileno(stdout)) < 0) return false;
        if (_dup2(fd, _fileno(stderr)) < 0) return false;
        return true;
    }

    std::string stop() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        if (savedOut >= 0) {
            _dup2(savedOut, _fileno(stdout));
            _close(savedOut);
            savedOut = -1;
        }
        if (savedErr >= 0) {
            _dup2(savedErr, _fileno(stderr));
            _close(savedErr);
            savedErr = -1;
        }
        std::string buf;
        if (tmp) {
            std::fseek(tmp, 0, SEEK_END);
            long sz = std::ftell(tmp);
            std::fseek(tmp, 0, SEEK_SET);
            if (sz > 0) {
                buf.resize(static_cast<size_t>(sz));
                size_t n = std::fread(buf.data(), 1, static_cast<size_t>(sz), tmp);
                buf.resize(n);
            }
            std::fclose(tmp);
            tmp = nullptr;
        }
        return buf;
    }
};

// Phase 5: 取当前进程 exe 全路径 (用于父进程派发子测试时的 argv[0])
std::string getSelfExePath() {
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= MAX_PATH) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    std::string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), out.data(), sz, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), sz);
    return out;
}

// Phase 5: 在 isolate=process 模式下, 给单个 #Test 起子进程跑。
// 子进程协议: `<self> test <mod>#<fn> --isolate-child --capture <tmpfile>`
// 子进程把所有 stdout/stderr 写入 capture 文件; 退出码 0=pass / SEH 码=fail / 2=child 自身错误。
struct IsolatedResult {
    unsigned long exitCode;
    std::string capture;
    bool spawnOk;
    std::string spawnError;
};
IsolatedResult spawnIsolatedTest(const std::string& exePath, const std::string& mod, const std::string& fn) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned> seq{0};
    fs::path capPath = fs::temp_directory_path() /
                       ("yuxtest_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++seq) + ".txt");

    // CreateProcessW 接收单条 cmdline; exe 路径与 capture 路径都用引号包起来防空格。
    std::string cmd =
        "\"" + exePath + "\" test \"" + mod + "#" + fn + "\" --isolate-child --capture \"" + capPath.string() + "\"";
    std::wstring wcmd = toWide(cmd);
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok =
        CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        return {.exitCode = 0,
                .capture = {},
                .spawnOk = false,
                .spawnError = "CreateProcess failed (GLE=" + std::to_string(GetLastError()) + ")"};
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string capContents;
    if (fs::exists(capPath)) {
        std::ifstream f(capPath, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        capContents = ss.str();
        f.close();
        std::error_code ec;
        fs::remove(capPath, ec);
    }
    return {.exitCode = code, .capture = std::move(capContents), .spawnOk = true, .spawnError = {}};
}

// 把捕获到的输出按行缩进打印到 std::cout, 便于在 RUN/FAIL 行下视觉归属
void printCapturedOutput(const std::string& out) {
    if (out.empty()) return;
    std::cout << "  ---- output ----\n";
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) {
            std::cout << "  | " << out.substr(pos) << "\n";
            break;
        }
        std::cout << "  | " << out.substr(pos, nl - pos) << "\n";
        pos = nl + 1;
    }
    std::cout << "  ----------------\n";
}

// 确保 SDK 已编译（非自构建项目：test 前自动 yux build）
// sdkPath: SDK 源文件目录 (.../sdk/yux/src/yux/core)
// sdkObjDir: SDK obj 产物目录 (.../sdk/yux/build/src/yux/core)
// 返回 true 表示 SDK 就绪（编译成功或已是最新），false 表示编译失败
bool ensureSdkBuilt(const std::string& sdkPath, const std::string& sdkObjDir) {
	namespace fs = std::filesystem;

	// SDK 项目根：core → src → yux → sdk/yux
	fs::path sdkRoot = fs::path(sdkPath).parent_path().parent_path().parent_path();
	std::string sdkBuildDir = (sdkRoot / "build").string();

	// 用 PkgCacheRegistry 检查每个 SDK 源文件是否需要重编
	// （基于编译器指纹 + 源文件 mtime/size，与 yux build 自身缓存一致）
	PkgCacheRegistry caches(sdkRoot.string(), sdkBuildDir);

	bool needBuild = false;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(sdkPath, ec); it != fs::recursive_directory_iterator(); ++it) {
		if (ec) break;
		if (!it->is_regular_file()) continue;
		if (it->path().extension() != ".yux") continue;
		// 跳过 .test.yux —— 不会被 yux build 编译
		auto fname = it->path().filename().string();
		if (fname.size() >= 9 && fname.ends_with(".test.yux")) continue;

		std::string srcAbs = fs::absolute(it->path()).string();
		std::string objPath = mirroredOutputBase(sdkRoot.string(), sdkBuildDir, srcAbs) + ".obj";

		if (!caches.isFresh(srcAbs, objPath)) {
			needBuild = true;
			break;
		}
	}

	// 即使 obj 都是新的，也要确保 .lib 存在（可能上次链接失败或被误删）
	if (!needBuild) {
		fs::path libPath = fs::path(sdkBuildDir) / "yux.lib";
		if (!fs::exists(libPath)) needBuild = true;
	}

	if (!needBuild) return true;

	// 需要编译 SDK：spawn yux build 子进程
	std::string self = getSelfExePath();
	if (self.empty()) {
		std::cerr << "Error: failed to resolve yux executable path for SDK build" << '\n';
		return false;
	}

	std::string cmd = "\"" + self + "\" build";
	std::wstring wcmd = toWide(cmd);
	std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
	cmdBuf.push_back(0);

	std::wstring wSdkRoot = toWide(sdkRoot.string());

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	PROCESS_INFORMATION pi{};

	std::cout << "Building SDK (" << sdkRoot.string() << ")..." << '\n';

	BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
	                         wSdkRoot.c_str(), &si, &pi);
	if (!ok) {
		std::cerr << "Error: failed to spawn yux build for SDK (GLE=" << GetLastError() << ")" << '\n';
		return false;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (code != 0) {
		std::cerr << "Error: SDK build failed (exit code " << code << ")" << '\n';
		return false;
	}
	return true;
}

} // namespace

bool maybeApplyChildRedirect(bool testCmdParsed, bool isolateChild, const std::string& captureFile) {
    bool isChildIsolated = testCmdParsed && isolateChild;
    if (!isChildIsolated) return false;
    if (captureFile.empty()) {
        std::cerr << "Error: --isolate-child requires --capture <path>" << '\n';
        std::exit(2);
    }
    FILE* cap = std::fopen(captureFile.c_str(), "wb");
    if (!cap) {
        std::cerr << "Error: cannot open capture file: " << captureFile << '\n';
        std::exit(2);
    }
    std::fflush(stdout);
    std::fflush(stderr);
    int fd = _fileno(cap);
    _dup2(fd, _fileno(stdout));
    _dup2(fd, _fileno(stderr));
    // 不缓冲: 避免子进程异常退出时父进程读到截断输出
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    return true;
}

[[noreturn]] void runTestCommand(const TestCmdOptions& opts, bool isChildIsolated) {
    namespace fs = std::filesystem;
    auto suiteT0 = std::chrono::steady_clock::now();
    if (opts.hasPositionalInput) {
        std::cerr << "Error: `yux test` does not accept positional input file" << '\n';
        std::exit(1);
    }
    // 解析 selector: 每项形如 `<prefix>` 或 `<module>#<fnName>`;
    // 多个 selector 之间「任一命中即收」。空列表 = 全收。
    // 子进程模式 (--isolate-child) 父进程派发时永远只传 1 个 `<mod>#<fn>`, 无需特判。
    struct SelectorPart {
        std::string mod;
        std::string fn;
    };
    std::vector<SelectorPart> selectors;
    const auto& testSelectors = opts.selectors;
    selectors.reserve(testSelectors.size());
    for (auto& s : testSelectors) {
        auto hash = s.find('#');
        if (hash == std::string::npos)
            selectors.push_back({.mod = s, .fn = {}});
        else
            selectors.push_back({.mod = s.substr(0, hash), .fn = s.substr(hash + 1)});
    }
    // 模块边界感知匹配: m == sel.mod 或 m 以 `sel.mod.` 开头。
    auto modCovers = [](const std::string& m, const std::string& sm) {
        if (sm.empty()) return true;
        if (m == sm) return true;
        return m.size() > sm.size() + 1 && m.starts_with(sm) && m[sm.size()] == '.';
    };
    // 扫描期 *.test.yux 裁剪: 任一 selector 的模块范围覆盖即保留。
    auto matchesSelModule = [&](const std::string& m) {
        if (selectors.empty()) return true;
        for (auto& sel : selectors)
            if (modCovers(m, sel.mod)) return true;
        return false;
    };
    // 函数维度过滤: 任一 selector「模块覆盖 + (selFn 空或 fn 相等)」即命中。
    auto matchesSelFull = [&](const std::string& m, const std::string& fn) {
        if (selectors.empty()) return true;
        for (auto& sel : selectors) {
            if (!modCovers(m, sel.mod)) continue;
            if (sel.fn.empty() || fn == sel.fn) return true;
        }
        return false;
    };
    // selector 列表的人类可读串, 用于错误/告示输出
    auto joinSelectors = [&]() {
        std::string r;
        for (size_t i = 0; i < testSelectors.size(); ++i) {
            if (i) r += ", ";
            r += "`" + testSelectors[i] + "`";
        }
        return r;
    };

    Yux yux;
    std::string cwd = fs::current_path().string();
    try {
        yux.initProjectFromDir(cwd);
    } catch (runtime_error& e) {
        std::cerr << e.what() << '\n';
        std::exit(1);
    }

    // SDK 符号表加载（仅解析不编译；SDK 自构建时模块 AOT 编为 obj，
    // 其余模块 JIT 编为 IR——均在下方统一处理）
    std::string sdkPath;
    if (yux.projectName() == "yux") {
        fs::path candidate = fs::path(yux.sourceRoot()) / "yux" / "core";
        if (fs::is_directory(candidate)) {
            sdkPath = candidate.string();
        }
    } else {
        sdkPath = sdk_loader::findSdkPath();
    }
    bool isSdkSelfBuild = yux.projectName() == "yux" && !sdkPath.empty();
    std::string sdkObjDir;
    if (!sdkPath.empty()) {
        sdkPath = fs::absolute(sdkPath).string();
        // SDK obj 产物目录：sdkRoot/build/src/yux/core/
        auto sdkRootPath = fs::path(sdkPath).parent_path().parent_path().parent_path();
        sdkObjDir = (sdkRootPath / "build" / "src" / "yux" / "core").string();

        // 解析 SDK 源码获取符号表（_sdkFile + 各模块 AST）
        try {
            sdk_loader::parseSdkDir(sdkPath, yux);
        } catch (std::runtime_error& e) {
            reportRuntimeError(sdkPath, e, "Error in SDK: ");
            std::exit(1);
        }
    }

    // 非自构建项目：确保 SDK 已编译（test 需要加载 SDK .obj 到 JIT）
    if (!isSdkSelfBuild && !sdkPath.empty()) {
        if (!ensureSdkBuilt(sdkPath, sdkObjDir)) {
            std::exit(1);
        }
    }

    // 递归扫 src/ 下所有 .yux (含 .test.yux)
    fs::path srcDir(yux.sourceRoot());
    if (!fs::is_directory(srcDir)) {
        std::cerr << "Error: project missing `src/` directory at " << srcDir.string() << '\n';
        std::exit(1);
    }
    struct LoadEntry {
        std::string abs;
        std::string mod;
        bool isTest;
    };
    std::vector<LoadEntry> entries;
    std::string sdkPathAbs = sdkPath.empty() ? std::string() : fs::absolute(sdkPath).string();
    std::error_code walkEc;
    for (auto it = fs::recursive_directory_iterator(srcDir, walkEc); it != fs::recursive_directory_iterator(); ++it) {
        if (walkEc) break;
        if (!it->is_regular_file()) continue;
        const auto& p = it->path();
        if (p.extension() != ".yux") continue;
        auto fname = p.filename().string();
        bool isTest = fname.size() >= 9 && fname.ends_with(".test.yux");
        std::string absPath = fs::absolute(p).string();
        // SDK 自构建：非 test SDK 文件也作为项目源文件参与编译
        // 非 SDK 项目：SDK 文件已预编译为 obj，跳过
        if (!isTest && !isSdkSelfBuild && !sdkPathAbs.empty() &&
            fs::path(absPath).parent_path().string() == sdkPathAbs) {
            continue; // SDK preload 已处理 sdk 目录下非 test 文件
        }
        auto rel = fs::relative(p, srcDir);
        std::string modName = rel.generic_string();
        // strip ".yux" (保留 ".test" 段, 例如 "yux/core/arithmetic.test.yux" → "yux.core.arithmetic.test")
        modName = modName.substr(0, modName.size() - 4);
        for (auto& c : modName)
            if (c == '/' || c == '\\') c = '.';
        // selector 扫描期裁剪: 不匹配 selModule 的 *.test.yux 直接跳过, 避免无谓的解析/codegen。
        // 普通 .yux 仍保留 —— 它们可能是被选中 test 模块的依赖。
        if (isTest && !matchesSelModule(modName)) continue;
        entries.push_back({.abs = absPath, .mod = modName, .isTest = isTest});
    }
    std::ranges::sort(entries, [](const LoadEntry& a, const LoadEntry& b) { return a.mod < b.mod; });

    // 加载所有 AST。若模块名已加载 (被 SDK 抢先), 跳过避免冲突。
    // 自维护加载顺序: Yux::loadMainFile 不写 _loadOrder。
    std::vector<std::string> loadedMods;
    for (auto& e : entries) {
        if (yux.module(e.mod)) continue;
        try {
            yux.loadMainFile(e.abs, e.mod);
            loadedMods.push_back(e.mod);
        } catch (runtime_error& re) {
            reportRuntimeError(e.abs, re, e.mod + ": ");
            std::exit(1);
        }
    }

    // codegen 每个加载的用户模块为独立 LLVM Module
    // 注意声明顺序: ctxs 先于 mods, 析构时 mods 先释放、ctxs 后释放,
    // 满足 LLVM "Module 必须在其 Context 之前销毁" 的约束。
    // 正常路径靠 `_exit` 跳过析构, 但任一 `return 1` 错误路径会触发栈展开,
    // 反序销毁会让 Module 落在已释放的 Context 上 → SIGSEGV (BUG#2)。
    std::vector<std::unique_ptr<llvm::LLVMContext>> ctxs;
    std::vector<std::unique_ptr<llvm::Module>> mods;
    // 测试函数收集表: (modName, fnName, mangledSymbol)
    // isolate=true: 函数声明了 `#TestIsolate`, 默认模式下也强制走子进程 (规避 JIT 跨帧 SEH)。
    struct TestEntry {
        std::string mod;
        std::string fn;
        std::string sym;
        bool isolate;
    };
    // SDK 自构建：读 pkg 文件确定 runtime base 模块，对其传 isSdk=true
    std::map<std::string, SdkPkgEntry> sdkPkgMap;
    if (isSdkSelfBuild && !sdkPath.empty()) {
        sdkPkgMap = sdk_loader::readSdkPkg(sdkPath);
    }

    std::vector<TestEntry> tests;
    for (auto& modName : loadedMods) {
        auto file = yux.module(modName);
        if (!file || file == yux.sdkFile()) continue;

        // SDK 自构建：runtime base 模块需发射运行时辅助
        bool isSdkRuntime = false;
        if (isSdkSelfBuild) {
            std::string srcPath = yux.modulePath(modName);
            auto stem = fs::path(srcPath).stem().string();
            auto it = sdkPkgMap.find(stem);
            bool isFlatDep = (it == sdkPkgMap.end()) || it->second.isFlat;
            isSdkRuntime = isFlatDep && (stem == "base");
        }

        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto mod = std::make_unique<llvm::Module>(modName, *ctx);
        llvm::IRBuilder<> builder(*ctx);
        try {
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, isSdkRuntime);
            compiler.compile(file);
        } catch (runtime_error& re) {
            std::string mp = yux.modulePath(modName);
            reportRuntimeError(mp, re, modName + ": ");
            std::exit(1);
        }

        // --emit-ir：输出 JIT 模块的 .ll 文件（与 build --emit-ir 同路径规则）
        if (opts.emitIr) {
            std::string srcPath = yux.modulePath(modName);
            std::string irDir = opts.emitIrDir.empty() ? getBuildDir(yux.projectRoot()) : opts.emitIrDir;
            std::string irPath = mirroredOutputBase(yux.projectRoot(), irDir, srcPath) + ".ll";
            std::filesystem::create_directories(std::filesystem::path(irPath).parent_path());
            std::error_code ec;
            llvm::raw_fd_ostream irFile(irPath, ec);
            if (!ec) {
                mod->print(irFile, nullptr);
                irFile.flush();
                std::cout << "Write IR: " << irPath << '\n';
            }
        }

        // 收集本模块内的 #Test 函数 (仅顶层 fn; 方法 v1 暂不收集)
        for (auto& fn : file->getFunctions()) {
            if (!fn->header()->hasAnno("Test")) continue;
            std::string fnName = fn->header()->name().getText();
            // mangler: function(module, name, params=[], isPrivate=false) → "mod_name()"
            std::string sym = Mangler::function(modName, fnName, {}, false);
            bool isolate = fn->header()->hasAnno("TestIsolate");
            tests.push_back({.mod = modName, .fn = fnName, .sym = sym, .isolate = isolate});
        }

        mods.push_back(std::move(mod));
        ctxs.push_back(std::move(ctx));
    }

    // selector 过滤 (扫描期已裁掉不匹配的 *.test.yux, 这里再做模块+函数维度过滤)
    std::vector<TestEntry> filtered;
    for (auto& t : tests) {
        if (!matchesSelFull(t.mod, t.fn)) continue;
        filtered.push_back(t);
    }

    if (filtered.empty()) {
        if (!isChildIsolated) {
            std::cout << "no tests matched";
            if (!testSelectors.empty()) std::cout << " selector(s) " << joinSelectors();
            std::cout << "\n";
            std::cout.flush();
            std::cerr.flush();
        } else {
            std::cerr << "child: selector(s) " << joinSelectors() << " matched no test\n";
        }
        _exit(isChildIsolated ? 2 : 0);
    }

    // Phase 5: 子进程模式必须命中且仅命中一个测试 (父进程派发时用 `<mod>#<fn>` 形式)
    if (isChildIsolated && filtered.size() != 1) {
        std::cerr << "child: --isolate-child expects exactly one test, got " << filtered.size() << "\n";
        _exit(2);
    }

    // 每个测试的耗时格式化为 `[s.SSS]`, 附在 OK/FAIL 行末
    auto fmtElapsed = [](std::chrono::steady_clock::time_point t0) {
        using namespace std::chrono;
        auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
        std::array<char, 32> buf{};
        std::snprintf(buf.data(), buf.size(), " [%lld.%03llds]", static_cast<long long>(ms / 1000),
                      static_cast<long long>(ms % 1000));
        return std::string(buf.data());
    };

    // Phase 5: 父进程在 isolate=process 模式下走子进程派发路径, 跳过本进程 JIT。
    bool useProcessIsolation = !isChildIsolated && opts.isolate == "process";

    if (useProcessIsolation) {
        std::string self = getSelfExePath();
        if (self.empty()) {
            std::cerr << "[test] failed to resolve self exe path\n";
            std::exit(1);
        }
        size_t passed = 0, failed = 0;
        // 失败名单: suite 末尾汇报, 方便从一屏 OK/FAIL 里直接挑出来 re-run
        std::vector<std::pair<std::string, std::string>> failures;
        size_t total = filtered.size();
        for (size_t i = 0; i < filtered.size(); ++i) {
            auto& t = filtered[i];
            std::string prog = "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] ";
            std::string name = t.mod + "#" + t.fn;
            std::cout << "RUN  " << prog << name << "\n";
            std::cout.flush();
            auto t0 = std::chrono::steady_clock::now();
            auto r = spawnIsolatedTest(self, t.mod, t.fn);
            std::string elapsed = fmtElapsed(t0);
            if (!r.spawnOk) {
                std::cout << "FAIL " << prog << name << " (" << r.spawnError << ")" << elapsed << "\n";
                failures.emplace_back(name, r.spawnError);
                ++failed;
                continue;
            }
            if (r.exitCode == 0) {
                std::cout << "OK   " << prog << name << elapsed << "\n";
                if (opts.verbose) printCapturedOutput(r.capture);
                ++passed;
            } else if (r.exitCode == 2) {
                std::cout << "FAIL " << prog << name << " (child runner error)" << elapsed << "\n";
                printCapturedOutput(r.capture);
                failures.emplace_back(name, "child runner error");
                ++failed;
            } else {
                std::cout << "FAIL " << prog << name << " (SEH " << sehExceptionName(r.exitCode) << " 0x" << std::hex
                          << r.exitCode << std::dec << ")" << elapsed << "\n";
                printCapturedOutput(r.capture);
                failures.emplace_back(name, std::string("SEH ") + sehExceptionName(r.exitCode));
                ++failed;
            }
        }
        if (!failures.empty()) {
            std::cout << "\nFailed tests:\n";
            for (auto& f : failures) {
                std::cout << "  - " << f.first << "  (" << f.second << ")\n";
            }
        }
        std::cout << "\n" << passed << " passed, " << failed << " failed," << fmtElapsed(suiteT0) << "\n";
        std::cout.flush();
        std::cerr.flush();
        _exit(failed == 0 ? 0 : 1);
    }

    // 启动 LLJIT, 加载 sdk obj + 所有用户模块 IR
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jitOrErr = llvm::orc::LLJITBuilder().setObjectLinkingLayerCreator(&jit::makeYuxObjectLinkingLayer).create();
    if (!jitOrErr) {
        llvm::errs() << "[test] LLJIT create failed: " << llvm::toString(jitOrErr.takeError()) << "\n";
        std::exit(1);
    }
    auto& jit = *jitOrErr;
    auto& jd = jit->getMainJITDylib();

    auto procGen =
        llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(jit->getDataLayout().getGlobalPrefix());
    if (!procGen) {
        llvm::errs() << "[test] process generator failed: " << llvm::toString(procGen.takeError()) << "\n";
        std::exit(1);
    }
    jd.addGenerator(std::move(*procGen));

    if (!sdkObjDir.empty() && fs::exists(sdkObjDir)) {
        for (const auto& entry : fs::directory_iterator(sdkObjDir)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".obj") continue;
            auto bufOrErr = llvm::MemoryBuffer::getFile(entry.path().string());
            if (!bufOrErr) {
                llvm::errs() << "[test] read sdk obj failed: " << entry.path().string() << "\n";
                std::exit(1);
            }
            if (auto e = jit->addObjectFile(std::move(*bufOrErr))) {
                llvm::errs() << "[test] addObjectFile(sdk) failed: " << llvm::toString(std::move(e)) << "\n";
                std::exit(1);
            }
        }
    }
    for (size_t i = 0; i < mods.size(); ++i) {
        mods[i]->setDataLayout(jit->getDataLayout());
        llvm::orc::ThreadSafeModule tsm(std::move(mods[i]), std::move(ctxs[i]));
        if (auto e = jit->addIRModule(std::move(tsm))) {
            llvm::errs() << "[test] addIRModule failed: " << llvm::toString(std::move(e)) << "\n";
            std::exit(1);
        }
    }

    // 顺序执行: 每个测试用 SEH 包裹 + 输出捕获, 崩溃单条失败不再终止 suite
    // Phase 5: 子进程模式 (isChildIsolated) 下不打 RUN/OK/FAIL/summary,
    // 退出码 = SEH 码 (0=pass, ASSERT_FAILED/AV/... 透传给父进程翻译)。
    // 子进程模式也不再用 TestOutputCapture (stdout/stderr 已在入口被重定向到 capture 文件)。
    size_t passed = 0, failed = 0;
    // 失败名单: suite 末尾汇报, 方便从一屏 OK/FAIL 里直接挑出来 re-run
    std::vector<std::pair<std::string, std::string>> failures;
    unsigned long childExitCode = 0;
    size_t total = filtered.size();
    // `#TestIsolate` 命中的测试在默认模式下也要走子进程, 懒解析 self 路径
    std::string selfExeForIsolate;
    for (size_t i = 0; i < filtered.size(); ++i) {
        auto& t = filtered[i];
        std::string prog =
            isChildIsolated ? std::string() : "[" + std::to_string(i + 1) + "/" + std::to_string(total) + "] ";
        std::string name = t.mod + "#" + t.fn;
        if (!isChildIsolated) {
            std::cout << "RUN  " << prog << name << "\n";
            std::cout.flush();
        }
        auto t0 = std::chrono::steady_clock::now();

        // 父进程默认模式 + 该测试要求隔离 → fork 子进程跑 (与 useProcessIsolation 分支同协议)
        if (!isChildIsolated && t.isolate) {
            if (selfExeForIsolate.empty()) selfExeForIsolate = getSelfExePath();
            if (selfExeForIsolate.empty()) {
                std::cout << "FAIL " << prog << name << " (#TestIsolate: failed to resolve self exe)" << fmtElapsed(t0)
                          << "\n";
                failures.emplace_back(name, "#TestIsolate: failed to resolve self exe");
                ++failed;
                continue;
            }
            auto r = spawnIsolatedTest(selfExeForIsolate, t.mod, t.fn);
            std::string elapsed = fmtElapsed(t0);
            if (!r.spawnOk) {
                std::cout << "FAIL " << prog << name << " (#TestIsolate: " << r.spawnError << ")" << elapsed << "\n";
                failures.emplace_back(name, "#TestIsolate: " + r.spawnError);
                ++failed;
            } else if (r.exitCode == 0) {
                std::cout << "OK   " << prog << name << " (isolated)" << elapsed << "\n";
                if (opts.verbose) printCapturedOutput(r.capture);
                ++passed;
            } else if (r.exitCode == 2) {
                std::cout << "FAIL " << prog << name << " (#TestIsolate: child runner error)" << elapsed << "\n";
                printCapturedOutput(r.capture);
                failures.emplace_back(name, "#TestIsolate: child runner error");
                ++failed;
            } else {
                std::cout << "FAIL " << prog << name << " (SEH " << sehExceptionName(r.exitCode) << " 0x" << std::hex
                          << r.exitCode << std::dec << ", isolated)" << elapsed << "\n";
                printCapturedOutput(r.capture);
                failures.emplace_back(name, std::string("SEH ") + sehExceptionName(r.exitCode) + ", isolated");
                ++failed;
            }
            continue;
        }

        auto sym = jit->lookup(t.sym);
        if (!sym) {
            if (isChildIsolated) {
                std::cerr << "child: lookup failed: " << llvm::toString(sym.takeError()) << "\n";
                childExitCode = 2;
            } else {
                std::cout << "FAIL " << prog << name << " (lookup failed: " << llvm::toString(sym.takeError()) << ")"
                          << fmtElapsed(t0) << "\n";
                failures.emplace_back(name, "lookup failed");
                ++failed;
            }
            continue;
        }
        auto fn = sym->toPtr<void (*)()>();

        unsigned long code;
        if (isChildIsolated) {
            code = runTestSEH(fn);
            childExitCode = code;
        } else {
            TestOutputCapture cap;
            bool capOk = cap.start();
            code = runTestSEH(fn);
            std::string out = capOk ? cap.stop() : std::string();
            std::string elapsed = fmtElapsed(t0);

            if (code == 0) {
                std::cout << "OK   " << prog << name << elapsed << "\n";
                if (opts.verbose) printCapturedOutput(out);
                ++passed;
            } else {
                std::cout << "FAIL " << prog << name << " (SEH " << sehExceptionName(code) << " 0x" << std::hex << code
                          << std::dec << ")" << elapsed << "\n";
                printCapturedOutput(out);
                failures.emplace_back(name, std::string("SEH ") + sehExceptionName(code));
                ++failed;
            }
        }
    }
    if (isChildIsolated) {
        std::fflush(stdout);
        std::fflush(stderr);
        _exit(static_cast<int>(childExitCode));
    }
    if (!failures.empty()) {
        std::cout << "\nFailed tests:\n";
        for (auto& f : failures) {
            std::cout << "  - " << f.first << "  (" << f.second << ")\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed," << fmtElapsed(suiteT0) << "\n";
    std::cout.flush();
    std::cerr.flush();
    _exit(failed == 0 ? 0 : 1);
}

} // namespace yux::cli
