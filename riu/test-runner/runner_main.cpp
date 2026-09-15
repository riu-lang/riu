// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// riu-test-runner.exe — 独立测试运行器（单 DLL 模式）
//
// 加载单个 *.test.dll，顺序执行其中所有测试并打印结果。
// 由 `riu test` 多子进程并行调用，每个子进程负责一个 DLL。
//
// 用法：riu-test-runner.exe <dll-path> [--verbose]
//
// DLL 协议：
//   LoadLibrary → GetProcAddress("riu_test_init") 调之
//   → GetProcAddress("riu_test_get_count") 取测试数
//   → GetProcAddress("riu_test_get_name")(i) 取名称
//   → GetProcAddress("riu_test_get_fn")(i) 取函数指针
//   → 顺序调用 fn() 并 SEH 包裹
//   → 打印汇总 → (无严重 SEH 时 FreeLibrary，否则跳过) → ExitProcess

#define NOMINMAX
#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// ==================== 命令行参数 ====================

struct RunnerOptions {
    std::string dllPath;
    bool verbose = false;
};

RunnerOptions parseArgs(int argc, char* argv[]) { // NOLINT(modernize-avoid-c-arrays)
    RunnerOptions opts;
    if (argc < 2) {
        std::cerr << "Usage: riu-test-runner <dll-path> [--verbose]\n";
        std::exit(1);
    }
    // --version：打印版本号后退出
    if (std::string(argv[1]) == "--version") {
        std::cout << "riu-test-runner " RIU_VERSION "\n";
        std::exit(0);
    }
    opts.dllPath = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verbose") {
            opts.verbose = true;
        }
    }
    return opts;
}

// ==================== SEH 包装 ====================

// riu 测试断言失败码 (spec §11.3.5)
constexpr unsigned long ASSERT_FAILED_CODE = 0xE0FA17ED;

