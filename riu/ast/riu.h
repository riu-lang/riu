// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_RIU_H
#define RIU_LANG_RIU_H

#include "node/file_node.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>

class RdBuilder;
class SpecRegistry;
class SpecImplChecker;

namespace mod_decl {
class NodeOwner;
}

// pkg 文件导出项（§10.2.4.2）
struct PkgExportItem {
    string name;              // 源模块名（如 "add"）
    string rename;            // 重命名导出别名（空 = 用 name 本身，如 "addition"）
    bool wildcard = false;    // true 表示 "name.*"，false 表示 "name"
    vector<string> toTargets; // 空 = 公开；非空 = 定向开放（`to t1; t2`）
    int sourceLine = 1;       // pkg 诊断定位。
};

[[nodiscard]] inline string pkgExportName(const PkgExportItem& item) {
    return item.rename.empty() ? item.name : item.rename;
}

[[nodiscard]] inline bool pkgExportIsPublic(const PkgExportItem& item) {
    return item.toTargets.empty();
}

// 解析给定路径的 pkg 文件。文件不存在或不是普通文件 → 空列表。
// 非法行抛 E5020（§10.2.4.2.3），不再静默跳过。
[[nodiscard]] vector<PkgExportItem> parsePkgFileAt(const string& pkgPath);

// riu.toml 产物上的一条外部链接（#22）。`path` / `dll` 带 `//` 或 `./` 前缀，不含库后缀。
struct ExternalLink {
    string path;
    string dll; // 空 = 不复制运行时库
};

struct Library {
    string name; // 产出文件名（无扩展名）；解析时已把缺省补成项目名
    string type; // "static" | "dynamic"
    string lib_mod;
    vector<ExternalLink> external_links;
};

struct Executable {
    string name;
    string entry;
    vector<ExternalLink> external_links;
};

// `[dependencies]` 一条。恰好一种来源（#9）。
enum class DepSourceKind : std::uint8_t { Sdk, Path, Git };

struct Dependency {
    string name; // 表键，须等于对方顶层 `name`
    DepSourceKind kind = DepSourceKind::Path;
    string sdk;
    string path; // 相对声明方项目根，或绝对路径
    string git;
    string rev;
};

// 解析后的项目身份 + 产物。供 Riu 与驱动（读 SDK toml 取传递链接）共用。
struct ProjectConfig {
    string name;
    string version;
    std::optional<Library> library;
    vector<Executable> executables;
    vector<Dependency> dependencies;
};

// `lib_mod` 扫到的一个源文件（模块名相对该包源根）。
struct LibModFile {
    string absPath;
    string moduleName;
};

// 图中一个依赖库（不含根项目）。后序：被依赖者在前。
struct ResolvedDep {
    string name;
    string projectRoot;
    string sourceRoot;
    ProjectConfig config;
    DepSourceKind kind = DepSourceKind::Path;
    string sdk; // kind==Sdk 时为工具链 id
};

// 读 `tomlPath`。缺字段 / 旧 schema / 非法前缀等抛 RiuError（E5002–E5008、E5021+、E5034+）。
[[nodiscard]] ProjectConfig parseRiuToml(const string& tomlPath);

// git clone / fetch / checkout 非零：透传 git 的输出与退出码，不是 RiuError。
class GitCommandFailed : public std::runtime_error {
    int _exitCode = 1;
    string _output;

public:
    GitCommandFailed(int exitCode, string output)
        : std::runtime_error(output.empty() ? string("git command failed") : output),
          _exitCode(exitCode == 0 ? 1 : exitCode), _output(std::move(output)) {}
    [[nodiscard]] int exitCode() const { return _exitCode; }
    [[nodiscard]] const string& output() const { return _output; }
};

// `{ path }` / `{ sdk }` / `{ git, rev }`：递归读 toml，校验键=对方 name、是库、同名不同源。
// locateSdk(`id`) → `sdk/<id>/`；空指针则跳过 sdk 边。
// git 检出到 `<rootDir>/build/dependences/<name>/`；失败抛 GitCommandFailed。
using SdkPackageLocator = string (*)(const string& id);
[[nodiscard]] vector<ResolvedDep> resolvePathDepGraph(const string& rootDir, const ProjectConfig& rootCfg,
                                                      SdkPackageLocator locateSdk = nullptr);

// 按 `[library].lib_mod` 列出源文件。找不到 → 空。
[[nodiscard]] vector<LibModFile> collectLibModFiles(const string& sourceRoot, const string& libMod);

class Riu {
    // 先于 FileNode 析构：intern 指针活过 AST。
    TypeIntern _typeIntern;
    StringIntern _stringIntern;
    vector<FileNode*> _files;
    FileNode* _sdkFile;

