// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Riu 的 spec/impl 驱动：注册表懒构建 + SpecImplChecker 全量校验。
//
// 实现放在 analyzer 而不是 ast/riu.cpp，避免 riu_ast 编译期 include analyzer/。
// 构造/析构也在本文件：unique_ptr<SpecRegistry/SpecImplChecker> 的构造期
// unwind 与析构都需要完整类型；FileNode / RdBuilder 所有权逻辑不变。

#include "ast/riu.h"

#include "ast/mod_decl.h"
#include "ast/rd_builder.h"
#include "spec_impl_checker.h"
#include "spec_registry.h"

Riu::Riu() : _sdkFile(nullptr) {}

Riu::~Riu() {
    _rdBuilders.clear();
    _declOwners.clear();
    for (auto file : _files) {
        delete file;
    }
    delete _sdkFile;
}

SpecRegistry& Riu::specRegistry() {
    if (!_specRegistry) {
        _specRegistry = std::make_unique<SpecRegistry>(this);
        _specRegistry->buildFromAllFiles();
    }
    return *_specRegistry;
}

void Riu::rebuildSpecRegistry() {
    if (!_specRegistry) {
        _specRegistry = std::make_unique<SpecRegistry>(this);
    }
    _specRegistry->buildFromAllFiles();
}

void Riu::validateSpecImpls() {
    if (_specImplValidated) return;
    if (!_specImplChecker) {
        _specImplChecker = std::make_unique<SpecImplChecker>(this);
    }
    _specImplChecker->validate();
    _specImplValidated = true;
}

SpecImplChecker& Riu::specImplChecker() {
    validateSpecImpls();
    return *_specImplChecker;
}
