// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 包级构建缓存实现
//
// 文件格式（行分隔，TAB 分隔字段）：
//   <riu.exe-mtime-ns>\t<riu.exe-size>     ; 第 1 行：编译器指纹
//   <filename>\t<src-mtime-ns>\t<src-size> ; 后续行：包内每个 .ut 源文件
//   <pkg-key>\t<pkg-mtime-ns>\t<pkg-size>   ; 保留键：同目录 pkg（不存在为 0/0）
//
// 第 1 行不匹配当前 riu.exe → 整 cache 作废，等同空文件。
// 写入用 tmp + rename 原子替换。

#include "pkg_cache.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

// Windows 文件名不能包含尖括号，因此不会与真实源文件 basename 冲突。
static constexpr const char* PKG_ENTRY_KEY = "<pkg>";

// ==================== 工具 ====================

// 取 file 的 last_write_time，返回 ns（time_since_epoch）。文件不存在返回 0。
static int64_t mtimeNs(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return 0;
    auto ftime = fs::last_write_time(path, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(ftime.time_since_epoch()).count();
}

static uintmax_t fsize(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return 0;
    auto sz = fs::file_size(path, ec);
    if (ec) return 0;
    return sz;
}

static std::string entryValue(int64_t ns, uintmax_t sz) {
    return std::to_string(ns) + "\t" + std::to_string(sz);
}

// ==================== 公共 API ====================

std::string computeRiuFingerprint() {
    std::array<char, MAX_PATH> buf{};
    DWORD n = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= MAX_PATH) return "";
    std::string exePath(buf.data(), n);
    return entryValue(mtimeNs(exePath), fsize(exePath));
}

std::string mirroredOutputBase(const std::string& projectRoot, const std::string& buildDir, const std::string& srcAbs) {
    std::error_code ec;
    fs::path rel = fs::relative(srcAbs, projectRoot, ec);
    if (ec || rel.empty()) {
        // 退路：项目根外的文件，按 basename 平铺到 buildDir
        rel = fs::path(srcAbs).filename();
    }
    fs::path out = fs::path(buildDir) / rel;
    out.replace_extension();
    return out.generic_string();
}

// ==================== PkgCache ====================

void PkgCache::load(const std::string& cachePath, const std::string& expectedFingerprint, const std::string& pkgPath) {
    _cachePath = cachePath;
    _valid = false;
    _pkgFresh = false;
    _dirty = false;
    _entries.clear();

    std::ifstream f(cachePath, std::ios::binary);
    if (!f.is_open()) return;

    std::string line;
    if (!std::getline(f, line)) return;
    if (line != expectedFingerprint) return; // 指纹不匹配 → 作废

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // 拆首个 TAB → filename，余下作为 value（mtime\tsize）
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string filename = line.substr(0, tab);
        std::string value = line.substr(tab + 1);
        _entries.emplace(std::move(filename), std::move(value));
    }
    _valid = true;
    auto pkgIt = _entries.find(PKG_ENTRY_KEY);
    _pkgFresh = pkgIt != _entries.end() && pkgIt->second == entryValue(mtimeNs(pkgPath), fsize(pkgPath));
}

bool PkgCache::isFresh(const std::string& srcAbs, const std::string& objPath) const {
    if (!_valid || !_pkgFresh) return false;
    std::error_code ec;
    if (!fs::exists(objPath, ec)) return false;
    std::string filename = fs::path(srcAbs).filename().string();
    auto it = _entries.find(filename);
    if (it == _entries.end()) return false;
    return it->second == entryValue(mtimeNs(srcAbs), fsize(srcAbs));
}

void PkgCache::mark(const std::string& srcAbs, const std::string& pkgPath) {
    std::string filename = fs::path(srcAbs).filename().string();
    std::string val = entryValue(mtimeNs(srcAbs), fsize(srcAbs));
    auto it = _entries.find(filename);
    if (it == _entries.end() || it->second != val) {
        _entries[filename] = std::move(val);
        _dirty = true;
    }
    std::string pkgVal = entryValue(mtimeNs(pkgPath), fsize(pkgPath));
    auto pkgIt = _entries.find(PKG_ENTRY_KEY);
    if (pkgIt == _entries.end() || pkgIt->second != pkgVal) {
        _entries[PKG_ENTRY_KEY] = std::move(pkgVal);
        _dirty = true;
    }
}

