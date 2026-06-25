// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "test_cmd.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yux::cli {

namespace {

// 取当前进程 exe 全路径 (用于派发 yux build --test 子进程)
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

// spawn 子进程，等待完成，返回退出码。
// spawn 失败时返回 MAXDWORD（调用方自行决定是否 exit）。
DWORD spawnAndWait(const std::wstring& cmdLine, const std::wstring& workingDir = {}) {
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    if (!ok) {
        std::cerr << "Error: failed to spawn (GLE=" << GetLastError() << ") — cmd: ";
        std::wcerr << cmdLine << L"\n";
        return MAXDWORD;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

} // namespace

[[noreturn]] void runTestCommand(const TestCmdOptions& opts) {
    namespace fs = std::filesystem;

    std::string self = getSelfExePath();
    if (self.empty()) {
        std::cerr << "Error: failed to resolve yux executable path\n";
        std::exit(1);
    }

    std::string cwd = fs::current_path().string();

    // 1. yux build --test（复用缓存，只重编变化的文件）
    std::cout.flush();
    std::cout << "Building test DLLs..." << '\n';
    std::cout.flush();
    {
        std::wstring cmd = L"\"" + toWide(self) + L"\" build --test";
        if (!opts.testMod.empty()) {
            cmd += L" --test-mod " + toWide(opts.testMod);
        }
        DWORD code = spawnAndWait(cmd, toWide(cwd));
        if (code != 0) {
            std::cerr << "Error: yux build --test failed (exit code " << code << ")\n";
            _exit(code == MAXDWORD ? 1 : static_cast<int>(code));
        }
    }

    // 2. 定位 tests 目录
    std::string buildDir = cwd + "/build";
    std::string testsDir = buildDir + "/tests";
    if (!fs::exists(testsDir)) {
        // 从 project root 找
        fs::path root = cwd;
        while (!root.empty() && !fs::exists(root / "yux.toml")) {
            auto parent = root.parent_path();
            if (parent == root) break;
            root = parent;
        }
        testsDir = (root / "build" / "tests").string();
    }
    if (!fs::exists(testsDir)) {
        std::cerr << "Error: test DLL directory not found.\n"
                  << "  Expected: " << testsDir << "\n";
        _exit(1);
    }

    // 3. 扫描 *.test.dll
    std::vector<std::string> dllPaths;
    std::error_code ec;
    for (auto it = fs::directory_iterator(testsDir, ec); it != fs::directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        auto& p = it->path();
        if (p.extension() == ".dll" && p.stem().string().ends_with(".test")) {
            dllPaths.push_back(p.string());
        }
    }
    std::ranges::sort(dllPaths);

    // 同时扫描 .test.failed 标记文件（构建失败的测试文件）
    std::vector<std::string> failedMarkers;
    for (auto it = fs::directory_iterator(testsDir, ec); it != fs::directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        auto& p = it->path();
        if (p.extension() == ".failed" && p.stem().string().ends_with(".test")) {
            failedMarkers.push_back(p.string());
        }
    }
    std::ranges::sort(failedMarkers);

    // --test-mod 过滤：只运行指定模块的 DLL
    if (!opts.testMod.empty()) {
        // DLL 名由模块名推导：yux.core.array → yux_core_array_test.test.dll
        std::string expectedStem = opts.testMod;
        for (auto& c : expectedStem)
            if (c == '.') c = '_';
        expectedStem += "_test"; // 对应 .test 后缀（模块名中 . → _）
        std::vector<std::string> filtered;
        for (auto& p : dllPaths) {
            std::string stem = fs::path(p).stem().string(); // 如 yux_core_array_test.test
            if (stem == expectedStem + ".test" || stem == expectedStem) {
                filtered.push_back(p);
            }
        }
        if (filtered.empty()) {
            std::cerr << "Error: no test DLL matches --test-mod " << opts.testMod << "\n";
            std::cerr << "  Expected stem: " << expectedStem << ".test.dll\n";
        }
        dllPaths = std::move(filtered);

        // 同时过滤失败标记文件
        std::vector<std::string> filteredMarkers;
        for (auto& p : failedMarkers) {
            std::string stem = fs::path(p).stem().string(); // 如 yux_core_array_test.test
            if (stem == expectedStem + ".test" || stem == expectedStem) {
                filteredMarkers.push_back(p);
            }
        }
        failedMarkers = std::move(filteredMarkers);
    }

    if (dllPaths.empty() && failedMarkers.empty()) {
        std::cout << "No *.test.dll found in " << testsDir << " — nothing to test.\n";
        _exit(0);
    }

    // 构建失败计数（从标记文件数量得出）
    int buildFailures = static_cast<int>(failedMarkers.size());

    // 4. 并行 spawn yux-test-runner 子进程（每 DLL 一个）
    std::atomic<int> passedDlls{0};
    std::atomic<int> failedDlls{0};
    std::vector<std::string> failedDllNames;
    if (!dllPaths.empty()) {
        std::cout.flush();
        std::string runnerExe = "yux-test-runner";

        int maxParallel = opts.threads > 0 ? opts.threads : static_cast<int>(std::thread::hardware_concurrency());
        if (maxParallel < 1) maxParallel = 1;

        std::cout << "Running " << dllPaths.size() << " DLL(s) with " << maxParallel
                  << " parallel worker(s)...\n\n";
        std::cout.flush();

        // 信号量：mutex + condition_variable 控制并发上限
        std::mutex cvMtx;
        std::condition_variable cv;
        int activeCount = 0;

        std::vector<std::thread> waiters;
        std::mutex failedMtx;

        for (const auto& dllPath : dllPaths) {
            {
                std::unique_lock lock(cvMtx);
                cv.wait(lock, [&] { return activeCount < maxParallel; });
                activeCount++;
            }

            // 每个 DLL 起一个线程负责 spawn + wait
            waiters.emplace_back([&, dllPath]() {
                std::wstring cmdLine =
                    L"\"" + toWide(runnerExe) + L"\" \"" + toWide(dllPath) + L"\"";
                if (opts.verbose) cmdLine += L" --verbose";

                DWORD code = spawnAndWait(cmdLine, toWide(cwd));

                // 提取 DLL 名（用于汇总报告）
                std::string dllName = dllPath;
                auto slashPos = dllName.find_last_of("/\\");
                if (slashPos != std::string::npos) dllName = dllName.substr(slashPos + 1);

                if (code == 0) {
                    ++passedDlls;
                } else {
                    ++failedDlls;
                    std::scoped_lock lock(failedMtx);
                    failedDllNames.push_back(dllName);
                }

                {
                    std::scoped_lock lock(cvMtx);
                    activeCount--;
                }
                cv.notify_one();
            });
        }

        // 等待所有子进程完成
        for (auto& t : waiters)
            t.join();
    }

    // 5. 汇总
    std::cout.flush();
        if (buildFailures > 0) {
            std::cout << "\nBuild failures:\n";
            for (auto& marker : failedMarkers) {
                std::string markerName = fs::path(marker).filename().string();
                // 去除 .failed 后缀得到应有的 DLL 名
                std::string dllStem = markerName.substr(0, markerName.size() - 7); // ".failed"
                std::cout << "  [BUILD FAIL] " << dllStem << ".dll\n";
                // 打印失败原因（标记文件内容）
                std::ifstream mf(marker);
                if (mf) {
                    std::string line;
                    while (std::getline(mf, line)) {
                        if (!line.empty()) std::cout << "    " << line << "\n";
                    }
                }
            }
        }
        if (!failedDllNames.empty()) {
            std::cout << "\nFailed DLLs:\n";
            for (auto& name : failedDllNames) {
                std::cout << "  - " << name << "\n";
            }
        }
        int totalCount = static_cast<int>(dllPaths.size()) + buildFailures;
        int totalFailed = failedDlls.load() + buildFailures;
        std::cout << "\n" << totalCount << " total, " << passedDlls << " passed, " << totalFailed << " failed\n";
        std::cout.flush();

        _exit(totalFailed == 0 ? 0 : 1);
    }

} // namespace yux::cli
