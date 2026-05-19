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

#include "ast/ast_builder.h"
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
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    fs::path exe(exePath);
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
        r[name] = {wild ? std::string("yux.core") : ("yux.core." + name), wild};
    }
    return r;
}

void registerSdkPkgAliases(Yux& yux, const std::map<std::string, SdkPkgEntry>& pkgMap) {
    auto sdk = yux.sdkFile();
    if (!sdk) return;
    for (auto& [stem, info] : pkgMap) {
        if (info.isFlat) continue;
        auto target = yux.module(info.moduleName);
        if (!target) continue;
        if (sdk->lookupSymbol(stem)) continue;
        SymbolInfo aliasSym(SymbolKind::Module, stem, TypeInfo());
        aliasSym.moduleName = info.moduleName;
        sdk->registerSymbol(stem, aliasSym);
        sdk->addModuleAlias(stem, target);
    }
}

void parseSdkDir(const std::string& sdkDir, Yux& yux) {
    auto pkgMap = readSdkPkg(sdkDir);

    std::vector<std::string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
            if (filename.size() >= 9 &&
                filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
            yuxFiles.push_back(entry.path().string());
        }
    }
    std::sort(yuxFiles.begin(), yuxFiles.end());

    // 第一遍：平铺（base.*）；先建好 _sdkFile 以便后续命名空间文件的父作用域有效
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlat = (it == pkgMap.end()) || it->second.isFlat;
        if (!isFlat) continue;

        antlr4::ANTLRFileStream file;
        file.loadFromFile(yuxFile);
        yux::yuxLexer lexer(&file);
        SyntaxErrorListener errListener(yuxFile, std::cerr);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errListener);
        antlr4::CommonTokenStream tokenStream(&lexer);
        yux::yuxParser parser(&tokenStream);
        parser.removeErrorListeners();
        parser.addErrorListener(&errListener);
        auto program = parser.program();
        if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
            throw YuxError(1, ErrorCode::E5010, yuxFile);
        }
        ASTBuilder astBuilder(yux, "yux.core", true);
        astBuilder.build(program);
    }

    // 第二遍：命名空间（math 等）→ 独立 FileNode 注册到 _modules
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        if (it == pkgMap.end() || it->second.isFlat) continue;

        yux.loadMainFile(fs::absolute(yuxFile).string(), it->second.moduleName);
    }

    registerSdkPkgAliases(yux, pkgMap);
}

}  // namespace sdk_loader
