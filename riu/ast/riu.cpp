// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "riu.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

#include <toml.hpp>

#include "mod_decl.h"
#include "rd_builder.h"

// 全局调试输出开关（声明于 include/types.h，仅 _DEBUG 构建可用），
// 由 `riu build -d` / `riu test -d` 在 main.cpp 中翻成 true；
// 编译器内部多处通过 DEBUG_LOG 宏引用
#ifdef _DEBUG
bool debug = false;
#endif

void Riu::keepRdBuilder(std::unique_ptr<RdBuilder> builder) {
    _rdBuilders.push_back(std::move(builder));
}

void Riu::adoptDeclOwner(std::unique_ptr<mod_decl::NodeOwner> owner) {
    _declOwners.push_back(std::move(owner));
}

void Riu::bindModule(FileNode* file, const string& absPath, const string& moduleName) {
    if (file) file->setRiu(this);
    _modules[moduleName] = file;
    _modulePaths[moduleName] = absPath;
    if (file) file->setSourcePath(absPath);
    _specImplValidated = false;
}

void Riu::registerModulePath(const string& absPath, const string& moduleName) {
    _modulePaths[moduleName] = absPath;
}

void Riu::addFile(FileNode* file) {
    if (file) file->setRiu(this);
    _files.push_back(file);
}

FileNode* Riu::createFile(const string& moduleName) {
    auto file = new FileNode(moduleName);
    file->setRiu(this);

    // 用户模块默认导入 riu.core 模块（`use riu.core.*` 的等价效果）。
    // 通过把 sdk 文件设为 parent scope，使得符号查找在本模块未命中时
    // 自然 fallback 到 riu.core 模块。
    if (_sdkFile && _sdkFile != file) {
        file->setParentScope(_sdkFile);
        file->addImport("riu.core");
    }

    _files.push_back(file);
    // 新文件加入后, 之前跑过的 draft 实现校验可能已经漏看新文件里的声明位
    // (典型: compileSdkDir 在用户文件加载前已经 compile(sdk) → validate,
    // 之后用户文件 createFile 才进来). 清回 flag, 下次 compile 重新跑全套.
    _specImplValidated = false;
    return file;
}

FileNode* Riu::createSdkFile() {
    // SDK 自举运行时。文件 sdk/riu/core/*.ut，模块名 "riu.core"。
    // 如果 _sdkFile 已存在，返回现有的，避免多个 SDK 文件互相覆盖。
    if (_sdkFile) {
        return _sdkFile;
    }
    _sdkFile = new FileNode("riu.core");
    _sdkFile->setRiu(this);
    _modules["riu.core"] = _sdkFile;
    return _sdkFile;
}

