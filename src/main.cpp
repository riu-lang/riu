// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "types.h"
#include "iostream"
#include <cstdio>
#include <filesystem>
#include <regex>
#include <csignal>
#include <fstream>

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
#include "formatter.h"
#include "lsp/lsp_server.h"

#include "CLI/CLI.hpp"

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

string getBuildDir(const string& projectRoot) {
    if (projectRoot.empty()) return "build";
    return (std::filesystem::path(projectRoot) / "build").string();
}

void ensureBuildDir(const string& buildDir) {
    std::filesystem::create_directories(buildDir);
}

// 模块名 → 构建产物基础路径（不含扩展名）。
// 多段 `A.B.C` → `<buildDir>/A/B/C`；
// 单段 `X`：项目模式 `<buildDir>/<projectName>/X`，单文件模式 `<buildDir>/X`。
string moduleOutputBase(const string& buildDir, const string& projectName, const string& moduleName) {
    std::filesystem::path p(buildDir);
    size_t start = 0;
    size_t dot = moduleName.find('.');
    if (dot == string::npos) {
        if (!projectName.empty()) p /= projectName;
        p /= moduleName;
        return p.string();
    }
    while (true) {
        size_t next = moduleName.find('.', start);
        if (next == string::npos) {
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
    return {std::move(context), std::move(module)};
}

bool needRecompileSdkDir(const string& sdkDir, const string& sdkObjPath) {
    if (!std::filesystem::exists(sdkObjPath)) {
        return true;
    }
    
    auto objTime = std::filesystem::last_write_time(sdkObjPath);
    
    for (const auto& entry : std::filesystem::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (std::filesystem::last_write_time(entry.path()) > objTime) {
                    return true;
                }
            }
        }
    }
    
    return false;
}

// pkg 文件解析：filename(去 .yux) → {moduleName, isFlat}
// `name.*` → 平铺到 yux.core；`name` → 命名空间 yux.core.<name>
struct SdkPkgEntry {
    string moduleName;
    bool isFlat;
};

static std::map<string, SdkPkgEntry> readSdkPkg(const string& sdkDir) {
    namespace fs = std::filesystem;
    std::map<string, SdkPkgEntry> r;
    fs::path pkgPath = fs::path(sdkDir) / "pkg";
    if (!fs::exists(pkgPath)) return r;
    std::ifstream f(pkgPath);
    string line;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r\n");
        if (s == string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(s, e - s + 1);
        if (line.empty() || line[0] == ';') continue;
        bool wild = false;
        string name = line;
        if (name.size() >= 2 && name.substr(name.size() - 2) == ".*") {
            wild = true;
            name = name.substr(0, name.size() - 2);
        }
        if (name.empty()) continue;
        r[name] = {wild ? string("yux.core") : ("yux.core." + name), wild};
    }
    return r;
}

// 在 _sdkFile 上为每个非平铺导出登记模块别名，使用户文件经父作用域可访问 `<name>.fn(...)`
static void registerSdkPkgAliases(Yux& yux, const std::map<string, SdkPkgEntry>& pkgMap) {
    auto sdk = yux.sdkFile();
    if (!sdk) return;
    for (auto& [stem, info] : pkgMap) {
        if (info.isFlat) continue;
        auto target = yux.module(info.moduleName);
        if (!target) continue;
        if (sdk->lookupSymbol(stem)) continue;
        SymbolInfo aliasSym(SymbolKind::Module, stem, TypeInfo());
        aliasSym.moduleName = info.moduleName;
        sdk->registerSymbol(stem, aliasSym);
        sdk->addModuleAlias(stem, target);
    }
}

