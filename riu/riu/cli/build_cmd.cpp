// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// windows.h 必须先于 riu 头 (`types.h` 经由 `using namespace std`) include,
// 否则 std::byte 与 winapi byte 冲突。NOGDI 跳 wingdi.h 的 ERROR 宏。
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <windows.h>
#undef ERROR
#endif

#include "build_cmd.h"

#include "process.h"
#include "sdk_compile.h"

#include "ast/node/expr_node.h"
#include "ast/node/fn_node.h"
#include "ast/riu.h"
#include "compiler/compiler.h"
#include "tools/diagnostic.h"
#include "tools/pkg_cache.h"
#include "tools/sdk_loader.h"
#include "types.h"

#include <lld/Common/Driver.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

LLD_HAS_DRIVER(coff)

namespace riu::cli {

// `riu.core.array.test` → `riu.core.array`（给 --test-mod）
static std::string stripTestSuffix(const std::string& mod) {
    if (mod.size() > 5 && mod.substr(mod.size() - 5) == ".test") {
        return mod.substr(0, mod.size() - 5);
    }
    return mod;
}

static std::string joinUnder(const std::string& dir, const std::string& stem, const std::string& suffix) {
    std::string p = dir;
    p += '/';
    p += stem;
    p += suffix;
    return p;
}

static std::string testDllFileStem(const std::string& testMod) {
    std::string dllFileName = testMod;
    for (auto& c : dllFileName)
        if (c == '.') c = '_';
    return dllFileName;
}

// 产物 / SDK 的 external_links → lld-link。storage 必须在 lldMain 返回前存活。
static void appendExternalLinkArgs(const std::vector<ExternalLink>& links, const std::string& projectRoot,
                                   std::vector<std::string>& storage, std::vector<const char*>& args) {
    namespace fs = std::filesystem;
    const size_t begin = storage.size();
    for (const auto& l : links) {
        if (l.path.starts_with("//")) {
            storage.push_back(l.path.substr(2) + ".lib");
        } else {
            fs::path stem = fs::path(projectRoot) / l.path.substr(2);
            fs::path lib = stem;
            lib += ".lib";
            storage.push_back(lib.lexically_normal().string());
        }
    }
    for (size_t i = begin; i < storage.size(); ++i) {
        args.push_back(storage[i].c_str());
    }
}

static int copyRuntimeDlls(const std::vector<ExternalLink>& links, const std::string& projectRoot,
                           const std::filesystem::path& outDir) {
    namespace fs = std::filesystem;
    for (const auto& l : links) {
        if (l.dll.empty() || l.dll.starts_with("//")) continue;
        fs::path stem = fs::path(projectRoot) / l.dll.substr(2);
        fs::path src = stem;
        src += ".dll";
        std::error_code ec;
        if (!fs::exists(src, ec)) {
            std::cerr << "Error: runtime dll not found: " << src.string() << '\n';
            return 1;
        }
        fs::path dest = outDir / (stem.filename().string() + ".dll");
        if (fs::exists(dest, ec) && fs::equivalent(src, dest, ec)) continue;
        fs::copy_file(src, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            std::cerr << "Error: failed to copy " << src.string() << " -> " << dest.string() << ": " << ec.message()
                      << '\n';
            return 1;
        }
    }
    return 0;
}

static std::vector<ExternalLink> loadLibraryLinks(const std::string& pkgRoot) {
    namespace fs = std::filesystem;
    if (pkgRoot.empty()) return {};
    fs::path toml = fs::path(pkgRoot) / "riu.toml";
    if (!fs::exists(toml)) return {};
    try {
        auto cfg = parseRiuToml(toml.string());
        if (cfg.library) return cfg.library->external_links;
    } catch (const std::exception&) {
        return {};
    }
    return {};
}

static bool libNewerThanSources(const std::string& libPath, const std::vector<LibModFile>& files) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (files.empty() || !fs::exists(libPath, ec)) return false;
    try {
        auto t = fs::last_write_time(libPath);
        for (const auto& f : files) {
            if (!fs::exists(f.absPath) || fs::last_write_time(f.absPath) > t) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

static bool checkEntryPath(Riu& riu, const std::string& entry, std::string& inputFile) {
    namespace fs = std::filesystem;
    string tomlPath = (fs::path(riu.projectRoot()) / "riu.toml").string();
    try {
        fs::path entryPath(entry);
        if (entryPath.is_absolute()) {
            throw RiuError(1, ErrorCode::E5013, entry);
        }
        inputFile = (fs::path(riu.sourceRoot()) / entry).string();
        if (!fs::exists(inputFile)) {
            std::cerr << "Error: entry file not found: " << inputFile << '\n';
            return false;
        }
        inputFile = fs::absolute(inputFile).string();
        std::error_code _cec;
        auto canonEntry = fs::canonical(inputFile, _cec);
        auto canonRoot = fs::canonical(riu.sourceRoot(), _cec);
        if (!_cec) {
            auto rel = fs::relative(canonEntry, canonRoot, _cec);
            bool escapes = _cec || rel.empty() || rel.native().starts_with(L"..") || rel.string().starts_with("..");
            if (escapes) {
                DiagnosticEngine::emit(tomlPath, RiuError(1, ErrorCode::E5014, entry));
            }
        }
    } catch (runtime_error& e) {
        reportRuntimeError(tomlPath, e);
        return false;
    }
    return true;
}

// DLL 是否仍新于 test obj + 全部非 test obj + sdk/riurt（保守：任一用户模块变了就 relink）
static bool isTestDllFresh(const std::string& dllPath, const std::string& testObj,
                           const std::map<std::string, std::string>& allObjMap, const std::string& sdkLibPath,
                           const std::string& riurtLibPath, const std::vector<std::string>& extraLibPaths) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dllPath, ec) || !fs::exists(testObj, ec)) return false;
    try {
        auto dllTime = fs::last_write_time(dllPath);
        if (fs::last_write_time(testObj) > dllTime) return false;
        for (auto& [_, o] : allObjMap) {
            if (fs::exists(o) && fs::last_write_time(o) > dllTime) return false;
        }
        if (!sdkLibPath.empty() && fs::exists(sdkLibPath) && fs::last_write_time(sdkLibPath) > dllTime) {
            return false;
        }
        if (!riurtLibPath.empty() && fs::exists(riurtLibPath) && fs::last_write_time(riurtLibPath) > dllTime) {
            return false;
        }
        for (const auto& extra : extraLibPaths) {
            if (!extra.empty() && fs::exists(extra) && fs::last_write_time(extra) > dllTime) {
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

// 从本库 obj 收集已定义全局符号，写成带引号的 .def。
// 不走 .drectve /EXPORT：lld parseExport 按 `,` 切选项，会把 mangle 里的 `(i32,i32)` 拆坏。
// .def 词法把 `"..."` 当完整 identifier，逗号保留；写出 PE 导出表仍是原样 C 字符串。
static bool writeDynLibDefFile(const std::string& defPath, const std::string& libStem,
                               const std::vector<std::string>& objs) {
    std::map<std::string, bool> exports; // name → isFunction
    for (const auto& objPath : objs) {
        auto binOrErr = llvm::object::ObjectFile::createObjectFile(objPath);
        if (!binOrErr) {
            std::cerr << "Error: failed to read " << objPath << ": " << llvm::toString(binOrErr.takeError()) << '\n';
            return false;
        }
        llvm::object::ObjectFile* obj = binOrErr->getBinary();
        for (const auto& sym : obj->symbols()) {
            auto flagsOr = sym.getFlags();
            if (!flagsOr) {
                llvm::consumeError(flagsOr.takeError());
                continue;
            }
            uint32_t flags = *flagsOr;
            if (flags & llvm::object::SymbolRef::SF_Undefined) continue;
            if (flags & llvm::object::SymbolRef::SF_FormatSpecific) continue;
            if (!(flags & llvm::object::SymbolRef::SF_Global)) continue;
            auto nameOr = sym.getName();
            if (!nameOr) {
                llvm::consumeError(nameOr.takeError());
                continue;
            }
            llvm::StringRef name = *nameOr;
            if (name.empty() || name.starts_with("llvm.") || name.starts_with(".") || name.contains('"')) continue;
            bool isFn = false;
            auto typeOr = sym.getType();
            if (typeOr)
                isFn = *typeOr == llvm::object::SymbolRef::ST_Function;
            else
                llvm::consumeError(typeOr.takeError());
            auto [it, inserted] = exports.emplace(std::string(name), isFn);
            if (!inserted) it->second = it->second || isFn;
        }
    }

    std::ofstream out(defPath);
    if (!out) {
        std::cerr << "Error: cannot write " << defPath << '\n';
        return false;
    }
    out << "LIBRARY " << libStem << "\nEXPORTS\n";
    for (const auto& [name, isFn] : exports) {
        out << "    \"" << name << '"';
        if (!isFn) out << " DATA";
        out << '\n';
    }
    out.flush();
    return static_cast<bool>(out);
}

// 编一个模块到 obj（依赖库与根产物共用）。
static bool compileOneModuleToObj(Riu& riu, FileNode* file, const std::string& moduleName, const std::string& objOut,
                                  const std::string& irOut, bool emitIr, bool isSdk) {
    std::cout << "Compile IR... (module: " << moduleName << ")" << '\n';
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>(moduleName, *ctx);
    llvm::IRBuilder<> builder(*ctx);
    try {
        Compiler compiler(*ctx, builder, mod.get(), file, &riu, isSdk);
        compiler.compile(file);
    } catch (runtime_error& e) {
        string srcPath = riu.modulePath(moduleName);
        reportRuntimeError(srcPath, e);
        return false;
    }
    if (emitIr) {
        std::error_code ec;
        llvm::raw_fd_ostream irFile(irOut, ec);
        if (ec) {
            std::cerr << "Error opening IR file: " << ec.message() << '\n';
        } else {
            mod->print(irFile, nullptr);
            irFile.flush();
            std::cout << "Write IR ok: " << irOut << '\n';
        }
    }
    if (!compileIRToObj(mod.get(), objOut)) {
        std::cerr << "Failed to compile IR to object file: " << objOut << '\n';
        return false;
    }
    std::cout << "Write obj: " << objOut << '\n';
    return true;
}

static void registerLibModPaths(Riu& riu, const std::string& sourceRoot, const std::string& libMod,
                                const std::vector<LibModFile>& files) {
    namespace fs = std::filesystem;
    string rel = libMod;
    for (char& c : rel)
        if (c == '.') c = '/';
    fs::path pkgDir = fs::path(sourceRoot) / rel;
    if (fs::is_directory(pkgDir)) {
        riu.registerModulePath(fs::absolute(pkgDir).lexically_normal().string(), libMod);
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(pkgDir, ec); it != fs::recursive_directory_iterator(); ++it) {
            if (ec) break;
            if (!it->is_directory()) continue;
            auto relDir = fs::relative(it->path(), sourceRoot);
            string mod = relDir.generic_string();
            for (char& c : mod)
                if (c == '/' || c == '\\') c = '.';
            riu.registerModulePath(fs::absolute(it->path()).lexically_normal().string(), mod);
        }
    }
    for (const auto& f : files) {
        riu.registerModulePath(f.absPath, f.moduleName);
    }
}

// 已加载的 `[library]` 源 → `outputRoot/<library.name>.lib`。镜像相对 `mirrorRoot`。
static bool compileLibraryToOutput(Riu& riu, const std::vector<LibModFile>& files, const std::string& mirrorRoot,
                                   const std::string& outputRoot, const Library& lib, bool emitIr,
                                   const std::string& irDir, const std::string& sdkLibPath,
                                   const std::string& riurtLibPath, const std::vector<ExternalLink>& sdkLinks,
                                   const std::vector<std::string>& extraLibPaths, bool& compiled) {
    namespace fs = std::filesystem;
    ensureBuildDir(outputRoot);
    PkgCacheRegistry caches(mirrorRoot, outputRoot);
    vector<std::string> libObjs;
    bool anyCodegenError = false;
    for (const auto& f : files) {
        auto file = riu.module(f.moduleName);
        if (!file) continue;
        string base = mirroredOutputBase(mirrorRoot, outputRoot, f.absPath);
        fs::create_directories(fs::path(base).parent_path());
        string obj = base + ".obj";
        string ir = mirroredOutputBase(mirrorRoot, irDir, f.absPath) + ".ll";
        if (emitIr) {
            fs::create_directories(fs::path(ir).parent_path());
        }
        if (!caches.isFresh(f.absPath, obj)) {
            file = riu.ensureFullAst(f.absPath, f.moduleName);
            if (!file) continue;
            if (!compileOneModuleToObj(riu, file, f.moduleName, obj, ir, emitIr,
                                       lib.lib_mod == "riu.core" && fs::path(f.absPath).stem() == "base")) {
                anyCodegenError = true;
                continue;
            }
            caches.mark(f.absPath);
            compiled = true;
        }
        libObjs.push_back(obj);
    }
    caches.flushAll();
    if (anyCodegenError) return false;

    const bool isDynLib = lib.type == "dynamic";
    string libPath = joinUnder(outputRoot, lib.name, ".lib");
    string dllPath = joinUnder(outputRoot, lib.name, ".dll");
    bool needLib = isDynLib ? (!fs::exists(dllPath) || !fs::exists(libPath)) : !fs::exists(libPath);
    if (!needLib) {
        try {
            auto t = fs::last_write_time(isDynLib ? dllPath : libPath);
            for (auto& o : libObjs) {
                if (fs::last_write_time(o) > t) {
                    needLib = true;
                    break;
                }
            }
            if (!needLib && isDynLib) {
                if (!sdkLibPath.empty() && fs::exists(sdkLibPath) && fs::last_write_time(sdkLibPath) > t) {
                    needLib = true;
                }
                if (!riurtLibPath.empty() && fs::exists(riurtLibPath) && fs::last_write_time(riurtLibPath) > t) {
                    needLib = true;
                }
                for (const auto& extra : extraLibPaths) {
                    if (!extra.empty() && fs::exists(extra) && fs::last_write_time(extra) > t) {
                        needLib = true;
                        break;
                    }
                }
            }
        } catch (...) {
            needLib = true;
        }
    }
    if (!needLib) return true;

    std::string outStr, errStr;
    llvm::raw_string_ostream oOS(outStr), eOS(errStr);
    lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
    if (isDynLib) {
        string defPath = joinUnder(outputRoot, lib.name, ".def");
        if (!writeDynLibDefFile(defPath, lib.name, libObjs)) return false;
        string defArg = "/def:" + defPath;
        string outArg = "/out:" + dllPath;
        string implibArg = "/implib:" + libPath;
        vector<const char*> args = {"lld-link", "/dll", "/noentry", outArg.c_str(), implibArg.c_str(), defArg.c_str()};
        for (auto& o : libObjs)
            args.push_back(o.c_str());
        if (!sdkLibPath.empty()) args.push_back(sdkLibPath.c_str());
        if (!riurtLibPath.empty()) args.push_back(riurtLibPath.c_str());
        for (const auto& extra : extraLibPaths) {
            if (!extra.empty()) args.push_back(extra.c_str());
        }
        std::vector<ExternalLink> libLinks = lib.external_links;
        libLinks.insert(libLinks.end(), sdkLinks.begin(), sdkLinks.end());
        std::vector<std::string> projLibArgs;
        appendExternalLinkArgs(libLinks, mirrorRoot, projLibArgs, args);
        std::cout << "Dynamic lib: " << dllPath << '\n';
        std::cout.flush();
        lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
        if (r.retCode) {
            std::cerr << errStr;
            return false;
        }
        if (copyRuntimeDlls(libLinks, mirrorRoot, fs::path(outputRoot)) != 0) return false;
    } else {
        string outArg = "/out:" + libPath;
        vector<const char*> args = {"lld-link", "/lib", outArg.c_str()};
        for (auto& o : libObjs)
            args.push_back(o.c_str());
        std::cout << "Static lib: " << libPath << '\n';
        lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
        if (r.retCode) {
            llvm::errs() << errStr;
            return false;
        }
    }
    compiled = true;
    return true;
}

static bool matchesTestModFilter(const std::string& mod, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    std::string base = stripTestSuffix(mod);
    for (auto& f : filters) {
        if (base == f || mod == f) return true;
    }
    return false;
}

// ====== buildTestDlls：lib 与 exe 模式共用的 test DLL 构建逻辑 ======
// allObjMap: modName → obj path（所有非 test 模块的 obj，由调用方预编译后传入）
static int buildTestDlls(Riu& riu, const std::filesystem::path& srcDir, const std::string& buildDir,
                         const std::string& irDir, bool emitIr, const std::string& sdkLibPath,
                         const std::string& riurtLibPath, const std::vector<std::string>& testModFilters,
                         const std::map<std::string, std::string>& allObjMap, int threads,
                         const std::vector<ExternalLink>& extraLinks, const std::vector<std::string>& extraLibPaths) {
    namespace fs = std::filesystem;

    // 构建 test 专用产物目录
    std::string testsDir = buildDir + "/tests";
    std::string testsObjDir = testsDir + "/obj";
    ensureBuildDir(testsDir);
    ensureBuildDir(testsObjDir);

    // 递归扫 src/ 下 *.test.ut
    std::vector<std::pair<std::string, std::string>> testFiles; // {abs, modName}
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec); it != fs::recursive_directory_iterator(); ++it) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        auto& p = it->path();
        if (p.extension() != ".ut") continue;
        auto fname = p.filename().string();
        if (!fname.ends_with(".test.ut")) continue;
        auto rel = fs::relative(p, srcDir);
        std::string modName = rel.generic_string();
        modName = modName.substr(0, modName.size() - 3); // strip .ut
        for (auto& c : modName)
            if (c == '/' || c == '\\') c = '.';
        testFiles.emplace_back(fs::absolute(p).string(), modName);
    }
    std::ranges::sort(testFiles);