namespace {

string asciiLower(string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool isReservedDeviceName(const string& name) {
    string stem = name;
    auto dot = stem.find('.');
    if (dot != string::npos) stem = stem.substr(0, dot);
    string l = asciiLower(stem);
    static const std::unordered_set<string> kNames = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    };
    return kNames.contains(l);
}

bool productNameHasPathSep(const string& name) {
    return name.find('/') != string::npos || name.find('\\') != string::npos;
}

void validateProductName(const string& name) {
    if (name.empty() || name == "." || name == ".." || productNameHasPathSep(name) || isReservedDeviceName(name)) {
        throw RiuError(1, ErrorCode::E5028, name);
    }
}

bool hasForbiddenLibSuffix(const string& spec) {
    auto slash = spec.find_last_of("/\\");
    string file = slash == string::npos ? spec : spec.substr(slash + 1);
    string lower = asciiLower(file);
    return lower.ends_with(".lib") || lower.ends_with(".dll") || lower.ends_with(".so") || lower.ends_with(".a") ||
           lower.ends_with(".dylib");
}

bool isDottedModulePath(const string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    while (i <= s.size()) {
        auto j = s.find('.', i);
        string part = s.substr(i, j == string::npos ? string::npos : j - i);
        if (part.empty()) return false;
        if (j == string::npos) break;
        i = j + 1;
    }
    return true;
}

void validateLinkSpec(const string& spec) {
    if (!spec.starts_with("//") && !spec.starts_with("./")) {
        throw RiuError(1, ErrorCode::E5026, spec);
    }
    string rest = spec.substr(2);
    if (rest.empty()) {
        throw RiuError(1, ErrorCode::E5026, spec);
    }
    if (hasForbiddenLibSuffix(spec)) {
        throw RiuError(1, ErrorCode::E5027, spec);
    }
}

vector<ExternalLink> parseExternalLinks(const toml::value& parent) {
    vector<ExternalLink> out;
    if (!parent.contains("external_links")) return out;
    const auto& v = parent.at("external_links");
    if (!v.is_array()) {
        throw RiuError(1, ErrorCode::E5031);
    }
    for (const auto& item : v.as_array()) {
        if (!item.is_table()) {
            throw RiuError(1, ErrorCode::E5031);
        }
        if (!item.contains("path") || !item.at("path").is_string()) {
            throw RiuError(1, ErrorCode::E5032);
        }
        ExternalLink link;
        link.path = item.at("path").as_string();
        validateLinkSpec(link.path);
        if (item.contains("dll")) {
            if (!item.at("dll").is_string()) {
                throw RiuError(1, ErrorCode::E5032);
            }
            link.dll = item.at("dll").as_string();
            validateLinkSpec(link.dll);
        }
        out.push_back(std::move(link));
    }
    return out;
}

string tomlFieldString(const toml::value& item, const char* field, const string& depName) {
    if (!item.at(field).is_string()) {
        throw RiuError(1, ErrorCode::E5009, string("`[dependencies].") + depName + "." + field + "` must be a string");
    }
    return item.at(field).as_string();
}

vector<Dependency> parseDependencies(const toml::value& data) {
    vector<Dependency> out;
    if (!data.contains("dependencies")) return out;
    const auto& depsVal = data.at("dependencies");
    if (!depsVal.is_table()) {
        throw RiuError(1, ErrorCode::E5009, string("`[dependencies]` must be a table"));
    }
    for (const auto& kv : depsVal.as_table()) {
        string depName = kv.first;
        const auto& item = kv.second;
        if (!item.is_table()) {
            throw RiuError(1, ErrorCode::E5009, string("`[dependencies].") + depName + "` must be a table");
        }
        if (item.contains("dir")) {
            throw RiuError(1, ErrorCode::E5039);
        }
        for (const auto& fieldKv : item.as_table()) {
            string field = fieldKv.first;
            if (field != "sdk" && field != "path" && field != "git" && field != "rev") {
                throw RiuError(1, ErrorCode::E5040, depName, field);
            }
        }
        bool hasSdk = item.contains("sdk");
        bool hasPath = item.contains("path");
        bool hasGit = item.contains("git");
        bool hasRev = item.contains("rev");
        int nsrc = (hasSdk ? 1 : 0) + (hasPath ? 1 : 0) + (hasGit ? 1 : 0);
        if (nsrc == 0) {
            throw RiuError(1, ErrorCode::E5035, depName);
        }
        if (nsrc > 1) {
            throw RiuError(1, ErrorCode::E5034, depName);
        }
        if (hasGit != hasRev) {
            throw RiuError(1, ErrorCode::E5042, depName);
        }
        Dependency d;
        d.name = depName;
        if (hasSdk) {
            d.kind = DepSourceKind::Sdk;
            d.sdk = tomlFieldString(item, "sdk", depName);
            if (d.sdk.empty()) {
                throw RiuError(1, ErrorCode::E5035, depName);
            }
            if (d.sdk == "core") {
                throw RiuError(1, ErrorCode::E5036);
            }
        } else if (hasPath) {
            d.kind = DepSourceKind::Path;
            d.path = tomlFieldString(item, "path", depName);
            if (d.path.empty()) {
                throw RiuError(1, ErrorCode::E5035, depName);
            }
        } else {
            d.kind = DepSourceKind::Git;
            d.git = tomlFieldString(item, "git", depName);
            d.rev = tomlFieldString(item, "rev", depName);
            if (d.git.empty() || d.rev.empty()) {
                throw RiuError(1, ErrorCode::E5042, depName);
            }
        }
        out.push_back(std::move(d));
    }
    return out;
}

string requireStringField(const toml::value& data, const char* key, const ErrorCodeDef& missing,
                          const ErrorCodeDef& notString, const ErrorCodeDef& empty) {
    if (!data.contains(key)) {
        throw RiuError(1, missing);
    }
    if (!data.at(key).is_string()) {
        throw RiuError(1, notString);
    }
    string v = data.at(key).as_string();
    if (v.empty()) {
        throw RiuError(1, empty);
    }
    return v;
}

} // namespace

