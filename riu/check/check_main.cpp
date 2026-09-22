// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// riu-check: 单文件快速语义检查 (阶段 0) + 批量诊断测试 (阶段 1)
//
// 与 riu 主二进制不同, 本工具:
// - 0 LLVM 依赖, 只链 riu_frontend
// - 不加载 SDK, 不解析 import 链, 不做 codegen
// - 仅 parse → RdBuilder → PassManager（Sema → fn checkers）→ 打印诊断
//
// 设计意图: 日常写 demo / 改代码时快速跑诊断, 避免每次 xmake build 编 LLVM.
// **报错不与 riu build 等价**: 仅检出 SemaPass 当前能接管的错误码; 漏的部分
// (源码从未写出 `S<Concrete>` 且无调用的泛型 struct 方法体、未调用泛型 fn 体内嵌套推断)
// 两边都不查，与未调用泛型 fn 一致。
//
// 详见 CURRENT.md "riu-check 最小可用 exe" 一节.
//
// 用法:
//   riu-check <input.ut>            ; 退出码: 0 = 无错, 1 = 文件 / 语法 / 语义错
//   riu-check test <dir>             ; 批量测试目录下所有 .ut (非递归)
//   riu-check test <dir>/c*          ; 通配当前目录 (c 前缀)
//   riu-check test <dir>/**/*        ; 递归子目录
//   riu-check test <path> --threads N ; 0 = 核数（默认）；1 = 串行

// windows.h 必须在拉入 riu frontend (经由 include/types.h 做了 `using namespace
// std`) 之前 #include, 否则 std::byte 与 winapi byte 冲突 (rpcndr.h).
// NOGDI 跳过 wingdi.h, 避免其 ERROR 宏与其它头冲突.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#include "ast/riu.h"
#include "pass/pass.h"
#include "tools/diagnostic.h"
#include "tools/sdk_loader.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std;

// ============================================================================
// "; check:" 注解解析 —— 读源文件文本行做字符串匹配
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
// 目录扫描 / 通配展开
// ============================================================================

// 扫描目录下的 .ut 文件（仅当前目录）。结果按路径排序, 保证输出确定性。
static vector<string> scanRiuFiles(const string& dir) {
    vector<string> result;
    error_code ec;

    for (auto it = filesystem::directory_iterator(dir, ec); it != filesystem::directory_iterator(); ++it) {
        if (ec) break;
        if (it->is_regular_file() && it->path().extension() == ".ut") {
            result.push_back(filesystem::absolute(it->path()).string());
        }
    }

    ranges::sort(result);
    return result;
}

static bool hasGlobMeta(string_view s) {
    return s.find_first_of("*?") != string_view::npos;
}