bool PkgCache::flush(const std::string& fingerprint) {
    if (!_dirty && _valid) return true;
    if (_cachePath.empty()) return false;

    std::error_code ec;
    fs::create_directories(fs::path(_cachePath).parent_path(), ec);

    std::string tmpPath = _cachePath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
        if (!f.is_open()) return false;
        f << fingerprint << '\n';
        for (auto& [filename, value] : _entries) {
            f << filename << '\t' << value << '\n';
        }
        f.flush();
        if (!f.good()) return false;
    }
    fs::rename(tmpPath, _cachePath, ec);
    if (ec) {
        // Windows 上 rename 到已存在文件可能失败，先删再 rename
        fs::remove(_cachePath, ec);
        ec.clear();
        fs::rename(tmpPath, _cachePath, ec);
        if (ec) return false;
    }
    _dirty = false;
    _valid = true;
    return true;
}

// ==================== PkgCacheRegistry ====================

PkgCacheRegistry::PkgCacheRegistry(std::string projectRoot, std::string buildDir)
    : _projectRoot(std::move(projectRoot)), _buildDir(std::move(buildDir)), _fingerprint(computeRiuFingerprint()) {}

std::string PkgCacheRegistry::cachePathFor(const std::string& srcAbs) const {
    // 镜像得到 obj base，取其父目录作为包目录；包名 = 该目录的 filename
    std::string base = mirroredOutputBase(_projectRoot, _buildDir, srcAbs);
    fs::path pkgDir = fs::path(base).parent_path();
    std::string pkgName = pkgDir.filename().string();
    if (pkgName.empty()) pkgName = "_root";
    return (pkgDir / (pkgName + ".cache")).generic_string();
}

PkgCache& PkgCacheRegistry::getOrLoad(const std::string& srcAbs) {
    std::string cp = cachePathFor(srcAbs);
    auto it = _caches.find(cp);
    if (it != _caches.end()) return it->second;
    PkgCache cache;
    std::string pkgPath = (fs::path(srcAbs).parent_path() / "pkg").string();
    cache.load(cp, _fingerprint, pkgPath);
    auto [ins, _] = _caches.emplace(cp, std::move(cache));
    return ins->second;
}

bool PkgCacheRegistry::isFresh(const std::string& srcAbs, const std::string& objPath) {
    return getOrLoad(srcAbs).isFresh(srcAbs, objPath);
}

void PkgCacheRegistry::mark(const std::string& srcAbs) {
    std::string pkgPath = (fs::path(srcAbs).parent_path() / "pkg").string();
    getOrLoad(srcAbs).mark(srcAbs, pkgPath);
}

void PkgCacheRegistry::flushAll() {
    for (auto& [_, cache] : _caches) {
        cache.flush(_fingerprint);
    }
}

// ==================== 单文件 stamp ====================

bool isStampFresh(const std::string& stampPath, const std::string& srcAbs, const std::string& objPath,
                  const std::string& fingerprint) {
    std::error_code ec;
    if (!fs::exists(objPath, ec)) return false;
    std::ifstream f(stampPath, std::ios::binary);
    if (!f.is_open()) return false;
    std::string line;
    if (!std::getline(f, line) || line != fingerprint) return false;
    if (!std::getline(f, line)) return false;
    return line == entryValue(mtimeNs(srcAbs), fsize(srcAbs));
}

void writeStamp(const std::string& stampPath, const std::string& srcAbs, const std::string& fingerprint) {
    std::error_code ec;
    fs::create_directories(fs::path(stampPath).parent_path(), ec);

    std::string tmpPath = stampPath + ".tmp";
    {
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc); // NOLINT(bugprone-signed-bitwise)
        if (!f.is_open()) return;
        f << fingerprint << '\n';
        f << entryValue(mtimeNs(srcAbs), fsize(srcAbs)) << '\n';
        f.flush();
        if (!f.good()) return;
    }
    fs::rename(tmpPath, stampPath, ec);
    if (ec) {
        fs::remove(stampPath, ec);
        ec.clear();
        fs::rename(tmpPath, stampPath, ec);
    }
}