ProjectConfig parseRiuToml(const string& tomlPath) {
    try {
        auto data = toml::parse(tomlPath);
        ProjectConfig cfg;
        cfg.name = requireStringField(data, "name", ErrorCode::E5002, ErrorCode::E5003, ErrorCode::E5004);
        checkDiscardDeclName(cfg.name, "package", 1, 1);
        cfg.version = requireStringField(data, "version", ErrorCode::E5029, ErrorCode::E5030, ErrorCode::E5030);

        if (data.contains("entry")) {
            throw RiuError(1, ErrorCode::E5021, string("entry"));
        }
        if (data.contains("lib")) {
            throw RiuError(1, ErrorCode::E5021, string("[lib]"));
        }
        if (data.contains("link")) {
            throw RiuError(1, ErrorCode::E5021, string("[link]"));
        }

        if (data.contains("library")) {
            const auto& lib = data.at("library");
            if (!lib.is_table()) {
                throw RiuError(1, ErrorCode::E5022);
            }
            Library L;
            if (lib.contains("name")) {
                if (!lib.at("name").is_string() || lib.at("name").as_string().empty()) {
                    throw RiuError(1, ErrorCode::E5028, string());
                }
                L.name = lib.at("name").as_string();
            }
            if (lib.contains("type")) {
                if (!lib.at("type").is_string()) {
                    throw RiuError(1, ErrorCode::E5006);
                }
                L.type = lib.at("type").as_string();
            } else {
                L.type = "static";
            }
            if (L.type != "static" && L.type != "dynamic") {
                throw RiuError(1, ErrorCode::E5006);
            }
            if (!lib.contains("lib_mod") || !lib.at("lib_mod").is_string()) {
                throw RiuError(1, ErrorCode::E5024);
            }
            L.lib_mod = lib.at("lib_mod").as_string();
            if (!isDottedModulePath(L.lib_mod)) {
                throw RiuError(1, ErrorCode::E5033);
            }
            L.external_links = parseExternalLinks(lib);
            cfg.library = std::move(L);
        }

        cfg.dependencies = parseDependencies(data);

        if (data.contains("executable")) {
            const auto& exeVal = data.at("executable");
            if (!exeVal.is_array()) {
                throw RiuError(1, ErrorCode::E5023);
            }
            for (const auto& item : exeVal.as_array()) {
                if (!item.is_table()) {
                    throw RiuError(1, ErrorCode::E5023);
                }
                Executable E;
                if (item.contains("name")) {
                    if (!item.at("name").is_string() || item.at("name").as_string().empty()) {
                        throw RiuError(1, ErrorCode::E5028, string());
                    }
                    E.name = item.at("name").as_string();
                }
                if (!item.contains("entry") || !item.at("entry").is_string() || item.at("entry").as_string().empty()) {
                    throw RiuError(1, ErrorCode::E5025);
                }
                E.entry = item.at("entry").as_string();
                E.external_links = parseExternalLinks(item);
                cfg.executables.push_back(std::move(E));
            }
        }

        if (!cfg.library && cfg.executables.empty()) {
            throw RiuError(1, ErrorCode::E5005);
        }

        if (cfg.library && cfg.library->name.empty()) {
            cfg.library->name = cfg.name;
        }
        for (auto& e : cfg.executables) {
            if (e.name.empty()) e.name = cfg.name;
        }

        vector<string> names;
        if (cfg.library) names.push_back(cfg.library->name);
        for (const auto& e : cfg.executables)
            names.push_back(e.name);

        std::unordered_set<string> seenLower;
        for (const auto& n : names) {
            validateProductName(n);
            string key = asciiLower(n);
            if (!seenLower.insert(key).second) {
                throw RiuError(1, ErrorCode::E5008, n);
            }
        }
        return cfg;
    } catch (const RiuError&) {
        throw;
    } catch (const std::exception& e) {
        throw RiuError(1, ErrorCode::E5009, e.what());
    }
}

void Riu::initFileRoot(const string& mainFileAbsPath) {
    _projectRoot = std::filesystem::path(mainFileAbsPath).parent_path().string();
    _sourceRoot = _projectRoot;
}

void Riu::initProjectFromDir(const string& rootDir) {
    namespace fs = std::filesystem;
    fs::path root(rootDir);
    fs::path tomlPath = root / "riu.toml";
    if (!fs::exists(tomlPath)) {
        throw RiuError(1, ErrorCode::E5001, rootDir);
    }
    _projectRoot = root.string();
    fs::path srcDir = root / "src";
    _sourceRoot = fs::is_directory(srcDir) ? srcDir.string() : _projectRoot;
    _config = parseRiuToml(tomlPath.string());
}