void parseSdkDir(string sdkDir, Yux& yux) {
    namespace fs = std::filesystem;
    auto pkgMap = readSdkPkg(sdkDir);

    vector<string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                yuxFiles.push_back(entry.path().string());
            }
        }
    }
    std::sort(yuxFiles.begin(), yuxFiles.end());

    // 第一遍：平铺（base.*）；先建好 _sdkFile 以便后续命名空间文件的父作用域有效
    for (const auto& yuxFile : yuxFiles) {
        string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlat = (it == pkgMap.end()) || it->second.isFlat;
        if (!isFlat) continue;

        antlr4::ANTLRFileStream file;
        file.loadFromFile(yuxFile);
        yuxLexer lexer(&file);
        antlr4::CommonTokenStream tokenStream(&lexer);
        yuxParser parser(&tokenStream);
        auto program = parser.program();
        if (parser.getNumberOfSyntaxErrors()) {
            std::cerr << "Syntax errors in SDK file: " << yuxFile << std::endl;
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
                if (line > 0) msg = "line " + to_string(line) + ": " + msg;
            }
            std::cerr << "Error in SDK file " << yuxFile << ": " << msg << std::endl;
            exit(1);
        }
    }

    // 第二遍：命名空间（math 等）→ 独立 FileNode 注册到 _modules
    for (const auto& yuxFile : yuxFiles) {
        string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        if (it == pkgMap.end() || it->second.isFlat) continue;

        try {
            yux.loadMainFile(fs::absolute(yuxFile).string(), it->second.moduleName);
        } catch (runtime_error& e) {
            string msg = e.what();
            if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
                int line = yuxErr->getLineNumber();
                if (line > 0) msg = "line " + to_string(line) + ": " + msg;
            }
            std::cerr << "Error in SDK file " << yuxFile << ": " << msg << std::endl;
            exit(1);
        }
    }

    registerSdkPkgAliases(yux, pkgMap);
}

IRResult compileSdkDir(string sdkDir, Yux& yux) {
    namespace fs = std::filesystem;
    std::cout << "Compiling SDK from directory: " << sdkDir << std::endl;

    auto context = make_unique<llvm::LLVMContext>();
    auto module = make_unique<llvm::Module>("yux.core", *context);
    llvm::IRBuilder<> builder(*context);

    auto pkgMap = readSdkPkg(sdkDir);

    vector<string> yuxFiles;
    for (const auto& entry : fs::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                yuxFiles.push_back(entry.path().string());
            }
        }
    }
    std::sort(yuxFiles.begin(), yuxFiles.end());

    auto reportErr = [&](const string& yuxFile, runtime_error& e) {
        string msg = e.what();
        if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
            int line = yuxErr->getLineNumber();
            if (line > 0) msg = "line " + to_string(line) + ": " + msg;
        }
        std::cerr << "Error in SDK file " << yuxFile << ": " << msg << std::endl;
    };

    // 第一遍：平铺文件 → 合并入 _sdkFile，Compiler isSdk=true 顺带发出运行时辅助
    for (const auto& yuxFile : yuxFiles) {
        string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlat = (it == pkgMap.end()) || it->second.isFlat;
        if (!isFlat) continue;

        std::cout << "  Processing: " << yuxFile << std::endl;
        antlr4::ANTLRFileStream file;
        file.loadFromFile(yuxFile);
        yuxLexer lexer(&file);
        antlr4::CommonTokenStream tokenStream(&lexer);
        yuxParser parser(&tokenStream);
        auto program = parser.program();
        if (parser.getNumberOfSyntaxErrors()) {
            std::cerr << "Syntax errors in SDK file: " << yuxFile << std::endl;
            exit(1);
        }
        ASTBuilder astBuilder(*context, yux, "yux.core", true);
        try {
            auto ast = astBuilder.build(program);
            Compiler compiler(*context, builder, module.get(), ast, &yux, true);
            compiler.compile(ast);
        } catch (runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }
    }

    // 第二遍：命名空间文件 → 独立 FileNode 注册到 _modules，Compiler isSdk=false（避免重复 emit 运行时辅助）
    for (const auto& yuxFile : yuxFiles) {
        string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        if (it == pkgMap.end() || it->second.isFlat) continue;
        const string& mn = it->second.moduleName;

        std::cout << "  Processing: " << yuxFile << " (module: " << mn << ")" << std::endl;
        try {
            auto fileNode = yux.loadMainFile(fs::absolute(yuxFile).string(), mn);
            Compiler compiler(*context, builder, module.get(), fileNode, &yux, false);
            compiler.compile(fileNode);
        } catch (runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }
    }

    registerSdkPkgAliases(yux, pkgMap);

    return {std::move(context), std::move(module)};
}

