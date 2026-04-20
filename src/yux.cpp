// Copyright (c) 2026. Yin-Jinlong@github

#include "yux.h"

Yux::Yux() : _sdkFile(nullptr) {}

Yux::~Yux() {
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

    // 用户模块默认导入 yux 模块。
    // 通过把 yux 文件设为 parent scope，使得符号查找在本模块未命中时
    // 自然 fallback 到 yux 模块（FnSymbolInfo 上的 moduleName="yux"
    // 会在 mangling 时还原为 yux_xxx 等外部符号）。
    if (_sdkFile && _sdkFile != file) {
        file->setParentScope(_sdkFile);
        file->addImport("yux");
    }

    _files.push_back(file);
    return file;
}

p<FileNode> Yux::createSdkFile() {
    // SDK 即 yux 模块（自举运行时）。文件名 sdk/yux.yux，模块名 "yux"。
    _sdkFile = new FileNode("yux");
    return _sdkFile;
}