static string toGenericSlashes(string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

static char globFold(char c) {
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
#endif
    return c;
}

// `*` 不跨目录; `**` 跨任意层; `?` 单字符（不含 `/`）。
static bool globMatchAt(string_view pat, size_t pi, string_view text, size_t ti) {
    while (pi < pat.size()) {
        if (pi + 1 < pat.size() && pat[pi] == '*' && pat[pi + 1] == '*') {
            size_t pj = pi + 2;
            while (pj < pat.size() && pat[pj] == '*')
                ++pj;
            size_t rest = (pj < pat.size() && pat[pj] == '/') ? pj + 1 : pj;
            if (globMatchAt(pat, rest, text, ti)) return true;
            for (size_t k = ti; k < text.size(); ++k) {
                if (globMatchAt(pat, rest, text, k + 1)) return true;
            }
            return false;
        }
        if (pat[pi] == '*') {
            ++pi;
            if (globMatchAt(pat, pi, text, ti)) return true;
            while (ti < text.size() && text[ti] != '/') {
                ++ti;
                if (globMatchAt(pat, pi, text, ti)) return true;
            }
            return false;
        }
        if (pat[pi] == '?') {
            if (ti >= text.size() || text[ti] == '/') return false;
            ++pi;
            ++ti;
            continue;
        }
        if (ti >= text.size() || globFold(pat[pi]) != globFold(text[ti])) return false;
        ++pi;
        ++ti;
    }
    return ti >= text.size();
}

static bool globMatch(string_view pat, string_view text) {
    return globMatchAt(pat, 0, text, 0);
}

// 通配根目录 = 第一个 * / ? 之前的最后一层目录; 其余为相对模式。
static pair<string, string> splitGlobRoot(const string& pattern) {
    size_t meta = pattern.find_first_of("*?");
    size_t slash = pattern.rfind('/', meta);
    if (slash == string::npos) return {".", pattern};
    string root = pattern.substr(0, slash);
    string rel = pattern.substr(slash + 1);
    if (root.empty()) root = "/";
#ifdef _WIN32
    if (root.size() == 2 && root[1] == ':') root += '/';
#endif
    return {root, rel};
}

static vector<string> expandRiuGlob(const string& raw) {
    namespace fs = filesystem;
    string gen = toGenericSlashes(raw);
    while (gen.size() > 1 && gen.back() == '/')
        gen.pop_back();

    auto [root, relPat] = splitGlobRoot(gen);
    vector<string> result;
    error_code ec;
    fs::path rootPath(root);
    if (!fs::is_directory(rootPath, ec)) return result;

    auto opts = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(rootPath, opts, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        bool regular = it->is_regular_file(ec);
        if (ec || !regular || it->path().extension() != ".ut") {
            ec.clear();
            continue;
        }
        fs::path rel = fs::relative(it->path(), rootPath, ec);
        if (ec) continue;
        if (globMatch(relPat, rel.generic_string())) {
            result.push_back(fs::absolute(it->path()).string());
        }
    }
    return result;
}

struct CollectTestFilesResult {
    vector<string> files;
    string error;
};

// paths: 目录 / .ut 文件 / 通配（`*` `?` `**`）。多路径去重。
static CollectTestFilesResult collectTestFiles(const vector<string>& paths) {
    namespace fs = filesystem;
    CollectTestFilesResult out;
    for (const auto& p : paths) {
        error_code ec;
        if (hasGlobMeta(p)) {
            string gen = toGenericSlashes(p);
            while (gen.size() > 1 && gen.back() == '/')
                gen.pop_back();
            string root = splitGlobRoot(gen).first;
            if (!fs::is_directory(root, ec)) {
                out.error = "Error: not a directory: " + root + '\n';
                out.files.clear();
                return out;
            }
            auto got = expandRiuGlob(p);
            out.files.insert(out.files.end(), got.begin(), got.end());
        } else if (fs::is_directory(p, ec)) {
            auto got = scanRiuFiles(p);
            out.files.insert(out.files.end(), got.begin(), got.end());
        } else if (fs::is_regular_file(p, ec)) {
            out.files.push_back(fs::absolute(p).string());
        } else {
            out.error = "Error: path not found: " + p + '\n';
            out.files.clear();
            return out;
        }
    }
    ranges::sort(out.files);
    auto u = ranges::unique(out.files);
    out.files.erase(u.begin(), u.end());
    return out;
}

// ============================================================================
// 单文件 sema 执行 —— parse → AST → PassManager（分析表）, 收集抛出的错误
// ============================================================================

struct CheckResult {
    bool ok = true;               // false = 有错误 (semaError 或 otherError)
    optional<RiuError> semaError; // parse / 分析 Pass 抛出的 RiuError
    string otherError;            // 非 RiuError 的错误信息 (读文件等)
};

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

// 对单个 .ut 文件执行 sema 检查, 复用已有 Riu 实例 (SDK 已预加载).
// loadMainFile → _parseFile 走 rd Scanner/Parser + RdBuilder.
static CheckResult runSemaOnFileWithRiu(const string& absPath, Riu& riu) {
    CheckResult cr;
    string moduleName = filesystem::path(absPath).stem().string();

    try {
        auto file = riu.loadMainFile(absPath, moduleName);
        riu.validateSpecImpls();
        runAnalysisPasses(file, &riu);
    } catch (const RiuError& e) {
        cr.ok = false;
        cr.semaError = e;
    } catch (const runtime_error& e) {
        cr.ok = false;
        cr.otherError = e.what();
    }

    return cr;
}

// 单文件结果评估，复用已有 Riu (SDK 已预加载)
static TestFileResult evaluateOneFileWithRiu(const string& absPath, Riu& riu) {
    namespace fs = filesystem;

    TestFileResult tfr;
    tfr.filename = fs::path(absPath).filename().string();
    tfr.annotations = parseCheckAnnotations(absPath);

    auto cr = runSemaOnFileWithRiu(absPath, riu);

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

// 独立 SDK 模块（如 riu.io）不仅依赖 sdkFile 父作用域，还依赖当前 Riu 的
// 模块表与 package child。此类用例须在自己的 Riu 中完整加载 SDK。
static bool fileRequiresSdkModules(const string& filePath) {
    ifstream in(filePath);
    if (!in.is_open()) return false;

    string line;
    for (int i = 0; i < 10 && getline(in, line); ++i) {
        if (line.find("; require-sdk-modules") != string::npos) {
            return true;
        }
    }
    return false;
}

// 0 → hardware_concurrency（至少 1）。
static int resolveThreadCount(int threads) {
    if (threads > 0) return threads;
    unsigned n = thread::hardware_concurrency();
    return n < 1 ? 1 : static_cast<int>(n);
}

static string getSelfExePath() {
#ifdef _WIN32
    array<wchar_t, MAX_PATH> buf{};
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= MAX_PATH) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf.data(), static_cast<int>(n), out.data(), sz, nullptr, nullptr);
    return out;
#else
    return {};
#endif
}

