// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// windows.h 必须先于 riu 头 (`types.h` 经由 `using namespace std`) include,
// 否则 std::byte 与 winapi byte 冲突 (rpcndr.h)。NOGDI 跳 wingdi.h 的 ERROR 宏。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#include "tools/sdk_loader.h"

#include <array>

#include "ast/mod_decl.h"
#include "ast/node/file_node.h"
#include "ast/node/node.h"
#include "ast/riu.h"
#include "sema/name_resolver.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace sdk_loader {

namespace fs = std::filesystem;

namespace {

fs::path toolchainSdkRoot() {
#ifdef _DEBUG
    if (fs::is_directory("sdk/core/src/riu/core")) {
        return {"sdk"};
    }
#endif
#ifdef _WIN32
    std::array<char, MAX_PATH> exePath{};
    GetModuleFileNameA(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    fs::path exe(exePath.data());
    fs::path root = exe.parent_path().parent_path();
    fs::path sdk = root / "sdk";
    if (fs::is_directory(sdk / "core" / "src" / "riu" / "core")) return sdk;
#endif
    return {};
}

fs::path findTomlRoot(const fs::path& start) {
    fs::path p = fs::absolute(start);
    while (true) {
        if (fs::exists(p / "riu.toml")) return p;
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

} // namespace

std::string findSdkPath() {
    fs::path sdk = toolchainSdkRoot();
    if (sdk.empty()) return "";
    fs::path core = sdk / "core" / "src" / "riu" / "core";
    if (fs::is_directory(core)) return core.string();
    return "";
}

std::string findSdkPackage(const std::string& id) {
    if (id.empty()) return "";
    fs::path sdk = toolchainSdkRoot();
    if (sdk.empty()) return "";
    fs::path pkg = sdk / id;
    if (fs::is_regular_file(pkg / "riu.toml")) return fs::absolute(pkg).lexically_normal().string();
    return "";
}

std::map<std::string, SdkPkgEntry> readSdkPkg(const std::string& sdkDir) {
    std::map<std::string, SdkPkgEntry> r;
    auto items = parsePkgFileAt((fs::path(sdkDir) / "pkg").string());
    for (const auto& item : items) {
        r[item.name] = {.moduleName = "riu.core." + item.name,
                        .exportName = pkgExportName(item),
                        .isFlat = item.wildcard,
                        .isPublic = pkgExportIsPublic(item)};
    }
    return r;
}

void registerSdkModulePaths(Riu& riu, const std::map<std::string, SdkPkgEntry>& pkgMap) {
    auto sdk = riu.sdkFile();
    if (!sdk) return;

    // 默认 `use riu.core.*`：只打开 core 子树。包根 `riu` 仅用于已导入的
    // `riu.core.<stem>` 消歧（`riu.core.map.Map`）。禁止「有 riu 就能点任意子包」。
    if (!sdk->localSymbols().contains("riu")) {
        SymbolInfo riuSym(SymbolKind::Package, "riu", TypeInfo());
        riuSym.moduleName = "riu";
        sdk->registerSymbol("riu", riuSym);
        sdk->addPackageAlias("riu", "riu");
    }

    for (const auto& pair : pkgMap) {
        const auto& entry = pair.second;
        if (!entry.isPublic) continue;
        auto file = riu.module(entry.moduleName);
        if (!file) continue;
        const string& exportedName = entry.exportName;

        // 末段别名：扁平导出的 map 也登记，才能写 `map.Map`（与 `math.abs` 同形）。
        if (!sdk->localSymbols().contains(exportedName)) {
            SymbolInfo aliasSym(SymbolKind::Module, exportedName, TypeInfo());
            aliasSym.moduleName = entry.moduleName;
            sdk->registerSymbol(exportedName, aliasSym);
            sdk->addModuleAlias(exportedName, file);
        }
        sdk->addPackageChild("riu", "core." + exportedName, file);
    }

    // 扁平进 riu.core 的成员（base.println 等）也可写 `riu.core.fn`：
    // 包孩子 `core` 指向 SDK 壳，lookup 走 wildcardImports。
    sdk->addPackageChild("riu", "core", sdk);
}

void parseSdkDir(const std::string& sdkDir, Riu& riu, bool allowDecl,
                 const std::unordered_set<std::string>* forceFullParseAbs) {
    auto pkgMap = readSdkPkg(sdkDir);

    std::vector<std::string> riuFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.ends_with(".ut")) {
            if (filename.ends_with(".test.ut")) continue;
            riuFiles.push_back(entry.path().string());
        }
    }
    std::ranges::sort(riuFiles);
    // 定向／未公开模块先加载，使同包公开模块在解析 `use riu.core.<name>` 时
    // 能命中已加载模块；它们仍不会进入用户 parent scope。
    std::ranges::stable_sort(riuFiles, [&](const std::string& lhs, const std::string& rhs) {
        auto priority = [&](const std::string& path) {
            auto it = pkgMap.find(fs::path(path).stem().string());
            return it == pkgMap.end() || !it->second.isPublic ? 0 : 1;
        };
        return priority(lhs) < priority(rhs);
    });

    auto sdk = riu.createSdkFile();

    fs::path sdkRoot = findTomlRoot(sdkDir);
    std::string declRoot = sdkRoot.empty() ? std::string() : sdkRoot.string();
    std::string declBuild = sdkRoot.empty() ? std::string() : (sdkRoot / "build").string();
    // riu-check 无项目名，但这里仍写 sdk/core/build/*.ud；必须保留 skeleton 原文。
    if (allowDecl && !declRoot.empty()) riu.setKeepItemSourceText(true);

    auto attachSdkParent = [&](FileNode* fileNode, bool flattenToCore) {
        if (!sdk || !fileNode || sdk == fileNode) return;
        fileNode->setParentScope(sdk);
        fileNode->addImport("riu.core");
        if (flattenToCore) sdk->addWildcardImport(fileNode);
    };

    auto loadOne = [&](const std::string& riuFile, const std::string& moduleName, bool flattenToCore) {
        if (auto* existing = riu.module(moduleName)) {
            attachSdkParent(existing, flattenToCore);
            return;
        }
        std::string abs = fs::absolute(riuFile).lexically_normal().generic_string();
        FileNode* fileNode = nullptr;
        const bool forceFull = forceFullParseAbs && forceFullParseAbs->contains(abs);
        if (allowDecl && !forceFull && !declRoot.empty()) {
            auto dpath = mod_decl::pathFor(declRoot, declBuild, abs);
            fileNode = mod_decl::tryLoad(riu, dpath, abs, moduleName);
        }
        if (!fileNode) {
            fileNode = riu.loadMainFile(abs, moduleName);
            if (!declRoot.empty()) {
                mod_decl::write(fileNode, abs, mod_decl::pathFor(declRoot, declBuild, abs));
            }
        }
        attachSdkParent(fileNode, flattenToCore);
    };

    for (const auto& riuFile : riuFiles) {
        std::string stem = fs::path(riuFile).stem().string();
        auto it = pkgMap.find(stem);

        std::string moduleName;
        if (it != pkgMap.end() && !it->second.moduleName.empty()) {
            moduleName = it->second.moduleName;
        } else {
            moduleName = "riu.core." + stem;
        }
        bool flattenToCore = it != pkgMap.end() && it->second.isPublic && it->second.isFlat;
        loadOne(riuFile, moduleName, flattenToCore);
    }

    registerSdkModulePaths(riu, pkgMap);

    // .ud 只记下 UseSpec，不跑 RdBuilder 的通配展开。消费方要按声明模块的通配把别名展开。
    for (auto* f : riu.files()) {
        if (!f) continue;
        for (auto& u : f->useSpecs()) {
            if (!u.wildcard) continue;
            if (auto* imp = riu.module(u.moduleName); imp && imp != f) {
                f->addWildcardImport(imp);
            }
        }
    }
    FileNode* sdkFile = riu.sdkFile();
    for (auto* f : riu.files()) {
        if (!f || f == sdkFile) continue;
        sema::recacheDeclFieldAliases(f, sdkFile);
    }
}

void parseSdkLibrary(const std::string& pkgRoot, const std::string& libMod, Riu& riu, bool allowDecl) {
    if (pkgRoot.empty() || libMod.empty()) return;
    fs::path root(pkgRoot);
    fs::path src = root / "src";
    std::string sourceRoot = fs::is_directory(src) ? src.string() : pkgRoot;
    auto files = collectLibModFiles(sourceRoot, libMod);
    if (files.empty()) return;

    string rel = libMod;
    for (char& c : rel)
        if (c == '.') c = '/';
    fs::path pkgDir = fs::path(sourceRoot) / rel;
    if (fs::is_directory(pkgDir)) {
        riu.registerModulePath(fs::absolute(pkgDir).lexically_normal().string(), libMod);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(pkgDir, ec); it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (!it->is_directory()) continue;
            auto relDir = fs::relative(it->path(), sourceRoot);
            string mod = relDir.generic_string();
            for (char& c : mod)
                if (c == '/' || c == '\\') c = '.';
            riu.registerModulePath(fs::absolute(it->path()).lexically_normal().string(), mod);
        }
    }
    for (const auto& f : files) {
        riu.registerModulePath(f.absPath, f.moduleName);
    }

