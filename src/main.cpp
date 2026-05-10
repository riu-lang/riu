// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include <windows.h>

#undef ERROR

#include "types.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>
#include <lld/Common/Driver.h>
#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Triple.h>
#include <regex>
#include <sstream>

#include "ast/ast_builder.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/yux.h"
#include "compiler/compiler.h"
#include "compiler/compiler_test_intrinsics.h"
#include "tools/build_cache.h"
#include "tools/pkg_cache.h"
#include "tools/diagnostic.h"
#include "tools/formatter.h"
#include "tools/syntax_error_listener.h"
#include "utf8.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include "CLI/CLI.hpp"
#include <toml.hpp>

LLD_HAS_DRIVER(coff)

LLD_HAS_DRIVER(elf)

LLD_HAS_DRIVER(macho)

LLD_HAS_DRIVER(wasm)

using namespace yux;

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

// ==================== Win64 SEH 修复: JIT 段 .pdata 注册 ====================
//
// 默认 RTDyldMemoryManager::registerEHFramesInProcess 只调用 __register_frame
// (libgcc DWARF unwind) 不调 RtlAddFunctionTable, 所以 RuntimeDyldCOFFX86_64
// 收集到的 .pdata 段从来没有真正注册到 OS。结果是 JIT 函数没有 SEH unwind
// info, RtlVirtualUnwind 跨多个 yux 帧时 RtlLookupFunctionEntry 找不到条目,
// SEH 派发失败 → 进程静默退出 (BUGS.md "yux test JIT SEH 跨帧" 条)。
//
// 修法: 子类化 SectionMemoryManager 覆盖 registerEHFrames/deregisterEHFrames。
// .pdata 是 RUNTIME_FUNCTION (3 个 DWORD: BeginAddress / EndAddress /
// UnwindInfoAddress, 全部为相对 ImageBase 的 RVA) 的紧凑数组, 直接交给
// RtlAddFunctionTable。ImageBase 取本对象内已分配 section 的最低非零地址,
// 与 RuntimeDyldCOFFX86_64::getImageBase() 一致 (RTDyldObjectLinkingLayer
// 每次 emit 都会 GetMemoryManager(), 所以一个 MemMgr 实例只服务一个 obj)。
class YuxSEHMemoryManager : public llvm::SectionMemoryManager {
public:
    YuxSEHMemoryManager() = default;
    ~YuxSEHMemoryManager() override {
        for (auto* table : registeredTables) {
            ::RtlDeleteFunctionTable(table);
        }
    }

    uint8_t* allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID,
                                 llvm::StringRef SectionName) override {
        auto* p = SectionMemoryManager::allocateCodeSection(
            Size, Alignment, SectionID, SectionName);
        if (p) recordSection(p);
        return p;
    }

    uint8_t* allocateDataSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID,
                                 llvm::StringRef SectionName,
                                 bool IsReadOnly) override {
        auto* p = SectionMemoryManager::allocateDataSection(
            Size, Alignment, SectionID, SectionName, IsReadOnly);
        if (p) recordSection(p);
        return p;
    }

    void registerEHFrames(uint8_t* Addr, uint64_t /*LoadAddr*/,
                          size_t Size) override {
        // .pdata 段必须是 RUNTIME_FUNCTION (12 字节) 的紧凑数组
        constexpr size_t kEntrySize = sizeof(RUNTIME_FUNCTION);
        if (Size == 0 || Size % kEntrySize != 0) return;

        uint64_t imageBase = std::numeric_limits<uint64_t>::max();
        for (uint64_t a : sectionAddrs) {
            if (a != 0) imageBase = std::min(imageBase, a);
        }
        if (imageBase == std::numeric_limits<uint64_t>::max()) return;

        auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(Addr);
        DWORD count = static_cast<DWORD>(Size / kEntrySize);
        if (::RtlAddFunctionTable(table, count, imageBase)) {
            registeredTables.push_back(table);
        }
    }

    void deregisterEHFrames() override {
        for (auto* table : registeredTables) {
            ::RtlDeleteFunctionTable(table);
        }
        registeredTables.clear();
    }

private:
    std::vector<uint64_t> sectionAddrs;
    std::vector<PRUNTIME_FUNCTION> registeredTables;

    void recordSection(uint8_t* p) {
        sectionAddrs.push_back(reinterpret_cast<uint64_t>(p));
    }
};

// 给 LLJITBuilder 用: 构造一个 RTDyldObjectLinkingLayer, 每个对象使用一个
// YuxSEHMemoryManager 实例 (用于 .pdata SEH 注册)。
static llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>>
makeYuxObjectLinkingLayer(llvm::orc::ExecutionSession& ES) {
    auto layer = std::make_unique<llvm::orc::RTDyldObjectLinkingLayer>(
        ES,
        [](const llvm::MemoryBuffer&) -> std::unique_ptr<llvm::RuntimeDyld::MemoryManager> {
            return std::make_unique<YuxSEHMemoryManager>();
        });
    // 与 LLJIT 默认 COFF 路径一致 (LLJIT.cpp::createObjectLinkingLayer)
    layer->setOverrideObjectFlagsWithResponsibilityFlags(true);
    layer->setAutoClaimResponsibilityForObjectSymbols(true);
    return std::unique_ptr<llvm::orc::ObjectLayer>(std::move(layer));
}

