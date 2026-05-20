// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "yux.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <toml.hpp>

#include "ast_builder.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "tools/syntax_error_listener.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

// 全局调试输出开关（声明于 include/types.h，仅 _DEBUG 构建可用），
// 由 `yux build -d` / `yux test -d` 在 main.cpp 中翻成 true；
// 编译器内部多处通过 DEBUG_LOG 宏引用
#ifdef _DEBUG
bool debug = false;
#endif

Yux::Yux() : _sdkFile(nullptr) {}

Yux::~Yux() {
    _moduleBuilders.clear();
    for (auto file : _files) {
        delete file;
    }
    delete _sdkFile;
}

void Yux::addFile(const p<FileNode>& file) {
    _files.push_back(file);
}

SpecRegistry& Yux::specRegistry() {
    if (!_specRegistry) {
        _specRegistry = std::make_unique<SpecRegistry>(this);
        _specRegistry->buildFromAllFiles();
    }
    return *_specRegistry;
}

void Yux::rebuildSpecRegistry() {
    if (!_specRegistry) {
        _specRegistry = std::make_unique<SpecRegistry>(this);
    }
    _specRegistry->buildFromAllFiles();
}

void Yux::validateSpecImpls() {
    if (_specImplValidated) return;
    if (!_specImplChecker) {
        _specImplChecker = std::make_unique<SpecImplChecker>(this);
    }
    _specImplChecker->validate();
    _specImplValidated = true;
}

SpecImplChecker& Yux::specImplChecker() {
    validateSpecImpls();
    return *_specImplChecker;
}

p<FileNode> Yux::createFile(const string& moduleName) {
    auto file = new FileNode(moduleName);

    // 用户模块默认导入 yux.core 模块（`use yux.core.*` 的等价效果）。
    // 通过把 sdk 文件设为 parent scope，使得符号查找在本模块未命中时
    // 自然 fallback 到 yux.core 模块。
    if (_sdkFile && _sdkFile != file) {
        file->setParentScope(_sdkFile);
        file->addImport("yux.core");
    }

    _files.push_back(file);
    // 新文件加入后, 之前跑过的 draft 实现校验可能已经漏看新文件里的声明位
    // (典型: compileSdkDir 在用户文件加载前已经 compile(sdk) → validate,
    // 之后用户文件 createFile 才进来). 清回 flag, 下次 compile 重新跑全套.
    _specImplValidated = false;
    return file;
}

p<FileNode> Yux::createSdkFile() {
    // SDK 自举运行时。文件 sdk/yux/core/*.yux，模块名 "yux.core"。
    // 如果 _sdkFile 已存在，返回现有的，避免多个 SDK 文件互相覆盖。
    if (_sdkFile) {
        return _sdkFile;
    }
    _sdkFile = new FileNode("yux.core");
    _modules["yux.core"] = _sdkFile;
    return _sdkFile;
}

void Yux::initSingleFileRoot(const string& mainFileAbsPath) {
    _projectRoot = std::filesystem::path(mainFileAbsPath).parent_path().string();
    _sourceRoot = _projectRoot;
}