    // 已加载的用户模块：key = 模块名（点分，如 "utils.math.mini"）
    map<string, FileNode*> _modules;
    map<string, string> _modulePaths; // 模块名 → 绝对路径
    vector<string> _loadOrder;        // 首次加载顺序，用于后续 codegen 与链接
    vector<string> _loadStack;        // 加载栈，用于循环依赖检测

    // 持有导入模块的 RdBuilder，使 AST 节点存活至 Riu 析构
    vector<std::unique_ptr<RdBuilder>> _rdBuilders;
    // .ud 重建的节点（不经 RdBuilder）
    vector<std::unique_ptr<mod_decl::NodeOwner>> _declOwners;

    // 项目根目录（含 `riu.toml` 的最近祖先目录；否则为主文件所在目录）
    string _projectRoot;
    // 模块查找根目录：项目模式 = `_projectRoot/src`，文件模式 = `_projectRoot`
    string _sourceRoot;
    // riu.toml；未走项目模式时 name 为空。
    ProjectConfig _config;
    // 额外 `.ud` 根：源码在 `sourceRoot` 下时写到 `outputRoot`（相对该源根镜像）。
    struct DeclOutputRoot {
        string sourceRoot;
        string outputRoot;
    };
    vector<DeclOutputRoot> _declOutputRoots;
    // parseSdkDir 写 SDK .ud 时需要 item 原文（skeleton）；无 riu.toml 的
    // riu-check 默认不拷。缺骨架的 .ud 会丢 #Spec 默认体（Eq 等）。
    bool _keepItemSourceText = false;

public:
    // 构造/析构都在 analyzer/riu_spec.cpp：unique_ptr<SpecRegistry /
    // SpecImplChecker> 的构造期 unwind 与析构都需要完整类型。
    Riu();
    ~Riu();

    TypeIntern& typeIntern() { return _typeIntern; }
    [[nodiscard]] const TypeIntern& typeIntern() const { return _typeIntern; }
    StringIntern& stringIntern() { return _stringIntern; }
    [[nodiscard]] const StringIntern& stringIntern() const { return _stringIntern; }

    void addFile(FileNode* file);
    FileNode* createFile(const string& moduleName);
    FileNode* createSdkFile();

    // .ud 加载：把已构造的 FileNode 登记为模块（不 parse）
    void bindModule(FileNode* file, const string& absPath, const string& moduleName);
    // SDK 自举等场景先登记已知依赖路径，供 pkg `to` 目标校验使用；不代表模块已加载。
    void registerModulePath(const string& absPath, const string& moduleName);
    void keepRdBuilder(std::unique_ptr<RdBuilder> builder);
    void adoptDeclOwner(std::unique_ptr<mod_decl::NodeOwner> owner);
    // parseSdkDir 即将写出 .ud 时打开：RdBuilder 保留 spec/泛型/全局原文。
    void setKeepItemSourceText(bool v) { _keepItemSourceText = v; }
    [[nodiscard]] bool keepItemSourceText() const { return _keepItemSourceText || !_config.name.empty(); }

    [[nodiscard]] FileNode* sdkFile() const { return _sdkFile; }
    // 设置外部 SDK 文件（不转移所有权）。用于批量测试中多文件共享一次 SDK 加载。
    // 调用方负责保证 sdkFile 在 Riu 使用期间存活，并在 Riu 析构前调用
    // setSdkFile(nullptr) 避免 double-free。
    void setSdkFile(FileNode* sdkFile) { _sdkFile = sdkFile; }
    [[nodiscard]] const vector<FileNode*>& files() const { return _files; }

    // draft 注册表 (spec §12). 首次访问时按当前已加载的 _files + _sdkFile
    // 全量索引一次. 后续如新增动态加载模块, 调用 rebuildSpecRegistry().
    // 实现在 analyzer/riu_spec.cpp，riu.cpp 不 include analyzer/。
    SpecRegistry& specRegistry();
    void rebuildSpecRegistry();

    // §12.2 / §12.3 / §12.5 显式 draft 实现校验 (Phase 3.2.e).
    // 首次调用时构造 SpecImplChecker 跑全套校验, 后续调用直接返回.
    // SemaPass::run 起始处兜底；Compiler::compile / riu-check 仍可显式调用.
    void validateSpecImpls();

    // 拿到长生命周期的 SpecImplChecker (Phase 3.3 边界匹配需要复用其
    // _seen 显式实现表与 typeSatisfiesSpec 结构匹配 helper).
    // 触发时自动调用 validateSpecImpls() 一次.
    SpecImplChecker& specImplChecker();

    // 按模块名加载 `.ut` 文件。首次加载解析并注册，后续命中缓存。
    // errorLine 仅用于错误报告。未找到文件 / 循环依赖时抛 RiuError。
    FileNode* loadModule(const string& moduleName, int errorLine = 0);