    // --test-mod 过滤：只编译指定模块的测试（可重复）
    if (!testModFilters.empty()) {
        std::vector<std::pair<std::string, std::string>> filtered;
        for (auto& [abs, mod] : testFiles) {
            if (matchesTestModFilter(mod, testModFilters)) {
                filtered.emplace_back(abs, mod);
            }
        }
        if (filtered.empty()) {
            std::cerr << "Error: no test file matches --test-mod";
            for (auto& f : testModFilters)
                std::cerr << " " << f;
            std::cerr << "\n";
        }
        testFiles = std::move(filtered);
    }

    // 清失败标记：全量编清全部；--test-mod 只清本模块（并行子进程互不删别人的）
    {
        std::error_code rmEc;
        if (testModFilters.empty()) {
            for (auto it = fs::directory_iterator(testsDir, rmEc); it != fs::directory_iterator(); ++it) {
                if (rmEc) break;
                if (!it->is_regular_file()) continue;
                if (it->path().extension() == ".failed") {
                    fs::remove(it->path(), rmEc);
                }
            }
        } else {
            for (auto& [_, testMod] : testFiles) {
                std::string failPath = joinUnder(testsDir, testDllFileStem(testMod), ".test.failed");
                fs::remove(failPath, rmEc);
            }
        }
    }