void Riu::addDeclOutputRoot(const string& sourceRoot, const string& outputRoot) {
    namespace fs = std::filesystem;
    std::error_code ec;
    DeclOutputRoot m;
    m.sourceRoot = fs::weakly_canonical(fs::absolute(sourceRoot), ec).lexically_normal().generic_string();
    if (m.sourceRoot.empty()) m.sourceRoot = sourceRoot;
    m.outputRoot = outputRoot;
    _declOutputRoots.push_back(std::move(m));
}

namespace {

string canonDir(const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto abs = fs::absolute(p, ec);
    auto c = fs::weakly_canonical(abs, ec);
    return c.lexically_normal().generic_string();
}

bool sameDir(const string& a, const string& b) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(a) && fs::exists(b)) {
        bool eq = fs::equivalent(a, b, ec);
        if (!ec) return eq;
    }
    return canonDir(a) == canonDir(b);
}

bool pathIsUnder(const string& child, const string& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto rel = fs::relative(canonDir(child), canonDir(root), ec);
    if (ec) return false;
    string s = rel.generic_string();
    if (s.empty() || s == ".") return true;
    return s != ".." && !s.starts_with("../");
}

string sourceRootOf(const string& projectRoot) {
    namespace fs = std::filesystem;
    fs::path src = fs::path(projectRoot) / "src";
    return fs::is_directory(src) ? src.string() : projectRoot;
}

} // namespace

vector<ResolvedDep> resolvePathDepGraph(const string& rootDir, const ProjectConfig& rootCfg) {
    namespace fs = std::filesystem;
    vector<ResolvedDep> out;
    std::map<string, string> nameToRoot;
    std::set<string> visiting;

    auto walk = [&](auto&& self, const string& fromRoot, const ProjectConfig& cfg) -> void {
        for (const auto& dep : cfg.dependencies) {
            if (dep.kind != DepSourceKind::Path) continue;
            fs::path raw(dep.path);
            fs::path target = raw.is_absolute() ? raw : (fs::path(fromRoot) / raw);
            fs::path toml = target / "riu.toml";
            if (!fs::exists(toml) || !fs::is_regular_file(toml)) {
                throw RiuError(1, ErrorCode::E5001, target.string()).withFile(toml.string());
            }
            ProjectConfig depCfg;
            try {
                depCfg = parseRiuToml(toml.string());
            } catch (RiuError& e) {
                if (e.file().empty()) throw std::move(e).withFile(toml.string());
                throw;
            }
            if (depCfg.name != dep.name) {
                throw RiuError(1, ErrorCode::E5037, dep.name, depCfg.name).withFile(toml.string());
            }
            if (!depCfg.library) {
                throw RiuError(1, ErrorCode::E5038, dep.name).withFile(toml.string());
            }
            string canon = canonDir(target);
            auto seen = nameToRoot.find(dep.name);
            if (seen != nameToRoot.end()) {
                if (!sameDir(seen->second, canon)) {
                    throw RiuError(1, ErrorCode::E5041, dep.name, seen->second, canon);
                }
                continue;
            }
            if (visiting.contains(canon)) continue;
            visiting.insert(canon);
            nameToRoot[dep.name] = canon;
            self(self, canon, depCfg);
            visiting.erase(canon);
            ResolvedDep node;
            node.name = dep.name;
            node.projectRoot = canon;
            node.sourceRoot = sourceRootOf(canon);
            node.config = std::move(depCfg);
            out.push_back(std::move(node));
        }
    };
    walk(walk, canonDir(rootDir), rootCfg);
    return out;
}

vector<LibModFile> collectLibModFiles(const string& sourceRoot, const string& libMod) {
    namespace fs = std::filesystem;
    vector<LibModFile> files;
    fs::path srcDir(sourceRoot);
    if (!fs::is_directory(srcDir) || libMod.empty()) return files;
    string rel = libMod;
    for (char& c : rel)
        if (c == '.') c = '/';
    fs::path pkgDir = srcDir / rel;
    fs::path fileMod = pkgDir;
    fileMod += ".ut";
    if (fs::is_directory(pkgDir)) {
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(pkgDir, walkEc); it != fs::recursive_directory_iterator();
             ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            auto& p = it->path();
            if (p.extension() != ".ut") continue;
            auto fname = p.filename().string();
            if (fname.ends_with(".test.ut")) continue;
            auto relFile = fs::relative(p, srcDir);
            string modName = relFile.generic_string();
            modName = modName.substr(0, modName.size() - 3);
            for (auto& c : modName)
                if (c == '/' || c == '\\') c = '.';
            files.push_back({fs::absolute(p).string(), std::move(modName)});
        }
        std::ranges::sort(files, [](const LibModFile& a, const LibModFile& b) { return a.moduleName < b.moduleName; });
        return files;
    }
    if (fs::is_regular_file(fileMod)) {
        files.push_back({fs::absolute(fileMod).string(), libMod});
    }
    return files;
}