    // 模块名在文件系统中对应的形态。
    enum class ModulePathKind : std::uint8_t { NotFound, File, Package, Conflict };
    // 将点分模块名解析到项目根下的路径，判断其形态。
    // Conflict = 同名 `<m>.ut` 与 `<m>/` 并存。
    // 项目根没有时，再认 `registerModulePath` 的 SDK 独立包（文件或目录）。
    [[nodiscard]] ModulePathKind modulePathKind(const string& moduleName) const;
    // 列出包（目录）下的直接 `.ut` 子项（去掉 .ut 后缀的简单名）。
    [[nodiscard]] vector<string> listPackageRiuChildren(const string& moduleName) const;
    // 列出包（目录）下的直接子目录名。
    [[nodiscard]] vector<string> listPackageSubdirs(const string& moduleName) const;

    // pkg 文件相关
    // 检查包目录下是否存在 pkg 文件
    [[nodiscard]] bool hasPkgFile(const string& moduleName) const;
    // 解析包目录下的 pkg 文件（走 parsePkgFileAt）。文件不存在或为空返回空列表。
    [[nodiscard]] vector<PkgExportItem> parsePkgFile(const string& moduleName) const;
    // 调用方可见清单：包内补入未列出的兄弟；包外仅公开项和定向项。
    [[nodiscard]] vector<PkgExportItem> visiblePkgItems(const FileNode* caller, const string& package) const;
    // 导出路径转源路径；每跨一层包边界检查清单，保留末尾类型名。
    [[nodiscard]] string resolvePkgPath(const FileNode* caller, const string& path, int line,
                                        const string& package = "") const;

    // 解析主入口 `.ut` 文件（不走 moduleName → path 映射）。
    // 产生的 RdBuilder 被 Riu 持有，AST 节点在 Riu 析构前有效。
    FileNode* loadMainFile(const string& absPath, const string& moduleName);

    // codegen 用：若当前是 .ud 重建的接口树，则整文件 parse 出带体的 AST。
    // 调用方已持有的 wildcard / alias 指针仍指向旧 FileNode（接口足够）；返回值给 codegen。
    FileNode* ensureFullAst(const string& absPath, const string& moduleName);

    // 从文件路径初始化项目根和源码根（不解析 riu.toml）。
    // 设 _projectRoot = _sourceRoot = 文件所在目录，用于 riu-check / LSP
    // 等工具的单文件快速检查。
    void initFileRoot(const string& mainFileAbsPath);
    // 项目模式：`rootDir` 必须包含 `riu.toml`。解析身份与产物（#22）。
    // `name` / `version` 必填；至少一份 `[library]` 或一条 `[[executable]]`。
    void initProjectFromDir(const string& rootDir);
    // path 依赖的 `.ud` 写到调用方 `build/deps/<name>/`，不写对方自己的 `build/`。
    void addDeclOutputRoot(const string& sourceRoot, const string& outputRoot);
    [[nodiscard]] const string& projectRoot() const { return _projectRoot; }
    // 模块/包源码搜索根：项目模式下为 `<projectRoot>/src`，文件模式下为主文件所在目录
    [[nodiscard]] const string& sourceRoot() const { return _sourceRoot; }
    // 项目名：riu.toml 的 `name` 字段（必填，非空）。不是默认输出文件名。
    [[nodiscard]] string projectName() const;
    [[nodiscard]] const string& projectVersion() const { return _config.version; }
    [[nodiscard]] const ProjectConfig& projectConfig() const { return _config; }
    [[nodiscard]] const Library* library() const { return _config.library ? &*_config.library : nullptr; }
    [[nodiscard]] const vector<Executable>& executables() const { return _config.executables; }
    [[nodiscard]] bool isLibProject() const { return _config.library.has_value(); }

    // 已成功加载的用户模块名列表（按首次加载顺序）。
    [[nodiscard]] const vector<string>& loadOrder() const { return _loadOrder; }
    // 按模块名取 FileNode；不存在返回 nullptr。
    [[nodiscard]] FileNode* module(const string& moduleName) const;
    // 模块源文件绝对路径；不存在返回空串。
    [[nodiscard]] string modulePath(const string& moduleName) const;

private:
    [[nodiscard]] string packageSourceDir(const string& package) const;
    [[nodiscard]] bool isInsidePackage(const FileNode* caller, const string& package) const;
    // 底层解析 + RdBuilder。内部用。
    FileNode* _parseFile(const string& absPath, const string& moduleName, int errorLine);

    // 项目模式（有 riu.toml / 项目名）才读写 `.ud`；riu-check 单文件不写。
    [[nodiscard]] bool declCacheEnabled() const;
    [[nodiscard]] string declPathFor(const string& srcAbs) const;
    void writeDeclIfPossible(FileNode* file, const string& srcAbs);

    std::unique_ptr<SpecRegistry> _specRegistry;
    std::unique_ptr<SpecImplChecker> _specImplChecker;
    bool _specImplValidated = false;
};

#endif // RIU_LANG_RIU_H