    std::string fingerprint = computeRiuFingerprint();
    auto stampOf = [&](const std::string& testAbs) {
        return mirroredOutputBase(riu.projectRoot(), testsObjDir, testAbs) + ".cache";
    };
    auto objOf = [&](const std::string& testAbs) {
        return mirroredOutputBase(riu.projectRoot(), testsObjDir, testAbs) + ".obj";
    };

    size_t testExeCount = 0;
    size_t testFailCount = 0;

    std::string self = getSelfExePath();
    int maxParallel = resolveThreadCount(threads);
    bool useParallel = maxParallel > 1 && testFiles.size() > 1 && testModFilters.empty() && !self.empty();

    if (useParallel) {
        std::vector<std::pair<std::string, std::string>> stale;
        for (auto& [testAbs, testMod] : testFiles) {
            std::string stem = testDllFileStem(testMod);
            std::string testObj = objOf(testAbs);
            std::string dllPath = joinUnder(testsDir, stem, ".test.dll");
            if (isStampFresh(stampOf(testAbs), testAbs, testObj, fingerprint) &&
                isTestDllFresh(dllPath, testObj, allObjMap, sdkLibPath, riurtLibPath, extraLibPaths)) {
                ++testExeCount;
            } else {
                stale.emplace_back(testAbs, testMod);
            }
        }

        if (!stale.empty()) {
            std::string logsDir = testsDir + "/logs";
            {
                std::error_code ec2;
                fs::create_directories(logsDir, ec2);
            }
            std::string cwd = fs::current_path().string();

            // 按 worker 分片：每个子进程 parse 一次 SDK，再串行编自己那一份。
            // 一文件一进程会把 SDK parse 重复 stale.size() 次，CPU 涨、墙钟不变。
            int nJobs = std::min(maxParallel, static_cast<int>(stale.size()));
            std::vector<std::vector<std::pair<std::string, std::string>>> shards(static_cast<size_t>(nJobs));
            for (size_t i = 0; i < stale.size(); ++i) {
                shards[i % static_cast<size_t>(nJobs)].push_back(stale[i]);
            }

            std::cout.flush();
            std::cout << "Compiling " << stale.size() << " test file(s) in " << nJobs << " job(s)";
            if (testExeCount > 0) std::cout << " (" << testExeCount << " up-to-date)";
            std::cout << "...\n";
            std::cout.flush();

            std::mutex printMtx;
            std::vector<std::thread> waiters;
            waiters.reserve(static_cast<size_t>(nJobs));

            for (int job = 0; job < nJobs; ++job) {
                // NOLINTNEXTLINE(bugprone-exception-escape)
                waiters.emplace_back([&, job, shard = shards[static_cast<size_t>(job)]]() {
                    std::string logPath = logsDir;
                    logPath += '/';
                    logPath += "job-";
                    logPath += std::to_string(job);
                    logPath += ".compile.log";

                    std::wstring cmd = L"\"" + toWide(self) + L"\" build --test --threads 1";
                    for (auto& [_, testMod] : shard) {
                        cmd += L" --test-mod ";
                        cmd += toWide(stripTestSuffix(testMod));
                    }
                    if (emitIr) {
                        cmd += L" --emit-ir --emit-ir-dir \"" + toWide(irDir) + L"\"";
                    }
#ifdef _DEBUG
                    if (debug) cmd += L" -d";
#endif

                    std::uint32_t code = spawnToLog(cmd, toWide(logPath), toWide(cwd));

                    std::scoped_lock lock(printMtx);
                    for (auto& [testAbs, testMod] : shard) {
                        std::string stem = testDllFileStem(testMod);
                        std::string failPath = joinUnder(testsDir, stem, ".test.failed");
                        std::string dllPath = joinUnder(testsDir, stem, ".test.dll");

                        bool failed = false;
                        std::string failReason;
                        {
                            std::error_code ec3;
                            bool stampOk = isStampFresh(stampOf(testAbs), testAbs, objOf(testAbs), fingerprint);
                            if (fs::exists(failPath, ec3)) {
                                failed = true;
                            } else if (!fs::exists(dllPath, ec3) || !stampOk) {
                                // job abort/assert 时旧 dll 仍在；stamp 未刷新就不能算本轮成功
                                failed = true;
                                failReason = code == kSpawnFailed ? "compile job spawn failed"
                                             : code != 0          ? ("compile job exit " + std::to_string(code))
                                                                  : "compile job produced no dll";
                            }
                        }

                        if (failed && !failReason.empty()) {
                            std::ofstream failFile(failPath);
                            if (failFile) failFile << failReason << "\n";
                            std::error_code rmEc;
                            fs::remove(dllPath, rmEc);
                        }

                        if (failed) {
                            ++testFailCount;
                            std::cout << "Compile " << testMod << " FAIL (log: " << logPath << ")\n";
                        } else {
                            ++testExeCount;
                            std::cout << "Compile " << testMod << " ok\n";
                        }
                    }
                    std::cout.flush();
                });
            }

            for (auto& t : waiters)
                t.join();
        }
    } else {
        // 测试 obj 每文件 stamp（与 lib 缓存隔离：testsObjDir 下独立 .cache）
        for (auto& [testAbs, testMod] : testFiles) {
            // 计算 DLL 文件名（后续多处使用，也用于失败标记文件路径）
            std::string dllFileName = testDllFileStem(testMod);

            // 构建失败时写标记文件 + 删除旧 DLL（避免 test 扫描时重复计入）
            auto markAsFailed = [&](const std::string& reason) {
                std::string failPath = std::string(testsDir).append("/").append(dllFileName).append(".test.failed");
                std::ofstream failFile(failPath);
                if (failFile) failFile << reason << "\n";
                // 删除旧 DLL：如果上次构建成功但本次失败，旧 DLL 已过时
                std::string oldDll = std::string(testsDir).append("/").append(dllFileName).append(".test.dll");
                std::error_code rmEc;
                fs::remove(oldDll, rmEc);
                ++testFailCount;
            };

            // 解析测试文件 AST
            std::string testModName = testMod;
            try {
                riu.loadMainFile(testAbs, testModName);
            } catch (std::runtime_error& e) {
                reportRuntimeError(testAbs, e, testModName + ": ");
                markAsFailed(e.what());
                continue;
            }

            // Codegen 测试模块（isTestDll=true）
            auto testFile = riu.module(testModName);
            if (!testFile) {
                markAsFailed("module not found after parse");
                continue;
            }

            std::string testObj = objOf(testAbs);
            fs::create_directories(fs::path(testObj).parent_path());
            std::string stampPath = stampOf(testAbs);

            // 缓存检查：test obj 新鲜则跳过 codegen
            if (!isStampFresh(stampPath, testAbs, testObj, fingerprint)) {
                auto tCtx = std::make_unique<llvm::LLVMContext>();
                auto tMod = std::make_unique<llvm::Module>(testModName, *tCtx);
                llvm::IRBuilder<> tBuilder(*tCtx);
                try {
                    Compiler compiler(*tCtx, tBuilder, tMod.get(), testFile, &riu, false, true); // isTestDll=true
                    compiler.compile(testFile);
                } catch (std::runtime_error& e) {
                    reportRuntimeError(testAbs, e, testModName + ": ");
                    markAsFailed(e.what());
                    continue;
                }

                if (emitIr) {
                    std::string testIr = mirroredOutputBase(riu.projectRoot(), irDir, testAbs) + ".ll";
                    fs::create_directories(fs::path(testIr).parent_path());
                    std::error_code ec2;
                    llvm::raw_fd_ostream irFile(testIr, ec2);
                    if (ec2) {
                        std::cerr << "Error opening test IR file: " << ec2.message() << '\n';
                    } else {
                        tMod->print(irFile, nullptr);
                        irFile.flush();
                        std::cout << "Write test IR: " << testIr << '\n';
                    }
                }

                if (!compileIRToObj(tMod.get(), testObj)) {
                    std::cerr << "Error: failed to compile test IR: " << testObj << '\n';
                    markAsFailed("compile IR to obj failed: " + testObj);
                    continue;
                }
                std::cout << "Write test obj: " << testObj << '\n';
                writeStamp(stampPath, testAbs, fingerprint);
            }

            // 收集依赖 obj（从 riu.loadOrder — 测试模块引用的依赖）
            std::vector<std::string> linkObjs = {testObj};
            std::set<std::string> seenMods;
            for (auto& depMod : riu.loadOrder()) {
                if (seenMods.count(depMod)) continue;
                seenMods.insert(depMod);
                auto it = allObjMap.find(depMod);
                if (it != allObjMap.end()) {
                    linkObjs.push_back(it->second);
                }
            }
            // SDK lib
            if (!sdkLibPath.empty()) linkObjs.push_back(sdkLibPath);
            for (const auto& extra : extraLibPaths) {
                if (!extra.empty()) linkObjs.push_back(extra);
            }

            // LLD 链接 test dll（dllFileName 已在循环顶部计算）
            std::string testDllPath = testsDir;
            testDllPath += '/';
            testDllPath += dllFileName;
            testDllPath += ".test.dll";

            // 检查 DLL 是否需要重新链接（DLL 比所有输入 obj 都新则跳过）
            bool needLink = true;
            if (fs::exists(testDllPath)) {
                needLink = false;
                try {
                    auto dllTime = fs::last_write_time(testDllPath);
                    for (auto& o : linkObjs) {
                        if (fs::exists(o) && fs::last_write_time(o) > dllTime) {
                            needLink = true;
                            break;
                        }
                    }
                } catch (...) {
                    needLink = true;
                }
            }

            if (needLink) {
                std::string dllOut = "/out:" + testDllPath;
                // DLL 模式：/dll，无需 /entry /subsystem /kernel32.lib
                std::vector<const char*> linkArgs = {"lld-link", dllOut.c_str(), "/dll", "/noentry"};
                for (auto& o : linkObjs)
                    linkArgs.insert(linkArgs.begin() + 1, o.c_str());
                // riurt 运行时库
                if (!riurtLibPath.empty()) linkArgs.push_back(riurtLibPath.c_str());
                std::vector<std::string> projLibArgs;
                appendExternalLinkArgs(extraLinks, riu.projectRoot(), projLibArgs, linkArgs);

                std::string outStr, errStr;
                llvm::raw_string_ostream oOS(outStr), eOS(errStr);
                std::cout << "Link test dll: " << testDllPath << '\n';
                lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
                lld::Result r = lldMain(linkArgs, oOS, eOS, llvm::ArrayRef{dd});
                if (r.retCode) {
                    std::cerr << "Error: test dll link failed for " << testMod << "\n" << errStr;
                    markAsFailed("link failed: " + errStr);
                    continue;
                }
                if (copyRuntimeDlls(extraLinks, riu.projectRoot(), fs::path(testsDir)) != 0) {
                    markAsFailed("copy runtime dll failed");
                    continue;
                }
            }
            // 构建成功，清除可能存在的旧失败标记
            std::string oldFailPath = std::string(testsDir).append("/").append(dllFileName).append(".test.failed");
            std::error_code rmEc;
            fs::remove(oldFailPath, rmEc);
            ++testExeCount;
        }
    }
    if (testExeCount > 0) {
        std::cout << "Built " << testExeCount << " test dll(s) into " << testsDir << '\n';
    } else if (!testFiles.empty()) {
        std::cout << "no test dll built (all failed)\n";
    } else if (testModFilters.empty()) {
        std::cout << "no *.test.ut files found\n";
    }
    if (testFailCount > 0) {
        std::cout << testFailCount << " test file(s) failed to build\n";
    }
    // --test-mod 无匹配时已在上面输出 stderr，不再重复 stdout
    return static_cast<int>(testFailCount);
}

