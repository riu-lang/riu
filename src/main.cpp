// Copyright (c) 2026. Yin-Jinlong@github

#include <windows.h>

#undef ERROR

#include "types.h"
#include "iostream"
#include <cstdio>
#include <filesystem>
#include <regex>
#include <csignal>

#include <lld/Common/Driver.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>

#include "ast_builder.h"
#include "utf8.h"
#include "yux.h"
#include "build_cache.h"

#include "CLI11.hpp"

LLD_HAS_DRIVER(coff)

LLD_HAS_DRIVER(elf)

LLD_HAS_DRIVER(macho)

LLD_HAS_DRIVER(wasm)

#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"
#include "yux/yuxVisitor.h"

#include "node/fn_node.h"
#include "node/expr_node.h"
#include "compiler.h"
#include <llvm/Support/Path.h>

using namespace yux;

#ifdef _DEBUG

bool debug = false;

#endif

string getBuildDir() {
    return "build";
}

void ensureBuildDir() {
    std::filesystem::create_directories(getBuildDir());
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
        std::cerr << "Error finding target: " << error << std::endl;
        return false;
    }

    llvm::TargetOptions options;
    llvm::Reloc::Model relocModel = llvm::Reloc::PIC_;
    llvm::CodeModel::Model codeModel = llvm::CodeModel::Small;
    llvm::CodeGenOptLevel optLevel = llvm::CodeGenOptLevel::None;

    llvm::TargetMachine* targetMachine = target->createTargetMachine(
        triple, "x86-64", "", options, relocModel, codeModel, optLevel
    );

    if (!targetMachine) {
        std::cerr << "Error creating target machine" << std::endl;
        return false;
    }

    module->setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream objFile(outputPath, ec);
    if (ec) {
        std::cerr << "Error opening output file: " << ec.message() << std::endl;
        delete targetMachine;
        return false;
    }

    llvm::legacy::PassManager pm;
    if (targetMachine->addPassesToEmitFile(pm, objFile, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        std::cerr << "Error emitting object file" << std::endl;
        delete targetMachine;
        return false;
    }

    pm.run(*module);
    objFile.flush();

    delete targetMachine;
    return true;
}

std::string wstr2str(const std::wstring& wstr) {
    std::u16string u16((char16_t*)wstr.c_str());
    auto u8 = utf8::utf16tou8(u16);
    return {u8.begin(), u8.end()};
}

struct IRResult {
    unique_ptr<llvm::LLVMContext> context;
    unique_ptr<llvm::Module> module;
};

void parseAST(string inputFile, Yux& yux, bool isSdk = false) {
    antlr4::ANTLRFileStream file;
    file.loadFromFile(inputFile);
    yuxLexer lexer(&file);

    antlr4::CommonTokenStream tokenStream(&lexer);

    yuxParser parser(&tokenStream);

    auto program = parser.program();
    if (parser.getNumberOfSyntaxErrors()) {
        exit(1);
    }

    llvm::LLVMContext context;
    ASTBuilder astBuilder(context, yux, "yux.core", true);

    try {
        astBuilder.build(program);
    } catch (runtime_error& e) {
        string msg = e.what();
        if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
            int line = yuxErr->getLineNumber();
            if (line > 0) {
                msg = "line " + to_string(line) + ": " + msg;
            }
        }
        std::cerr << msg << std::endl;
        exit(1);
    }
}

IRResult compileIR(string inputFile, Yux& yux, bool isSdk = false) {
    antlr4::ANTLRFileStream file;
    file.loadFromFile(inputFile);
    yuxLexer lexer(&file);

    antlr4::CommonTokenStream tokenStream(&lexer);

    yuxParser parser(&tokenStream);

    auto program = parser.program();
    if (parser.getNumberOfSyntaxErrors()) {
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

    std::cout << "Compile IR... (module: " << moduleName << ")" << std::endl;
    auto context = make_unique<llvm::LLVMContext>();
    auto module = make_unique<llvm::Module>(moduleName, *context);

    llvm::IRBuilder<> builder(*context);

    ASTBuilder astBuilder(*context, yux, moduleName, isSdk);

    try {
        auto ast = astBuilder.build(program);
        Compiler compiler(*context, builder, module.get(), ast, &yux, isSdk);
        compiler.compile(ast);
    } catch (runtime_error& e) {
        string msg = e.what();
        if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
            int line = yuxErr->getLineNumber();
            if (line > 0) {
                msg = "line " + to_string(line) + ": " + msg;
            }
        }
        std::cerr << msg << std::endl;
        exit(1);
    }
    return {(std::move(context)), std::move(module)};
}