string Riu::projectName() const {
    return _config.name;
}

FileNode* Riu::module(const string& moduleName) const {
    auto it = _modules.find(moduleName);
    if (it != _modules.end()) return it->second;
    return nullptr;
}

string Riu::modulePath(const string& moduleName) const {
    auto it = _modulePaths.find(moduleName);
    if (it != _modulePaths.end()) return it->second;
    return {};
}

FileNode* Riu::_parseFile(const string& absPath, const string& moduleName, int errorLine) {
    (void)errorLine;
    if (lastPathSegIsDiscard(moduleName)) {
        throw RiuError(1, ErrorCode::E3161, string("module"));
    }
    std::ifstream in(absPath, std::ios::binary);
    if (!in) throw RiuError(1, ErrorCode::E5012, moduleName, absPath);
    string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF && static_cast<unsigned char>(src[1]) == 0xBB &&
        static_cast<unsigned char>(src[2]) == 0xBF) {
        src.erase(0, 3);
    }

    // 测试文件按文件名后缀识别（spec §11.3.3.1）
    bool isTestFile = absPath.ends_with(".test.ut");
    auto builder = std::make_unique<RdBuilder>(*this, std::move(src), moduleName, isTestFile, absPath);
    builder->setIndexTokens(false);
    auto fileNode = builder->build();
    if (fileNode) fileNode->setSourcePath(absPath);
    keepRdBuilder(std::move(builder));
    return fileNode;
}

bool Riu::declCacheEnabled() const {
    return !_config.name.empty();
}

string Riu::declPathFor(const string& srcAbs) const {
    namespace fs = std::filesystem;
    for (const auto& m : _declOutputRoots) {
        if (pathIsUnder(srcAbs, m.sourceRoot)) {
            return mod_decl::pathFor(m.sourceRoot, m.outputRoot, srcAbs);
        }
    }
    auto buildDir = (fs::path(_projectRoot) / "build").string();
    return mod_decl::pathFor(_projectRoot, buildDir, srcAbs);
}

void Riu::writeDeclIfPossible(FileNode* file, const string& srcAbs) {
    if (!file || srcAbs.empty() || !declCacheEnabled()) return;
    if (srcAbs.ends_with(".test.ut")) return;
    mod_decl::write(file, srcAbs, declPathFor(srcAbs));
}

FileNode* Riu::loadMainFile(const string& absPath, const string& moduleName) {
    auto fileNode = _parseFile(absPath, moduleName, 1);
    _modules[moduleName] = fileNode;
    _modulePaths[moduleName] = absPath;
    if (fileNode) fileNode->setSourcePath(absPath);
    // 新文件加入后, 旧的 draft impl 校验结果失效: 例如 SDK 预编译触发了一次
    // validate, 此时 _seen 还没有该文件里的 `Type : Draft` 登记; 后续
    // boundSatisfied 调用必须重新跑 validate, 否则误判为不满足 → E1106.
    _specImplValidated = false;
    writeDeclIfPossible(fileNode, absPath);
    return fileNode;
}

FileNode* Riu::ensureFullAst(const string& absPath, const string& moduleName) {
    auto it = _modules.find(moduleName);
    if (it != _modules.end() && it->second && !it->second->isFromDecl()) {
        return it->second;
    }
    return loadMainFile(absPath, moduleName);
}

Riu::ModulePathKind Riu::modulePathKind(const string& moduleName) const {
    namespace fs = std::filesystem;
    if (moduleName.empty()) return ModulePathKind::NotFound;
    string rel = moduleName;
    for (auto& c : rel)
        if (c == '.') c = '/';
    fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
    fs::path dirPath = root.empty() ? fs::path(rel) : (root / rel);
    fs::path filePath = dirPath;
    filePath += ".ut";
    bool hasFile = fs::exists(filePath) && fs::is_regular_file(filePath);
    bool hasDir = fs::exists(dirPath) && fs::is_directory(dirPath);
    if (hasFile && hasDir) return ModulePathKind::Conflict;
    if (hasFile) return ModulePathKind::File;
    if (hasDir) return ModulePathKind::Package;
    auto extraIt = _modulePaths.find(moduleName);
    if (extraIt != _modulePaths.end()) {
        fs::path extra(extraIt->second);
        if (fs::is_directory(extra)) return ModulePathKind::Package;
        if (fs::is_regular_file(extra)) return ModulePathKind::File;
    }
    return ModulePathKind::NotFound;
}

