// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件 — 实现
//
// 见 sdk_compile.h 头注释。

#include "sdk_compile.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include <filesystem>
#include <iostream>

#include "tools/diagnostic.h"

namespace yux::cli {

std::string getBuildDir(const std::string& projectRoot) {
    if (projectRoot.empty()) return "build";
    return (std::filesystem::path(projectRoot) / "build").string();
}

void ensureBuildDir(const std::string& buildDir) {
    std::filesystem::create_directories(buildDir);
}

// 模块名 → 构建产物基础路径 (不含扩展名)。
// 多段 `A.B.C` → `<buildDir>/A/B/C`;
// 单段 `X`: 项目模式 `<buildDir>/<projectName>/X`, 单文件模式 `<buildDir>/X`。
std::string moduleOutputBase(const std::string& buildDir, const std::string& projectName,
                             const std::string& moduleName) {
    std::filesystem::path p(buildDir);
    size_t start = 0;
    size_t dot = moduleName.find('.');
    if (dot == std::string::npos) {
        if (!projectName.empty()) p /= projectName;
        p /= moduleName;
        return p.string();
    }
    while (true) {
        size_t next = moduleName.find('.', start);
        if (next == std::string::npos) {
            p /= moduleName.substr(start);
            break;
        }
        p /= moduleName.substr(start, next - start);
        start = next + 1;
    }
    return p.string();
}

bool compileIRToObj(llvm::Module* module, const std::string& outputPath) {
    llvm::InitializeAllTargetInfos();
    llvm::InitializeAllTargets();
    llvm::InitializeAllTargetMCs();
    llvm::InitializeAllAsmParsers();
    llvm::InitializeAllAsmPrinters();
    llvm::InitializeAllDisassemblers();

    std::string error;
    llvm::Triple triple("x86_64-pc-windows-msvc");
    auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) {
        std::cerr << "Error finding target: " << error << '\n';
        return false;
    }

    llvm::TargetOptions options;
    llvm::Reloc::Model relocModel = llvm::Reloc::PIC_;
    llvm::CodeModel::Model codeModel = llvm::CodeModel::Small;
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::None;

    llvm::TargetMachine* targetMachine =
        target->createTargetMachine(triple, "x86-64", "", options, relocModel, codeModel, optLevel);

    if (!targetMachine) {
        std::cerr << "Error creating target machine" << '\n';
        return false;
    }

    module->setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream objFile(outputPath, ec);
    if (ec) {
        std::cerr << "Error opening output file: " << ec.message() << '\n';
        delete targetMachine;
        return false;
    }

    llvm::legacy::PassManager pm;
    if (targetMachine->addPassesToEmitFile(pm, objFile, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "Error emitting object file" << '\n';
        delete targetMachine;
        return false;
    }

    pm.run(*module);
    objFile.flush();

    delete targetMachine;
    return true;
}

void reportRuntimeError(const std::string& sourcePath, const std::runtime_error& e, const std::string& prefix) {
    if (auto* yuxErr = dynamic_cast<const YuxError*>(&e)) {
        if (!prefix.empty()) std::cerr << prefix;
        DiagnosticEngine::renderYuxError(std::cerr, sourcePath, *yuxErr);
    } else {
        std::cerr << prefix << e.what() << '\n';
    }
}


} // namespace yux::cli