string findSdkPath() {
#ifdef _DEBUG
    if (std::filesystem::exists("sdk/yux/core.yux")) {
        return "sdk/yux/core.yux";
    }
#endif
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    auto exeDir = llvm::sys::path::parent_path(exePath).str();
    auto rootDir = llvm::sys::path::parent_path(exeDir).str();
    string sdkPath = rootDir + "/sdk/yux/core.yux";
    if (std::filesystem::exists(sdkPath)) {
        return sdkPath;
    }
    return "";
}

void handleCrash(int signal) {
    std::cerr << "\nProgram crashed! Signal: " << signal << std::endl;
    _exit(1);
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    signal(SIGSEGV, handleCrash);
    signal(SIGABRT, handleCrash);
    signal(SIGFPE, handleCrash);

    CLI::App app{"yux compiler"};

    bool emitIr = false;
    app.add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

#ifdef _DEBUG
    app.add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif
    std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << std::endl;

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file")
       ->required(true);

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: Input file not found: " << inputFile << std::endl;
        return 1;
    }

    inputFile = std::filesystem::absolute(inputFile).string();

    ensureBuildDir();
    string buildDir = getBuildDir();

    Yux yux;

    string sdkPath = findSdkPath();
    string sdkObjPath;
    bool compiled = false;

    if (!sdkPath.empty()) {
        sdkPath = std::filesystem::absolute(sdkPath).string();
        sdkObjPath = buildDir + "/yux.core.obj";

        bool needCompile = BuildCache::needRecompile(sdkObjPath, sdkPath);
        if (needCompile) {
            SdkLock sdkLock;
            sdkLock.tryLock();
            
            needCompile = BuildCache::needRecompile(sdkObjPath, sdkPath);
            if (needCompile) {
                std::cout << "Compiling SDK: " << sdkPath << std::endl;
                auto sdkIrr = compileIR(sdkPath, yux, true);
                auto sdkModule = sdkIrr.module.get();

                if (emitIr) {
                    string sdkIrPath = buildDir + "/yux.core.ll";
                    std::error_code ec;
                    llvm::raw_fd_ostream irFile(sdkIrPath, ec);
                    if (!ec) {
                        sdkModule->print(irFile, nullptr);
                        irFile.flush();
                        std::cout << "Write SDK IR: " << sdkIrPath << std::endl;
                    }
                }

                if (!compileIRToObj(sdkModule, sdkObjPath)) {
                    std::cerr << "Failed to compile SDK to object file" << std::endl;
                    return 1;
                }
                std::cout << "Write SDK obj: " << sdkObjPath << std::endl;
                BuildCache::updateCache(sdkObjPath, sdkPath);
                compiled = true;
            } else {
                parseAST(sdkPath, yux, true);
            }
        } else {
            parseAST(sdkPath, yux, true);
        }
    }

    std::string baseName = llvm::sys::path::stem(inputFile).str();
    std::string objPath = buildDir + "/" + baseName + ".obj";

    // 始终先解析主文件的 AST（也会触发 `use` 递归加载所有导入模块），
    // 以便后续决定哪些模块需要重新 codegen 与链接。
    p<FileNode> mainFile = nullptr;
    try {
        mainFile = yux.loadMainFile(inputFile, baseName);
    } catch (runtime_error& e) {
        string msg = e.what();
        if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
            int line = yuxErr->getLineNumber();
            if (line > 0) {
                msg = "line " + to_string(line) + ": " + msg;
            }
        }
        std::cerr << msg << std::endl;
        return 1;
    }

    auto codegenTo = [&](p<FileNode> file, const std::string& moduleName,
                         const std::string& objOut, const std::string& irOut) -> bool {
        std::cout << "Compile IR... (module: " << moduleName << ")" << std::endl;
        auto ctx = std::make_unique<llvm::LLVMContext>();
        auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
        llvm::IRBuilder<> builder(*ctx);
        try {
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, false);
            compiler.compile(file);
        } catch (runtime_error& e) {
            string msg = e.what();
            if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
                int line = yuxErr->getLineNumber();
                if (line > 0) msg = "line " + to_string(line) + ": " + msg;
            }
            std::cerr << msg << std::endl;
            return false;
        }

        if (emitIr) {
            std::error_code ec;
            llvm::raw_fd_ostream irFile(irOut, ec);
            if (ec) {
                std::cerr << "Error opening IR file: " << ec.message() << std::endl;
            } else {
                mod->print(irFile, nullptr);
                irFile.flush();
                std::cout << "Write IR ok: " << irOut << std::endl;
            }
        }

        if (!compileIRToObj(mod.get(), objOut)) {
            std::cerr << "Failed to compile IR to object file: " << objOut << std::endl;
            return false;
        }
        std::cout << "Write obj: " << objOut << std::endl;
        return true;
    };

    // 主模块
    bool needCompile = BuildCache::needRecompile(objPath, inputFile);
    if (needCompile) {
        std::string irPath = buildDir + "/" + baseName + ".ll";
        if (!codegenTo(mainFile, baseName, objPath, irPath)) {
            return 1;
        }
        BuildCache::updateCache(objPath, inputFile);
        compiled = true;
    }

    // 导入的用户模块
    std::vector<std::string> modObjPaths;
    for (auto& modName : yux.loadOrder()) {
        auto modFile = yux.module(modName);
        if (!modFile || modFile == yux.sdkFile()) continue;
        std::string modSrc = yux.modulePath(modName);
        std::string modObj = buildDir + "/" + modName + ".obj";
        std::string modIr = buildDir + "/" + modName + ".ll";
        if (BuildCache::needRecompile(modObj, modSrc)) {
            if (!codegenTo(modFile, modName, modObj, modIr)) {
                return 1;
            }
            BuildCache::updateCache(modObj, modSrc);
            compiled = true;
        }
        modObjPaths.push_back(modObj);
    }

    std::string exePath = buildDir + "/" + baseName + ".exe";

    bool needLink = !std::filesystem::exists(exePath);
    if (!needLink) {
        try {
            auto exeTime = std::filesystem::last_write_time(exePath);
            if (std::filesystem::last_write_time(objPath) > exeTime) {
                needLink = true;
            }
            if (!sdkObjPath.empty() &&
                std::filesystem::last_write_time(sdkObjPath) > exeTime) {
                needLink = true;
            }
            for (auto& mo : modObjPaths) {
                if (std::filesystem::last_write_time(mo) > exeTime) {
                    needLink = true;
                    break;
                }
            }
        } catch (const std::exception& e) {
            needLink = true;
        }
    }

    if (needLink) {
        auto exeOut = "/out:" + exePath;

        std::vector<const char*> args = {
            "lld-link",
            objPath.c_str(),
            exeOut.c_str(),
            "/subsystem:console",
            "/entry:mainStartup",
            "kernel32.lib"
        };

        if (!sdkObjPath.empty()) {
            args.insert(args.begin() + 2, sdkObjPath.c_str());
        }
        for (auto& mo : modObjPaths) {
            args.insert(args.begin() + 2, mo.c_str());
        }

        std::string stdoutStr, stderrStr;
        llvm::raw_string_ostream stdoutOS(stdoutStr), stderrOS(stderrStr);

        std::cout << "Link obj: " << exePath << std::endl;
        lld::DriverDef driverDef = {lld::WinLink, &lld::coff::link};
        lld::Result result = lldMain(args, stdoutOS, stderrOS, llvm::ArrayRef{driverDef});

        if (result.retCode) {
            llvm::errs() << stderrStr;
            return 1;
        }
        compiled = true;
    }

    if (!compiled) {
        std::cout << "no work to do." << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
