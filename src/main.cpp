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
    ASTBuilder astBuilder(context, yux, "yux", true);

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
        // SDK 文件即 yux 模块
        moduleName = "yux";
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
    if (std::filesystem::exists("sdk/yux.yux")) {
        return "sdk/yux.yux";
    }
#endif
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    auto exeDir = llvm::sys::path::parent_path(exePath).str();
    auto rootDir = llvm::sys::path::parent_path(exeDir).str();
    string sdkPath = rootDir + "/sdk/yux.yux";
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

    BuildCache cache(buildDir);
    cache.load();

    Yux yux;

    string sdkPath = findSdkPath();
    string sdkObjPath;
    bool compiled = false;

    if (!sdkPath.empty()) {
        sdkPath = std::filesystem::absolute(sdkPath).string();
        sdkObjPath = buildDir + "/yux.obj";

        bool needCompile = !std::filesystem::exists(sdkObjPath) || cache.needRecompile(sdkPath);
        if (needCompile) {
            std::cout << "Compiling SDK: " << sdkPath << std::endl;
            auto sdkIrr = compileIR(sdkPath, yux, true);
            auto sdkModule = sdkIrr.module.get();

            if (emitIr) {
                string sdkIrPath = buildDir + "/yux.ll";
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
            cache.updateCache(sdkPath);
            compiled = true;
        } else {
            parseAST(sdkPath, yux, true);
        }
    }

    std::string baseName = llvm::sys::path::stem(inputFile).str();
    std::string objPath = buildDir + "/" + baseName + ".obj";

    bool needCompile = !std::filesystem::exists(objPath) || cache.needRecompile(inputFile);
    if (needCompile) {
        auto irr = compileIR(inputFile, yux, false);
        auto module = irr.module.get();

        if (emitIr) {
            std::string irPath = buildDir + "/" + baseName + ".ll";
            std::error_code ec;
            llvm::raw_fd_ostream irFile(irPath, ec);
            if (ec) {
                std::cerr << "Error opening IR file: " << ec.message() << std::endl;
            } else {
                module->print(irFile, nullptr);
                irFile.flush();
                std::cout << "Write IR ok: " << irPath << std::endl;
            }
        }

        if (!compileIRToObj(module, objPath)) {
            std::cerr << "Failed to compile IR to object file" << std::endl;
            return 1;
        }

        std::cout << "Write obj: " << objPath << std::endl;
        cache.updateCache(inputFile);
        compiled = true;
    }

    std::string exePath = buildDir + "/" + baseName + ".exe";

    bool needLink = !std::filesystem::exists(exePath);
    if (!needLink) {
        try {
            auto exeTime = std::filesystem::last_write_time(exePath);
            auto objTime = std::filesystem::last_write_time(objPath);
            if (!sdkObjPath.empty()) {
                auto sdkTime = std::filesystem::last_write_time(sdkObjPath);
                needLink = (objTime > exeTime) || (sdkTime > exeTime);
            } else {
                needLink = objTime > exeTime;
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

    cache.save();
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
