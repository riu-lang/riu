-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- 版本号
-- 正在开发的v...-alpha
-- 已完成v...

set_project("yux-lang")
set_version("0.18.0-alpha")
set_languages("c++23")

add_rules("mode.debug", "mode.release")

set_toolchains("clang")

-- 输出目录：build/<plat>/<arch>/<profile>/{bin,lib}
-- 编译期 SDK symlink 在 xmake 内维护（不再由 init.js 手写）
set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)", { bindir = "bin", libdir = "lib" })

add_cxxflags("-Wno-language-extension-token", {force = true})

local third_party = path.join(os.projectdir(), "third_party")

target("antlr4_static")
    set_kind("static")
    set_languages("c++17")
    add_defines("ANTLR4CPP_STATIC")
    add_includedirs(path.join(third_party, "antlr4/runtime/Cpp/runtime/src"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/atn/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/dfa/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/misc/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/internal/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/support/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/tree/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/tree/pattern/*.cpp"))
    add_files(path.join(third_party, "antlr4/runtime/Cpp/runtime/src/tree/xpath/*.cpp"))
    add_cxxflags("-include chrono", {force = true})
    if is_plat("windows") then
        add_syslinks("shlwapi")
    end

target("zlib")
    set_kind("static")
    add_includedirs(path.join(third_party, "zlib"), {public = true})
    add_files(path.join(third_party, "zlib/*.c"))
    add_defines("ZLIB_COMPAT")


local llvm_libs = {
    "LLVMAggressiveInstCombine", "LLVMAnalysis", "LLVMAsmParser", "LLVMAsmPrinter",
    "LLVMBinaryFormat", "LLVMBitReader", "LLVMBitWriter", "LLVMBitstreamReader",
    "LLVMCFGuard", "LLVMCGData", "LLVMCodeGen", "LLVMCodeGenTypes",
    "LLVMMIRParser", "LLVMCore", "LLVMCoroutines", "LLVMDebugInfoBTF",
    "LLVMDebugInfoCodeView", "LLVMDebugInfoDWARF", "LLVMDebugInfoDWARFLowLevel",
    "LLVMDebugInfoGSYM", "LLVMDebugInfoLogicalView", "LLVMDebugInfoMSF",
    "LLVMDebugInfoPDB", "LLVMDemangle", "LLVMExecutionEngine", "LLVMFrontendAtomic",
    "LLVMFrontendHLSL", "LLVMFrontendOffloading", "LLVMFrontendOpenMP",
    "LLVMGlobalISel", "LLVMHipStdPar", "LLVMIRPrinter", "LLVMIRReader",
    "LLVMInstCombine", "LLVMInstrumentation", "LLVMJITLink", "LLVMLinker",
    "LLVMMC", "LLVMMCA", "LLVMMCDisassembler", "LLVMMCParser",
    "LLVMObjCARCOpts", "LLVMObject", "LLVMObjectYAML", "LLVMOptDriver",
    "LLVMOption", "LLVMOrcJIT", "LLVMOrcShared", "LLVMOrcTargetProcess",
    "LLVMPasses", "LLVMPlugins", "LLVMProfileData", "LLVMRemarks",
    "LLVMRuntimeDyld", "LLVMSandboxIR", "LLVMScalarOpts", "LLVMSelectionDAG",
    "LLVMSupport", "LLVMTarget", "LLVMTargetParser", "LLVMTextAPI",
    "LLVMTextAPIBinaryReader", "LLVMTransformUtils", "LLVMVectorize",
    "LLVMWindowsDriver", "LLVMWindowsManifest", "LLVMipo",
    "LLVMX86AsmParser", "LLVMX86CodeGen", "LLVMX86Desc", "LLVMX86Disassembler",
    "LLVMX86Info", "LLVMX86TargetMCA",
    "LLVMAArch64AsmParser", "LLVMAArch64CodeGen", "LLVMAArch64Desc",
    "LLVMAArch64Disassembler", "LLVMAArch64Info", "LLVMAArch64Utils",
    "LLVMARMAsmParser", "LLVMARMCodeGen", "LLVMARMDesc", "LLVMARMDisassembler",
    "LLVMARMInfo", "LLVMARMUtils",
    "LLVMRISCVAsmParser", "LLVMRISCVCodeGen", "LLVMRISCVDesc",
    "LLVMRISCVDisassembler", "LLVMRISCVInfo", "LLVMRISCVTargetMCA",
    "LLVMPowerPCAsmParser", "LLVMPowerPCCodeGen", "LLVMPowerPCDesc",
    "LLVMPowerPCDisassembler", "LLVMPowerPCInfo",
    "LLVMMipsAsmParser", "LLVMMipsCodeGen", "LLVMMipsDesc",
    "LLVMMipsDisassembler", "LLVMMipsInfo",
    "LLVMSparcAsmParser", "LLVMSparcCodeGen", "LLVMSparcDesc",
    "LLVMSparcDisassembler", "LLVMSparcInfo",
    "LLVMSystemZAsmParser", "LLVMSystemZCodeGen", "LLVMSystemZDesc",
    "LLVMSystemZDisassembler", "LLVMSystemZInfo",
    "LLVMWebAssemblyAsmParser", "LLVMWebAssemblyCodeGen", "LLVMWebAssemblyDesc",
    "LLVMWebAssemblyDisassembler", "LLVMWebAssemblyInfo", "LLVMWebAssemblyUtils",
    "LLVMAMDGPUAsmParser", "LLVMAMDGPUCodeGen", "LLVMAMDGPUDesc",
    "LLVMAMDGPUDisassembler", "LLVMAMDGPUInfo", "LLVMAMDGPUTargetMCA", "LLVMAMDGPUUtils",
    "LLVMNVPTXCodeGen", "LLVMNVPTXDesc", "LLVMNVPTXInfo",
    "LLVMBPFAsmParser", "LLVMBPFCodeGen", "LLVMBPFDesc", "LLVMBPFDisassembler", "LLVMBPFInfo",
    "LLVMHexagonAsmParser", "LLVMHexagonCodeGen", "LLVMHexagonDesc",
    "LLVMHexagonDisassembler", "LLVMHexagonInfo",
    "LLVMLoongArchAsmParser", "LLVMLoongArchCodeGen", "LLVMLoongArchDesc",
    "LLVMLoongArchDisassembler", "LLVMLoongArchInfo",
    "LLVMAVRAsmParser", "LLVMAVRCodeGen", "LLVMAVRDesc",
    "LLVMAVRDisassembler", "LLVMAVRInfo",
    "LLVMXCoreCodeGen", "LLVMXCoreDesc", "LLVMXCoreDisassembler", "LLVMXCoreInfo",
    "LLVMLanaiAsmParser", "LLVMLanaiCodeGen", "LLVMLanaiDesc",
    "LLVMLanaiDisassembler", "LLVMLanaiInfo",
    "LLVMMSP430AsmParser", "LLVMMSP430CodeGen", "LLVMMSP430Desc",
    "LLVMMSP430Disassembler", "LLVMMSP430Info",
    "LLVMSPIRVAnalysis", "LLVMSPIRVCodeGen", "LLVMSPIRVDesc", "LLVMSPIRVInfo",
    "LLVMVEAsmParser", "LLVMVECodeGen", "LLVMVEDesc", "LLVMVEDisassembler", "LLVMVEInfo",
    "lldCommon", "lldCOFF", "lldELF", "lldMachO", "lldWasm",
    "LLVMLibDriver", "LLVMLTO", "LLVMDTLTO"
}

target("llvm")
    set_kind("phony")
    set_policy("build.fence", true)

    on_build(function (target)
        import("lib.detect.find_tool")

        local llvm_build_dir = path.join(target:autogendir(), "llvm")
        local cmake_cache = path.join(llvm_build_dir, "CMakeCache.txt")
        local lib_dir = path.join(llvm_build_dir, "lib")

        local function check_libs_exist()
            for _, lib in ipairs(llvm_libs) do
                local lib_path = path.join(lib_dir, lib .. ".lib")
                if not os.isfile(lib_path) then
                    return false, lib
                end
            end
            return true, nil
        end

        local cmake = find_tool("cmake")
        if not cmake then
            raise("cmake not found")
        end

        if not os.isfile(cmake_cache) then
            os.mkdir(llvm_build_dir)
            local cmake_configs = {
                "-DCMAKE_BUILD_TYPE=" .. (is_mode("debug") and "Debug" or "Release"),
                "-DCMAKE_INSTALL_PREFIX=" .. llvm_build_dir,
                "-DLLVM_ENABLE_PROJECTS=lld",
                "-DLLVM_ENABLE_RTTI=OFF",
                "-DLLVM_ENABLE_EH=OFF",
                "-DLLVM_INCLUDE_EXAMPLES=OFF",
                "-DLLVM_INCLUDE_TESTS=OFF",
                "-DLLVM_INCLUDE_BENCHMARKS=OFF",
                "-DLLVM_ENABLE_RUNTIMES=libc",
                "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=TRUE",
                "-G", "Ninja",
                path.join(third_party, "llvm", "llvm")
            }
            os.vrunv(cmake.program, cmake_configs, {curdir = llvm_build_dir})
        end

        local all_exist, missing_lib = check_libs_exist()
        if not all_exist then
            os.vrunv(cmake.program, {"--build", ".", "-j", tostring(os.cpuinfo().ncpu - 2)}, {curdir = llvm_build_dir})
        end
    end)

-- 前端静态库：词法/语法（ANTLR）+ AST + 格式化器 + 诊断/符号建议 + 模块加载（Yux 类）
-- 不引用任何 LLVM 头/链接，使 yux-lsp 可以脱离 LLVM 编译链路构建
target("yux_frontend")
    set_kind("static")
    add_deps("antlr4_static")
    add_includedirs("src", "gen", {public = true})
    add_includedirs(path.join(third_party, "antlr4/runtime/Cpp/runtime/src"), {public = true})
    add_includedirs(path.join(third_party, "utfcpp/source"), {public = true})
    add_includedirs(path.join(third_party, "toml11/single_include"), {public = true})
    add_includedirs(path.join(third_party, "nlohmann_json/single_include"), {public = true})

    add_files(
        "src/ast/ast_builder.cpp",
        "src/ast/ast_builder_decl.cpp",
        "src/ast/ast_builder_struct.cpp",
        "src/ast/ast_builder_fn.cpp",
        "src/ast/ast_builder_stmt.cpp",
        "src/ast/ast_builder_expr.cpp",
        "src/ast/ast_builder_type.cpp",
        "src/ast/yux.cpp",
        "src/tools/diagnostic.cpp",
        "src/tools/format/doc.cpp",
        "src/tools/format/trivia.cpp",
        "src/tools/format/render.cpp",
        "src/tools/format/printer.cpp",
        "src/tools/syntax_error_listener.cpp",
        "src/ast/mangler.cpp",
        "src/tools/pkg_cache.cpp",
        "src/tools/sdk_loader.cpp",
        "src/analyzer/borrow_checker.cpp",
        "src/analyzer/const_mut_checker.cpp",
        "src/analyzer/flow_terminate_checker.cpp",
        "src/analyzer/symbol_suggest.cpp",
        "src/analyzer/spec_registry.cpp",
        "src/analyzer/spec_impl_checker.cpp",
        "src/sema/sema_pass.cpp",
        "src/sema/call_resolve.cpp",
        "src/sema/const_eval.cpp"
    )
    add_files("src/ast/node/*.cpp")
    add_files("gen/yux/*.cpp")

    add_defines("UNICODE", "NOMINMAX", "ANTLR4CPP_STATIC", {public = true})

-- codegen 静态库：所有依赖 LLVM 的实现（compiler*.cpp）
-- yux 主二进制依赖之；yux-lsp 不依赖
target("yux_codegen")
    set_kind("static")
    add_deps("yux_frontend", "zlib", "llvm")
    add_includedirs("src", {public = true})
    add_includedirs(path.join(third_party, "llvm/llvm/include"), {public = true})
    add_includedirs(path.join(third_party, "llvm/lld/include"), {public = true})

    add_files(
        "src/compiler/*.cpp",
        "src/compiler/call/*.cpp",
        "src/compiler/expr/*.cpp"
    )

    add_syslinks("ntdll", {public = true})

    for _, lib in ipairs(llvm_libs) do
        add_links(lib, {public = true})
    end

    after_load(function (target)
        local llvm_target = target:dep("llvm")
        if llvm_target then
            local llvm_build_dir = path.join(llvm_target:autogendir(), "llvm")
            target:add("includedirs", path.join(llvm_build_dir, "include"), {public = true})
            target:add("linkdirs", path.join(llvm_build_dir, "lib"), {public = true})
        end
    end)

target("yux-test-runner")
    set_kind("binary")
    add_files("src/tools/runner_main.cpp")

target("yux")
    set_kind("binary")
    add_deps("yux_codegen", "yuxrt")
    add_includedirs(path.join(third_party, "cli11/include"))
    add_files("src/main.cpp")
    add_files("src/cli/*.cpp")
    set_rundir("$(projectdir)")
    before_build(function (target)
        -- SDK symlink: build/<plat>/<arch>/<mode>/sdk → <repo>/sdk
        -- 替代旧 sync-deps.js 在 build/<plat>/<arch>/ 平铺 symlink 的方式
        local sdk_src = path.join(os.projectdir(), "sdk")
        local sdk_link = path.join(path.directory(target:targetdir()), "sdk")
        if not os.isdir(sdk_link) then
            os.ln(sdk_src, sdk_link)
            cprint("${dim}  [sdk-link] %s → sdk/${clear}", sdk_link)
        end
    end)

target("yux-lsp")
    set_kind("binary")
    add_deps("yux_frontend")
    add_includedirs("src", {public = true})
    add_files("src/lsp/*.cpp")
    set_rundir("$(projectdir)")
    -- Claude Code 插件走独立 server 进程 yux-lsp-claude (避免与 yux-vscode
    -- clientInfo 等串扰; 见 plugins/yux-claude-code/README.md). 同源, 仅文件名
    -- 不同 —— 直接构建后复制, 避免历史上 "手动 copy 后忘记同步, 旧副本不识别
    -- 新 g4 (例 #Fallible(E) 单参注解), Claude Code 看到误报" 一类的坑.
    -- Windows 上若 .exe 正在运行: 不能覆盖也不能删, 但可以 rename. 先把占用
    -- 的副本改名到 .old, 再 cp 新文件, 打警告提示用户重启 Claude Code.
    after_build(function (target)
        import("core.base.option")
        local src = target:targetfile()
        local dst = path.join(path.directory(src), "yux-lsp-claude" .. (is_host("windows") and ".exe" or ""))
        if not os.isfile(src) then return end
        local ok = try { function () os.cp(src, dst); return true end }
        if ok then
            cprint("${dim}  [yux-lsp-claude] %s${clear}", dst)
            return
        end
        -- 占用: rename → cp
        local stale = dst .. ".old"
        os.tryrm(stale)
        local moved = try { function () os.mv(dst, stale); return true end }
        if not moved then
            cprint("${yellow}warning: yux-lsp-claude 既无法覆盖也无法重命名 (%s); 跳过${clear}", dst)
            return
        end
        try { function () os.cp(src, dst) end }
        cprint("${yellow}warning: yux-lsp-claude.exe 正被占用, 已重命名旧副本到 %s 并写入新文件; 重启 Claude Code 后生效${clear}", stale)
    end)

-- yux-ast: 独立的 parse tree 转储工具, 仅 ANTLR 词法 + 语法, 不做 AST/语义/codegen
target("yux-ast")
    set_kind("binary")
    add_deps("yux_frontend")
    add_includedirs(path.join(third_party, "cli11/include"))
    add_files("src/tools/ast_main.cpp")
    set_rundir("$(projectdir)")

-- yux-check: 单文件快速语义检查 (阶段 0), 0 LLVM 依赖.
-- 详见 CURRENT.md "yux-check 最小可用 exe" 一节.
target("yux-check")
    set_kind("binary")
    add_deps("yux_frontend")
    add_includedirs(path.join(third_party, "cli11/include"))
    add_files("src/tools/check_main.cpp")
    set_rundir("$(projectdir)")

includes("tests")
includes("yuxrt")
includes("@builtin/xpack")

local third_party_licenses = {
    {lib = "antlr4", license = "LICENSE.txt"},
    {lib = "cli11", license = "LICENSE"},
    {lib = "llvm", license = "LICENSE.TXT", subdir = "llvm"},
    {lib = "utfcpp", license = "LICENSE"},
    {lib = "zlib", license = "LICENSE"},
    {lib = "toml11", license = "LICENSE"},
    {lib = "nlohmann_json", license = "LICENSE.MIT"},
}

xpack("yux")
    set_formats("zip")
    set_basename("yux-$(version)")

    add_installfiles("build/windows/x64/**/yux.exe", {prefixdir = "bin", flat = true})
    add_installfiles("build/windows/x64/**/yux-lsp.exe", {prefixdir = "bin", flat = true})
    add_installfiles("build/windows/x64/**/yuxrt.lib", {prefixdir = "lib", flat = true})
    add_installfiles("sdk/(**)", {prefixdir = "sdk"})
    add_installfiles("examples/(**)", {prefixdir = "examples"})
    add_installfiles("README.md", {prefixdir = "."})
    add_installfiles("LICENSE.txt", {prefixdir = "."})
    add_installfiles("docs/(**)", {prefixdir = "docs"})
    for _, t in ipairs(third_party_licenses) do
        local lib_path = t.subdir and path.join("third_party", t.lib, t.subdir, t.license)
                         or path.join("third_party", t.lib, t.license)
        add_installfiles(lib_path, {prefixdir = path.join("shared/licenses", t.lib), filename = t.license})
    end
