// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#undef ERROR
#endif

#include "git_dep.h"

#include "riu.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

using std::string;
using std::vector;
using std::wstring;

namespace {

wstring toWide(const string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), sz);
    return out;
}

wstring quoteArg(const wstring& arg) {
    if (arg.find_first_of(L" \t\"") == wstring::npos) return arg;
    wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(static_cast<size_t>(backslashes) * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(static_cast<size_t>(backslashes), L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(static_cast<size_t>(backslashes) * 2, L'\\');
    out.push_back(L'"');
    return out;
}

string trimCopy(string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        ++i;
    }
    return s.substr(i);
}

bool ieqHex(const string& a, const string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct GitRun {
    int code = 1;
    string output;
};

GitRun runGit(const vector<string>& args, const string& cwd) {
    SetEnvironmentVariableW(L"GIT_TERMINAL_PROMPT", L"0");

    wstring cmd = L"git.exe";
    auto add = [&](const string& a) {
        cmd.push_back(L' ');
        cmd += quoteArg(toWide(a));
    };
    add("-c");
    add("core.autocrlf=false");
    add("-c");
    add("advice.detachedHead=false");
    for (const auto& a : args)
        add(a);

    SECURITY_ATTRIBUTES sa{.nLength = sizeof(sa), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE rd = INVALID_HANDLE_VALUE;
    HANDLE wr = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        return {.code = 1, .output = "Error: failed to create pipe for git\n"};
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);

    wstring wcwd = cwd.empty() ? wstring() : toWide(cwd);
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return {.code = 1,
                .output = string("Error: failed to spawn git.exe (GLE=") + std::to_string(GetLastError()) + ")\n"};
    }

    string out;
    std::array<char, 4096> tmp{};
    DWORD n = 0;
    while (ReadFile(rd, tmp.data(), static_cast<DWORD>(tmp.size()), &n, nullptr) && n > 0) {
        out.append(tmp.data(), tmp.data() + n);
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return {.code = static_cast<int>(code), .output = std::move(out)};
}

[[noreturn]] void throwGit(const GitRun& r) {
    throw GitCommandFailed(r.code, r.output);
}

string canonDest(const std::filesystem::path& dest) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto abs = fs::absolute(dest, ec);
    auto c = fs::weakly_canonical(abs, ec);
    return c.lexically_normal().generic_string();
}

} // namespace

string resolveGitUrl(const string& rootDir, const string& url) {
    namespace fs = std::filesystem;
    if (url.find("://") != string::npos) return url;
    auto at = url.find('@');
    auto colon = url.find(':');
    if (at != string::npos && colon != string::npos && colon > at) return url;
    fs::path p(url);
    std::error_code ec;
    fs::path abs = p.is_absolute() ? fs::absolute(p, ec) : fs::absolute(fs::path(rootDir) / p, ec);
    auto c = fs::weakly_canonical(abs, ec);
    return c.lexically_normal().string();
}

string checkoutGitDep(const string& rootDir, const string& name, const string& url, const string& rev) {
    namespace fs = std::filesystem;
    fs::path dest = fs::path(rootDir) / "build" / "dependences" / name;
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const string resolvedUrl = resolveGitUrl(rootDir, url);

    const bool exists = fs::exists(dest);
    const bool isGit = exists && fs::exists(dest / ".git");

    if (isGit) {
        auto st = runGit({"status", "--porcelain"}, dest.string());
        if (st.code != 0) throwGit(st);
        if (!trimCopy(st.output).empty()) {
            throw RiuError(1, ErrorCode::E5043, name, dest.string());
        }
        auto head = runGit({"rev-parse", "HEAD"}, dest.string());
        if (head.code != 0) throwGit(head);
        if (ieqHex(trimCopy(head.output), rev)) {
            return canonDest(dest);
        }
        auto fetch = runGit({"fetch", "--", resolvedUrl, rev}, dest.string());
        if (fetch.code != 0) throwGit(fetch);
        auto co = runGit({"checkout", "--detach", rev}, dest.string());
        if (co.code != 0) throwGit(co);
        return canonDest(dest);
    }

    auto clone = runGit({"clone", "-q", "--", resolvedUrl, dest.string()}, {});
    if (clone.code != 0) throwGit(clone);
    auto co = runGit({"checkout", "-q", "--detach", rev}, dest.string());
    if (co.code != 0) throwGit(co);
    return canonDest(dest);
}
