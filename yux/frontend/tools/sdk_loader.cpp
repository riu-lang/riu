// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// windows.h 必须先于 yux 头 (`types.h` 经由 `using namespace std`) include,
// 否则 std::byte 与 winapi byte 冲突 (rpcndr.h)。NOGDI 跳 wingdi.h 的 ERROR 宏。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#endif

#include "tools/sdk_loader.h"

#include <array>

#include "ast/ast_builder.h"
#include "ast/mod_decl.h"
#include "ast/node/file_node.h"
#include "ast/node/node.h"
#include "ast/yux.h"
#include "tools/syntax_error_listener.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>

namespace sdk_loader {

namespace fs = std::filesystem;

std::string findSdkPath() {
#ifdef _DEBUG
    if (fs::is_directory("sdk/yux/src/yux/core")) {
        return "sdk/yux/src/yux/core";
    }
#endif
#ifdef _WIN32
    std::array<char, MAX_PATH> exePath{};
    GetModuleFileNameA(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    fs::path exe(exePath.data());
    fs::path root = exe.parent_path().parent_path();
    fs::path newSdk = root / "sdk" / "yux" / "src" / "yux" / "core";
    if (fs::is_directory(newSdk)) return newSdk.string();
    fs::path oldSdk = root / "sdk" / "yux" / "core";
    if (fs::is_directory(oldSdk)) return oldSdk.string();
#endif
    return "";
}

std::map<std::string, SdkPkgEntry> readSdkPkg(const std::string& sdkDir) {
    std::map<std::string, SdkPkgEntry> r;
    auto items = parsePkgFileAt((fs::path(sdkDir) / "pkg").string());
    for (const auto& item : items) {
        r[item.name] = {.moduleName = "yux.core." + item.name,
                        .exportName = pkgExportName(item),
                        .isFlat = item.wildcard,
                        .isPublic = pkgExportIsPublic(item)};
    }
    return r;
}

void registerSdkModulePaths(Yux& yux, const std::map<std::string, SdkPkgEntry>& pkgMap) {
    auto sdk = yux.sdkFile();
    if (!sdk) return;

    // 默认 `use yux.core.*`：只打开 core 子树。包根 `yux` 仅用于已导入的
    // `yux.core.<stem>` 消歧（`yux.core.map.Map`）。禁止「有 yux 就能点任意子包」。
    if (!sdk->localSymbols().contains("yux")) {
        SymbolInfo yuxSym(SymbolKind::Package, "yux", TypeInfo());
        yuxSym.moduleName = "yux";
        sdk->registerSymbol("yux", yuxSym);
        sdk->addPackageAlias("yux", "yux");
    }

    for (const auto& pair : pkgMap) {
        const auto& entry = pair.second;
        if (!entry.isPublic) continue;
        auto file = yux.module(entry.moduleName);
        if (!file) continue;
        const string& exportedName = entry.exportName;

        // 末段别名：扁平导出的 map 也登记，才能写 `map.Map`（与 `math.abs` 同形）。
        if (!sdk->localSymbols().contains(exportedName)) {
            SymbolInfo aliasSym(SymbolKind::Module, exportedName, TypeInfo());
            aliasSym.moduleName = entry.moduleName;
            sdk->registerSymbol(exportedName, aliasSym);
            sdk->addModuleAlias(exportedName, file);
        }
        sdk->addPackageChild("yux", "core." + exportedName, file);
    }

    // 扁平进 yux.core 的成员（base.println 等）也可写 `yux.core.fn`：
    // 包孩子 `core` 指向 SDK 壳，lookup 走 wildcardImports。
    sdk->addPackageChild("yux", "core", sdk);
}

void parseSdkDir(const std::string& sdkDir, Yux& yux, bool allowDecl) {
    auto pkgMap = readSdkPkg(sdkDir);

    fs::path ioFile = fs::path(sdkDir).parent_path() / "io.yux";
    if (fs::is_regular_file(ioFile)) {
        yux.registerModulePath(fs::absolute(ioFile).string(), "yux.io");
    }

    std::vector<std::string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
            if (filename.size() >= 9 && filename.ends_with(".test.yux")) continue;
            yuxFiles.push_back(entry.path().string());
        }
    }
    std::ranges::sort(yuxFiles);
    // 定向／未公开模块先加载，使同包公开模块在解析 `use yux.core.<name>` 时
    // 能命中已加载模块；它们仍不会进入用户 parent scope。
    std::ranges::stable_sort(yuxFiles, [&](const std::string& lhs, const std::string& rhs) {
        auto priority = [&](const std::string& path) {
            auto it = pkgMap.find(fs::path(path).stem().string());
            return it == pkgMap.end() || !it->second.isPublic ? 0 : 1;
        };
        return priority(lhs) < priority(rhs);
    });

    // 创建 _sdkFile 空壳作为父作用域（不再合并 AST）
    auto sdk = yux.createSdkFile();

    // .decl 落在 SDK 项目 build/ 下；源码 hash 变了才重 parse，重编 yux.exe 不重 parse
    fs::path sdkRoot;
    {
        fs::path p = fs::absolute(sdkDir);
        while (true) {
            if (fs::exists(p / "yux.toml")) {
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

    auto loadOne = [&](const std::string& yuxFile, const std::string& moduleName, bool flattenToCore) {
        std::string abs = fs::absolute(yuxFile).string();
        p<FileNode> fileNode = nullptr;
        if (allowDecl && !declRoot.empty()) {
            auto dpath = mod_decl::pathFor(declRoot, declBuild, abs);
            fileNode = mod_decl::tryLoad(yux, dpath, abs, moduleName);
        }
        if (!fileNode) {
            fileNode = yux.loadMainFile(abs, moduleName);
            if (!declRoot.empty()) {
                mod_decl::write(fileNode, abs, mod_decl::pathFor(declRoot, declBuild, abs));
            }
        }
        if (sdk && sdk != fileNode) {
            fileNode->setParentScope(sdk);
            fileNode->addImport("yux.core");
            if (flattenToCore) sdk->addWildcardImport(fileNode);
        }
    };

    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);

        std::string moduleName;
        if (it != pkgMap.end() && !it->second.moduleName.empty()) {
            moduleName = it->second.moduleName;
        } else {
            moduleName = "yux.core." + stem;
        }
        bool flattenToCore = it != pkgMap.end() && it->second.isPublic && it->second.isFlat;
        loadOne(yuxFile, moduleName, flattenToCore);
    }

    // 独立包 yux.io：不扁平进 core，未 use 时不可点 `yux.io` / 裸名 IoErr。
    if (fs::is_regular_file(ioFile)) {
        loadOne(ioFile.string(), "yux.io", false);
    }

    registerSdkModulePaths(yux, pkgMap);
}

} // namespace sdk_loader
