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
#include <fstream>
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
    fs::path pkgPath = fs::path(sdkDir) / "pkg";
    if (!fs::exists(pkgPath)) return r;
    std::ifstream f(pkgPath);
    std::string line;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(s, e - s + 1);
        if (line.empty() || line[0] == ';') continue;
        bool wild = false;
        std::string name = line;
        if (name.size() >= 2 && name.substr(name.size() - 2) == ".*") {
            wild = true;
            name = name.substr(0, name.size() - 2);
        }
        if (name.empty()) continue;
        r[name] = {.moduleName = wild ? std::string("yux.core") : ("yux.core." + name), .isFlat = wild};
    }
    return r;
}

void registerSdkModulePaths(Yux& yux) {
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

    for (auto& file : yux.files()) {
        if (!file || file == sdk) continue;
        const string& mn = file->moduleName();
        // 只登记 yux.core 直接子模块，不扫 yux.* 任意包
        if (mn.size() <= 9 || !mn.starts_with("yux.core.")) continue;
        string stem = mn.substr(9);
        if (stem.empty() || stem.find('.') != string::npos) continue;

        // 末段别名：扁平导出的 map 也登记，才能写 `map.Map`（与 `math.abs` 同形）
        if (!sdk->localSymbols().contains(stem)) {
            SymbolInfo aliasSym(SymbolKind::Module, stem, TypeInfo());
            aliasSym.moduleName = mn;
            sdk->registerSymbol(stem, aliasSym);
            sdk->addModuleAlias(stem, file);
        }
        sdk->addPackageChild("yux", "core." + stem, file);
    }
}

void parseSdkDir(const std::string& sdkDir, Yux& yux, bool allowDecl) {
    auto pkgMap = readSdkPkg(sdkDir);

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

    // 单遍：每个文件独立 FileNode，通过 wildcardImport + parentScope 双向关联 _sdkFile
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);

        // 分配 moduleName: 命名空间文件沿用 pkg 指定的全名，其余统一用 yux.core.<stem>
        std::string moduleName;
        if (it != pkgMap.end() && !it->second.moduleName.empty() && !it->second.isFlat) {
            moduleName = it->second.moduleName;
        } else {
            moduleName = "yux.core." + stem;
        }

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

        // 所有 SDK 文件双向关联 _sdkFile：
        // 1) fileNode 设 _sdkFile 为 parentScope → 可通过 parentScope 链找到其他 SDK 文件
        // 2) _sdkFile 设 fileNode 为 wildcardImport → lookupFnSymbol/collectFnOverloads 可回退到这里
        if (sdk && sdk != fileNode) {
            fileNode->setParentScope(sdk);
            fileNode->addImport("yux.core");
            sdk->addWildcardImport(fileNode);
        }
    }

    registerSdkModulePaths(yux);
}

} // namespace sdk_loader
