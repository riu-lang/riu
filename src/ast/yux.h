// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_YUX_H
#define YUX_LANG_YUX_H

#include "node/file_node.h"

#include <memory>

class ASTBuilder;
class SpecRegistry;
class SpecImplChecker;

// pkg 文件导出项
struct PkgExportItem {
    string name;   // 源模块名（如 "add"）
    string rename; // 重命名导出别名（空 = 用 name 本身，如 "addition"）
    bool wildcard; // true 表示 "name.*"，false 表示 "name"
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
    // 模块查找根目录：项目模式 = `_projectRoot/src`，文件模式 = `_projectRoot`
    string _sourceRoot;
    // yux.toml 字段；未找到 toml 或字段缺省则为空。
    string _projectName;
    string _projectEntry;
    string _projectVersion;
    // [lib].type："static" / "dynamic"；空 = 非库项目
    // TODO(dynamic)：当前仅 static 生效；dynamic 待项目依赖功能补齐
    string _projectLibType;
    // [link].libs：系统库名列表（不含 .lib 后缀），如 ["user32", "shell32"]
    vector<string> _projectLinkLibs;

public:
    Yux();
    ~Yux();

    void addFile(const p<FileNode>& file);
    p<FileNode> createFile(const string& moduleName);
    p<FileNode> createSdkFile();

    [[nodiscard]] p<FileNode> sdkFile() const { return _sdkFile; }
    // 设置外部 SDK 文件（不转移所有权）。用于批量测试中多文件共享一次 SDK 加载。
    // 调用方负责保证 sdkFile 在 Yux 使用期间存活，并在 Yux 析构前调用
    // setSdkFile(nullptr) 避免 double-free。
    void setSdkFile(p<FileNode> sdkFile) { _sdkFile = sdkFile; }
    [[nodiscard]] const vector<p<FileNode>>& files() const { return _files; }

    // draft 注册表 (spec §12). 首次访问时按当前已加载的 _files + _sdkFile
    // 全量索引一次. 后续如新增动态加载模块, 调用 rebuildSpecRegistry().
    SpecRegistry& specRegistry();
    void rebuildSpecRegistry();

    // §12.2 / §12.3 / §12.5 显式 draft 实现校验 (Phase 3.2.e).
    // 首次调用时构造 SpecImplChecker 跑全套校验, 后续调用直接返回.
    // 由 Compiler::compile() 起始处调用, SDK 与用户文件统一一次.
    void validateSpecImpls();

    // 拿到长生命周期的 SpecImplChecker (Phase 3.3 边界匹配需要复用其
    // _seen 显式实现表与 typeSatisfiesSpec 结构匹配 helper).
    // 触发时自动调用 validateSpecImpls() 一次.
    SpecImplChecker& specImplChecker();

    // 按模块名加载 `.yux` 文件。首次加载解析并注册，后续命中缓存。
    // errorLine 仅用于错误报告。未找到文件 / 循环依赖时抛 YuxError。
    p<FileNode> loadModule(const string& moduleName, int errorLine = 0);

    // 模块名在文件系统中对应的形态。
    enum class ModulePathKind : std::uint8_t { NotFound, File, Package, Conflict };
    // 将点分模块名解析到项目根下的路径，判断其形态。
    // Conflict = 同名 `<m>.yux` 与 `<m>/` 并存。
    [[nodiscard]] ModulePathKind modulePathKind(const string& moduleName) const;
    // 列出包（目录）下的直接 `.yux` 子项（去掉 .yux 后缀的简单名）。
    [[nodiscard]] vector<string> listPackageYuxChildren(const string& moduleName) const;
    // 列出包（目录）下的直接子目录名。
    [[nodiscard]] vector<string> listPackageSubdirs(const string& moduleName) const;

    // pkg 文件相关
    // 检查包目录下是否存在 pkg 文件
    [[nodiscard]] bool hasPkgFile(const string& moduleName) const;
    // 解析 pkg 文件，返回导出项列表。文件不存在或为空返回空列表。
    [[nodiscard]] vector<PkgExportItem> parsePkgFile(const string& moduleName) const;

    // 解析主入口 `.yux` 文件（不走 moduleName → path 映射）。
    // 产生的 ASTBuilder 被 Yux 持有，AST 节点在 Yux 析构前有效。
    p<FileNode> loadMainFile(const string& absPath, const string& moduleName);

    // 从文件路径初始化项目根和源码根（不解析 yux.toml）。
    // 设 _projectRoot = _sourceRoot = 文件所在目录，用于 yux-check / LSP
    // 等工具的单文件快速检查。
    void initFileRoot(const string& mainFileAbsPath);
    // 项目模式：`rootDir` 必须包含 `yux.toml`。解析 name/entry/version。
    // `name` 为必填字段；未找到 yux.toml 或缺 `name` 抛 YuxError。
    // toml11 原生 UTF-8，允许中文等非 ASCII 的项目名。
    void initProjectFromDir(const string& rootDir);
    [[nodiscard]] const string& projectRoot() const { return _projectRoot; }
    // 模块/包源码搜索根：项目模式下为 `<projectRoot>/src`，文件模式下为主文件所在目录
    [[nodiscard]] const string& sourceRoot() const { return _sourceRoot; }
    // 项目名：yux.toml 的 `name` 字段（必填，非空）。
    [[nodiscard]] string projectName() const;
    // yux.toml 的 `entry` 字段，缺省返回空串。
    [[nodiscard]] const string& projectEntry() const { return _projectEntry; }
    [[nodiscard]] const string& projectVersion() const { return _projectVersion; }
    // 是否为库项目；true 时 entry 应为空
    [[nodiscard]] bool isLibProject() const { return !_projectLibType.empty(); }
    [[nodiscard]] const string& projectLibType() const { return _projectLibType; }
    // [link].libs：项目级系统库名列表
    [[nodiscard]] const vector<string>& projectLinkLibs() const { return _projectLinkLibs; }

    // 已成功加载的用户模块名列表（按首次加载顺序）。
    [[nodiscard]] const vector<string>& loadOrder() const { return _loadOrder; }
    // 按模块名取 FileNode；不存在返回 nullptr。
    [[nodiscard]] p<FileNode> module(const string& moduleName) const;
    // 模块源文件绝对路径；不存在返回空串。
    [[nodiscard]] string modulePath(const string& moduleName) const;

private:
    // 底层解析 + ASTBuilder。内部用。
    p<FileNode> _parseFile(const string& absPath, const string& moduleName, int errorLine);

    std::unique_ptr<SpecRegistry> _specRegistry;
    std::unique_ptr<SpecImplChecker> _specImplChecker;
    bool _specImplValidated = false;
};

#endif // YUX_LANG_YUX_H