// SEH 包裹单次测试调用。返回 0=pass；非 0=GetExceptionCode()
// 必须 extern "C" + 无 C++ 析构对象
extern "C" unsigned long runTestSEH(void (*fn)()) noexcept {
    __try {
        fn();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// 把 SEH 异常码翻译为可读名字
const char* sehExceptionName(unsigned long code) {
    if (code == ASSERT_FAILED_CODE) return "ASSERT_FAILED";
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

// ==================== 格式化耗时 ====================

std::string formatElapsed(std::chrono::steady_clock::time_point t0) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%lld.%03llds", static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    return buf.data();
}

// ==================== 主函数 ====================

int main(int argc, char* argv[]) { // NOLINT(bugprone-exception-escape)
    auto opts = parseArgs(argc, argv);

    // 设置控制台 UTF-8
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // 提取 DLL 文件名用于显示
    std::string dllName = opts.dllPath;
    auto slashPos = dllName.find_last_of("/\\");
    if (slashPos != std::string::npos) dllName = dllName.substr(slashPos + 1);

    // 加载 DLL
    HMODULE hDll = LoadLibraryA(opts.dllPath.c_str());
    if (!hDll) {
        std::cerr << "ERROR: LoadLibrary failed for " << opts.dllPath << " (GLE=" << GetLastError() << ")\n";
        return 1;
    }

    // 调 riu_test_init
    auto initFn = reinterpret_cast<void (*)()>(GetProcAddress(hDll, "riu_test_init"));
    if (!initFn) {
        std::cerr << "ERROR: riu_test_init not found in " << opts.dllPath << "\n";
        FreeLibrary(hDll);
        return 1;
    }
    initFn();

    // 取访问器
    auto countFn = reinterpret_cast<int (*)()>(GetProcAddress(hDll, "riu_test_get_count"));
    auto nameFn = reinterpret_cast<const char* (*)(int)>(GetProcAddress(hDll, "riu_test_get_name"));
    auto fnFn = reinterpret_cast<void (*(*)(int))()>(GetProcAddress(hDll, "riu_test_get_fn"));

    if (!countFn || !nameFn || !fnFn) {
        std::cerr << "ERROR: DLL accessors missing in " << opts.dllPath << "\n";
        FreeLibrary(hDll);
        return 1;
    }

    int total = countFn();
    if (total == 0) {
        std::cout << dllName << ": no tests\n";
        FreeLibrary(hDll);
        return 0;
    }

    // 收集测试条目
    struct TestEntry {
        std::string name;
        void (*fn)();
    };
    std::vector<TestEntry> tests;
    for (int i = 0; i < total; ++i) {
        const char* name = nameFn(i);
        auto fn = fnFn(i);
        if (name && fn) {
            tests.push_back({.name = name, .fn = fn});
        }
    }

    if (tests.empty()) {
        std::cout << dllName << ": no tests\n";
        FreeLibrary(hDll);
        return 0;
    }

    // 顺序执行所有测试
    auto suiteT0 = std::chrono::steady_clock::now();
    int passed = 0;
    int failed = 0;
    bool hadSevereSEH = false; // 是否有非断言失败的 SEH 异常（进程状态可能损坏）
    std::vector<std::pair<std::string, std::string>> failures;

    for (size_t i = 0; i < tests.size(); ++i) {
        auto& test = tests[i];
        size_t idx = i + 1; // 1-based 显示

        // 打印 RUN
        std::cout << "RUN  [" << idx << "/" << tests.size() << "] " << test.name << "\n";
        std::cout.flush();

        auto t0 = std::chrono::steady_clock::now();
        bool ok = true;
        std::string errorMsg;

        unsigned long sehCode = runTestSEH(test.fn);
        if (sehCode != 0) {
            ok = false;
            if (sehCode == ASSERT_FAILED_CODE) {
                // 测试断言失败（assert_eq / assert_true / assert_false / fail）
                // 这是正常的测试失败，不是编译器 BUG
                errorMsg = "assertion failed";
            } else {
                // 非断言失败的 SEH（ACCESS_VIOLATION 等）意味着进程状态可能已损坏，
                // 后续不能安全调用 FreeLibrary（会在 DLL_PROCESS_DETACH / CRT 清理时死锁）
                hadSevereSEH = true;
                errorMsg = std::string("SEH ") + sehExceptionName(sehCode) + " (0x";
                std::array<char, 16> hexBuf{};
                std::snprintf(hexBuf.data(), hexBuf.size(), "%08lX", sehCode);
                errorMsg += hexBuf.data();
                errorMsg += ')';
            }
        }

        auto elapsed = formatElapsed(t0);

        if (ok) {
            std::cout << "OK   [" << idx << "/" << tests.size() << "] " << test.name << " [" << elapsed << "]\n";
            ++passed;
        } else {
            std::cout << "FAIL [" << idx << "/" << tests.size() << "] " << test.name << " (" << errorMsg << ") ["
                      << elapsed << "]\n";
            ++failed;
            failures.emplace_back(test.name, errorMsg);
        }
        std::cout.flush();
    }

    // 打印本 DLL 汇总（必须在 FreeLibrary 之前——SEH 损坏进程状态后 FreeLibrary 可能死锁）
    if (!failures.empty()) {
        std::cout << "\nFailed tests in " << dllName << ":\n";
        for (auto& f : failures) {
            std::cout << "  - " << f.first << "  (" << f.second << ")\n";
        }
    }
    std::cout << "\n"
              << dllName << ": " << passed << " passed, " << failed << " failed, [" << formatElapsed(suiteT0) << "]\n";
    std::cout.flush();

    // 卸载 DLL。有严重 SEH 异常时跳过——进程状态可能已损坏，
    // FreeLibrary → DLL_PROCESS_DETACH → CRT/TLS 清理可能永久阻塞。
    // 跳过 FreeLibrary 无害：ExitProcess 时 OS 会回收所有资源。
    if (!hadSevereSEH) {
        FreeLibrary(hDll);
    }

    ExitProcess(failed == 0 ? 0 : 1);
}