// Phase 1 spike: build a user IR module and run via in-process LLJIT.
// 加载预编译 sdk core.obj 作为对象层符号源，再加用户 IR；用 process loader
// 兜底解析 kernel32 等动态库符号；查 mainStartup 直接调用并返回退出码。
//
// 该路径绕过 obj 写盘 + LLD 链接，单次成功用例从 ~2.1s 降到 IR 生成 + JIT 装载耗时。
// 仅供 Phase 1 验证；Phase 2 起会被 `yux test` 子命令收编。
int runViaJIT(std::unique_ptr<llvm::Module> mod,
              std::unique_ptr<llvm::LLVMContext> ctx,
              const std::vector<std::unique_ptr<llvm::Module>>& extraMods,
              std::vector<std::unique_ptr<llvm::LLVMContext>>& extraCtxs,
              const std::string& sdkObjPath) {
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jitOrErr = llvm::orc::LLJITBuilder()
        .setObjectLinkingLayerCreator(&makeYuxObjectLinkingLayer)
        .create();
    if (!jitOrErr) {
        llvm::errs() << "[jit] LLJIT create failed: "
                     << llvm::toString(jitOrErr.takeError()) << "\n";
        return 1;
    }
    auto& jit = *jitOrErr;
    auto& jd = jit->getMainJITDylib();

    // 进程内符号兜底（kernel32: HeapAlloc, GetStdHandle, WriteFile, ...）
    auto procGen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!procGen) {
        llvm::errs() << "[jit] process generator failed: "
                     << llvm::toString(procGen.takeError()) << "\n";
        return 1;
    }
    jd.addGenerator(std::move(*procGen));

    // 加载 sdk core.obj
    if (!sdkObjPath.empty() && std::filesystem::exists(sdkObjPath)) {
        auto bufOrErr = llvm::MemoryBuffer::getFile(sdkObjPath);
        if (!bufOrErr) {
            llvm::errs() << "[jit] read sdk obj failed: " << sdkObjPath << "\n";
            return 1;
        }
        if (auto e = jit->addObjectFile(std::move(*bufOrErr))) {
            llvm::errs() << "[jit] addObjectFile failed: "
                         << llvm::toString(std::move(e)) << "\n";
            return 1;
        }
    } else {
        llvm::errs() << "[jit] warning: sdk obj not found at " << sdkObjPath << "\n";
    }

    // 用户主模块
    mod->setDataLayout(jit->getDataLayout());
    llvm::orc::ThreadSafeModule mainTsm(std::move(mod), std::move(ctx));
    if (auto e = jit->addIRModule(std::move(mainTsm))) {
        llvm::errs() << "[jit] addIRModule(main) failed: "
                     << llvm::toString(std::move(e)) << "\n";
        return 1;
    }

    // 用户导入模块
    for (size_t i = 0; i < extraMods.size(); ++i) {
        auto& m = const_cast<std::unique_ptr<llvm::Module>&>(extraMods[i]);
        if (!m) continue;
        m->setDataLayout(jit->getDataLayout());
        llvm::orc::ThreadSafeModule tsm(std::move(m), std::move(extraCtxs[i]));
        if (auto e = jit->addIRModule(std::move(tsm))) {
            llvm::errs() << "[jit] addIRModule(extra) failed: "
                         << llvm::toString(std::move(e)) << "\n";
            return 1;
        }
    }

    auto sym = jit->lookup("mainStartup");
    if (!sym) {
        llvm::errs() << "[jit] lookup mainStartup failed: "
                     << llvm::toString(sym.takeError()) << "\n";
        return 1;
    }
    auto fn = sym->toPtr<int (*)()>();
    return fn();
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

// 把 runtime_error 渲染成统一格式的诊断到 stderr。
// sourcePath 提供文件名（可空），用于源码片段查找与错误头打印。
// prefix 为可选前缀（如 "Error in SDK file " + path + ": "），写在诊断头之前。
// 对非 YuxError 异常仅打印 prefix + msg。
void reportRuntimeError(const string& sourcePath, const runtime_error& e, const string& prefix = "") {
    if (auto* yuxErr = dynamic_cast<const YuxError*>(&e)) {
        if (!prefix.empty()) std::cerr << prefix;
        DiagnosticEngine::renderYuxError(std::cerr, sourcePath, *yuxErr);
    } else {
        std::cerr << prefix << e.what() << std::endl;
    }
}

void parseAST(string inputFile, Yux& yux, bool isSdk = false) {
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
    } catch (runtime_error& e) {
        reportRuntimeError(inputFile, e);
        exit(1);
    }
}

IRResult compileIR(string inputFile, Yux& yux, bool isSdk = false) {
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

    std::cout << "Compile IR... (module: " << moduleName << ")" << std::endl;
    auto context = make_unique<llvm::LLVMContext>();
    auto module = make_unique<llvm::Module>(moduleName, *context);

    llvm::IRBuilder<> builder(*context);

    ASTBuilder astBuilder(yux, moduleName, isSdk);

    try {
        auto ast = astBuilder.build(program);
        Compiler compiler(*context, builder, module.get(), ast, &yux, isSdk);
        compiler.compile(ast);
    } catch (runtime_error& e) {
        reportRuntimeError(inputFile, e);
        exit(1);
    }
    return {std::move(context), std::move(module)};
}

// SDK 构建路径（新布局，与 aeaefa6 项目模式一致）：
//   sdkRoot = <sdk-project-root>（含 yux.toml）
//   .lib    = sdkRoot/build/yux.lib                        （最终产物，去掉 <name>/ 子层）
//   .obj    = sdkRoot/build/src/yux/core.obj               （中间产物，镜像源相对项目根的路径）
//   .ll     = sdkRoot/build/src/yux/core.ll
// sdkPath 形如 .../sdk/yux/src/yux/core，向上 3 级即 sdkRoot。
struct SdkPaths {
    std::string objPath;
    std::string libPath;
    std::string irPath;
};
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

