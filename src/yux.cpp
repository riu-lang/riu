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
    
    if (_sdkFile) {
        for (auto& [name, fnSymbols] : _sdkFile->localFnSymbols()) {
            for (auto& fnSym : fnSymbols) {
                file->registerFnSymbol(name, fnSym);
            }
        }
        for (auto& [name, sym] : _sdkFile->localSymbols()) {
            file->registerSymbol(name, sym);
        }
    }
    
    _files.push_back(file);
    return file;
}

p<FileNode> Yux::createSdkFile() {
    _sdkFile = new FileNode("sdk");
    return _sdkFile;
}