vector<string> Riu::listPackageRiuChildren(const string& moduleName) const {
    namespace fs = std::filesystem;
    vector<string> out;
    fs::path dirPath;
    auto it = _modulePaths.find(moduleName);
    if (it != _modulePaths.end() && fs::is_directory(it->second)) {
        dirPath = it->second;
    } else {
        string rel = moduleName;
        for (auto& c : rel)
            if (c == '.') c = '/';
        fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
        dirPath = root.empty() ? fs::path(rel) : (root / rel);
    }
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return out;
    for (auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        if (p.extension() != ".ut") continue;
        out.push_back(p.stem().string());
    }
    std::ranges::sort(out);
    return out;
}

vector<string> Riu::listPackageSubdirs(const string& moduleName) const {
    namespace fs = std::filesystem;
    vector<string> out;
    fs::path dirPath;
    auto it = _modulePaths.find(moduleName);
    if (it != _modulePaths.end() && fs::is_directory(it->second)) {
        dirPath = it->second;
    } else {
        string rel = moduleName;
        for (auto& c : rel)
            if (c == '.') c = '/';
        fs::path root = _sourceRoot.empty() ? fs::path() : fs::path(_sourceRoot);
        dirPath = root.empty() ? fs::path(rel) : (root / rel);
    }
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) return out;
    for (auto& entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_directory()) continue;
        out.push_back(entry.path().filename().string());
    }
    std::ranges::sort(out);
    return out;
}

bool Riu::hasPkgFile(const string& moduleName) const {
    namespace fs = std::filesystem;
    fs::path pkgPath = fs::path(packageSourceDir(moduleName)) / "pkg";
    return fs::exists(pkgPath) && fs::is_regular_file(pkgPath);
}

string Riu::packageSourceDir(const string& package) const {
    namespace fs = std::filesystem;
    string rel = package;
    std::ranges::replace(rel, '.', '/');
    fs::path local = fs::path(_sourceRoot) / rel;
    if (fs::is_directory(local)) return local.string();
    auto registered = _modulePaths.find(package);
    if (registered != _modulePaths.end() && fs::is_directory(registered->second)) {
        return registered->second;
    }
    // 已加载依赖的源路径也能定位包，不为检查可见性加载额外模块。
    for (const auto& [name, path] : _modulePaths) {
        if (!name.starts_with(package + ".")) continue;
        auto dir = fs::path(path).parent_path();
        auto suffix = name.substr(package.size() + 1);
        for (char c : suffix)
            if (c == '.') dir = dir.parent_path();
        return dir.string();
    }
    // riu-check 批量检查共享 SDK；依赖目录仍由其所属上下文定位。
    if (_sdkFile && _sdkFile->riu() && _sdkFile->riu() != this) {
        auto dependencyDir = _sdkFile->riu()->packageSourceDir(package);
        if (fs::is_directory(dependencyDir)) return dependencyDir;
    }
    return local.string();
}

bool Riu::isInsidePackage(const FileNode* caller, const string& package) const {
    if (!caller || caller->sourcePath().empty()) return false;
    namespace fs = std::filesystem;
    return fs::weakly_canonical(fs::path(caller->sourcePath()).parent_path()) ==
           fs::weakly_canonical(packageSourceDir(package));
}

vector<PkgExportItem> Riu::visiblePkgItems(const FileNode* caller, const string& package) const {
    auto items = parsePkgFile(package);
    if (isInsidePackage(caller, package)) {
        // 包内沿用物理兄弟名；显式导出别名仍可使用。
        auto addSibling = [&](const string& name) {
            if (std::ranges::none_of(items, [&](const auto& item) { return pkgExportName(item) == name; })) {
                PkgExportItem item;
                item.name = name;
                items.push_back(std::move(item));
            }
        };
        for (const auto& name : listPackageRiuChildren(package))
            addSibling(name);
        for (const auto& name : listPackageSubdirs(package))
            addSibling(name);
        return items;
    }
    std::erase_if(items, [&](const auto& item) {
        if (pkgExportIsPublic(item)) return false;
        return std::ranges::none_of(item.toTargets, [&](const string& target) {
            return caller && (caller->moduleName() == target || isInsidePackage(caller, target));
        });
    });
    return items;
}