bool needRecompileSdkDir(const string& sdkDir, const string& sdkObjPath) {
    if (!std::filesystem::exists(sdkObjPath)) {
        return true;
    }
    
    auto objTime = std::filesystem::last_write_time(sdkObjPath);
    
    for (const auto& entry : std::filesystem::directory_iterator(sdkDir)) {
        if (entry.is_regular_file()) {
            string filename = entry.path().filename().string();
            if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".yux") {
                if (filename.size() >= 9 &&
                    filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
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
                if (filename.size() >= 9 &&
                    filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
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
        SyntaxErrorListener errListener(yuxFile, std::cerr);
        lexer.removeErrorListeners();
        lexer.addErrorListener(&errListener);
        antlr4::CommonTokenStream tokenStream(&lexer);
        yuxParser parser(&tokenStream);
        parser.removeErrorListeners();
        parser.addErrorListener(&errListener);
        auto program = parser.program();
        if (errListener.hasErrors() || parser.getNumberOfSyntaxErrors()) {
            std::cerr << "Syntax errors in SDK file: " << yuxFile << std::endl;
            exit(1);
        }
        ASTBuilder astBuilder(yux, "yux.core", true);
        try {
            astBuilder.build(program);
        } catch (runtime_error& e) {
            reportRuntimeError(yuxFile, e, "Error in SDK file " + yuxFile + ": ");
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
            reportRuntimeError(fs::absolute(yuxFile).string(), e,
                "Error in SDK file " + yuxFile + ": ");
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
                if (filename.size() >= 9 &&
                    filename.compare(filename.size() - 9, 9, ".test.yux") == 0) continue;
                yuxFiles.push_back(entry.path().string());
            }
        }
    }
    std::sort(yuxFiles.begin(), yuxFiles.end());

    auto reportErr = [&](const string& yuxFile, runtime_error& e) {
        reportRuntimeError(yuxFile, e, "Error in SDK file " + yuxFile + ": ");
    };

    // 第一遍：平铺文件 → 合并入 _sdkFile。
    // 先把所有平铺文件的 AST 累加进 _sdkFile，然后再做一次性 IR 编译。
    // 旧版本是「每文件 parse + compile 一次」：因为 _sdkFile 是单例，每次 compile
    // 都会把已经处理过的文件的函数再编一遍，触发 LLVM 「bad signature」断言。
    p<FileNode> sdkAst;
    for (const auto& yuxFile : yuxFiles) {
        string stem = fs::path(yuxFile).stem().string();
        auto it = pkgMap.find(stem);
        bool isFlat = (it == pkgMap.end()) || it->second.isFlat;
        if (!isFlat) continue;

        std::cout << "  Processing: " << yuxFile << std::endl;
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
            std::cerr << "Syntax errors in SDK file: " << yuxFile << std::endl;
            exit(1);
        }
        ASTBuilder astBuilder(yux, "yux.core", true);
        try {
            sdkAst = astBuilder.build(program);  // 始终返回 _sdkFile（单例）
        } catch (runtime_error& e) {
            reportErr(yuxFile, e);
            exit(1);
        }
    }

    // 平铺文件累加完成后，用合并后的 _sdkFile 一次性发射 IR（含运行时辅助）。
    if (sdkAst) {
        try {
            Compiler compiler(*context, builder, module.get(), sdkAst, &yux, true);
            compiler.compile(sdkAst);
        } catch (runtime_error& e) {
            reportErr("(sdk flat compile)", e);
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

// ==================== `yux test` 运行辅助 ====================

// SEH 包裹单次测试调用。返回 0 表示无异常；非 0 为 GetExceptionCode()。
// 必须保持 extern "C" + 无 C++ 析构对象，避免 clang 对 SEH + 局部对象的限制。
extern "C" unsigned long runTestSEH(void (*fn)()) noexcept {
    __try {
        fn();
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

// 把 Win32 SEH 异常码翻译成可读名字
static const char* sehExceptionName(unsigned long code) {
    // yux test 自定义码：测试断言失败（spec §11.3.5）
    if (code == test_intrinsics::ASSERT_FAILED_CODE) return "ASSERT_FAILED";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:          return "INT_OVERFLOW";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_OVERFLOW:          return "FLT_OVERFLOW";
        case EXCEPTION_FLT_UNDERFLOW:         return "FLT_UNDERFLOW";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_BREAKPOINT:            return "BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        default:                              return "UNKNOWN";
    }
}

// 把 stdout / stderr 的底层 fd 重定向到一个临时文件，stop() 时还原并读出内容。
// 用 tmpfile()（C 运行时，自动删除）避免 pipe 缓冲被填满后被测函数阻塞。
struct TestOutputCapture {
    int savedOut = -1;
    int savedErr = -1;
    FILE* tmp = nullptr;

    bool start() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        tmp = std::tmpfile();
        if (!tmp) return false;
        int fd = _fileno(tmp);
        savedOut = _dup(_fileno(stdout));
        savedErr = _dup(_fileno(stderr));
        if (savedOut < 0 || savedErr < 0) return false;
        if (_dup2(fd, _fileno(stdout)) < 0) return false;
        if (_dup2(fd, _fileno(stderr)) < 0) return false;
        return true;
    }

    std::string stop() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        if (savedOut >= 0) { _dup2(savedOut, _fileno(stdout)); _close(savedOut); savedOut = -1; }
        if (savedErr >= 0) { _dup2(savedErr, _fileno(stderr)); _close(savedErr); savedErr = -1; }
        std::string buf;
        if (tmp) {
            std::fseek(tmp, 0, SEEK_END);
            long sz = std::ftell(tmp);
            std::fseek(tmp, 0, SEEK_SET);
            if (sz > 0) {
                buf.resize(static_cast<size_t>(sz));
                size_t n = std::fread(buf.data(), 1, static_cast<size_t>(sz), tmp);
                buf.resize(n);
            }
            std::fclose(tmp);
            tmp = nullptr;
        }
        return buf;
    }
};

// Phase 5：取当前进程 exe 全路径（用于父进程派发子测试时的 argv[0]）
static std::string getSelfExePath() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    int sz = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    std::string out(sz, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, out.data(), sz, nullptr, nullptr);
    return out;
}

static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int sz = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), sz);
    return out;
}