void Yux::initProjectFromDir(const string& rootDir) {
    namespace fs = std::filesystem;
    fs::path root(rootDir);
    fs::path tomlPath = root / "yux.toml";
    if (!fs::exists(tomlPath)) {
        throw YuxError(1, ErrorCode::E5001, rootDir);
    }
    _projectRoot = root.string();
    // 项目模式下，模块/包根路径是 `<projectRoot>/src`
    fs::path srcDir = root / "src";
    _sourceRoot = fs::is_directory(srcDir) ? srcDir.string() : _projectRoot;
    try {
        auto data = toml::parse(tomlPath.string());
        // name 是必填字段：缺失或空串都视为配置错误。
        // toml11 原生 UTF-8，`name="中文"` 能正确读入。
        if (!data.contains("name")) {
            throw YuxError(1, ErrorCode::E5002);
        }
        if (!data.at("name").is_string()) {
            throw YuxError(1, ErrorCode::E5003);
        }
        _projectName = data.at("name").as_string();
        if (_projectName.empty()) {
            throw YuxError(1, ErrorCode::E5004);
        }
        if (data.contains("entry") && data.at("entry").is_string()) {
            _projectEntry = data.at("entry").as_string();
        }
        if (data.contains("version") && data.at("version").is_string()) {
            _projectVersion = data.at("version").as_string();
        }
        // [lib] 表：type = "static" | "dynamic"
        if (data.contains("lib")) {
            const auto& lib = data.at("lib");
            if (!lib.is_table()) {
                throw YuxError(1, ErrorCode::E5005);
            }
            if (lib.contains("type") && lib.at("type").is_string()) {
                _projectLibType = lib.at("type").as_string();
            } else {
                _projectLibType = "static";
            }
            if (_projectLibType != "static" && _projectLibType != "dynamic") {
                throw YuxError(1, ErrorCode::E5006);
            }
            // TODO(dynamic): 暂只实现 static；dynamic 待项目依赖功能完善后做
            if (_projectLibType == "dynamic") {
                throw YuxError(1, ErrorCode::E5007);
            }
        }
        // lib 与 entry 互斥
        if (!_projectLibType.empty() && !_projectEntry.empty()) {
            throw YuxError(1, ErrorCode::E5008);
        }
    } catch (const YuxError&) {
        throw;
    } catch (const std::exception& e) {
        throw YuxError(1, ErrorCode::E5009, e.what());
    }
}

string Yux::projectName() const {
    return _projectName;
}

p<FileNode> Yux::module(const string& moduleName) const {
    auto it = _modules.find(moduleName);
    if (it != _modules.end()) return it->second;
    return nullptr;
}

string Yux::modulePath(const string& moduleName) const {
    auto it = _modulePaths.find(moduleName);
    if (it != _modulePaths.end()) return it->second;
    return {};
}

p<FileNode> Yux::_parseFile(const string& absPath, const string& moduleName, int errorLine) {
    antlr4::ANTLRFileStream stream;
    stream.loadFromFile(absPath);
    yux::yuxLexer lexer(&stream);
    SyntaxErrorListener errListener(absPath, std::cerr);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);
    antlr4::CommonTokenStream tokenStream(&lexer);
    yux::yuxParser parser(&tokenStream);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);
    auto program = parser.program();
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        throw YuxError(errorLine, ErrorCode::E5010, absPath);
    }

    // 测试文件按文件名后缀识别（spec §11.3.3.1）
    bool isTestFile = absPath.size() >= 9 &&
                      absPath.ends_with(".test.yux");
    auto astBuilder = std::make_unique<ASTBuilder>(*this, moduleName, false,
                                                    isTestFile, absPath);
    auto fileNode = astBuilder->build(program);
    _moduleBuilders.push_back(std::move(astBuilder));
    return fileNode;
}

p<FileNode> Yux::loadMainFile(const string& absPath, const string& moduleName) {
    auto fileNode = _parseFile(absPath, moduleName, 1);
    _modules[moduleName] = fileNode;
    _modulePaths[moduleName] = absPath;
    // 新文件加入后, 旧的 draft impl 校验结果失效: 例如 SDK 预编译触发了一次
    // validate, 此时 _seen 还没有该文件里的 `Type : Draft` 登记; 后续
    // boundSatisfied 调用必须重新跑 validate, 否则误判为不满足 → E1106.
    _specImplValidated = false;
    return fileNode;
}

Yux::ModulePathKind Yux::modulePathKind(const string& moduleName) const {
    namespace fs = std::filesystem;
    if (moduleName.empty()) return ModulePathKind::NotFound;
    string rel = moduleName;
    for (auto& c : rel) if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    fs::path filePath = dirPath; filePath += ".yux";
    bool hasFile = fs::exists(filePath) && fs::is_regular_file(filePath);
    bool hasDir  = fs::exists(dirPath) && fs::is_directory(dirPath);
    if (hasFile && hasDir) return ModulePathKind::Conflict;
    if (hasFile) return ModulePathKind::File;
    if (hasDir)  return ModulePathKind::Package;
    return ModulePathKind::NotFound;
}

