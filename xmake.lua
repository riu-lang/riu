set_project("yux-lang")
set_version("0.1.0")
set_languages("c++23")

add_rules("mode.debug", "mode.release")

set_toolchains("clang")

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

target("yux")
    set_kind("binary")
    add_deps("antlr4_static", "zlib", "llvm")
    add_includedirs("include", "gen")
    add_includedirs(path.join(third_party, "antlr4/runtime/Cpp/runtime/src"))
    add_includedirs(path.join(third_party, "utfcpp/source"))
    add_includedirs(path.join(third_party, "toml11/single_include"))
    add_includedirs(path.join(third_party, "llvm/llvm/include"))
    add_includedirs(path.join(third_party, "llvm/lld/include"))

    add_files("src/*.cpp")
    add_files("src/node/*.cpp")
    add_files("gen/yux/*.cpp")

    add_defines("UNICODE", "NOMINMAX", "ANTLR4CPP_STATIC")

    add_syslinks("ntdll")

    for _, lib in ipairs(llvm_libs) do
        add_links(lib)
    end

    after_load(function (target)
        local llvm_target = target:dep("llvm")
        if llvm_target then
            local llvm_build_dir = path.join(llvm_target:autogendir(), "llvm")
            target:add("includedirs", path.join(llvm_build_dir, "include"), {public = true})
            target:add("linkdirs", path.join(llvm_build_dir, "lib"))
        end
    end)

    set_rundir("$(projectdir)")

includes("tests")
includes("@builtin/xpack")

local third_party_licenses = {
    {lib = "antlr4", license = "LICENSE.txt"},
    {lib = "llvm", license = "LICENSE.TXT"},
    {lib = "utfcpp", license = "LICENSE"},
    {lib = "zlib", license = "LICENSE"},
}

xpack("yux")
    set_formats("zip")
    set_basename("yux-$(version)")

    add_installfiles("build/windows/x64/**/yux.exe", {prefixdir = "bin", flat = true})
    add_installfiles("sdk/sdk.yux", {prefixdir = "sdk", flat = true})
    for _, t in ipairs(third_party_licenses) do
        add_installfiles(path.join("third_party", t.lib, t.license), {prefixdir = path.join("shared/license", t.lib), filename = t.license})
    end
