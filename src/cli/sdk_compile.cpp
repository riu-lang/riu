// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件 — 实现
//
// 见 sdk_compile.h 头注释; 本文件从 src/main.cpp 拆出, 方法体未变,
// 仅整体包入 yux::cli namespace + 必要的 yux:: 限定。

#include "sdk_compile.h"

#include <lld/Common/Driver.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <vector>

#include "ast/ast_builder.h"
#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/yux.h"
#include "compiler/compiler.h"
#include "tools/diagnostic.h"
#include "tools/sdk_loader.h"
#include "tools/syntax_error_listener.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

LLD_HAS_DRIVER(coff)

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

void parseAST(const std::string& inputFile, Yux& yux, bool isSdk) {
    antlr4::ANTLRFileStream file;
    file.loadFromFile(inputFile);
    yuxLexer lexer(&file);
    SyntaxErrorListener errListener(inputFile, std::cerr);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);

    antlr4::CommonTokenStream tokenStream(&lexer);

    yuxParser parser(&tokenStream);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);

    auto program = parser.program();
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        exit(1);
    }

    ASTBuilder astBuilder(yux, "yux.core", true);

    try {
        astBuilder.build(program);
    } catch (std::runtime_error& e) {
        reportRuntimeError(inputFile, e);
        exit(1);
    }
}

IRResult compileIR(const std::string& inputFile, Yux& yux, bool isSdk) {
    antlr4::ANTLRFileStream file;
    file.loadFromFile(inputFile);
    yuxLexer lexer(&file);
    SyntaxErrorListener errListener(inputFile, std::cerr);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errListener);

    antlr4::CommonTokenStream tokenStream(&lexer);

    yuxParser parser(&tokenStream);
    parser.removeErrorListeners();
    parser.addErrorListener(&errListener);

    auto program = parser.program();
    if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
        exit(1);
    }

    std::string moduleName = llvm::sys::path::stem(inputFile).str();
    auto parentPath = llvm::sys::path::parent_path(inputFile);
    if (!parentPath.empty()) {
        auto parentName = llvm::sys::path::filename(parentPath).str();
        if (parentName != "." && parentName != "..") {
            moduleName = parentName + "." + moduleName;
        }
    }

    if (isSdk) {
        moduleName = "yux.core";
    }

    std::cout << "Compile IR... (module: " << moduleName << ")" << '\n';
    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>(moduleName, *context);

    llvm::IRBuilder<> builder(*context);

    ASTBuilder astBuilder(yux, moduleName, isSdk);

    try {
        auto ast = astBuilder.build(program);
        Compiler compiler(*context, builder, module.get(), ast, &yux, isSdk);
        compiler.compile(ast);
    } catch (std::runtime_error& e) {
        reportRuntimeError(inputFile, e);
        exit(1);
    }
    return {.context = std::move(context), .module = std::move(module)};
}

SdkPaths sdkBuildPaths(const std::string& sdkPathAbs) {
    namespace fs = std::filesystem;
    fs::path sdkRoot = fs::path(sdkPathAbs).parent_path().parent_path().parent_path();
    fs::path build = sdkRoot / "build";
    fs::path objDir = build / "src" / "yux" / "core";
    fs::create_directories(objDir);
    return {
        .objDir = objDir.string(),
        .libPath = (build / "yux.lib").string(),
        .irDir = objDir.string(),
    };
}

bool needRecompileSdkDir(const std::string& sdkDir, const std::string& sdkObjDir) {
    // 检查 yux.lib 是否存在且比所有 SDK 源文件新
    std::string libPath = (std::filesystem::path(sdkObjDir).parent_path().parent_path() / "yux.lib").string();
    if (!std::filesystem::exists(libPath)) {
        return true;
    }

    auto libTime = std::filesystem::last_write_time(libPath);

    for (const auto& entry : std::filesystem::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (filename.size() >= 9 && filename.ends_with(".test.yux")) continue;
                if (std::filesystem::last_write_time(entry.path()) > libTime) {
                    return true;
                }
            }
        }
    }

    return false;
}

