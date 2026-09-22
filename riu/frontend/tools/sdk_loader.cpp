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

std::string findSdkPath() {
#ifdef _DEBUG
    if (fs::is_directory("sdk/riu/src/riu/core")) {
        return "sdk/riu/src/riu/core";
    }
#endif
#ifdef _WIN32
    std::array<char, MAX_PATH> exePath{};
    GetModuleFileNameA(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    fs::path exe(exePath.data());
    fs::path root = exe.parent_path().parent_path();
    fs::path newSdk = root / "sdk" / "riu" / "src" / "riu" / "core";
    if (fs::is_directory(newSdk)) return newSdk.string();
    fs::path oldSdk = root / "sdk" / "riu" / "core";
    if (fs::is_directory(oldSdk)) return oldSdk.string();
#endif
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

std::vector<SdkExtraPkg> extraSdkPackages(const std::string& sdkDir) {
    static constexpr std::array kNames{"io", "time"};
    std::vector<SdkExtraPkg> out;
    fs::path parent = fs::path(sdkDir).parent_path();
    for (const char* name : kNames) {
        fs::path f = parent / (std::string(name) + ".ut");
        if (!fs::is_regular_file(f)) continue;
        out.push_back(
            {.absPath = fs::absolute(f).lexically_normal().generic_string(), .moduleName = std::string("riu.") + name});
    }
    // 目录包：须有 pkg，避免空目录被当成模块。
    fs::path winDir = parent / "platform" / "windows";
    if (fs::is_directory(winDir) && fs::is_regular_file(winDir / "pkg")) {
        out.push_back({.absPath = fs::absolute(winDir).lexically_normal().generic_string(),
                       .moduleName = "riu.platform.windows",
                       .isDir = true});
    }
    return out;
}

std::vector<SdkExtraPkg> extraSdkSourceModules(const SdkExtraPkg& extra) {
    if (!extra.isDir) return {extra};
    std::vector<SdkExtraPkg> out;
    fs::path dir(extra.absPath);
    if (!fs::is_directory(dir)) return out;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (!filename.ends_with(".ut") || filename.ends_with(".test.ut")) continue;
        std::string stem = entry.path().stem().string();
        out.push_back({.absPath = fs::absolute(entry.path()).lexically_normal().generic_string(),
                       .moduleName = extra.moduleName + "." + stem});
    }
    std::ranges::sort(out, [](const SdkExtraPkg& a, const SdkExtraPkg& b) { return a.moduleName < b.moduleName; });
    return out;
}

void parseSdkDir(const std::string& sdkDir, Riu& riu, bool allowDecl,
                 const std::unordered_set<std::string>* forceFullParseAbs) {
    auto pkgMap = readSdkPkg(sdkDir);

    auto extras = extraSdkPackages(sdkDir);
    for (const auto& extra : extras) {
        riu.registerModulePath(extra.absPath, extra.moduleName);
        for (const auto& src : extraSdkSourceModules(extra)) {
            riu.registerModulePath(src.absPath, src.moduleName);
        }
    }

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

    // 创建 _sdkFile 空壳作为父作用域（不再合并 AST）
    auto sdk = riu.createSdkFile();

    // .ud 落在 SDK 项目 build/ 下；源码 hash 变了才重 parse，重编 riu.exe 不重 parse
    fs::path sdkRoot;
    {
        fs::path p = fs::absolute(sdkDir);
        while (true) {
            if (fs::exists(p / "riu.toml")) {
                sdkRoot = p;
                break;
            }
            auto parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
    }
    std::string declRoot = sdkRoot.empty() ? std::string() : sdkRoot.string();
    std::string declBuild = sdkRoot.empty() ? std::string() : (sdkRoot / "build").string();
    // riu-check 无项目名，但这里仍写 sdk/riu/build/*.ud；必须保留 skeleton 原文。
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

    // 独立包：不扁平进 core，未 use 时不可点 `riu.io` / `riu.time` / `riu.platform.windows` / 裸名。
    for (const auto& extra : extras) {
        for (const auto& src : extraSdkSourceModules(extra)) {
            loadOne(src.absPath, src.moduleName, false);
        }
    }

    registerSdkModulePaths(riu, pkgMap);

    // .ud 只记下 UseSpec，不跑 RdBuilder 的通配展开。`FileInputStream._handle HANDLE`
    // 在消费方（未 `use types.*`）要按声明模块的通配把别名收成 `Ptr<_HANDLE>`。
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
