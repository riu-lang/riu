// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 工作区索引
//
// 设计（P2 Plan A）：
// - 每打开一个文档 → 找最近 yux.toml 作为项目根；找不到则按单文件项目处理
//   （key = 文件绝对路径），保证不互相串扰
// - 每个项目持有一个 Yux 实例 + main FileNode + URI/path → FileNode 的索引
// - 重建时机：didOpen（首次）与 didSave（用户保存）。didChange 不重建，
//   避免编辑期反复跑 ASTBuilder。代价：未保存时跳转/补全停留在上次保存版本
// - ASTBuilder/SDK 解析的 YuxError 一律 catch，记录为 buildError，仍保留旧
//   FileNode 不清空，让 LSP 至少能基于上次成功的快照工作
//
// URI 与路径：
// - file URI → 绝对路径：去 `file://`，Windows 去前导 `/`，URL 解码
// - 路径键归一化：Windows 盘符小写 + 全部 `/` 分隔符

#pragma once

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/yux.h"

class FileNode;

namespace yux::lsp {

// 把 LSP file URI 转为绝对路径（跨平台），失败返回空串
std::string uriToPath(const std::string& uri);
// 把绝对路径转为 LSP file URI
std::string pathToUri(const std::string& absPath);
// 路径归一化：Windows 盘符小写、统一 `/`、清理 `.`/`..`
std::string normalizePath(const std::string& p);

class Project {
public:
    Project();
    ~Project();

    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    // 项目模式：rootDir 须含 yux.toml；entry 由 toml 决定
    // 单文件模式：rootDir 为文件所在目录，mainAbsPath 即文件
    // 两者构造完后状态一致：_yux 已 init、若成功则 _mainFile 非空
    bool buildProject(const std::string& rootDir);
    bool buildSingleFile(const std::string& mainAbsPath);

    // 重建（保留原 root/main 配置，重新跑 SDK + loadMainFile）
    bool rebuild();

    // 查找：归一化路径 → FileNode
    [[nodiscard]] FileNode* fileForPath(const std::string& normPath) const;
    // 项目里所有已加载的 (normPath, FileNode) — 用于跨文件查找
    [[nodiscard]] const std::map<std::string, FileNode*>& allFiles() const { return _byPath; }

    [[nodiscard]] FileNode* sdkFile() const { return _yux ? _yux->sdkFile() : nullptr; }

    [[nodiscard]] const std::string& rootKey() const { return _rootKey; } // 用于 workspace map
    [[nodiscard]] const std::string& mainPath() const { return _mainPath; }
    [[nodiscard]] const std::string& buildError() const { return _buildError; }
    [[nodiscard]] bool ok() const { return _ok; }

private:
    enum class Mode : u8 { Project, SingleFile };
    Mode _mode = Mode::Project;
    std::string _rootKey;  // 归一化后的项目根（或单文件路径）
    std::string _rootDir;  // 原始 rootDir（项目模式）
    std::string _mainPath; // 主文件绝对路径
    std::unique_ptr<Yux> _yux;
    FileNode* _mainFile;
    std::map<std::string, FileNode*> _byPath;
    std::string _buildError;
    bool _ok = false;

    void rebuildIndex();
};

class Workspace {
public:
    // 给定文档 URI，返回它所属的（已构建/新建的）项目；找不到合适项目返回 nullptr
    Project* projectForUri(const std::string& uri);
    // 给定 URI，触发其所属项目重建（didSave 入口）；返回项目（可能为 nullptr）
    Project* rebuildForUri(const std::string& uri);

    // 给定 URI，返回 (project, FileNode)；FileNode 可能为 nullptr（构建失败 / 未被项目导入）
    struct Resolved {
        Project* project = nullptr;
        FileNode* file = nullptr;
        std::string normPath;
    };
    Resolved resolveUri(const std::string& uri);

private:
    // key = 项目根归一化路径（项目模式）或文件归一化路径（单文件模式）
    std::unordered_map<std::string, std::unique_ptr<Project>> _projects;

    // 给定文档绝对路径，找到最近 yux.toml 所在目录；找不到返回空串
    static std::string findProjectRoot(const std::string& docAbsPath);
};

} // namespace yux::lsp
