// Copyright (c) 2026. Yin-Jinlong@github

#include "yux.h"

#include <filesystem>

#include "ast_builder.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

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
    return file;
}

p<FileNode> Yux::createSdkFile() {
    // SDK 自举运行时。文件 sdk/yux/core.yux，模块名 "yux.core"。
    _sdkFile = new FileNode("yux.core");
    _modules["yux.core"] = _sdkFile;
    return _sdkFile;
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
    antlr4::CommonTokenStream tokenStream(&lexer);
    yux::yuxParser parser(&tokenStream);
    auto program = parser.program();
    if (parser.getNumberOfSyntaxErrors()) {
        throw YuxError("syntax errors in " + absPath, errorLine);
    }

    auto astBuilder = std::make_unique<ASTBuilder>(_astContext, *this, moduleName, false);
    auto fileNode = astBuilder->build(program);
    _moduleBuilders.push_back(std::move(astBuilder));
    return fileNode;
}

p<FileNode> Yux::loadMainFile(const string& absPath, const string& moduleName) {
    auto fileNode = _parseFile(absPath, moduleName, 0);
    _modules[moduleName] = fileNode;
    _modulePaths[moduleName] = absPath;
    return fileNode;
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
            throw YuxError("circular module import: " + chain, errorLine);
        }
    }

    // 点分 → 相对路径
    string relPath = moduleName;
    for (auto& c : relPath) {
        if (c == '.') c = '/';
    }
    relPath += ".yux";

    if (!std::filesystem::exists(relPath)) {
        throw YuxError("module not found: " + moduleName + " (expected file " + relPath + ")", errorLine);
    }

    string absPath = std::filesystem::absolute(relPath).string();

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
