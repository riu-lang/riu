// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// CLI 编译路径共享件 — 实现
//
// 见 sdk_compile.h 头注释; 本文件从 src/main.cpp 拆出, 方法体未变,
// 仅整体包入 yux::cli namespace + 必要的 yux:: 限定。

#include "sdk_compile.h"

#include <llvm/IR/IRBuilder.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>

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

void parseAST(std::string inputFile, Yux& yux, bool isSdk) {
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

IRResult compileIR(std::string inputFile, Yux& yux, bool isSdk) {
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
    return {std::move(context), std::move(module)};
}

SdkPaths sdkBuildPaths(const std::string& sdkPathAbs) {
    namespace fs = std::filesystem;
    fs::path sdkRoot = fs::path(sdkPathAbs).parent_path().parent_path().parent_path();
    fs::path build = sdkRoot / "build";
    fs::path objDir = build / "src" / "yux";
    fs::create_directories(objDir);
    return {
        (objDir / "core.obj").string(),
        (build / "yux.lib").string(),
        (objDir / "core.ll").string(),
    };
}

bool needRecompileSdkDir(const std::string& sdkDir, const std::string& sdkObjPath) {
    if (!std::filesystem::exists(sdkObjPath)) {
        return true;
    }

    auto objTime = std::filesystem::last_write_time(sdkObjPath);

    for (const auto& entry : std::filesystem::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (filename.size() >= 9 && filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
                if (std::filesystem::last_write_time(entry.path()) > objTime) {
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

IRResult compileSdkDir(std::string sdkDir, Yux& yux) {
    namespace fs = std::filesystem;
    std::cout << "Compiling SDK from directory: " << sdkDir << '\n';

    auto context = std::make_unique<llvm::LLVMContext>();
    auto module = std::make_unique<llvm::Module>("yux.core", *context);
    llvm::IRBuilder<> builder(*context);

    auto pkgMap = sdk_loader::readSdkPkg(sdkDir);

    std::vector<std::string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            std::string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (filename.size() >= 9 && filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
                yuxFiles.push_back(entry.path().string());
            }
        }
    }
    std::sort(yuxFiles.begin(), yuxFiles.end());

    auto reportErr = [&](const std::string& yuxFile, std::runtime_error& e) {
        reportRuntimeError(yuxFile, e, "Error in SDK file " + yuxFile + ": ");
    };

    // 第一遍: 平铺文件 → 合并入 _sdkFile。
    // 先把所有平铺文件的 AST 累加进 _sdkFile, 然后再做一次性 IR 编译。
    // 旧版本是「每文件 parse + compile 一次」: 因为 _sdkFile 是单例, 每次 compile
    // 都会把已经处理过的文件的函数再编一遍, 触发 LLVM 「bad signature」断言。
    p<FileNode> sdkAst;
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlat = (it == pkgMap.end()) || it->second.isFlat;
        if (!isFlat) continue;

        std::cout << "  Processing: " << yuxFile << '\n';
        antlr4::ANTLRFileStream file;
        file.loadFromFile(yuxFile);
        yuxLexer lexer(&file);
        SyntaxErrorListener errListener(yuxFile, std::cerr);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errListener);
        antlr4::CommonTokenStream tokenStream(&lexer);
        yuxParser parser(&tokenStream);
        parser.removeErrorListeners();
        parser.addErrorListener(&errListener);
        auto program = parser.program();
        if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
            std::cerr << "Syntax errors in SDK file: " << yuxFile << '\n';
            exit(1);
        }
        ASTBuilder astBuilder(yux, "yux.core", true);
        try {
            sdkAst = astBuilder.build(program); // 始终返回 _sdkFile (单例)
        } catch (std::runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }
    }

    // 平铺文件累加完成后, 用合并后的 _sdkFile 一次性发射 IR (含运行时辅助)。
    if (sdkAst) {
        try {
            Compiler compiler(*context, builder, module.get(), sdkAst, &yux, true);
            compiler.compile(sdkAst);
        } catch (std::runtime_error& e) {
            reportErr("(sdk flat compile)", e);
            exit(1);
        }
    }

    // 第二遍: 命名空间文件 → 独立 FileNode 注册到 _modules, Compiler isSdk=false (避免重复 emit 运行时辅助)
    for (const auto& yuxFile : yuxFiles) {
        std::string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        if (it == pkgMap.end() || it->second.isFlat) continue;
        const std::string& mn = it->second.moduleName;

        std::cout << "  Processing: " << yuxFile << " (module: " << mn << ")" << '\n';
        try {
            auto fileNode = yux.loadMainFile(fs::absolute(yuxFile).string(), mn);
            Compiler compiler(*context, builder, module.get(), fileNode, &yux, false);
            compiler.compile(fileNode);
        } catch (std::runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }
    }

    sdk_loader::registerSdkPkgAliases(yux, pkgMap);

    return {std::move(context), std::move(module)};
}

} // namespace yux::cli
