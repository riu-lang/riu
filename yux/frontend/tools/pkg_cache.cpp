// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 包级构建缓存实现
//
// 文件格式（行分隔，TAB 分隔字段）：
//   <yux.exe-mtime-ns>\t<yux.exe-size>     ; 第 1 行：编译器指纹
//   <filename>\t<src-mtime-ns>\t<src-size> ; 后续行：包内每个 .yux 源文件
//
// 第 1 行不匹配当前 yux.exe → 整 cache 作废，等同空文件。
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

std::string computeYuxFingerprint() {
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

void PkgCache::load(const std::string& cachePath, const std::string& expectedFingerprint) {
    _cachePath = cachePath;
    _valid = false;
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
}

bool PkgCache::isFresh(const std::string& srcAbs, const std::string& objPath) const {
    if (!_valid) return false;
    std::error_code ec;
    if (!fs::exists(objPath, ec)) return false;
    std::string filename = fs::path(srcAbs).filename().string();
    auto it = _entries.find(filename);
    if (it == _entries.end()) return false;
    return it->second == entryValue(mtimeNs(srcAbs), fsize(srcAbs));
}

void PkgCache::mark(const std::string& srcAbs) {
    std::string filename = fs::path(srcAbs).filename().string();
    std::string val = entryValue(mtimeNs(srcAbs), fsize(srcAbs));
    auto it = _entries.find(filename);
    if (it == _entries.end() || it->second != val) {
        _entries[filename] = std::move(val);
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
        std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
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
    : _projectRoot(std::move(projectRoot)), _buildDir(std::move(buildDir)), _fingerprint(computeYuxFingerprint()) {}

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
    cache.load(cp, _fingerprint);
    auto [ins, _] = _caches.emplace(cp, std::move(cache));
    return ins->second;
}

bool PkgCacheRegistry::isFresh(const std::string& srcAbs, const std::string& objPath) {
    return getOrLoad(srcAbs).isFresh(srcAbs, objPath);
}

void PkgCacheRegistry::mark(const std::string& srcAbs) {
    getOrLoad(srcAbs).mark(srcAbs);
}

void PkgCacheRegistry::flushAll() {
    for (auto& [_, cache] : _caches) {
        cache.flush(_fingerprint);
    }
}
