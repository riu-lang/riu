-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

local third_party = path.join(os.projectdir(), "third_party")

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

        local llvm_src_dir = path.join(third_party, "llvm")
        local llvm_build_dir = path.join(target:autogendir(), "llvm")
        local cmake_cache = path.join(llvm_build_dir, "CMakeCache.txt")
        local stamp_file = path.join(llvm_build_dir, "yux-llvm-src.stamp")
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

        local function read_src_commit()
            local out = os.iorunv("git", {"-C", llvm_src_dir, "rev-parse", "HEAD"})
            if not out then
                raise("failed to read llvm source commit (git rev-parse HEAD in " .. llvm_src_dir .. ")")
            end
            return out:trim()
        end

        local function read_stamp()
            if not os.isfile(stamp_file) then
                return nil
            end
            return io.readfile(stamp_file):trim()
        end

        local cmake = find_tool("cmake")
        if not cmake then
            raise("cmake not found")
        end

        local src_commit = read_src_commit()
        local stamped = read_stamp()
        local src_dirty = (stamped ~= src_commit)
        if src_dirty then
            if stamped then
                cprint("${yellow}  [llvm] source commit changed: %s → %s${clear}",
                    stamped:sub(1, 8), src_commit:sub(1, 8))
            else
                cprint("${yellow}  [llvm] no source stamp; will configure/build${clear}")
            end
            if os.isfile(cmake_cache) then
                os.rm(cmake_cache)
            end
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
                "-G", "Ninja",
                path.join(llvm_src_dir, "llvm")
            }
            os.vrunv(cmake.program, cmake_configs, {curdir = llvm_build_dir})
        end

        local all_exist, missing_lib = check_libs_exist()
        if src_dirty or not all_exist then
            if not all_exist and missing_lib then
                cprint("${dim}  [llvm] missing lib: %s${clear}", missing_lib)
            end
            os.vrunv(cmake.program, {"--build", ".", "-j", tostring(os.cpuinfo().ncpu - 2)}, {curdir = llvm_build_dir})
            io.writefile(stamp_file, src_commit .. "\n")
            cprint("${green}  [llvm] stamped %s${clear}", src_commit:sub(1, 8))
        end
    end)

-- 主编译器：yux.exe
-- 依赖 LLVM（codegen）+ 前端（sema/tools）+ 运行时（yuxrt）
-- compiler/*.cpp 直接编入本 target，不再经过中间 .lib（仅 yux 一个消费者，无复用价值）
target("yux")
    set_kind("binary")
    add_deps("yux_frontend", "yuxrt", "zlib", "llvm")
    add_defines("YUX_VERSION=\"" .. _YUX_VERSION_STR .. "\"")

    -- compiler/ + cli/ 的内部 include（private 即可，binary 不导出）
    add_includedirs(".")
    add_includedirs(path.join(third_party, "cli11/include"))
    add_includedirs(path.join(third_party, "llvm/llvm/include"))
    add_includedirs(path.join(third_party, "llvm/lld/include"))

    add_files("main.cpp")
    add_files("cli/*.cpp")
    add_files("compiler/*.cpp")
    add_files("compiler/call/*.cpp")
    add_files("compiler/expr/*.cpp")

    add_syslinks("ntdll")

    for _, lib in ipairs(llvm_libs) do
        add_links(lib)
    end

    after_load(function (target)
        local llvm_target = target:dep("llvm")
        if llvm_target then
            local llvm_build_dir = path.join(llvm_target:autogendir(), "llvm")
            target:add("includedirs", path.join(llvm_build_dir, "include"))
            target:add("linkdirs", path.join(llvm_build_dir, "lib"))
        end
    end)

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