static wstring toWide(const string& s) {
#ifdef _WIN32
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), sz);
    return out;
#else
    return wstring(s.begin(), s.end());
#endif
}

static constexpr uint32_t kSpawnFailed = 0xFFFFFFFFu;

static uint32_t spawnToLog(const wstring& cmdLine, const wstring& logPath, const wstring& workingDir) {
#ifdef _WIN32
    vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    {
        filesystem::path lp(logPath);
        error_code ec;
        filesystem::create_directories(lp.parent_path(), ec);
    }

    SECURITY_ATTRIBUTES sa{.nLength = sizeof(sa), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) return kSpawnFailed;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);
    CloseHandle(hLog);
    if (!ok) return kSpawnFailed;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
#else
    (void)cmdLine;
    (void)logPath;
    (void)workingDir;
    return kSpawnFailed;
#endif
}

static vector<TestFileResult> parseJobLog(const string& logPath) {
    vector<TestFileResult> out;
    ifstream in(logPath);
    if (!in) return out;
    string line;
    while (getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("  [PASS] ")) {
            TestFileResult r;
            r.filename = line.substr(9);
            r.passed = true;
            out.push_back(std::move(r));
        } else if (line.starts_with("  [FAIL] ")) {
            TestFileResult r;
            r.filename = line.substr(9);
            r.passed = false;
            out.push_back(std::move(r));
        } else if (!out.empty() && !out.back().passed && line.starts_with("  ") && !line.starts_with("  Summary:")) {
            out.back().failReason += line;
            out.back().failReason += '\n';
        }
    }
    return out;
}

// parseSdkDir 默认 allowDecl：有 .ud 就读，没有则 parse 并写出。
static bool loadSdkInto(Riu& sdkRiu, const string& anyFilePath) {
    string sdkPath = sdk_loader::findSdkPath();
    if (sdkPath.empty()) return false;
    sdkRiu.initFileRoot(anyFilePath);
    sdk_loader::parseSdkDir(sdkPath, sdkRiu);
    return sdkRiu.sdkFile() != nullptr;
}

static void loadStdlibInto(Riu& riu) {
    string root = sdk_loader::findSdkPackage("stdlib");
    if (root.empty()) return;
    auto toml = (filesystem::path(root) / "riu.toml").string();
    if (!filesystem::is_regular_file(toml)) return;
    auto cfg = parseRiuToml(toml);
    if (cfg.library) sdk_loader::parseSdkLibrary(root, cfg.library->lib_mod, riu);
}

static TestFileResult checkFileWithOptionalSdk(const string& absPath, Riu* sdkRiu) {
    Riu riu;
    riu.initFileRoot(absPath);
    const bool fullSdk = fileRequiresSdkModules(absPath);
    bool attached = !fullSdk && sdkRiu && sdkRiu->sdkFile();
    if (fullSdk) {
        string sdkPath = sdk_loader::findSdkPath();
        if (!sdkPath.empty()) {
            sdk_loader::parseSdkDir(sdkPath, riu);
            loadStdlibInto(riu);
        }
    } else if (attached) {
        riu.setSdkFile(sdkRiu->sdkFile());
    }
    auto tfr = evaluateOneFileWithRiu(absPath, riu);
    // 解除共享引用，避免 Riu 析构时 delete 不属于它的 SDK FileNode
    if (attached) {
        riu.setSdkFile(nullptr);
    }
    return tfr;
}