// 当前 SDK 源目录，content = sdk/yux/src/yux/core/。
// TODO(phase-C)：SDK 改用 lib 链路后，此函数返回 SDK 项目根（含 yux.toml），不再直接给 core 目录
string findSdkPath() {
    namespace fs = std::filesystem;
#ifdef _DEBUG
    if (fs::is_directory("sdk/yux/src/yux/core")) {
        return "sdk/yux/src/yux/core";
    }
#endif
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    auto exeDir = llvm::sys::path::parent_path(exePath).str();
    auto rootDir = llvm::sys::path::parent_path(exeDir).str();
    // 优先新布局：<rootDir>/sdk/yux/src/yux/core
    string newSdkPath = rootDir + "/sdk/yux/src/yux/core";
    if (fs::is_directory(newSdkPath)) {
        return newSdkPath;
    }
    // 回退到旧布局
    string oldSdkPath = rootDir + "/sdk/yux/core";
    if (fs::is_directory(oldSdkPath)) {
        return oldSdkPath;
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
    app.require_subcommand(0, 1);

    bool emitIr = false;
    app.add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

#ifdef _DEBUG
    app.add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file (single-file mode)");

    auto* buildCmd = app.add_subcommand("build", "Build project (must run at project root containing yux.toml)");
    std::string buildNameArg;
    buildCmd->add_option("name", buildNameArg, "Project name; must match `name` in yux.toml")->required();
    buildCmd->add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");

#ifdef _DEBUG
    buildCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    auto* lspCmd = app.add_subcommand("lsp", "Run as a Language Server (stdio JSON-RPC)");

    auto* formatCmd = app.add_subcommand("format", "Format a .yux source file");
    std::string formatFile;
    formatCmd->add_option("file", formatFile, "Input .yux file to format");
    bool formatInPlace = false;
    formatCmd->add_flag("-i,--in-place", formatInPlace, "Edit file in place");
    bool formatStdin = false;
    formatCmd->add_flag("--stdin", formatStdin, "Read from stdin instead of file");

    CLI11_PARSE(app, argc, argv);

    // 处理 LSP 子命令：进入 stdio JSON-RPC 主循环
    if (lspCmd->parsed()) {
        return yux::lsp::runServer();
    }

    // 处理格式化命令
    if (formatCmd->parsed()) {
        std::string source;
        std::string filePath;
        
        if (formatStdin) {
            // 从 stdin 读取
            std::stringstream buffer;
            buffer << std::cin.rdbuf();
            source = buffer.str();
        } else {
            // 从文件读取
            if (formatFile.empty()) {
                std::cerr << "Error: No input file specified" << std::endl;
                return 1;
            }
            if (!std::filesystem::exists(formatFile)) {
                std::cerr << "Error: Input file not found: " << formatFile << std::endl;
                return 1;
            }
            
            std::ifstream inFile(formatFile);
            if (!inFile) {
                std::cerr << "Error: Cannot open file: " << formatFile << std::endl;
                return 1;
            }
            
            std::stringstream buffer;
            buffer << inFile.rdbuf();
            source = buffer.str();
            inFile.close();
            filePath = formatFile;
        }
        
        try {
            Formatter formatter(source);
            std::string formatted = formatter.format();
            
            if (formatInPlace && !filePath.empty()) {
                std::ofstream outFile(filePath);
                if (!outFile) {
                    std::cerr << "Error: Cannot write to file: " << filePath << std::endl;
                    return 1;
                }
                outFile << formatted;
                outFile.close();
                std::cout << "Formatted: " << filePath << std::endl;
            } else {
                std::cout << formatted;
            }
        } catch (const std::exception& e) {
            std::cerr << "Format error: " << e.what() << std::endl;
            return 1;
        }
        
        return 0;
    }

    std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << std::endl;

    bool projectMode = buildCmd->parsed();

    Yux yux;
    string projectName;
    string projectBuildDir;
    string buildDir;

    if (projectMode) {
        if (!inputFile.empty()) {
            std::cerr << "Error: cannot combine `build` subcommand with an input file positional" << std::endl;
            return 1;
        }
        string cwd = std::filesystem::current_path().string();
        try {
            yux.initProjectFromDir(cwd);
        } catch (runtime_error& e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }
        if (yux.projectName() != buildNameArg) {
            std::cerr << "Error: build target `" << buildNameArg
                      << "` does not match yux.toml name `" << yux.projectName() << "`" << std::endl;
            return 1;
        }
        if (!yux.isLibProject()) {
            if (yux.projectEntry().empty()) {
                std::cerr << "Error: yux.toml is missing `entry`" << std::endl;
                return 1;
            }
            inputFile = (std::filesystem::path(yux.sourceRoot()) / yux.projectEntry()).string();
            if (!std::filesystem::exists(inputFile)) {
                std::cerr << "Error: entry file not found: " << inputFile << std::endl;
                return 1;
            }
            inputFile = std::filesystem::absolute(inputFile).string();
        }
        projectName = yux.projectName();
        buildDir = getBuildDir(yux.projectRoot());
        projectBuildDir = buildDir + "/" + projectName;
    } else {
        if (inputFile.empty()) {
            std::cerr << app.help() << std::endl;
            return 1;
        }
        if (!std::filesystem::exists(inputFile)) {
            std::cerr << "Error: Input file not found: " << inputFile << std::endl;
            return 1;
        }
        inputFile = std::filesystem::absolute(inputFile).string();
        yux.initSingleFileRoot(inputFile);
        // 单文件模式：产物扁平放在 `<srcDir>/build/`，不使用项目名子目录。
        projectName = "";
        buildDir = getBuildDir(yux.projectRoot());
        projectBuildDir = buildDir;
    }

    std::cout << "Project root: " << yux.projectRoot() << std::endl;
    ensureBuildDir(buildDir);
    ensureBuildDir(projectBuildDir);

    string sdkPath = findSdkPath();
    // SDK 静态库路径：放在 sdk 目录下的 build 中（不放用户项目）
    string sdkLibPath;
    bool compiled = false;

    if (!sdkPath.empty()) {
        namespace fs = std::filesystem;
        sdkPath = fs::absolute(sdkPath).string();
        // sdkPath = <sdkRoot>/src/yux/core；3 级 parent 得到 <sdkRoot>
        fs::path sdkRoot = fs::path(sdkPath).parent_path().parent_path().parent_path();
        fs::path sdkBuildDir = sdkRoot / "build" / "yux";
        fs::create_directories(sdkBuildDir);
        string sdkObjPath = (sdkBuildDir / "core.obj").string();
        sdkLibPath = (sdkBuildDir / "yux.lib").string();

        bool libExists = fs::exists(sdkLibPath);
        bool needCompile = !libExists || needRecompileSdkDir(sdkPath, sdkObjPath);
        if (needCompile) {
            SdkLock sdkLock;
            sdkLock.tryLock();
            needCompile = !fs::exists(sdkLibPath) || needRecompileSdkDir(sdkPath, sdkObjPath);
            if (needCompile) {
                auto sdkIrr = compileSdkDir(sdkPath, yux);
                auto sdkModule = sdkIrr.module.get();

                if (emitIr) {
                    string sdkIrPath = (sdkBuildDir / "core.ll").string();
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

                // 用 lld-link /lib 打包 obj 为静态库
                string libOutArg = "/out:" + sdkLibPath;
                std::vector<const char*> libArgs = {
                    "lld-link", "/lib", sdkObjPath.c_str(), libOutArg.c_str()
                };
                std::string libOutStr, libErrStr;
                llvm::raw_string_ostream libOOS(libOutStr), libEOS(libErrStr);
                lld::DriverDef libDD = {lld::WinLink, &lld::coff::link};
                auto libR = lldMain(libArgs, libOOS, libEOS, llvm::ArrayRef{libDD});
                if (libR.retCode) {
                    llvm::errs() << libErrStr;
                    std::cerr << "Failed to archive SDK lib" << std::endl;
                    return 1;
                }
                std::cout << "Write SDK lib: " << sdkLibPath << std::endl;
                compiled = true;
            } else {
                parseSdkDir(sdkPath, yux);
            }
        } else {
            parseSdkDir(sdkPath, yux);
        }
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

    // ====== lib 模式：递归扫描 src/ + 静态库 ======
    if (yux.isLibProject()) {
        namespace fs = std::filesystem;
        fs::path srcDir(yux.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: lib project missing `src/` directory at " << srcDir.string() << std::endl;
            return 1;
        }
        // 递归扫 src/ 下 .yux；模块名 = src 下相对路径，点分（不加项目名前缀）
        vector<std::pair<std::string, std::string>> libFiles; // {abs, modName}
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(srcDir, walkEc);
             it != fs::recursive_directory_iterator(); ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            auto& p = it->path();
            if (p.extension() != ".yux") continue;
            auto rel = fs::relative(p, srcDir);
            string modName = rel.generic_string();
            modName = modName.substr(0, modName.size() - 4); // strip .yux
            for (auto& c : modName) if (c == '/' || c == '\\') c = '.';
            libFiles.emplace_back(fs::absolute(p).string(), modName);
        }
        std::sort(libFiles.begin(), libFiles.end());

        // 加载所有 AST
        for (auto& [abs, mn] : libFiles) {
            try {
                yux.loadMainFile(abs, mn);
            } catch (runtime_error& e) {
                string msg = e.what();
                if (auto* yuxErr = dynamic_cast<YuxError*>(&e)) {
                    int line = yuxErr->getLineNumber();
                    if (line > 0) msg = "line " + to_string(line) + ": " + msg;
                }
                std::cerr << mn << ": " << msg << std::endl;
                return 1;
            }
        }

        // 各模块 codegen
        vector<std::string> libObjs;
        for (auto& [abs, mn] : libFiles) {
            auto file = yux.module(mn);
            if (!file) continue;
            string base = moduleOutputBase(buildDir, projectName, mn);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = base + ".ll";
            if (BuildCache::needRecompile(obj, abs)) {
                if (!codegenTo(file, mn, obj, ir)) return 1;
                BuildCache::updateCache(obj, abs);
                compiled = true;
            }
            libObjs.push_back(obj);
        }

        // 链接为静态库
        string libPath = projectBuildDir + "/" + projectName + ".lib";
        bool needLib = !fs::exists(libPath);
        if (!needLib) {
            try {
                auto t = fs::last_write_time(libPath);
                for (auto& o : libObjs) {
                    if (fs::last_write_time(o) > t) { needLib = true; break; }
                }
            } catch (...) { needLib = true; }
        }
        if (needLib) {
            string outArg = "/out:" + libPath;
            vector<const char*> args = {"lld-link", "/lib", outArg.c_str()};
            for (auto& o : libObjs) args.push_back(o.c_str());

            std::string outStr, errStr;
            llvm::raw_string_ostream oOS(outStr), eOS(errStr);
            std::cout << "Static lib: " << libPath << std::endl;
            lld::DriverDef dd = {lld::WinLink, &lld::coff::link};
            lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
            if (r.retCode) {
                llvm::errs() << errStr;
                return 1;
            }
            compiled = true;
        }

        if (!compiled) std::cout << "no work to do." << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(0);
    }

    // ====== exe 模式：原流程 ======
    std::string baseName = llvm::sys::path::stem(inputFile).str();
    std::string objPath = projectBuildDir + "/" + baseName + ".obj";

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

    // 主模块
    bool needCompile = BuildCache::needRecompile(objPath, inputFile);
    if (needCompile) {
        std::string irPath = projectBuildDir + "/" + baseName + ".ll";
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
        std::string modBase = moduleOutputBase(buildDir, projectName, modName);
        std::filesystem::create_directories(std::filesystem::path(modBase).parent_path());
        std::string modObj = modBase + ".obj";
        std::string modIr = modBase + ".ll";
        if (BuildCache::needRecompile(modObj, modSrc)) {
            if (!codegenTo(modFile, modName, modObj, modIr)) {
                return 1;
            }
            BuildCache::updateCache(modObj, modSrc);
            compiled = true;
        }
        modObjPaths.push_back(modObj);
    }

    // 项目模式 exe 使用 yux.toml 的 name；单文件模式用源文件 basename。
    std::string exeStem = projectMode ? projectName : baseName;
    std::string exePath = projectBuildDir + "/" + exeStem + ".exe";

    bool needLink = !std::filesystem::exists(exePath);
    if (!needLink) {
        try {
            auto exeTime = std::filesystem::last_write_time(exePath);
            if (std::filesystem::last_write_time(objPath) > exeTime) {
                needLink = true;
            }
            if (!sdkLibPath.empty() &&
                std::filesystem::last_write_time(sdkLibPath) > exeTime) {
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

        if (!sdkLibPath.empty()) {
            args.insert(args.begin() + 2, sdkLibPath.c_str());
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
