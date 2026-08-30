// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "test_cmd.h"
#include "process.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace yux::cli {

namespace {

// 从日志文件中提取摘要行（包含 "passed" 和 "failed" 的末行）
std::string extractSummary(const std::string& logPath) {
    std::ifstream f(logPath);
    if (!f) return "(log missing)";
    std::string line;
    std::string summary;
    while (std::getline(f, line)) {
        if (line.find("passed") != std::string::npos && line.find("failed") != std::string::npos) {
            summary = line;
        }
    }
    if (summary.empty()) return "(no summary)";
    return summary;
}

// 格式化耗时（用于汇总）
std::string formatElapsed(std::chrono::steady_clock::time_point t0) {
    using namespace std::chrono;
    auto ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    std::array<char, 32> buf{};
    std::snprintf(buf.data(), buf.size(), "%lld.%03llds", static_cast<long long>(ms / 1000),
                  static_cast<long long>(ms % 1000));
    return buf.data();
}

} // namespace

[[noreturn]] void runTestCommand(const TestCmdOptions& opts) {
    namespace fs = std::filesystem;

    // 总耗时计时（从命令入口开始）
    auto cmdT0 = std::chrono::steady_clock::now();

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
        if (opts.threads > 0) {
            cmd += L" --threads " + std::to_wstring(opts.threads);
        }
        std::uint32_t code = spawnAndWait(cmd, toWide(cwd));
        if (code != 0) {
            std::cerr << "Error: yux build --test failed (exit code " << code << ")\n";
            _exit(code == kSpawnFailed ? 1 : static_cast<int>(code));
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

    // 防御过滤：排除已有 .failed 标记的 DLL（build 过程已删除旧 DLL，
    // 但保留此过滤以应对极端竞态；同时避免重复统计）
    {
        std::set<std::string> failedStems;
        for (auto& marker : failedMarkers) {
            std::string fname = fs::path(marker).filename().string();
            // fname = "yux_core_array_test.test.failed" → "yux_core_array_test.test.dll"
            if (fname.ends_with(".failed")) {
                std::string dllName = fname.substr(0, fname.size() - 7) + ".dll"; // 7 = len(".failed")
                failedStems.insert(dllName);
            }
        }
        std::vector<std::string> filtered;
        for (auto& dll : dllPaths) {
            std::string dllName = fs::path(dll).filename().string();
            if (failedStems.find(dllName) == failedStems.end()) {
                filtered.push_back(dll);
            }
        }
        dllPaths = std::move(filtered);
    }

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

    // 4. 并行 spawn yux-test-runner，输出重定向到日志文件
    //    控制台只打印每个 DLL 的摘要行（从日志提取），
    //    详细 RUN/OK/FAIL 保留到 build/tests/logs/<dll>.log
    std::string logsDir = testsDir + "/logs";
    {
        std::error_code ec2;
        fs::create_directories(logsDir, ec2);
    }

    std::atomic<int> passedDlls{0};
    std::atomic<int> failedDlls{0};
    std::vector<std::string> failedDllNames;
    std::mutex printMtx; // 保护控制台输出的原子打印

    if (!dllPaths.empty()) {
        std::cout.flush();
        std::string runnerExe = "yux-test-runner";

        int maxParallel = resolveThreadCount(opts.threads);

        std::cout << "Running " << dllPaths.size() << " DLL(s) with " << maxParallel << " parallel worker(s)...\n";
        if (opts.verbose) std::cout << "  Logs: " << logsDir << "/\n";
        std::cout << '\n';
        std::cout.flush();

        // 信号量：mutex + condition_variable 控制并发上限
        std::mutex cvMtx;
        std::condition_variable cv;
        int activeCount = 0;

        std::vector<std::thread> waiters;

        for (const auto& dllPath : dllPaths) {
            {
                std::unique_lock lock(cvMtx);
                cv.wait(lock, [&] { return activeCount < maxParallel; });
                activeCount++;
            }

            // NOLINTNEXTLINE(bugprone-exception-escape)
            waiters.emplace_back([&, dllPath]() {
                // 提取 DLL 名（用于日志文件名 + 汇总报告）
                std::string dllName = dllPath;
                auto slashPos = dllName.find_last_of("/\\");
                if (slashPos != std::string::npos) dllName = dllName.substr(slashPos + 1);

                // 日志路径：build/tests/logs/<dllName>.log
                std::string logPath = logsDir;
                logPath += '/';
                logPath += dllName;
                logPath += ".log";

                // 构建命令行：runner + DLL 路径 [+ --verbose 仍然影响日志内容]
                std::wstring cmdLine = L"\"" + toWide(runnerExe) + L"\" \"" + toWide(dllPath) + L"\"";
                if (opts.verbose) cmdLine += L" --verbose";

                std::uint32_t code = spawnToLog(cmdLine, toWide(logPath), toWide(cwd));

                // 提取 runner 输出的摘要行
                std::string summary = extractSummary(logPath);

                // 原子打印到控制台
                {
                    std::scoped_lock lock(printMtx);
                    std::cout << summary << "\n";
                    std::cout.flush();

                    if (code == 0) {
                        ++passedDlls;
                    } else {
                        ++failedDlls;
                        failedDllNames.push_back(dllName);
                    }
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
            std::cout << "  - " << name << " (log: " << logsDir << "/" << name << ".log)\n";
        }
    }
    int totalCount = static_cast<int>(dllPaths.size()) + buildFailures;
    int totalFailed = failedDlls.load() + buildFailures;
    std::cout << "\n"
              << totalCount << " total, " << passedDlls << " passed, " << totalFailed << " failed"
              << " [" << formatElapsed(cmdT0) << "]\n";
    std::cout.flush();

    _exit(totalFailed == 0 ? 0 : 1);
}

} // namespace yux::cli