static void runSerialChecks(const vector<string>& files, vector<TestFileResult>& results) {
    namespace fs = filesystem;
    Riu sdkRiu;
    Riu* sdkPtr = nullptr;
    bool sdkTried = false;
    for (size_t i = 0; i < files.size(); ++i) {
        try {
            if (fileRequiresSdk(files[i]) && !sdkTried) {
                sdkTried = true;
                if (loadSdkInto(sdkRiu, files[i])) sdkPtr = &sdkRiu;
            }
            Riu* use = fileRequiresSdk(files[i]) ? sdkPtr : nullptr;
            results[i] = checkFileWithOptionalSdk(files[i], use);
        } catch (const exception& e) {
            results[i].filename = fs::path(files[i]).filename().string();
            results[i].passed = false;
            results[i].failReason = string("  error: ") + e.what() + "\n";
        }
    }
}

static int runCheckTest(const vector<string>& paths, int threads, const vector<string>& explicitFiles) {
    namespace fs = filesystem;
    auto t0 = chrono::steady_clock::now();

    vector<string> files;
    if (!explicitFiles.empty()) {
        for (auto& f : explicitFiles)
            files.push_back(fs::absolute(f).string());
    } else {
        auto collected = collectTestFiles(paths);
        if (!collected.error.empty()) {
            cerr << collected.error;
            return 1;
        }
        files = std::move(collected.files);
    }
    if (files.empty()) {
        ostringstream oss;
        if (paths.empty()) {
            oss << ".";
        } else {
            for (size_t i = 0; i < paths.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << paths[i];
            }
        }
        cout << "No .ut files found matching " << oss.str() << '\n';
        return 0;
    }

    // 每文件独立 Riu（BUG#3）。`; require-sdk` 在本进程 parseSdkDir 一次（走 .ud）。
    // 并行：按 worker 分片 spawn（每进程 --threads 1）。
    size_t fileCount = files.size();
    vector<TestFileResult> results(fileCount);

    int maxParallel = resolveThreadCount(threads);
    if (std::cmp_greater(maxParallel, fileCount)) {
        maxParallel = static_cast<int>(fileCount);
    }

    string self = getSelfExePath();
    bool useParallel = maxParallel > 1 && fileCount > 1 && explicitFiles.empty() && !self.empty();

    if (!useParallel) {
        runSerialChecks(files, results);
    } else {
        int nJobs = maxParallel;
        vector<vector<size_t>> shards(static_cast<size_t>(nJobs));
        for (size_t i = 0; i < fileCount; ++i) {
            shards[i % static_cast<size_t>(nJobs)].push_back(i);
            results[i].filename = fs::path(files[i]).filename().string();
            results[i].failReason = "  check worker failed before producing result\n";
        }

        string logsDir = (fs::temp_directory_path() / "riu-check-jobs").string();
        string cwd = fs::current_path().string();
        cout << "Checking " << fileCount << " file(s) in " << nJobs << " job(s)...\n";
        cout.flush();

        vector<thread> waiters;
        waiters.reserve(static_cast<size_t>(nJobs));
        for (int job = 0; job < nJobs; ++job) {
            waiters.emplace_back([&, job]() noexcept {
                try {
                    auto& shard = shards[static_cast<size_t>(job)];
                    if (shard.empty()) return;

                    string logPath = logsDir + "/job-" + std::to_string(job) + ".log";
                    string spawnPath = paths.empty() ? string(".") : paths[0];
                    wstring cmd = L"\"" + toWide(self) + L"\" test \"" + toWide(spawnPath) + L"\" --threads 1";
                    for (size_t idx : shard) {
                        cmd += L" --file \"";
                        cmd += toWide(files[idx]);
                        cmd += L'"';
                    }

                    uint32_t code = spawnToLog(cmd, toWide(logPath), toWide(cwd));
                    auto parsed = parseJobLog(logPath);
                    for (size_t k = 0; k < shard.size(); ++k) {
                        size_t i = shard[k];
                        string expectName = fs::path(files[i]).filename().string();
                        if (k < parsed.size() && parsed[k].filename == expectName) {
                            results[i] = std::move(parsed[k]);
                        } else {
                            results[i].filename = expectName;
                            results[i].passed = false;
                            string reason;
                            if (code == kSpawnFailed) {
                                reason = "  check job spawn failed\n";
                            } else if (code != 0) {
                                reason = "  check job exit ";
                                reason += std::to_string(code);
                                reason += " (log: ";
                                reason += logPath;
                                reason += ")\n";
                            } else {
                                reason = "  check job missing result for ";
                                reason += expectName;
                                reason += " (log: ";
                                reason += logPath;
                                reason += ")\n";
                            }
                            results[i].failReason = std::move(reason);
                        }
                    }
                } catch (...) {
                    // 结果已预置为失败；线程入口不得让异常越过 std::thread 的 noexcept 边界。
                    for (size_t idx : shards[static_cast<size_t>(job)])
                        results[idx].passed = false;
                }
            });
        }
        for (auto& w : waiters)
            w.join();
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

// NOLINTNEXTLINE(bugprone-exception-escape) — main 入口点, filesystem API 可能抛 system_error
int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    // --version：在 CLI11 解析之前手动处理，避免 required option 冲突。
    // CLI11 仍注册同名 flag 以确保 -h 显示 --version。
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << "riu-check " RIU_VERSION "\n";
            return 0;
        }
    }

    CLI::App app{"riu-check: fast standalone semantic check (no LLVM)"};
    bool versionFlag = false;
    app.add_flag("--version", versionFlag, "Print version and exit");
    app.require_subcommand(0, 1);

    // ---- 单文件模式 (保持现有行为) ----
    string inputFile;
    app.add_option("input", inputFile, "Input .ut file");

    // ---- test 子命令 ----
    auto* testCmd = app.add_subcommand("test", "Batch test .ut files with ; check: annotations");
    vector<string> testPaths;
    testCmd->add_option("path", testPaths, "Directory, .ut file, or glob (* ? **; e.g. diag_* or **/*.ut). Repeatable.")
        ->required();
    int testThreads = 0;
    testCmd->add_option("--threads", testThreads, "Parallel check jobs (default: CPU cores; 1 = serial)");
    vector<string> testFiles;
    testCmd->add_option("--file", testFiles, "Check specific .ut files (repeatable; used by parallel workers)");

    CLI11_PARSE(app, argc, argv);

    // ---- test 子命令分支 ----
    if (testCmd->parsed()) {
        return runCheckTest(testPaths, testThreads, testFiles);
    }

    // ==== 单文件模式 (原逻辑) ====

    if (!filesystem::exists(inputFile)) {
        cerr << "Error: input file not found: " << inputFile << '\n';
        return 1;
    }

    string absPath = filesystem::absolute(inputFile).string();
    string moduleName = filesystem::path(absPath).stem().string();

    Riu riu;
    riu.initFileRoot(absPath);

    try {
        // 先加载 SDK (riu.core), 让用户文件经父作用域看到 String / ToString /
        // StringBuilder 等; sdk_loader::parseSdkDir 0 LLVM, 失败抛 RiuError.
        // SDK 找不到时不致命 —— 仅打印警告并继续 (退化为 riu-check 阶段 0 行为).
        string sdkPath = sdk_loader::findSdkPath();
        if (sdkPath.empty()) {
            cerr << "warning: SDK not found (riu.core 未加载); 仅做 builtin 范围内的 sema 检查" << '\n';
        } else {
            sdk_loader::parseSdkDir(sdkPath, riu);
        }

        // loadMainFile → RdBuilder；语法错为 E1001/E1002。
        auto file = riu.loadMainFile(absPath, moduleName);
        riu.validateSpecImpls();
        runAnalysisPasses(file, &riu);
    } catch (const runtime_error& e) {
        if (auto* riuErr = dynamic_cast<const RiuError*>(&e)) {
            DiagnosticEngine::renderRiuError(cerr, absPath, *riuErr);
        } else {
            cerr << e.what() << '\n';
        }
        return 1;
    }

    return 0;
}
