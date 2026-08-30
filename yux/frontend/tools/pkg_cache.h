// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 包级构建缓存
//
// 一个目录视为一个"包"，缓存文件落在该包对应的 build 目录下，名为 `<pkgname>.cache`。
// 文件首行为编译器指纹（yux.exe mtime + size），重编 yux 即整文件作废；
// 后续每行 `<filename>\t<src-mtime-ns>\t<src-size>` 记录该目录中单个 .yux 源文件的元信息。
//
// 用法：
//   PkgCacheRegistry caches(projectRoot, buildDir);
//   if (!caches.isFresh(srcAbs, objPath)) { ...重编...; caches.mark(srcAbs); }
//   ...
//   caches.flushAll();

#ifndef YUX_LANG_PKG_CACHE_H
#define YUX_LANG_PKG_CACHE_H

#include <string>
#include <unordered_map>

class PkgCache {
public:
    PkgCache() = default;
    // 加载 cachePath 下的缓存。指纹不匹配或文件缺失则置 _valid = false（即所有 isFresh 返回 false）。
    void load(const std::string& cachePath, const std::string& expectedFingerprint);
    // src 是否仍与缓存条目一致，且 obj 存在。
    [[nodiscard]] bool isFresh(const std::string& srcAbs, const std::string& objPath) const;
    // 写入/更新条目。
    void mark(const std::string& srcAbs);
    // 是否需要写回。
    [[nodiscard]] bool dirty() const { return _dirty; }
    // 原子写回到 cachePath。
    bool flush(const std::string& fingerprint);

private:
    std::string _cachePath;
    bool _valid = false;
    bool _dirty = false;
    // filename(basename of src) -> "mtime_ns\tsize"
    std::unordered_map<std::string, std::string> _entries;
};

// 按"源文件所属目录"组织 PkgCache。每个 src 目录一份缓存，
// cache 文件落在该 src 对应的 build 目录下（`<buildDir>/<src-rel-dir>/<dirname>.cache`）。
class PkgCacheRegistry {
public:
    // projectRoot：用于把 srcAbs 转为相对路径；buildDir：build 根目录（`<projectRoot>/build`）。
    PkgCacheRegistry(std::string projectRoot, std::string buildDir);

    // 关联到 srcAbs 所属包的 cache，并查询是否新鲜。
    bool isFresh(const std::string& srcAbs, const std::string& objPath);
    // 标记 srcAbs 已（重）编译，元信息将写入对应包 cache。
    void mark(const std::string& srcAbs);
    // 把所有 dirty 的 PkgCache flush 到磁盘。
    void flushAll();

    [[nodiscard]] const std::string& fingerprint() const { return _fingerprint; }

private:
    // 计算 srcAbs 应该归属的 cache 文件路径（`<buildDir>/<rel-dir>/<dirname>.cache`）。
    [[nodiscard]] std::string cachePathFor(const std::string& srcAbs) const;
    PkgCache& getOrLoad(const std::string& srcAbs);

    std::string _projectRoot;
    std::string _buildDir;
    std::string _fingerprint;
    std::unordered_map<std::string, PkgCache> _caches; // key = cachePath
};

// 计算当前 yux.exe 的指纹（mtime_ns + size，TAB 分隔）。
std::string computeYuxFingerprint();

// 单文件 stamp：与 PkgCache 同指纹规则，但每源文件一份，供并行编译互不抢写。
// 格式两行：`<fingerprint>\n<mtime_ns>\t<size>\n`
bool isStampFresh(const std::string& stampPath, const std::string& srcAbs, const std::string& objPath,
                  const std::string& fingerprint);
void writeStamp(const std::string& stampPath, const std::string& srcAbs, const std::string& fingerprint);

// 把源文件相对项目根的相对路径镜像到 buildDir 下，得到 obj/ir 路径（不含扩展名的 base）。
//   `<projectRoot>/src/A/B/foo.yux` -> `<buildDir>/src/A/B/foo`
// 调用方再拼 `.obj` / `.ll`。
std::string mirroredOutputBase(const std::string& projectRoot, const std::string& buildDir, const std::string& srcAbs);

#endif