string Riu::resolvePkgPath(const FileNode* caller, const string& path, int line, const string& package) const {
    string resolved = package;
    size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('.', start);
        string name = path.substr(start, end == string::npos ? string::npos : end - start);
        if (!resolved.empty() && hasPkgFile(resolved)) {
            auto items = visiblePkgItems(caller, resolved);
            // 包内物理名优先，避免被同名导出别名改写。
            bool sibling = false;
            if (isInsidePackage(caller, resolved)) {
                string sourcePath = resolved;
                sourcePath += '.';
                sourcePath += name;
                sibling = modulePathKind(sourcePath) != ModulePathKind::NotFound;
            }
            if (!sibling) {
                auto it = std::ranges::find_if(items, [&](const auto& item) { return pkgExportName(item) == name; });
                if (it == items.end()) throw RiuError(line, ErrorCode::E5018, name, resolved);
                name = it->name;
            }
        }
        if (!resolved.empty()) resolved += '.';
        resolved += name;
        if (end == string::npos) break;
        start = end + 1;
    }
    return resolved;
}

namespace {

bool pkgIsWs(char c) {
    return c == ' ' || c == '\t';
}

void pkgSkipWs(const string& s, size_t& i) {
    while (i < s.size() && pkgIsWs(s[i]))
        ++i;
}

string pkgTakeToken(const string& s, size_t& i) {
    size_t start = i;
    while (i < s.size() && !pkgIsWs(s[i]) && s[i] != ';')
        ++i;
    return s.substr(start, i - start);
}

[[noreturn]] void throwPkgInvalid(const string& pkgPath, int line, int col, const string& reason) {
    throw RiuError(static_cast<size_t>(line), col, ErrorCode::E5020, pkgPath, reason).withFile(pkgPath);
}

int pkgCol(size_t i) {
    return static_cast<int>(i) + 1;
}

// 解析一行。空行 / 整行注释 → nullopt。非法 → E5020。
std::optional<PkgExportItem> parsePkgLine(const string& pkgPath, int lineNo, string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();

    size_t i = 0;
    pkgSkipWs(line, i);
    if (i >= line.size() || line[i] == ';') return std::nullopt;

    const int nameCol = pkgCol(i);
    string nameTok = pkgTakeToken(line, i);
    if (nameTok.empty()) return std::nullopt;

    PkgExportItem item;
    if (nameTok.size() >= 2 && nameTok.ends_with(".*")) {
        item.wildcard = true;
        item.name = nameTok.substr(0, nameTok.size() - 2);
    } else {
        item.name = nameTok;
    }
    if (item.name.empty()) {
        throwPkgInvalid(pkgPath, lineNo, nameCol, "missing module name");
    }

    auto rejectWildcardAsTo = [&](size_t kwStart) {
        if (item.wildcard) {
            throwPkgInvalid(pkgPath, lineNo, pkgCol(kwStart), "`name.*` cannot use `as` or `to`");
        }
    };

    auto atCommentOrEnd = [&]() {
        pkgSkipWs(line, i);
        return i >= line.size() || line[i] == ';';
    };

    if (atCommentOrEnd()) return item;

    size_t kwStart = i;
    string kw = pkgTakeToken(line, i);
    if (kw == "as") {
        rejectWildcardAsTo(kwStart);
        pkgSkipWs(line, i);
        if (i >= line.size() || line[i] == ';') {
            throwPkgInvalid(pkgPath, lineNo, pkgCol(kwStart), "missing alias after `as`");
        }
        size_t aliasStart = i;
        string alias = pkgTakeToken(line, i);
        if (alias.empty()) {
            throwPkgInvalid(pkgPath, lineNo, pkgCol(aliasStart), "missing alias after `as`");
        }
        item.rename = std::move(alias);
        if (atCommentOrEnd()) return item;
        kwStart = i;
        kw = pkgTakeToken(line, i);
    }

    if (kw == "to") {
        rejectWildcardAsTo(kwStart);
        pkgSkipWs(line, i);
        string rest = (i < line.size()) ? line.substr(i) : string();
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r')) {
            rest.pop_back();
        }
        if (rest.empty()) {
            throwPkgInvalid(pkgPath, lineNo, pkgCol(kwStart), "empty `to` list");
        }
        size_t p = 0;
        while (p <= rest.size()) {
            size_t semi = rest.find(';', p);
            string part = (semi == string::npos) ? rest.substr(p) : rest.substr(p, semi - p);
            size_t a = part.find_first_not_of(" \t");
            if (a == string::npos) {
                // 末尾多余 `;`（已有至少一个目标）忽略；中间空段仍非法
                if (semi == string::npos && !item.toTargets.empty()) break;
                throwPkgInvalid(pkgPath, lineNo, pkgCol(i + p), "empty `to` target");
            }
            size_t b = part.find_last_not_of(" \t");
            item.toTargets.push_back(part.substr(a, b - a + 1));
            if (semi == string::npos) break;
            p = semi + 1;
        }
        if (item.toTargets.empty()) {
            throwPkgInvalid(pkgPath, lineNo, pkgCol(kwStart), "empty `to` list");
        }
        return item;
    }