vector<string> Yux::listPackageYuxChildren(const string& moduleName) const {
    namespace fs = std::filesystem;
    vector<string> out;
    string rel = moduleName;
    for (auto& c : rel) if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return out;
    for (auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != ".yux") continue;
        out.push_back(p.stem().string());
    }
    std::ranges::sort(out);
    return out;
}

vector<string> Yux::listPackageSubdirs(const string& moduleName) const {
    namespace fs = std::filesystem;
    vector<string> out;
    string rel = moduleName;
    for (auto& c : rel) if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return out;
    for (auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_directory()) continue;
        out.push_back(entry.path().filename().string());
    }
    std::ranges::sort(out);
    return out;
}

bool Yux::hasPkgFile(const string& moduleName) const {
    namespace fs = std::filesystem;
    string rel = moduleName;
    for (auto& c : rel) if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    fs::path pkgPath = dirPath / "pkg";
    return fs::exists(pkgPath) && fs::is_regular_file(pkgPath);
}

vector<PkgExportItem> Yux::parsePkgFile(const string& moduleName) const {
    namespace fs = std::filesystem;
    vector<PkgExportItem> items;
    
    string rel = moduleName;
    for (auto& c : rel) if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    fs::path pkgPath = dirPath / "pkg";
    
    if (!fs::exists(pkgPath) || !fs::is_regular_file(pkgPath)) {
        return items;
    }
    
    std::ifstream file(pkgPath);
    if (!file.is_open()) {
        return items;
    }
    
    string line;
    while (std::getline(file, line)) {
        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == string::npos) continue;  // 空行
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        
        // 跳过空行和注释行（以 ; 开头）
        if (line.empty() || line[0] == ';') continue;
        
        // 解析导出项
        PkgExportItem item;
        if (line.size() >= 2 && line.substr(line.size() - 2) == ".*") {
            item.name = line.substr(0, line.size() - 2);
            item.wildcard = true;
        } else {
            item.name = line;
            item.wildcard = false;
        }
        
        if (!item.name.empty()) {
            items.push_back(item);
        }
    }
    
    return items;
}

p<FileNode> Yux::loadModule(const string& moduleName, int errorLine) {
    // 命中缓存
    auto it = _modules.find(moduleName);
    if (it != _modules.end()) return it->second;

    // 循环依赖检测
    for (auto& m : _loadStack) {
        if (m == moduleName) {
            string chain;
            for (auto& s : _loadStack) {
                chain += s;
                chain += " -> ";
            }
            chain += moduleName;
            throw YuxError(errorLine, ErrorCode::E5011, chain);
        }
    }

    // 点分 → 相对路径
    string relPath = moduleName;
    for (auto& c : relPath) {
        if (c == '.') c = '/';
    }
    relPath += ".yux";

    std::filesystem::path fullPath = _sourceRoot.empty()
        ? std::filesystem::path(relPath)
        : std::filesystem::path(_sourceRoot) / relPath;

    if (!std::filesystem::exists(fullPath)) {
        throw YuxError(errorLine, ErrorCode::E5012, moduleName, fullPath.string());
    }

    string absPath = std::filesystem::absolute(fullPath).string();

    _loadStack.push_back(moduleName);
    p<FileNode> fileNode = nullptr;
    try {
        fileNode = _parseFile(absPath, moduleName, errorLine);
    } catch (...) {
        _loadStack.pop_back();
        throw;
    }
    _loadStack.pop_back();

    _modules[moduleName] = fileNode;
    _modulePaths[moduleName] = absPath;
    _loadOrder.push_back(moduleName);
    return fileNode;
}