    auto sdk = riu.sdkFile();
    std::string declBuild = (root / "build").string();
    if (allowDecl) riu.setKeepItemSourceText(true);

    for (const auto& f : files) {
        if (riu.module(f.moduleName)) continue;
        std::string abs = fs::absolute(f.absPath).lexically_normal().generic_string();
        FileNode* fileNode = nullptr;
        if (allowDecl) {
            auto dpath = mod_decl::pathFor(pkgRoot, declBuild, abs);
            fileNode = mod_decl::tryLoad(riu, dpath, abs, f.moduleName);
        }
        if (!fileNode) {
            fileNode = riu.loadMainFile(abs, f.moduleName);
            mod_decl::write(fileNode, abs, mod_decl::pathFor(pkgRoot, declBuild, abs));
        }
        if (sdk && fileNode && fileNode != sdk) {
            fileNode->setParentScope(sdk);
            fileNode->addImport("riu.core");
        }
    }

    for (auto* f : riu.files()) {
        if (!f) continue;
        for (auto& u : f->useSpecs()) {
            if (!u.wildcard) continue;
            if (auto* imp = riu.module(u.moduleName); imp && imp != f) {
                f->addWildcardImport(imp);
            }
        }
    }
    FileNode* sdkFile = riu.sdkFile();
    for (auto* f : riu.files()) {
        if (!f || f == sdkFile) continue;
        sema::recacheDeclFieldAliases(f, sdkFile);
    }
}

} // namespace sdk_loader