int runBuildCommand(const BuildCmdOptions& opts) {
    bool projectMode = opts.projectMode;
    bool emitIr = opts.emitIr;
    const std::string& buildNameArg = opts.buildNameArg;
    const std::string& emitIrDir = opts.emitIrDir;

    Riu riu;
    string projectName;
    string projectBuildDir;
    string buildDir;
    std::vector<std::string> exeInputFiles;
    bool buildLib = false;
    std::vector<const Executable*> exesToBuild;
    vector<ResolvedDep> pathDeps;

    {
        string cwd = std::filesystem::current_path().string();
        string tomlPath = (std::filesystem::path(cwd) / "riu.toml").string();
        try {
            riu.initProjectFromDir(cwd);
        } catch (runtime_error& e) {
            reportRuntimeError(tomlPath, e);
            return 1;
        }

        vector<ResolvedDep> resolved;
        try {
            resolved = resolvePathDepGraph(cwd, riu.projectConfig(), sdk_loader::findSdkPackage);
        } catch (runtime_error& e) {
            reportRuntimeError(tomlPath, e);
            return 1;
        }
        pathDeps = std::move(resolved);

        if (!buildNameArg.empty()) {
            bool found = false;
            if (riu.library() && riu.library()->name == buildNameArg) {
                buildLib = true;
                found = true;
            }
            for (const auto& e : riu.executables()) {
                if (e.name == buildNameArg) {
                    exesToBuild.push_back(&e);
                    found = true;
                }
            }
            if (!found) {
                std::cerr << "Error: unknown build target `" << buildNameArg << "`\n";
                return 1;
            }
        } else {
            buildLib = riu.library() != nullptr;
            for (const auto& e : riu.executables())
                exesToBuild.push_back(&e);
        }

        if (exesToBuild.empty() && !buildLib) {
            std::cerr << "Error: riu.toml is missing `[library]` or `[[executable]]`\n";
            return 1;
        }
        exeInputFiles.reserve(exesToBuild.size());
        for (const Executable* e : exesToBuild) {
            std::string in;
            if (!checkEntryPath(riu, e->entry, in)) return 1;
            exeInputFiles.push_back(std::move(in));
        }

        projectName = riu.projectName();
        buildDir = getBuildDir(riu.projectRoot());
        projectBuildDir = buildDir;
    }

    std::cout << "Project root: " << riu.projectRoot() << '\n';
    ensureBuildDir(buildDir);
    ensureBuildDir(projectBuildDir);

    // IR 输出目录：默认 build/，可通过 --emit-ir-dir 覆盖
    std::string irDir = emitIrDir.empty() ? buildDir : emitIrDir;
    if (emitIr) ensureBuildDir(irDir);

    // SDK 符号表：core 始终加载。stdlib 仅在图中出现（或本包就是 stdlib）时加载。
    string sdkPath;
    {
        namespace fs = std::filesystem;
        if (riu.projectName() == "core") {
            fs::path candidate = fs::path(riu.projectRoot()) / "src" / "riu" / "core";
            if (fs::is_directory(candidate)) {
                sdkPath = candidate.string();
            }
        }
        if (sdkPath.empty()) sdkPath = sdk_loader::findSdkPath();
    }
    string sdkLibPath;
    const bool isCoreSelfBuild = riu.projectName() == "core" && !sdkPath.empty();
    const bool isStdlibSelfBuild = riu.projectName() == "stdlib";
    const bool isSdkSelfBuild = isCoreSelfBuild || isStdlibSelfBuild;

    if (!sdkPath.empty()) {
        namespace fs = std::filesystem;
        sdkPath = fs::absolute(sdkPath).string();

        std::unordered_set<std::string> forceFullParse;
        if (isCoreSelfBuild) {
            PkgCacheRegistry probe(riu.projectRoot(), buildDir);
            auto consider = [&](const std::string& abs) {
                string obj = mirroredOutputBase(riu.projectRoot(), buildDir, abs) + ".obj";
                if (!probe.isFresh(abs, obj)) forceFullParse.insert(abs);
            };
            for (const auto& entry : fs::directory_iterator(sdkPath)) {
                if (!entry.is_regular_file()) continue;
                auto fname = entry.path().filename().string();
                if (!fname.ends_with(".ut")) continue;
                if (fname.ends_with(".test.ut")) continue;
                consider(fs::absolute(entry.path()).lexically_normal().generic_string());
            }
        }
        try {
            sdk_loader::parseSdkDir(sdkPath, riu, true, isCoreSelfBuild ? &forceFullParse : nullptr);
        } catch (std::runtime_error& e) {
            reportRuntimeError(sdkPath, e, "Error in SDK: ");
            return 1;
        }
    }

    // riurt 静态库路径推导：与 riu.exe 同 GN 输出树下，exe=bin/，lib=lib/
    // 开发期：build/<plat>/<arch>/<mode>/lib/riurt.lib；发布后随编译器安装。
    // 开发期：build/<plat>/<arch>/<mode>/lib/riurt.lib；发布后随编译器安装。
    string riurtLibPath;
    {
        namespace fs = std::filesystem;
        // 从 exe 路径反推：riu.exe 在 bin/，riurt.lib 在 lib/
        std::array<char, MAX_PATH> exeBuf{};
        DWORD len = GetModuleFileNameA(nullptr, exeBuf.data(), static_cast<DWORD>(exeBuf.size()));
        if (len > 0 && len < exeBuf.size()) {
            fs::path exeDir = fs::path(exeBuf.data()).parent_path(); // bin/
            fs::path buildDir2 = exeDir.parent_path();               // <mode>/
            fs::path candidate = buildDir2 / "lib" / "riurt.lib";
            if (fs::exists(candidate)) {
                riurtLibPath = candidate.string();
            }
        }
        // 备选：相对 SDK 项目根（dogfood build）
        if (riurtLibPath.empty()) {
            fs::path alt = fs::path(riu.projectRoot()) / "build" / "lib" / "riurt.lib";
            if (fs::exists(alt)) riurtLibPath = alt.string();
        }
        if (riurtLibPath.empty()) {
            std::cerr
                << "Warning: riurt.lib not found — runtime functions (riurt_alloc/free/math) will be unresolved\n";
        }
    }

    bool compiled = false;

    std::vector<ExternalLink> sdkLinks;
    std::vector<ExternalLink> testLinks;
    if (riu.library()) {
        testLinks.insert(testLinks.end(), riu.library()->external_links.begin(), riu.library()->external_links.end());
    }
    for (const auto& e : riu.executables()) {
        testLinks.insert(testLinks.end(), e.external_links.begin(), e.external_links.end());
    }

    std::vector<std::string> depLibPaths;
    std::vector<ExternalLink> depLinks;

    auto sourceRootOfPkg = [](const std::string& projectRoot) {
        namespace fs = std::filesystem;
        fs::path src = fs::path(projectRoot) / "src";
        return fs::is_directory(src) ? src.string() : projectRoot;
    };

    // 隐式 core：用户与 stdlib 自构建链 core.lib；core 自构建用本包 external_links。
    string corePkgRoot = sdk_loader::findSdkPackage("core");
    if (isCoreSelfBuild) {
        sdkLinks = riu.library() ? riu.library()->external_links : std::vector<ExternalLink>{};
        sdkLibPath = joinUnder(projectBuildDir, riu.library() ? riu.library()->name : "riu.core", ".lib");
    } else if (!corePkgRoot.empty()) {
        sdkLinks = loadLibraryLinks(corePkgRoot);
        auto coreFiles = collectLibModFiles(sourceRootOfPkg(corePkgRoot), "riu.core");
        string toolchainLib = joinUnder((std::filesystem::path(corePkgRoot) / "build").string(), "riu.core", ".lib");
        if (isStdlibSelfBuild || libNewerThanSources(toolchainLib, coreFiles)) {
            sdkLibPath = toolchainLib;
        } else {
            namespace fs = std::filesystem;
            string outputRoot = (fs::path(buildDir) / "deps" / "core").string();
            riu.addDeclOutputRoot(corePkgRoot, outputRoot);
            Library coreLib;
            coreLib.name = "riu.core";
            coreLib.type = "static";
            coreLib.lib_mod = "riu.core";
            if (!compileLibraryToOutput(riu, coreFiles, corePkgRoot, outputRoot, coreLib, emitIr, irDir, "",
                                        riurtLibPath, {}, {}, compiled)) {
                return 1;
            }
            sdkLibPath = joinUnder(outputRoot, "riu.core", ".lib");
        }
    }
    if (!isCoreSelfBuild) {
        testLinks.insert(testLinks.end(), sdkLinks.begin(), sdkLinks.end());
    }
    if (isStdlibSelfBuild && riu.library()) {
        depLibPaths.push_back(joinUnder(projectBuildDir, riu.library()->name, ".lib"));
        testLinks.insert(testLinks.end(), riu.library()->external_links.begin(), riu.library()->external_links.end());
    }

    if (!isSdkSelfBuild && !pathDeps.empty()) {
        namespace fs = std::filesystem;
        struct PathDepWork {
            ResolvedDep pkg;
            string outputRoot;
            vector<LibModFile> files;
            string libPath;
            bool skipCompile = false;
        };
        vector<PathDepWork> works;
        for (const auto& pkg : pathDeps) {
            if (!pkg.config.library) continue;
            PathDepWork w;
            w.pkg = pkg;
            w.files = collectLibModFiles(pkg.sourceRoot, pkg.config.library->lib_mod);
            if (w.files.empty()) {
                std::cerr << "Error: `[library].lib_mod` not found: " << pkg.config.library->lib_mod << '\n';
                return 1;
            }
            if (pkg.kind == DepSourceKind::Sdk) {
                string toolchainLib =
                    joinUnder((fs::path(pkg.projectRoot) / "build").string(), pkg.config.library->name, ".lib");
                if (libNewerThanSources(toolchainLib, w.files)) {
                    w.libPath = toolchainLib;
                    w.skipCompile = true;
                    w.outputRoot = (fs::path(pkg.projectRoot) / "build").string();
                }
            }
            if (!w.skipCompile) {
                w.outputRoot = (fs::path(buildDir) / "deps" / pkg.name).string();
                w.libPath = joinUnder(w.outputRoot, pkg.config.library->name, ".lib");
                riu.addDeclOutputRoot(pkg.projectRoot, w.outputRoot);
            }
            registerLibModPaths(riu, pkg.sourceRoot, pkg.config.library->lib_mod, w.files);
            works.push_back(std::move(w));
        }
        for (auto& w : works) {
            if (w.pkg.kind == DepSourceKind::Sdk) {
                try {
                    sdk_loader::parseSdkLibrary(w.pkg.projectRoot, w.pkg.config.library->lib_mod, riu, true);
                } catch (runtime_error& e) {
                    reportRuntimeError(w.pkg.projectRoot, e, w.pkg.name + ": ");
                    return 1;
                }
                continue;
            }
            for (const auto& f : w.files) {
                if (riu.module(f.moduleName)) continue;
                try {
                    riu.loadMainFile(f.absPath, f.moduleName);
                } catch (runtime_error& e) {
                    reportRuntimeError(f.absPath, e, f.moduleName + ": ");
                    return 1;
                }
            }
        }
        vector<string> builtSoFar;
        if (!sdkLibPath.empty()) builtSoFar.push_back(sdkLibPath);
        for (auto& w : works) {
            if (!w.skipCompile) {
                string depIrDir = emitIr ? (w.outputRoot + "/ir") : irDir;
                if (!compileLibraryToOutput(riu, w.files, w.pkg.projectRoot, w.outputRoot, *w.pkg.config.library,
                                            emitIr, depIrDir, sdkLibPath, riurtLibPath, sdkLinks, builtSoFar,
                                            compiled)) {
                    return 1;
                }
            }
            depLibPaths.push_back(w.libPath);
            builtSoFar.push_back(w.libPath);
            depLinks.insert(depLinks.end(), w.pkg.config.library->external_links.begin(),
                            w.pkg.config.library->external_links.end());
            testLinks.insert(testLinks.end(), w.pkg.config.library->external_links.begin(),
                             w.pkg.config.library->external_links.end());
        }
    }

    auto codegenTo = [&](FileNode* file, const std::string& moduleName, const std::string& objOut,
                         const std::string& irOut, bool isSdk = false) -> bool {
        return compileOneModuleToObj(riu, file, moduleName, objOut, irOut, emitIr, isSdk);
    };

    // ====== lib 模式：按 `[library].lib_mod` 扫源 + 静态库 / 动态库 ======
    if (buildLib) {
        namespace fs = std::filesystem;
        fs::path srcDir(riu.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: lib project missing `src/` directory at " << srcDir.string() << '\n';
            return 1;
        }
        vector<std::pair<std::string, std::string>> libFiles; // {abs, modName}
        fs::path scanDir = srcDir;
        bool doScan = true;
        if (const Library* lib = riu.library()) {
            string rel = lib->lib_mod;
            for (char& c : rel)
                if (c == '.') c = '/';
            fs::path pkgDir = srcDir / rel;
            fs::path fileMod = pkgDir;
            fileMod += ".ut";
            if (fs::is_directory(pkgDir)) {
                scanDir = pkgDir;
            } else if (fs::is_regular_file(fileMod)) {
                libFiles.emplace_back(fs::absolute(fileMod).string(), lib->lib_mod);
                doScan = false;
            } else {
                std::cerr << "Error: `[library].lib_mod` not found: " << lib->lib_mod << '\n';
                return 1;
            }
        }
        std::error_code walkEc;
        if (doScan) {
            for (auto it = fs::recursive_directory_iterator(scanDir, walkEc); it != fs::recursive_directory_iterator();
                 ++it) {
                if (walkEc) break;
                if (!it->is_regular_file()) continue;
                auto& p = it->path();
                if (p.extension() != ".ut") continue;
                {
                    auto fname = p.filename().string();
                    if (fname.ends_with(".test.ut")) {
                        continue;
                    }
                }
                auto rel = fs::relative(p, srcDir);
                string modName = rel.generic_string();
                modName = modName.substr(0, modName.size() - 3); // strip .ut
                for (auto& c : modName)
                    if (c == '/' || c == '\\') c = '.';
                libFiles.emplace_back(fs::absolute(p).string(), modName);
            }
        }
        std::ranges::sort(libFiles);

        // 加载所有 AST（core 自构建时 parseSdkDir 已加载）
        if (!isCoreSelfBuild) {
            for (auto& [abs, mn] : libFiles) {
                try {
                    riu.loadMainFile(abs, mn);
                } catch (runtime_error& e) {
                    reportRuntimeError(abs, e, mn + ": ");
                    return 1;
                }
            }
        }

        // 各模块 codegen — 文件级聚合：单个文件失败不立即退出，继续编译其余文件，最终再决定是否链接
        vector<std::string> libObjs;
        bool anyCodegenError = false;
        PkgCacheRegistry libCaches(riu.projectRoot(), buildDir);

        // core 自构建：编译列表来自 parseSdkDir 已加载的模块，读 pkg 确定 runtime base
        std::map<std::string, SdkPkgEntry> sdkPkgMap;
        if (isCoreSelfBuild) {
            sdkPkgMap = sdk_loader::readSdkPkg(sdkPath);
        }

        vector<std::pair<std::string, std::string>> compileList; // {abs, modName}
        if (isCoreSelfBuild) {
            for (auto& file : riu.files()) {
                if (file == riu.sdkFile()) continue;
                string mn = file->moduleName();
                string abs = riu.modulePath(mn);
                if (abs.empty()) continue;
                compileList.emplace_back(abs, mn);
            }
            std::ranges::sort(compileList);
        } else {
            compileList = libFiles;
        }

        for (auto& [abs, mn] : compileList) {
            auto file = riu.module(mn);
            if (!file) continue;
            string base = mirroredOutputBase(riu.projectRoot(), buildDir, abs);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = mirroredOutputBase(riu.projectRoot(), irDir, abs) + ".ll";
            if (emitIr) {
                fs::create_directories(fs::path(ir).parent_path());
            }
            bool isSdkRuntime = false;
            if (isCoreSelfBuild) {
                auto stem = fs::path(abs).stem().string();
                auto it = sdkPkgMap.find(stem);
                bool isFlatDep = (it == sdkPkgMap.end()) || it->second.isFlat;
                isSdkRuntime = isFlatDep && (stem == "base");
            }

            if (!libCaches.isFresh(abs, obj)) {
                file = riu.ensureFullAst(abs, mn);
                if (!file) continue;
                if (!codegenTo(file, mn, obj, ir, isSdkRuntime)) {
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

        // 链接为静态库（/lib 只归档）或动态库（/dll + 导入库）
        const bool isDynLib = riu.library() && riu.library()->type == "dynamic";
        string libStem = riu.library() ? riu.library()->name : projectName;
        string libPath = joinUnder(projectBuildDir, libStem, ".lib");
        string dllPath = joinUnder(projectBuildDir, libStem, ".dll");
        bool needLib = isDynLib ? (!fs::exists(dllPath) || !fs::exists(libPath)) : !fs::exists(libPath);
        if (!needLib) {
            try {
                auto t = fs::last_write_time(isDynLib ? dllPath : libPath);
                for (auto& o : libObjs) {
                    if (fs::last_write_time(o) > t) {
                        needLib = true;
                        break;
                    }
                }
                if (!needLib && isDynLib) {
                    if (!sdkLibPath.empty() && fs::exists(sdkLibPath) && fs::last_write_time(sdkLibPath) > t) {
                        needLib = true;
                    }
                    if (!riurtLibPath.empty() && fs::exists(riurtLibPath) && fs::last_write_time(riurtLibPath) > t) {
                        needLib = true;
                    }
                }
            } catch (...) {
                needLib = true;
            }
        }
        if (needLib) {
            std::string outStr, errStr;
            llvm::raw_string_ostream oOS(outStr), eOS(errStr);
            lld::DriverDef dd = {.f = lld::WinLink, .d = &lld::coff::link};
            if (isDynLib) {
                string defPath = joinUnder(projectBuildDir, libStem, ".def");
                if (!writeDynLibDefFile(defPath, libStem, libObjs)) return 1;
                string defArg = "/def:" + defPath;
                string outArg = "/out:" + dllPath;
                string implibArg = "/implib:" + libPath;
                vector<const char*> args = {"lld-link",        "/dll",        "/noentry", outArg.c_str(),
                                            implibArg.c_str(), defArg.c_str()};
                for (auto& o : libObjs)
                    args.push_back(o.c_str());
                if (!isSdkSelfBuild && !sdkLibPath.empty()) args.push_back(sdkLibPath.c_str());
                for (const auto& depLib : depLibPaths)
                    args.push_back(depLib.c_str());
                if (!riurtLibPath.empty()) args.push_back(riurtLibPath.c_str());
                std::vector<ExternalLink> libLinks =
                    riu.library() ? riu.library()->external_links : std::vector<ExternalLink>{};
                if (!isSdkSelfBuild) {
                    libLinks.insert(libLinks.end(), sdkLinks.begin(), sdkLinks.end());
                    libLinks.insert(libLinks.end(), depLinks.begin(), depLinks.end());
                }
                std::vector<std::string> projLibArgs;
                appendExternalLinkArgs(libLinks, riu.projectRoot(), projLibArgs, args);

                std::cout << "Dynamic lib: " << dllPath << '\n';
                std::cout.flush();
                lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
                if (r.retCode) {
                    std::cerr << errStr;
                    return 1;
                }
                if (copyRuntimeDlls(libLinks, riu.projectRoot(), fs::path(projectBuildDir)) != 0) {
                    return 1;
                }
            } else {
                string outArg = "/out:" + libPath;
                vector<const char*> args = {"lld-link", "/lib", outArg.c_str()};
                for (auto& o : libObjs)
                    args.push_back(o.c_str());

                std::cout << "Static lib: " << libPath << '\n';
                lld::Result r = lldMain(args, oOS, eOS, llvm::ArrayRef{dd});
                if (r.retCode) {
                    llvm::errs() << errStr;
                    return 1;
                }
            }
            compiled = true;
        }

        // 还要编 exe 时不在这里报 no work：后面链接可能仍有活
        if (!compiled && exesToBuild.empty() && !opts.testMode) {
            std::cout << "no work to do." << '\n';
        }

        // ====== test 模式：构建 test DLL（lib 与 exe 共用 buildTestDlls） ======
        if (opts.testMode) {
            // 构建非 test 模块 obj 映射用于 test DLL 依赖收集
            std::map<std::string, std::string> allObjMap; // modName → obj path
            for (auto& [abs, mn] : libFiles) {
                std::string obj = mirroredOutputBase(riu.projectRoot(), buildDir, abs) + ".obj";
                allObjMap[mn] = obj;
            }
            int testFails = buildTestDlls(riu, srcDir, buildDir, irDir, emitIr, sdkLibPath, riurtLibPath, opts.testMods,
                                          allObjMap, opts.threads, testLinks, depLibPaths);
            std::cout.flush();
            std::cerr.flush();
            _exit(testFails > 0 ? 1 : 0);
        }

        if (exesToBuild.empty()) {
            std::cout.flush();
            std::cerr.flush();
            _exit(0);
        }
    }

    // ====== exe 模式 test：全量扫描 src/ → 编译非 test 模块 → test DLL ======
    if (opts.testMode) {
        namespace fs = std::filesystem;
        fs::path srcDir(riu.sourceRoot());
        if (!fs::is_directory(srcDir)) {
            std::cerr << "Error: project missing `src/` directory at " << srcDir.string() << '\n';
            return 1;
        }

        // 递归扫 src/ 下 *.ut（跳过 *.test.ut）
        vector<std::pair<std::string, std::string>> nonTestFiles; // {abs, modName}
        std::error_code walkEc;
        for (auto it = fs::recursive_directory_iterator(srcDir, walkEc); it != fs::recursive_directory_iterator();
             ++it) {
            if (walkEc) break;
            if (!it->is_regular_file()) continue;
            auto& p = it->path();
            if (p.extension() != ".ut") continue;
            auto fname = p.filename().string();
            if (fname.ends_with(".test.ut")) continue;
            auto rel = fs::relative(p, srcDir);
            string modName = rel.generic_string();
            modName = modName.substr(0, modName.size() - 3); // strip .ut
            for (auto& c : modName)
                if (c == '/' || c == '\\') c = '.';
            nonTestFiles.emplace_back(fs::absolute(p).string(), modName);
        }
        std::ranges::sort(nonTestFiles);

        // 加载所有非 test 模块 AST（core 自构建时 parseSdkDir 已加载）
        if (!isCoreSelfBuild) {
            for (auto& [abs, mn] : nonTestFiles) {
                try {
                    riu.loadMainFile(abs, mn);
                } catch (runtime_error& e) {
                    reportRuntimeError(abs, e, mn + ": ");
                    return 1;
                }
            }
        }

        vector<std::pair<std::string, std::string>> compileList; // {abs, modName}
        std::map<std::string, SdkPkgEntry> sdkPkgMap;
        if (isCoreSelfBuild) {
            sdkPkgMap = sdk_loader::readSdkPkg(sdkPath);
            for (auto& file : riu.files()) {
                if (file == riu.sdkFile()) continue;
                string mn = file->moduleName();
                string abs = riu.modulePath(mn);
                if (abs.empty()) continue;
                compileList.emplace_back(abs, mn);
            }
            std::ranges::sort(compileList);
        } else {
            compileList = nonTestFiles;
        }

        // 各模块 codegen → obj（文件级聚合：单文件失败继续编译其余）
        bool anyCodegenError = false;
        PkgCacheRegistry exeTestCaches(riu.projectRoot(), buildDir);

        for (auto& [abs, mn] : compileList) {
            auto file = riu.module(mn);
            if (!file) continue;
            string base = mirroredOutputBase(riu.projectRoot(), buildDir, abs);
            fs::create_directories(fs::path(base).parent_path());
            string obj = base + ".obj";
            string ir = mirroredOutputBase(riu.projectRoot(), irDir, abs) + ".ll";
            if (emitIr) {
                fs::create_directories(fs::path(ir).parent_path());
            }
            bool isSdkRuntime = false;
            if (isCoreSelfBuild) {
                auto stem = fs::path(abs).stem().string();
                auto it = sdkPkgMap.find(stem);
                bool isFlatDep = (it == sdkPkgMap.end()) || it->second.isFlat;
                isSdkRuntime = isFlatDep && (stem == "base");
            }

            if (!exeTestCaches.isFresh(abs, obj)) {
                file = riu.ensureFullAst(abs, mn);
                if (!file) continue;
                if (!codegenTo(file, mn, obj, ir, isSdkRuntime)) {
                    anyCodegenError = true;
                    continue;
                }
                exeTestCaches.mark(abs);
                compiled = true;
            }
        }
        exeTestCaches.flushAll();
        if (anyCodegenError) {
            return 1;
        }

        // 构建非 test 模块 obj 映射用于 test DLL 依赖收集
        std::map<std::string, std::string> allObjMap; // modName → obj path
        for (auto& [abs, mn] : nonTestFiles) {
            std::string obj = mirroredOutputBase(riu.projectRoot(), buildDir, abs) + ".obj";
            allObjMap[mn] = obj;
        }
        int testFails = buildTestDlls(riu, srcDir, buildDir, irDir, emitIr, sdkLibPath, riurtLibPath, opts.testMods,
                                      allObjMap, opts.threads, testLinks, depLibPaths);

        std::cout.flush();
        std::cerr.flush();
        _exit(testFails > 0 ? 1 : 0);
    }

    // ====== exe 模式：按 [[executable]] 声明顺序逐个编（不互链、不把其它 entry 的 main 链进来） ======
    if (exesToBuild.empty()) {
        std::cerr << "Error: no executable to build\n";
        return 1;
    }

    PkgCacheRegistry exeCaches(riu.projectRoot(), buildDir);

    for (size_t ei = 0; ei < exesToBuild.size(); ++ei) {
        const Executable* exe = exesToBuild[ei];
        const std::string& inputFile = exeInputFiles[ei];

        std::string baseName = llvm::sys::path::stem(inputFile).str();
        std::string objPath =
            mirroredOutputBase(riu.projectRoot(), buildDir, std::filesystem::absolute(inputFile).string()) + ".obj";
        std::filesystem::create_directories(std::filesystem::path(objPath).parent_path());

        FileNode* mainFile = nullptr;
        try {
            mainFile = riu.loadMainFile(inputFile, baseName);
        } catch (runtime_error& e) {
            reportRuntimeError(inputFile, e);
            return 1;
        }

        // 文件级聚合：主模块与各导入模块逐个 codegen，单文件失败不立即退出，继续编译其余文件
        bool anyCodegenError = false;

        // 主模块。
        std::string mainAbs = std::filesystem::absolute(inputFile).string();
        bool needCompile = !exeCaches.isFresh(mainAbs, objPath);
        if (needCompile) {
            std::string irPath = mirroredOutputBase(riu.projectRoot(), irDir, mainAbs) + ".ll";
            if (emitIr) {
                std::filesystem::create_directories(std::filesystem::path(irPath).parent_path());
            }
            if (!codegenTo(mainFile, baseName, objPath, irPath)) {
                anyCodegenError = true;
            } else {
                exeCaches.mark(mainAbs);
                compiled = true;
            }
        }

        // 本 exe 的 entry 绝对路径；其它 exe 的 main 不进本产物
        std::unordered_set<std::string> otherEntryAbs;
        for (size_t oj = 0; oj < exesToBuild.size(); ++oj) {
            if (oj == ei) continue;
            otherEntryAbs.insert(std::filesystem::absolute(exeInputFiles[oj]).string());
        }

        // 导入的用户模块（跳过其它 [[executable]] 的入口，避免 duplicate main）
        std::vector<std::string> modObjPaths;
        for (auto& modName : riu.loadOrder()) {
            auto modFile = riu.module(modName);
            if (!modFile || modFile == riu.sdkFile()) continue;
            std::string modSrc = riu.modulePath(modName);
            if (otherEntryAbs.contains(modSrc)) continue;
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                auto rel = fs::relative(fs::weakly_canonical(fs::path(modSrc), ec),
                                        fs::weakly_canonical(fs::path(riu.projectRoot()), ec), ec);
                if (ec) continue;
                string s = rel.generic_string();
                if (!(s.empty() || s == "." || (s != ".." && !s.starts_with("../")))) continue;
            }
            std::string modBase = mirroredOutputBase(riu.projectRoot(), buildDir, modSrc);
            std::filesystem::create_directories(std::filesystem::path(modBase).parent_path());
            std::string modObj = modBase + ".obj";
            std::string modIr = mirroredOutputBase(riu.projectRoot(), irDir, modSrc) + ".ll";
            if (emitIr) {
                std::filesystem::create_directories(std::filesystem::path(modIr).parent_path());
            }
            if (!exeCaches.isFresh(modSrc, modObj)) {
                modFile = riu.ensureFullAst(modSrc, modName);
                if (!modFile) continue;
                if (!codegenTo(modFile, modName, modObj, modIr)) {
                    anyCodegenError = true;
                    continue; // 继续尝试下一个模块的 codegen
                }
                exeCaches.mark(modSrc);
                compiled = true;
            }
            modObjPaths.push_back(modObj);
        }
        exeCaches.flushAll();

        // 任一模块（含主模块）codegen 失败：跳过链接，统一非零退出
        if (anyCodegenError) {
            std::cout.flush();
            std::cerr.flush();
            return 1;
        }

        // 产出名取 `[[executable]].name`（缺省已补成项目名）
        std::string exeStem = exe->name;
        std::string exePath = joinUnder(projectBuildDir, exeStem, ".exe");

        bool needLink = !std::filesystem::exists(exePath);
        if (!needLink) {
            try {
                auto exeTime = std::filesystem::last_write_time(exePath);
                if (std::filesystem::last_write_time(objPath) > exeTime) {
                    needLink = true;
                }
                if (!sdkLibPath.empty() && std::filesystem::last_write_time(sdkLibPath) > exeTime) {
                    needLink = true;
                }
                for (const auto& depLib : depLibPaths) {
                    if (!depLib.empty() && std::filesystem::exists(depLib) &&
                        std::filesystem::last_write_time(depLib) > exeTime) {
                        needLink = true;
                        break;
                    }
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

            std::vector<const char*> args = {"lld-link", objPath.c_str(), exeOut.c_str(), "/subsystem:console",
                                             "/entry:mainStartup"};

            if (!sdkLibPath.empty()) {
                args.insert(args.begin() + 2, sdkLibPath.c_str());
            }
            for (const auto& depLib : depLibPaths) {
                args.insert(args.begin() + 2, depLib.c_str());
            }
            for (auto& mo : modObjPaths) {
                args.insert(args.begin() + 2, mo.c_str());
            }
            if (!riurtLibPath.empty()) args.push_back(riurtLibPath.c_str());
            std::vector<ExternalLink> exeLinks = exe->external_links;
            if (!isSdkSelfBuild) {
                exeLinks.insert(exeLinks.end(), sdkLinks.begin(), sdkLinks.end());
                exeLinks.insert(exeLinks.end(), depLinks.begin(), depLinks.end());
            }
            std::vector<std::string> projLibArgs;
            appendExternalLinkArgs(exeLinks, riu.projectRoot(), projLibArgs, args);

            std::string stdoutStr, stderrStr;
            llvm::raw_string_ostream stdoutOS(stdoutStr), stderrOS(stderrStr);

            std::cout << "Link obj: " << exePath << '\n';
            lld::DriverDef driverDef = {.f = lld::WinLink, .d = &lld::coff::link};
            lld::Result result = lldMain(args, stdoutOS, stderrOS, llvm::ArrayRef{driverDef});

            if (result.retCode) {
                llvm::errs() << stderrStr;
                return 1;
            }
            compiled = true;
        }

        {
            std::vector<ExternalLink> exeLinks = exe->external_links;
            if (!isSdkSelfBuild) {
                exeLinks.insert(exeLinks.end(), sdkLinks.begin(), sdkLinks.end());
                exeLinks.insert(exeLinks.end(), depLinks.begin(), depLinks.end());
            }
            if (copyRuntimeDlls(exeLinks, riu.projectRoot(), std::filesystem::path(projectBuildDir)) != 0) {
                return 1;
            }
        }
    }

    if (!compiled) {
        std::cout << "no work to do." << '\n';
    }

    std::cout.flush();
    std::cerr.flush();
    _exit(0);
}

} // namespace riu::cli
