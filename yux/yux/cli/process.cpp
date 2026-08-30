// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "process.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace yux::cli {

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

int resolveThreadCount(int threads) {
    if (threads > 0) return threads;
    unsigned n = std::thread::hardware_concurrency();
    return n < 1 ? 1 : static_cast<int>(n);
}

std::uint32_t spawnAndWait(const std::wstring& cmdLine, const std::wstring& workingDir) {
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
        return kSpawnFailed;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

std::uint32_t spawnToLog(const std::wstring& cmdLine, const std::wstring& logPath, const std::wstring& workingDir) {
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(0);

    {
        std::filesystem::path lp(logPath);
        std::error_code ec;
        std::filesystem::create_directories(lp.parent_path(), ec);
    }

    SECURITY_ATTRIBUTES sa{.nLength = sizeof(sa), .lpSecurityDescriptor = nullptr, .bInheritHandle = TRUE};
    HANDLE hLog = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        std::cerr << "Error: cannot create log file " << std::string(logPath.begin(), logPath.end())
                  << " (GLE=" << GetLastError() << ")\n";
        return kSpawnFailed;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.hStdOutput = hLog;
    si.hStdError = hLog;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                             workingDir.empty() ? nullptr : workingDir.c_str(), &si, &pi);

    CloseHandle(hLog);

    if (!ok) {
        std::cerr << "Error: failed to spawn (GLE=" << GetLastError() << ") — cmd: ";
        std::wcerr << cmdLine << L"\n";
        return kSpawnFailed;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

} // namespace yux::cli