// Phase 5：在 isolate=process 模式下，给单个 #Test 起子进程跑。
// 子进程协议：`<self> test <mod>#<fn> --isolate-child --capture <tmpfile>`
// 子进程把所有 stdout/stderr 写入 capture 文件；退出码 0=pass / SEH 码=fail / 2=child 自身错误。
struct IsolatedResult { unsigned long exitCode; std::string capture; bool spawnOk; std::string spawnError; };
static IsolatedResult spawnIsolatedTest(const std::string& exePath,
                                         const std::string& mod,
                                         const std::string& fn) {
    namespace fs = std::filesystem;
    static std::atomic<unsigned> seq{0};
    fs::path capPath = fs::temp_directory_path() /
        ("yuxtest_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(++seq) + ".txt");

    // CreateProcessW 接收单条 cmdline；exe 路径与 capture 路径都用引号包起来防空格。
    std::string cmd = "\"" + exePath + "\" test \"" + mod + "#" + fn +
                      "\" --isolate-child --capture \"" + capPath.string() + "\"";
    std::wstring wcmd = toWide(cmd);
    std::vector<wchar_t> cmdBuf(wcmd.begin(), wcmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        return {0, {}, false, "CreateProcess failed (GLE=" + std::to_string(GetLastError()) + ")"};
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string capContents;
    if (fs::exists(capPath)) {
        std::ifstream f(capPath, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        capContents = ss.str();
        f.close();
        std::error_code ec;
        fs::remove(capPath, ec);
    }
    return {code, std::move(capContents), true, {}};
}

// 把捕获到的输出按行缩进打印到 std::cout，便于在 RUN/FAIL 行下视觉归属
static void printCapturedOutput(const std::string& out) {
    if (out.empty()) return;
    std::cout << "  ---- output ----\n";
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) {
            std::cout << "  | " << out.substr(pos) << "\n";
            break;
        }
        std::cout << "  | " << out.substr(pos, nl - pos) << "\n";
        pos = nl + 1;
    }
    std::cout << "  ----------------\n";
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

    // [Phase 1 spike] 在进程内 JIT 跑入口模块，绕开 obj 写盘 + LLD 链接。
    // 仅单文件模式生效；正式 `yux test` 子命令会替代它。
    bool jitRun = false;
    app.add_flag("--jit-run", jitRun,
                 "[spike] Run input via in-process JIT (single-file only; skips obj/exe)");

    // 诊断严重度开关：允许在主命令和 build 子命令上都使用
    // --warn=<code>  把指定 code 视为 warning（仅对默认 sev <= Warning 的码生效；Error 码拒绝降级）
    // --allow=<code> 把指定 code 视为 note（同上规则）
    // --deny=<code>  把指定 code 视为 error
    // -Werror        把所有 warning 视为 error
    std::vector<std::string> warnCodes, allowCodes, denyCodes;
    bool werror = false;
    // 仅在根 app 注册一次；buildCmd 通过 fallthrough() 继承
    // expected(1) + allow_extra_args(false)：每次出现只吞 1 个值，不吃后续 positional
    app.add_option("--warn", warnCodes, "Treat code as warning (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
    app.add_option("--allow", allowCodes, "Treat code as note (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
    app.add_option("--deny", denyCodes, "Treat code as error (Exxxx; can repeat)")->expected(1)->allow_extra_args(false);
    app.add_flag("--Werror", werror, "Treat all warnings as errors");

#ifdef _DEBUG
    app.add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    std::string inputFile;
    app.add_option("input", inputFile, "Input .yux file (single-file mode)");

    auto* buildCmd = app.add_subcommand("build", "Build project (must run at project root containing yux.toml)");
    std::string buildNameArg;
    // name 可省略：当前每个 yux.toml 仅声明一个目标，省略时直接取 toml 的 name；显式给出则必须与之一致。
    buildCmd->add_option("name", buildNameArg, "Project name (optional; must match `name` in yux.toml when given)");
    buildCmd->add_flag("--emit-ir", emitIr, "Emit LLVM IR to .ll file");
    buildCmd->fallthrough(); // 允许 --warn / --allow / --deny / -Werror 在 build 子命令上使用

#ifdef _DEBUG
    buildCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    // `yux test [selector]` 子命令（仅项目模式；spec §11.3.4）
    // selector 形态：
    //   <prefix>             模块名前缀匹配（例：yux.core 命中 yux.core.*.test）
    //   <module>#<fnName>    精确匹配模块名 + 函数名
    auto* testCmd = app.add_subcommand("test", "Run #Test functions in *.test.yux files (project mode only)");
    std::string testSelector;
    bool testVerbose = false;
    testCmd->add_option("selector", testSelector, "Module prefix or `<module>#<fnName>` selector");
    testCmd->add_flag("-v,--verbose", testVerbose, "Print captured stdout/stderr for every test (default: only on failure)");
    // Phase 5：进程隔离开关
    std::string testIsolate = "none";
    testCmd->add_option("--isolate", testIsolate, "Isolation mode: none|process (default: none)")
           ->check(CLI::IsMember({"none", "process"}));
    bool testIsolateChild = false;
    auto* childOpt = testCmd->add_flag("--isolate-child", testIsolateChild, "(internal) child runner for --isolate=process");
    childOpt->group("");  // 隐藏
    std::string testCaptureFile;
    auto* capOpt = testCmd->add_option("--capture", testCaptureFile, "(internal) child capture file path");
    capOpt->group("");
    testCmd->fallthrough();
#ifdef _DEBUG
    testCmd->add_flag("-d,--debug", debug, "Output compilation IR debug information");
#endif

    auto* formatCmd = app.add_subcommand("format", "Format a .yux source file");
    std::string formatFile;
    formatCmd->add_option("file", formatFile, "Input .yux file to format");
    bool formatInPlace = false;
    formatCmd->add_flag("-i,--in-place", formatInPlace, "Edit file in place");
    bool formatStdin = false;
    formatCmd->add_flag("--stdin", formatStdin, "Read from stdin instead of file");
    int formatLineWidth = 0;  // 0 表示使用默认值或从 yux.toml 读取
    formatCmd->add_option("--line-width", formatLineWidth, "Line width threshold (default: 120)");

    CLI11_PARSE(app, argc, argv);

    // Phase 5：子进程模式 — 在任何输出前把 stdout/stderr 重定向到 capture 文件
    bool isChildIsolated = testCmd->parsed() && testIsolateChild;
    if (isChildIsolated) {
        if (testCaptureFile.empty()) {
            std::cerr << "Error: --isolate-child requires --capture <path>" << std::endl;
            return 2;
        }
        FILE* cap = std::fopen(testCaptureFile.c_str(), "wb");
        if (!cap) {
            std::cerr << "Error: cannot open capture file: " << testCaptureFile << std::endl;
            return 2;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        int fd = _fileno(cap);
        _dup2(fd, _fileno(stdout));
        _dup2(fd, _fileno(stderr));
        // 不缓冲：避免子进程异常退出时父进程读到截断输出
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::cout.setf(std::ios::unitbuf);
        std::cerr.setf(std::ios::unitbuf);
    }

    // 把诊断 severity 开关下发到 DiagPolicy
    // applyOverride: 校验 code 已知 + 允许策略；不可降级时打印拒绝信息
    auto applyOverride = [](const std::vector<std::string>& codes, DiagSeverity newSev,
                            const char* flagName) {
        for (const auto& code : codes) {
            const auto* def = ErrorCode::lookupDefaultSeverity(code);
            if (!def) {
                std::cerr << "warning: unknown error code '" << code << "' for " << flagName
                          << " (ignored)" << std::endl;
                continue;
            }
            if (!DiagPolicy::setSeverityOverride(code, *def, newSev)) {
                // 默认 Error 的码不允许降级
                std::cerr << "warning: cannot downgrade error code '" << code
                          << "' (default severity is error); " << flagName << " ignored"
                          << std::endl;
            }
        }
    };
    applyOverride(warnCodes, DiagSeverity::Warning, "--warn");
    applyOverride(allowCodes, DiagSeverity::Note, "--allow");
    applyOverride(denyCodes, DiagSeverity::Error, "--deny");
    DiagPolicy::setWerror(werror);

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
        
        // 构建格式化配置
        yux::FormatConfig config;
        
        // 优先使用命令行参数
        if (formatLineWidth > 0) {
            config.lineWidth = static_cast<size_t>(formatLineWidth);
        } else {
            // 尝试从 yux.toml 读取配置
            namespace fs = std::filesystem;
            fs::path searchDir;
            if (!filePath.empty()) {
                searchDir = fs::path(filePath).parent_path();
            } else {
                searchDir = fs::current_path();
            }
            
            // 向上查找 yux.toml
            while (!searchDir.empty()) {
                fs::path tomlPath = searchDir / "yux.toml";
                if (fs::exists(tomlPath)) {
                    try {
                        auto data = toml::parse(tomlPath.string());
                        if (data.contains("fmt")) {
                            const auto& fmt = data.at("fmt");
                            if (fmt.is_table()) {
                                if (fmt.contains("line_width") && fmt.at("line_width").is_integer()) {
                                    config.lineWidth = static_cast<size_t>(fmt.at("line_width").as_integer());
                                }
                            }
                        }
                        break;
                    } catch (const std::exception& e) {
                        // 解析失败，使用默认配置
                    }
                }
                if (searchDir.has_parent_path()) {
                    searchDir = searchDir.parent_path();
                } else {
                    break;
                }
            }
        }
        
        try {
            yux::Formatter formatter(source, config);
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

    if (!isChildIsolated) {
        std::cout << "Working at: " << std::filesystem::absolute(std::filesystem::current_path()).string() << std::endl;
    }

    // ==================== `yux test` 子命令 ====================
    // Phase 2 实现：仅项目模式；递归扫描 src/ 下 *.yux + *.test.yux；codegen 全部模块后
    // 走 LLJIT，按 selector 过滤 #Test 函数逐个 lookup 调用。
    // Phase 3：每个测试用 Windows SEH __try/__except 包裹，AV/除零/栈溢出等硬件异常
    // 单条失败不再终止整个 suite；同时把每个测试的 stdout/stderr 重定向到临时文件，
    // 默认隐藏成功测试的输出，失败时回放（--verbose 时全部回放）。
    if (testCmd->parsed()) {
        namespace fs = std::filesystem;
        if (!inputFile.empty()) {
            std::cerr << "Error: `yux test` does not accept positional input file" << std::endl;
            return 1;
        }
        // 解析 selector：形如 `<prefix>` 或 `<module>#<fnName>`
        std::string selModule, selFn;
        if (!testSelector.empty()) {
            auto hash = testSelector.find('#');
            if (hash == std::string::npos) {
                selModule = testSelector;
            } else {
                selModule = testSelector.substr(0, hash);
                selFn = testSelector.substr(hash + 1);
            }
        }

        Yux yux;
        std::string cwd = fs::current_path().string();
        try {
            yux.initProjectFromDir(cwd);
        } catch (runtime_error& e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }

        // SDK：与 build 路径共享。需要 sdk obj 给 JIT 加载；如不存在则现编。
        // SDK self-project（yux.toml name="yux"）特殊处理：
        // findSdkPath() 返回 build 目录下的 SDK 拷贝，与项目源里的原文件不在同一路径，
        // 这会导致递归扫描误把原 SDK 文件当成用户文件再加载一遍 → 符号重复。
        // 用项目源里的 SDK 路径覆盖，让 sdkPathAbs 与递归扫描看到的 parent 一致。
        std::string sdkPath;
        if (yux.projectName() == "yux") {
            fs::path candidate = fs::path(yux.sourceRoot()) / "yux" / "core";
            if (fs::is_directory(candidate)) {
                sdkPath = candidate.string();
            }
        } else {
            sdkPath = findSdkPath();
        }
        std::string sdkObjPath;
        if (!sdkPath.empty()) {
            sdkPath = fs::absolute(sdkPath).string();
            sdkObjPath = sdkBuildPaths(sdkPath).objPath;

            bool needCompile = !fs::exists(sdkObjPath) || needRecompileSdkDir(sdkPath, sdkObjPath);
            if (needCompile) {
                SdkLock sdkLock;
                sdkLock.tryLock();
                needCompile = !fs::exists(sdkObjPath) || needRecompileSdkDir(sdkPath, sdkObjPath);
                if (needCompile) {
                    auto sdkIrr = compileSdkDir(sdkPath, yux);
                    if (!compileIRToObj(sdkIrr.module.get(), sdkObjPath)) {
                        std::cerr << "Failed to compile SDK to object file" << std::endl;
                        return 1;
                    }
                    std::cout << "Write SDK obj: " << sdkObjPath << std::endl;
                } else {
                    parseSdkDir(sdkPath, yux);
                }
            } else {
                parseSdkDir(sdkPath, yux);
            }
        }

        // 递归扫 src/ 下所有 .yux（含 .test.yux）
        fs::path srcDir(yux.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: project missing `src/` directory at " << srcDir.string() << std::endl;
            return 1;
        }
        struct LoadEntry { std::string abs; std::string mod; bool isTest; };
        std::vector<LoadEntry> entries;
        std::string sdkPathAbs = sdkPath.empty() ? std::string() : fs::absolute(sdkPath).string();
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(srcDir, walkEc);
             it != fs::recursive_directory_iterator(); ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            const auto& p = it->path();
            if (p.extension() != ".yux") continue;
            auto fname = p.filename().string();
            bool isTest = fname.size() >= 9 &&
                          fname.compare(fname.size() - 9, 9, ".test.yux") == 0;
            std::string absPath = fs::absolute(p).string();
            if (!isTest && !sdkPathAbs.empty() &&
                fs::path(absPath).parent_path().string() == sdkPathAbs) {
                continue;  // SDK preload 已处理 sdk 目录下非 test 文件
            }
            auto rel = fs::relative(p, srcDir);
            std::string modName = rel.generic_string();
            // strip ".yux"（保留 ".test" 段，例如 "yux/core/arithmetic.test.yux" → "yux.core.arithmetic.test"）
            modName = modName.substr(0, modName.size() - 4);
            for (auto& c : modName) if (c == '/' || c == '\\') c = '.';
            entries.push_back({absPath, modName, isTest});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const LoadEntry& a, const LoadEntry& b) { return a.mod < b.mod; });

        // 加载所有 AST。若模块名已加载（被 SDK 抢先），跳过避免冲突。
        // 自维护加载顺序：Yux::loadMainFile 不写 _loadOrder。
        std::vector<std::string> loadedMods;
        for (auto& e : entries) {
            if (yux.module(e.mod)) continue;
            try {
                yux.loadMainFile(e.abs, e.mod);
                loadedMods.push_back(e.mod);
            } catch (runtime_error& re) {
                reportRuntimeError(e.abs, re, e.mod + ": ");
                return 1;
            }
        }

        // codegen 每个加载的用户模块为独立 LLVM Module
        std::vector<std::unique_ptr<llvm::Module>> mods;
        std::vector<std::unique_ptr<llvm::LLVMContext>> ctxs;
        // 测试函数收集表：(modName, fnName, mangledSymbol)
        struct TestEntry { std::string mod; std::string fn; std::string sym; };
        std::vector<TestEntry> tests;
        for (auto& modName : loadedMods) {
            auto file = yux.module(modName);
            if (!file || file == yux.sdkFile()) continue;

            auto ctx = std::make_unique<llvm::LLVMContext>();
            auto mod = std::make_unique<llvm::Module>(modName, *ctx);
            llvm::IRBuilder<> builder(*ctx);
            try {
                Compiler compiler(*ctx, builder, mod.get(), file, &yux, false);
                compiler.compile(file);
            } catch (runtime_error& re) {
                std::string mp = yux.modulePath(modName);
                reportRuntimeError(mp, re, modName + ": ");
                return 1;
            }

            // 收集本模块内的 #Test 函数（仅顶层 fn；方法 v1 暂不收集）
            for (auto& fn : file->getFunctions()) {
                if (!fn->header()->hasAnno("Test")) continue;
                std::string fnName = fn->header()->name().getText();
                // mangler: function(module, name, params=[], isPrivate=false) → "mod_name()"
                std::string sym = Mangler::function(modName, fnName, {}, false);
                tests.push_back({modName, fnName, sym});
            }

            mods.push_back(std::move(mod));
            ctxs.push_back(std::move(ctx));
        }

        // selector 过滤
        auto matchesPrefix = [&](const std::string& m) {
            if (selModule.empty()) return true;
            if (m == selModule) return true;
            return m.size() > selModule.size() + 1 &&
                   m.compare(0, selModule.size(), selModule) == 0 &&
                   m[selModule.size()] == '.';
        };
        std::vector<TestEntry> filtered;
        for (auto& t : tests) {
            if (!matchesPrefix(t.mod)) continue;
            if (!selFn.empty() && t.fn != selFn) continue;
            filtered.push_back(t);
        }

        if (filtered.empty()) {
            if (!isChildIsolated) {
                std::cout << "no tests matched";
                if (!testSelector.empty()) std::cout << " selector `" << testSelector << "`";
                std::cout << std::endl;
                std::cout.flush();
                std::cerr.flush();
            } else {
                std::cerr << "child: selector `" << testSelector << "` matched no test\n";
            }
            _exit(isChildIsolated ? 2 : 0);
        }

        // Phase 5：子进程模式必须命中且仅命中一个测试（父进程派发时用 `<mod>#<fn>` 形式）
        if (isChildIsolated && filtered.size() != 1) {
            std::cerr << "child: --isolate-child expects exactly one test, got "
                      << filtered.size() << "\n";
            _exit(2);
        }

        // Phase 5：父进程在 isolate=process 模式下走子进程派发路径，跳过本进程 JIT。
        bool useProcessIsolation = !isChildIsolated && testIsolate == "process";

        if (useProcessIsolation) {
            std::string self = getSelfExePath();
            if (self.empty()) {
                std::cerr << "[test] failed to resolve self exe path\n";
                return 1;
            }
            size_t passed = 0, failed = 0;
            for (auto& t : filtered) {
                std::cout << "RUN  " << t.mod << "#" << t.fn << std::endl;
                std::cout.flush();
                auto r = spawnIsolatedTest(self, t.mod, t.fn);
                if (!r.spawnOk) {
                    std::cout << "FAIL " << t.mod << "#" << t.fn << " (" << r.spawnError << ")\n";
                    ++failed;
                    continue;
                }
                if (r.exitCode == 0) {
                    std::cout << "OK   " << t.mod << "#" << t.fn << std::endl;
                    if (testVerbose) printCapturedOutput(r.capture);
                    ++passed;
                } else if (r.exitCode == 2) {
                    std::cout << "FAIL " << t.mod << "#" << t.fn << " (child runner error)\n";
                    printCapturedOutput(r.capture);
                    ++failed;
                } else {
                    std::cout << "FAIL " << t.mod << "#" << t.fn
                              << " (SEH " << sehExceptionName(r.exitCode)
                              << " 0x" << std::hex << r.exitCode << std::dec << ")\n";
                    printCapturedOutput(r.capture);
                    ++failed;
                }
            }
            std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
            std::cout.flush();
            std::cerr.flush();
            _exit(failed == 0 ? 0 : 1);
        }

        // 启动 LLJIT，加载 sdk obj + 所有用户模块 IR
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();

        auto jitOrErr = llvm::orc::LLJITBuilder()
            .setObjectLinkingLayerCreator(&makeYuxObjectLinkingLayer)
            .create();
        if (!jitOrErr) {
            llvm::errs() << "[test] LLJIT create failed: "
                         << llvm::toString(jitOrErr.takeError()) << "\n";
            return 1;
        }
        auto& jit = *jitOrErr;
        auto& jd = jit->getMainJITDylib();

        auto procGen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (!procGen) {
            llvm::errs() << "[test] process generator failed: "
                         << llvm::toString(procGen.takeError()) << "\n";
            return 1;
        }
        jd.addGenerator(std::move(*procGen));

        if (!sdkObjPath.empty() && fs::exists(sdkObjPath)) {
            auto bufOrErr = llvm::MemoryBuffer::getFile(sdkObjPath);
            if (!bufOrErr) {
                llvm::errs() << "[test] read sdk obj failed: " << sdkObjPath << "\n";
                return 1;
            }
            if (auto e = jit->addObjectFile(std::move(*bufOrErr))) {
                llvm::errs() << "[test] addObjectFile(sdk) failed: "
                             << llvm::toString(std::move(e)) << "\n";
                return 1;
            }
        }
        for (size_t i = 0; i < mods.size(); ++i) {
            mods[i]->setDataLayout(jit->getDataLayout());
            llvm::orc::ThreadSafeModule tsm(std::move(mods[i]), std::move(ctxs[i]));
            if (auto e = jit->addIRModule(std::move(tsm))) {
                llvm::errs() << "[test] addIRModule failed: "
                             << llvm::toString(std::move(e)) << "\n";
                return 1;
            }
        }

        // 顺序执行：每个测试用 SEH 包裹 + 输出捕获，崩溃单条失败不再终止 suite
        // Phase 5：子进程模式（isChildIsolated）下不打 RUN/OK/FAIL/summary，
        // 退出码 = SEH 码（0=pass，ASSERT_FAILED/AV/... 透传给父进程翻译）。
        // 子进程模式也不再用 TestOutputCapture（stdout/stderr 已在入口被重定向到 capture 文件）。
        size_t passed = 0, failed = 0;
        unsigned long childExitCode = 0;
        for (auto& t : filtered) {
            if (!isChildIsolated) {
                std::cout << "RUN  " << t.mod << "#" << t.fn << std::endl;
                std::cout.flush();
            }
            auto sym = jit->lookup(t.sym);
            if (!sym) {
                if (isChildIsolated) {
                    std::cerr << "child: lookup failed: " << llvm::toString(sym.takeError()) << "\n";
                    childExitCode = 2;
                } else {
                    std::cout << "FAIL " << t.mod << "#" << t.fn
                              << " (lookup failed: " << llvm::toString(sym.takeError()) << ")"
                              << std::endl;
                    ++failed;
                }
                continue;
            }
            auto fn = sym->toPtr<void (*)()>();

            unsigned long code;
            if (isChildIsolated) {
                code = runTestSEH(fn);
                childExitCode = code;
            } else {
                TestOutputCapture cap;
                bool capOk = cap.start();
                code = runTestSEH(fn);
                std::string out = capOk ? cap.stop() : std::string();

                if (code == 0) {
                    std::cout << "OK   " << t.mod << "#" << t.fn << std::endl;
                    if (testVerbose) printCapturedOutput(out);
                    ++passed;
                } else {
                    std::cout << "FAIL " << t.mod << "#" << t.fn
                              << " (SEH " << sehExceptionName(code)
                              << " 0x" << std::hex << code << std::dec << ")"
                              << std::endl;
                    printCapturedOutput(out);
                    ++failed;
                }
            }
        }
        if (isChildIsolated) {
            std::fflush(stdout);
            std::fflush(stderr);
            _exit(static_cast<int>(childExitCode));
        }
        std::cout << "\n" << passed << " passed, " << failed << " failed" << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(failed == 0 ? 0 : 1);
    }

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
        if (!buildNameArg.empty() && yux.projectName() != buildNameArg) {
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
        // 项目模式不再用 <projectName>/ 子层隔离；exe / lib 直接落在 build/ 下，
        // 单文件 obj 镜像 src 相对路径到 build/<rel>.obj。
        projectBuildDir = buildDir;
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

    // 单文件模式：obj/ir 写到 pid 隔离的 tmp 目录，避免多进程同时编译同一被 use 的模块时
    // 互相覆盖中间产物。exe 仍落在 projectBuildDir。链接成功后清理。
    string intermediateDir = projectBuildDir;
    string intermediateRoot;
    bool cleanupIntermediate = false;
    if (!projectMode) {
        intermediateRoot = buildDir + "/.tmp";
        intermediateDir = intermediateRoot + "/yux-" + std::to_string(GetCurrentProcessId());
        std::error_code _ec;
        std::filesystem::remove_all(intermediateDir, _ec); // 防御性：清理同 pid 残留
        ensureBuildDir(intermediateDir);
        cleanupIntermediate = true;
    }
    auto cleanupTmp = [&]() {
        if (!cleanupIntermediate) return;
        std::error_code _ec;
        std::filesystem::remove_all(intermediateDir, _ec);
        // 若 .tmp 已空，顺手删掉
        std::filesystem::remove(intermediateRoot, _ec);
    };

    // SDK 自构建（cd sdk/yux && yux build [yux]）：sdkPath 必须指向项目源里的 SDK，
    // 否则会与 findSdkPath() 返回的安装拷贝走两条路径，最终把同一批文件编译两次。
    string sdkPath;
    {
        namespace fs = std::filesystem;
        if (projectMode && yux.projectName() == "yux") {
            fs::path candidate = fs::path(yux.projectRoot()) / "src" / "yux" / "core";
            if (fs::is_directory(candidate)) {
                sdkPath = candidate.string();
            }
        }
        if (sdkPath.empty()) sdkPath = findSdkPath();
    }
    // SDK 静态库路径：放在 sdk 目录下的 build 中（不放用户项目）
    string sdkLibPath;
    bool compiled = false;

    if (!sdkPath.empty()) {
        namespace fs = std::filesystem;
        sdkPath = fs::absolute(sdkPath).string();
        SdkPaths sp = sdkBuildPaths(sdkPath);
        string sdkObjPath = sp.objPath;
        sdkLibPath = sp.libPath;

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
                    string sdkIrPath = sp.irPath;
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

    // SDK 自构建：上面 compileSdkDir → core.obj → yux.lib 的产物已经就是项目目标 lib，
    // 路径与 lib 模式下 `<projectRoot>/build/yux/yux.lib` 一致。再走 lib 走法会把同一批
    // 源文件以 isSdk=false 重新编译一次（且会与已注册到 _sdkFile 的模块名冲突），
    // 因此这里直接收尾退出。
    if (projectMode && yux.projectName() == "yux") {
        if (!compiled) std::cout << "no work to do." << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(0);
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
            // 通过模块名查回源文件路径（Yux::modulePath 维护映射）
            string srcPath = yux.modulePath(moduleName);
            reportRuntimeError(srcPath, e);
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
            // 跳过 *.test.yux —— 测试文件仅由 `yux test` 子命令处理（spec §11.3.3.2）
            {
                auto fname = p.filename().string();
                if (fname.size() >= 9 && fname.compare(fname.size() - 9, 9, ".test.yux") == 0) {
                    continue;
                }
            }
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
                reportRuntimeError(abs, e, mn + ": ");
                return 1;
            }
        }

        // 各模块 codegen — 文件级聚合：单个文件失败不立即退出，继续编译其余文件，最终再决定是否链接
        vector<std::string> libObjs;
        bool anyCodegenError = false;
        PkgCacheRegistry libCaches(yux.projectRoot(), buildDir);
        for (auto& [abs, mn] : libFiles) {
            auto file = yux.module(mn);
            if (!file) continue;
            string base = mirroredOutputBase(yux.projectRoot(), buildDir, abs);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = base + ".ll";
            if (!libCaches.isFresh(abs, obj)) {
                if (!codegenTo(file, mn, obj, ir)) {
                    anyCodegenError = true;
                    continue; // 跳过 cache 更新与 obj 收集；继续下一个模块
                }
                libCaches.mark(abs);
                compiled = true;
            }
            libObjs.push_back(obj);
        }
        libCaches.flushAll();
        if (anyCodegenError) {
            return 1;
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
    // 项目模式：obj 镜像 src 相对路径到 build/<rel>.obj；
    // 单文件模式：仍走 intermediateDir（pid 隔离的 .tmp，已跳过缓存）。
    std::string objPath = projectMode
        ? mirroredOutputBase(yux.projectRoot(), buildDir, std::filesystem::absolute(inputFile).string()) + ".obj"
        : intermediateDir + "/" + baseName + ".obj";
    if (projectMode) {
        std::filesystem::create_directories(std::filesystem::path(objPath).parent_path());
    }

    p<FileNode> mainFile = nullptr;
    try {
        mainFile = yux.loadMainFile(inputFile, baseName);
    } catch (runtime_error& e) {
        reportRuntimeError(inputFile, e);
        return 1;
    }

    // [Phase 1 spike] --jit-run：把主模块 + 用户导入模块 IR 直接送入 LLJIT 执行。
    // 不写 obj、不调 LLD；sdk 通过预编译的 core.obj 装载。
    if (jitRun) {
        if (projectMode) {
            std::cerr << "Error: --jit-run only supports single-file mode in Phase 1 spike\n";
            return 1;
        }
        auto buildIR = [&](p<FileNode> file, const std::string& moduleName)
            -> std::pair<std::unique_ptr<llvm::Module>, std::unique_ptr<llvm::LLVMContext>> {
            auto ctx = std::make_unique<llvm::LLVMContext>();
            auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
            llvm::IRBuilder<> builder(*ctx);
            Compiler compiler(*ctx, builder, mod.get(), file, &yux, false);
            compiler.compile(file);
            return {std::move(mod), std::move(ctx)};
        };

        std::unique_ptr<llvm::Module> mainMod;
        std::unique_ptr<llvm::LLVMContext> mainCtx;
        try {
            auto pr = buildIR(mainFile, baseName);
            mainMod = std::move(pr.first);
            mainCtx = std::move(pr.second);
        } catch (runtime_error& e) {
            reportRuntimeError(inputFile, e);
            return 1;
        }

        std::vector<std::unique_ptr<llvm::Module>> extraMods;
        std::vector<std::unique_ptr<llvm::LLVMContext>> extraCtxs;
        for (auto& modName : yux.loadOrder()) {
            auto modFile = yux.module(modName);
            if (!modFile || modFile == yux.sdkFile()) continue;
            try {
                auto pr = buildIR(modFile, modName);
                extraMods.push_back(std::move(pr.first));
                extraCtxs.push_back(std::move(pr.second));
            } catch (runtime_error& e) {
                std::string mp = yux.modulePath(modName);
                reportRuntimeError(mp, e, modName + ": ");
                return 1;
            }
        }

        std::string sdkObjPath;
        if (!sdkPath.empty()) {
            sdkObjPath = sdkBuildPaths(sdkPath).objPath;
        }

        int rc = runViaJIT(std::move(mainMod), std::move(mainCtx),
                           extraMods, extraCtxs, sdkObjPath);
        std::cout << "[jit-run] exit code = " << rc << std::endl;
        std::cout.flush();
        std::cerr.flush();
        _exit(rc);
    }

    // 文件级聚合：主模块与各导入模块逐个 codegen，单文件失败不立即退出，继续编译其余文件
    bool anyCodegenError = false;

    // 项目模式：包级缓存（每目录一份 <dirname>.cache，含编译器指纹）。
    // 单文件模式：不使用缓存（中间产物在 pid tmp 目录，每次重编）。
    PkgCacheRegistry exeCaches(yux.projectRoot(), buildDir);

    // 主模块。
    std::string mainAbs = std::filesystem::absolute(inputFile).string();
    bool needCompile = !projectMode || !exeCaches.isFresh(mainAbs, objPath);
    if (needCompile) {
        std::string irPath = projectMode
            ? mirroredOutputBase(yux.projectRoot(), buildDir, mainAbs) + ".ll"
            : intermediateDir + "/" + baseName + ".ll";
        if (!codegenTo(mainFile, baseName, objPath, irPath)) {
            anyCodegenError = true;
        } else {
            if (projectMode) exeCaches.mark(mainAbs);
            compiled = true;
        }
    }

    // 导入的用户模块
    std::vector<std::string> modObjPaths;
    for (auto& modName : yux.loadOrder()) {
        auto modFile = yux.module(modName);
        if (!modFile || modFile == yux.sdkFile()) continue;
        std::string modSrc = yux.modulePath(modName);
        std::string modBase = projectMode
            ? mirroredOutputBase(yux.projectRoot(), buildDir, modSrc)
            : moduleOutputBase(intermediateDir, projectName, modName);
        std::filesystem::create_directories(std::filesystem::path(modBase).parent_path());
        std::string modObj = modBase + ".obj";
        std::string modIr = modBase + ".ll";
        if (!projectMode || !exeCaches.isFresh(modSrc, modObj)) {
            if (!codegenTo(modFile, modName, modObj, modIr)) {
                anyCodegenError = true;
                continue; // 继续尝试下一个模块的 codegen
            }
            if (projectMode) exeCaches.mark(modSrc);
            compiled = true;
        }
        modObjPaths.push_back(modObj);
    }
    if (projectMode) exeCaches.flushAll();

    // 任一模块（含主模块）codegen 失败：跳过链接，统一非零退出
    if (anyCodegenError) {
        cleanupTmp();
        std::cout.flush();
        std::cerr.flush();
        return 1;
    }

    // 项目模式 exe 使用 yux.toml 的 name；单文件模式用源文件 basename。
    std::string exeStem = projectMode ? projectName : baseName;
    std::string exePath = projectBuildDir + "/" + exeStem + ".exe";

    bool needLink = !projectMode || !std::filesystem::exists(exePath);
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
            cleanupTmp();
            return 1;
        }
        compiled = true;
    }

    if (!compiled) {
        std::cout << "no work to do." << std::endl;
    }

    cleanupTmp();
    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}
