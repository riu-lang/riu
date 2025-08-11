// Copyright (c) 2026. Yin-Jinlong@github

#include <windows.h>

#undef ERROR

#include "types.h"
#include "iostream"
#include <cstdio>
#include <filesystem>
#include <regex>

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
    // TODO 跨平台
    std::u16string u16((char16_t*)wstr.c_str());
    auto u8 = utf8::utf16tou8(u16);
    return {u8.begin(), u8.end()};
}

struct IRResult {
    unique_ptr<llvm::LLVMContext> context;
    unique_ptr<llvm::Module> module;
};

IRResult compileIR(string inputFile) {
    antlr4::ANTLRFileStream file;
    file.loadFromFile(inputFile);
    yuxLexer lexer(&file);

    antlr4::CommonTokenStream tokenStream(&lexer);

    yuxParser parser(&tokenStream);

    auto program = parser.program();
    if (parser.getNumberOfSyntaxErrors()) {
        exit(1);
    }

    std::cout << "Compile IR... " << std::endl;
    auto context = make_unique<llvm::LLVMContext>();
    auto module = make_unique<llvm::Module>("main", *context);

    llvm::IRBuilder<> builder(*context);

    ASTBuilder astBuilder(*context);

    try {
        auto ast = astBuilder.build(program);
        Compiler compiler(*context, builder, module.get(), ast);
        compiler.compile(ast);
    } catch (runtime_error& e) {
        std::cerr << e.what() << std::endl;
        exit(1);
    }
    return {(std::move(context)), std::move(module)};
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    CLI::App app{"yux compiler"};

    bool emitIr = false;
    app.add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

#ifdef _DEBUG
    app.add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file")
       ->required(true);

    CLI11_PARSE(app, argc, argv);

    if (!std::filesystem::exists(inputFile)) {
        std::cerr << "Error: Input file not found: " << inputFile << std::endl;
        return 1;
    }

    std::string baseName = llvm::sys::path::stem(inputFile).str();
    std::string objName = baseName + ".obj";

    auto irr = compileIR(inputFile);
    auto module = irr.module.get();

    if (emitIr) {
        std::string irName = baseName + ".ll";
        std::error_code ec;
        llvm::raw_fd_ostream irFile(irName, ec);
        if (ec) {
            std::cerr << "Error opening IR file: " << ec.message() << std::endl;
        } else {
            module->print(irFile, nullptr);
            irFile.flush();
            std::cout << "Write IR ok: " << irName << std::endl;
        }
    }

    if (!compileIRToObj(module, objName)) {
        std::cerr << "Failed to compile IR to object file" << std::endl;
        delete module;
        return 1;
    }

    std::cout << "Write obj: " << objName << std::endl;

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string libExePath = "/LIBPATH:" + llvm::sys::path::parent_path(exePath).str();
    auto exeOut = "/out:" + baseName + ".exe";

    std::vector args = {
        "lld-link",
        objName.c_str(),
        exeOut.c_str(),
        "/subsystem:console",
        "/entry:mainStartup",
        "/LIBPATH:.",
        libExePath.c_str(),
        "yux_rt.lib",
        "kernel32.lib"
    };
    std::string stdoutStr, stderrStr;
    llvm::raw_string_ostream stdoutOS(stdoutStr), stderrOS(stderrStr);

    std::cout << "Link obj: " << baseName + ".exe" << std::endl;
    lld::DriverDef driverDef = {lld::WinLink, &lld::coff::link};
    lld::Result result = lldMain(args, stdoutOS, stderrOS, llvm::ArrayRef{driverDef});

    if (result.retCode) {
        llvm::errs() << stderrStr;
        return 1;
    }

    return 0;
}