// parseSdkDir 的薄壳: 捕获 YuxError 并按原行为 reportRuntimeError + exit(1)。
void parseSdkDirOrExit(const std::string& sdkDir, Yux& yux) {
    try {
        sdk_loader::parseSdkDir(sdkDir, yux);
    } catch (std::runtime_error& e) {
        // sdk_loader 抛 YuxError 时 .what() 已是格式化串, 但缺 sourcePath context;
        // 退到 SDK 目录维度报, 与旧版 "Error in SDK file <dir>: " 等价的可读性。
        reportRuntimeError(sdkDir, e, "Error in SDK: ");
        exit(1);
    }
}

void compileSdkDir(const std::string& sdkDir, Yux& yux) {
    namespace fs = std::filesystem;
    std::cout << "Compiling SDK from directory: " << sdkDir << '\n';

    auto pkgMap = sdk_loader::readSdkPkg(sdkDir);
    SdkPaths sp = sdkBuildPaths(sdkDir);

    std::vector<std::string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (filename.size() >= 9 && filename.ends_with(".test.yux")) continue;
                yuxFiles.push_back(entry.path().string());
            }
        }
    }
    std::ranges::sort(yuxFiles);

    auto reportErr = [&](const std::string& yuxFile, std::runtime_error& e) {
        reportRuntimeError(yuxFile, e, "Error in SDK file " + yuxFile + ": ");
    };

    // 确保 _sdkFile 已解析 (含 AST 注册)
    // parseSdkDir 应在调用前由 parseSdkDirOrExit() 完成；
    // 此处仅作防御性检查：若 _modules 里缺少 SDK 模块，补跑一次 parse。
    {
        bool hasSdkModules = false;
        for (auto& [stem, info] : pkgMap) {
            if (yux.module(info.moduleName)) {
                hasSdkModules = true;
                break;
            }
        }
        if (!hasSdkModules) {
            // sdk_loader 内部保证至少创建 _sdkFile 与各子模块
            sdk_loader::parseSdkDir(sdkDir, yux);
        }
    }

    vector<std::string> objPaths;

    // 每文件独立 LLVM Module + Compiler 遍
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlatDep = (it == pkgMap.end()) || it->second.isFlat;

        std::string moduleName;
        if (it != pkgMap.end() && !it->second.moduleName.empty() && !it->second.isFlat) {
            moduleName = it->second.moduleName;
        } else {
            moduleName = "yux.core." + stem;
        }

        // 只有 base.yux 发射运行时辅助（isSdkRuntime=true）
        bool isSdkRuntime = isFlatDep && (stem == "base");

        auto file = yux.module(moduleName);
        if (!file) {
            std::cerr << "Error: SDK module " << moduleName << " not loaded from " << yuxFile << '\n';
            exit(1);
        }

        std::cout << "  Compiling: " << moduleName << " (" << stem << ".yux)" << (isSdkRuntime ? " [runtime]" : "")
                  << '\n';

        auto context = std::make_unique<llvm::LLVMContext>();
        auto module = std::make_unique<llvm::Module>(moduleName, *context);
        llvm::IRBuilder<> builder(*context);

        try {
            Compiler compiler(*context, builder, module.get(), file, &yux, isSdkRuntime);
            compiler.compile(file);
        } catch (std::runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }

        std::string objPath = (fs::path(sp.objDir) / (stem + ".obj")).string();
        if (!compileIRToObj(module.get(), objPath)) {
            std::cerr << "Failed to compile SDK obj: " << objPath << '\n';
            exit(1);
        }
        std::cout << "  Write obj: " << objPath << '\n';
        objPaths.push_back(objPath);
    }

    sdk_loader::registerSdkPkgAliases(yux, pkgMap);

    // 用 lld-link /lib 将所有 obj 归档为 yux.lib
    if (!objPaths.empty()) {
        string libOutArg = "/out:" + sp.libPath;
        vector<const char*> libArgs = {"lld-link", "/lib", libOutArg.c_str()};
        for (auto& o : objPaths)
            libArgs.push_back(o.c_str());

        string outStr, errStr;
        llvm::raw_string_ostream oOS(outStr), eOS(errStr);
        lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
        auto r = lldMain(libArgs, oOS, eOS, llvm::ArrayRef{dd});
        if (r.retCode) {
            llvm::errs() << errStr;
            std::cerr << "Failed to archive SDK lib" << '\n';
            exit(1);
        }
        std::cout << "Write SDK lib: " << sp.libPath << '\n';
    }
}

} // namespace yux::cli