    if (!kw.empty()) {
        throwPkgInvalid(pkgPath, lineNo, pkgCol(kwStart), "unexpected `" + kw + "`");
    }
    return item;
}

} // namespace

vector<PkgExportItem> parsePkgFileAt(const string& pkgPath) {
    namespace fs = std::filesystem;
    vector<PkgExportItem> items;
    fs::path p(pkgPath);
    if (!fs::exists(p) || !fs::is_regular_file(p)) return items;

    std::ifstream file(p);
    if (!file.is_open()) return items;

    map<string, int> seenNameLine;
    string line;
    int lineNo = 0;
    while (std::getline(file, line)) {
        ++lineNo;
        auto parsed = parsePkgLine(pkgPath, lineNo, line);
        if (!parsed) continue;
        parsed->sourceLine = lineNo;
        auto it = seenNameLine.find(parsed->name);
        if (it != seenNameLine.end()) {
            throw RiuError(static_cast<size_t>(lineNo), 1, ErrorCode::E5020, pkgPath,
                           "submodule `" + parsed->name + "` appears more than once")
                .withFile(pkgPath)
                .withNote("first listed at line " + std::to_string(it->second));
        }
        seenNameLine[parsed->name] = lineNo;
        items.push_back(std::move(*parsed));
    }
    return items;
}

vector<PkgExportItem> Riu::parsePkgFile(const string& moduleName) const {
    namespace fs = std::filesystem;
    auto path = (fs::path(packageSourceDir(moduleName)) / "pkg").string();
    auto items = parsePkgFileAt(path);
    for (const auto& item : items) {
        for (const auto& target : item.toTargets) {
            if (modulePathKind(target) != ModulePathKind::NotFound || module(target)) continue;
            if (!modulePath(target).empty()) continue;
            if (target == "riu.core" && _sdkFile) continue;
            if (_sdkFile && _sdkFile->riu() && _sdkFile->riu()->module(target)) continue;
            if (fs::is_directory(packageSourceDir(target))) continue;
            throw RiuError(item.sourceLine, ErrorCode::E5019, target).withFile(path);
        }
    }
    return items;
}

FileNode* Riu::loadModule(const string& moduleName, int errorLine) {
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
            throw RiuError(errorLine, ErrorCode::E5011, chain);
        }
    }

    // 点分 → 相对路径
    string relPath = moduleName;
    for (auto& c : relPath) {
        if (c == '.') c = '/';
    }
    relPath += ".ut";

    std::filesystem::path fullPath =
        _sourceRoot.empty() ? std::filesystem::path(relPath) : std::filesystem::path(_sourceRoot) / relPath;

    if (!std::filesystem::exists(fullPath) || !std::filesystem::is_regular_file(fullPath)) {
        auto pit = _modulePaths.find(moduleName);
        if (pit != _modulePaths.end() && std::filesystem::is_regular_file(pit->second)) {
            fullPath = pit->second;
        } else {
            throw RiuError(errorLine, ErrorCode::E5012, moduleName, fullPath.string());
        }
    }

    string absPath = std::filesystem::absolute(fullPath).string();

    _loadStack.push_back(moduleName);
    FileNode* fileNode = nullptr;
    try {
        if (declCacheEnabled()) {
            fileNode = mod_decl::tryLoad(*this, declPathFor(absPath), absPath, moduleName);
        }
        if (!fileNode) {
            fileNode = _parseFile(absPath, moduleName, errorLine);
            writeDeclIfPossible(fileNode, absPath);
        }
    } catch (...) {
        _loadStack.pop_back();
        throw;
    }
    _loadStack.pop_back();

    _modules[moduleName] = fileNode;
    _modulePaths[moduleName] = absPath;
    if (fileNode) fileNode->setSourcePath(absPath);
    _loadOrder.push_back(moduleName);
    return fileNode;
}
