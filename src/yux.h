// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_YUX_H
#define YUX_LANG_YUX_H

#include "node/file_node.h"

class ASTBuilder;

// pkg 文件导出项
struct PkgExportItem {
    string name;        // 模块名或包名
    bool wildcard;      // true 表示 "name.*"，false 表示 "name"
};

class Yux {
    vector<p<FileNode>> _files;
    p<FileNode> _sdkFile;

    // 已加载的用户模块：key = 模块名（点分，如 "utils.math.mini"）
    map<string, p<FileNode>> _modules;
    map<string, string> _modulePaths; // 模块名 → 绝对路径
    vector<string> _loadOrder;        // 首次加载顺序，用于后续 codegen 与链接
    vector<string> _loadStack;        // 加载栈，用于循环依赖检测

    // 持有导入模块的 ASTBuilder，使 AST 节点存活至 Yux 析构
    vector<std::unique_ptr<ASTBuilder>> _moduleBuilders;

    // 项目根目录（含 `yux.toml` 的最近祖先目录；否则为主文件所在目录）
    string _projectRoot;
    // 模块查找根目录：项目模式 = `_projectRoot/src`，单文件模式 = `_projectRoot`
    string _sourceRoot;
    // yux.toml 字段；未找到 toml 或字段缺省则为空。
    string _projectName;
    string _projectEntry;
    string _projectVersion;
    // [lib].type："static" / "dynamic"；空 = 非库项目
    // TODO(dynamic)：当前仅 static 生效；dynamic 待项目依赖功能补齐
    string _projectLibType;

public:
    Yux();
    ~Yux();

    void addFile(const p<FileNode>& file);
    p<FileNode> createFile(const string& moduleName);
    p<FileNode> createSdkFile();

    p<FileNode> sdkFile() const { return _sdkFile; }
    const vector<p<FileNode>>& files() const { return _files; }

    // 按模块名加载 `.yux` 文件。首次加载解析并注册，后续命中缓存。
    // errorLine 仅用于错误报告。未找到文件 / 循环依赖时抛 YuxError。
    p<FileNode> loadModule(const string& moduleName, int errorLine = 0);

    // 模块名在文件系统中对应的形态。
    enum class ModulePathKind { NotFound, File, Package, Conflict };
    // 将点分模块名解析到项目根下的路径，判断其形态。
    // Conflict = 同名 `<m>.yux` 与 `<m>/` 并存。
    ModulePathKind modulePathKind(const string& moduleName) const;
    // 列出包（目录）下的直接 `.yux` 子项（去掉 .yux 后缀的简单名）。
    vector<string> listPackageYuxChildren(const string& moduleName) const;
    // 列出包（目录）下的直接子目录名。
    vector<string> listPackageSubdirs(const string& moduleName) const;
    
    // pkg 文件相关
    // 检查包目录下是否存在 pkg 文件
    bool hasPkgFile(const string& moduleName) const;
    // 解析 pkg 文件，返回导出项列表。文件不存在或为空返回空列表。
    vector<PkgExportItem> parsePkgFile(const string& moduleName) const;

    // 解析主入口 `.yux` 文件（不走 moduleName → path 映射）。
    // 产生的 ASTBuilder 被 Yux 持有，AST 节点在 Yux 析构前有效。
    p<FileNode> loadMainFile(const string& absPath, const string& moduleName);

    // 单文件模式：`_projectRoot` 设为主文件所在目录，不解析 yux.toml。
    // 用于 `yux <src.yux>` 调用，产物扁平放在 `build/`。
    void initSingleFileRoot(const string& mainFileAbsPath);
    // 项目模式：`rootDir` 必须包含 `yux.toml`。解析 name/entry/version。
    // `name` 为必填字段；未找到 yux.toml 或缺 `name` 抛 YuxError。
    // toml11 原生 UTF-8，允许中文等非 ASCII 的项目名。
    void initProjectFromDir(const string& rootDir);
    const string& projectRoot() const { return _projectRoot; }
    // 模块/包源码搜索根：项目模式下为 `<projectRoot>/src`，单文件模式下为主文件所在目录
    const string& sourceRoot() const { return _sourceRoot; }
    // 项目名：yux.toml 的 `name` 字段（必填，非空）。
    string projectName() const;
    // yux.toml 的 `entry` 字段，缺省返回空串。
    const string& projectEntry() const { return _projectEntry; }
    const string& projectVersion() const { return _projectVersion; }
    // 是否为库项目；true 时 entry 应为空
    bool isLibProject() const { return !_projectLibType.empty(); }
    const string& projectLibType() const { return _projectLibType; }

    // 已成功加载的用户模块名列表（按首次加载顺序）。
    const vector<string>& loadOrder() const { return _loadOrder; }
    // 按模块名取 FileNode；不存在返回 nullptr。
    p<FileNode> module(const string& moduleName) const;
    // 模块源文件绝对路径；不存在返回空串。
    string modulePath(const string& moduleName) const;

private:
    // 底层解析 + ASTBuilder。内部用。
    p<FileNode> _parseFile(const string& absPath, const string& moduleName, int errorLine);
};

#endif //YUX_LANG_YUX_H
