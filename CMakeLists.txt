# Copyright (c) 2025. Yin-Jinlong@github

cmake_minimum_required(VERSION 4.0)
project(yux-lang CXX C)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "Only clang compiler")
endif()

add_compile_options(-Wno-language-extension-token)

add_subdirectory(tests)
add_subdirectory(libs)

include_directories(
        "include"
        "gen"
        ${LLVM_INCLUDE_DIR}
        ${utfcpp_INCLUDE_DIR}
        ${antlr4_INCLUDE_DIR_STATIC}
)

aux_source_directory(src SRCS)
aux_source_directory(src/ast AST_SRCS)
aux_source_directory(gen/yux GEN_SRCS)
aux_source_directory(src/node NODE_SRCS)


add_executable(yux
        ${SRCS}
        ${AST_SRCS}
        ${GEN_SRCS}
        ${NODE_SRCS}
)

target_compile_features(yux PRIVATE cxx_std_23)

target_compile_definitions(
        yux
        PUBLIC
        UNICODE
        NOMINMAX
        ANTLR4CPP_STATIC
)

add_dependencies(yux
        antlr4_static

        llvm-libraries
)

target_link_libraries(yux
        ntdll
        $<TARGET_FILE:antlr4_static>

        $<TARGET_FILE:LLVMAggressiveInstCombine>
        $<TARGET_FILE:LLVMAnalysis>
        $<TARGET_FILE:LLVMAsmParser>
        $<TARGET_FILE:LLVMAsmPrinter>
        $<TARGET_FILE:LLVMBinaryFormat>
        $<TARGET_FILE:LLVMBitReader>
        $<TARGET_FILE:LLVMBitWriter>
        $<TARGET_FILE:LLVMBitstreamReader>
        $<TARGET_FILE:LLVMCFGuard>
        $<TARGET_FILE:LLVMCGData>
        $<TARGET_FILE:LLVMCodeGen>
        $<TARGET_FILE:LLVMCodeGenTypes>
        $<TARGET_FILE:LLVMMIRParser>
        $<TARGET_FILE:LLVMCore>
        $<TARGET_FILE:LLVMCoroutines>
        $<TARGET_FILE:LLVMDebugInfoBTF>
        $<TARGET_FILE:LLVMDebugInfoCodeView>
        $<TARGET_FILE:LLVMDebugInfoDWARF>
        $<TARGET_FILE:LLVMDebugInfoDWARFLowLevel>
        $<TARGET_FILE:LLVMDebugInfoGSYM>
        $<TARGET_FILE:LLVMDebugInfoLogicalView>
        $<TARGET_FILE:LLVMDebugInfoMSF>
        $<TARGET_FILE:LLVMDebugInfoPDB>
        $<TARGET_FILE:LLVMDemangle>
        $<TARGET_FILE:LLVMExecutionEngine>
        $<TARGET_FILE:LLVMFrontendAtomic>
        $<TARGET_FILE:LLVMFrontendHLSL>
        $<TARGET_FILE:LLVMFrontendOffloading>
        $<TARGET_FILE:LLVMFrontendOpenMP>
        $<TARGET_FILE:LLVMGlobalISel>
        $<TARGET_FILE:LLVMHipStdPar>
        $<TARGET_FILE:LLVMIRPrinter>
        $<TARGET_FILE:LLVMIRReader>
        $<TARGET_FILE:LLVMInstCombine>
        $<TARGET_FILE:LLVMInstrumentation>
        $<TARGET_FILE:LLVMJITLink>
        $<TARGET_FILE:LLVMLinker>
        $<TARGET_FILE:LLVMMC>
        $<TARGET_FILE:LLVMMCA>
        $<TARGET_FILE:LLVMMCDisassembler>
        $<TARGET_FILE:LLVMMCParser>
        $<TARGET_FILE:LLVMObjCARCOpts>
        $<TARGET_FILE:LLVMObject>
        $<TARGET_FILE:LLVMObjectYAML>
        $<TARGET_FILE:LLVMOptDriver>
        $<TARGET_FILE:LLVMOption>
        $<TARGET_FILE:LLVMOrcJIT>
        $<TARGET_FILE:LLVMOrcShared>
        $<TARGET_FILE:LLVMOrcTargetProcess>
        $<TARGET_FILE:LLVMPasses>
        $<TARGET_FILE:LLVMPlugins>
        $<TARGET_FILE:LLVMProfileData>
        $<TARGET_FILE:LLVMRemarks>
        $<TARGET_FILE:LLVMRuntimeDyld>
        $<TARGET_FILE:LLVMSandboxIR>
        $<TARGET_FILE:LLVMScalarOpts>
        $<TARGET_FILE:LLVMSelectionDAG>
        $<TARGET_FILE:LLVMSupport>
        $<TARGET_FILE:LLVMTarget>
        $<TARGET_FILE:LLVMTargetParser>
        $<TARGET_FILE:LLVMTextAPI>
        $<TARGET_FILE:LLVMTextAPIBinaryReader>
        $<TARGET_FILE:LLVMTransformUtils>
        $<TARGET_FILE:LLVMVectorize>
        $<TARGET_FILE:LLVMWindowsDriver>
        $<TARGET_FILE:LLVMWindowsManifest>
        $<TARGET_FILE:LLVMipo>

        # X86 target
        $<TARGET_FILE:LLVMX86AsmParser>
        $<TARGET_FILE:LLVMX86CodeGen>
        $<TARGET_FILE:LLVMX86Desc>
        $<TARGET_FILE:LLVMX86Disassembler>
        $<TARGET_FILE:LLVMX86Info>
        $<TARGET_FILE:LLVMX86TargetMCA>

        # AArch64 target
        $<TARGET_FILE:LLVMAArch64AsmParser>
        $<TARGET_FILE:LLVMAArch64CodeGen>
        $<TARGET_FILE:LLVMAArch64Desc>
        $<TARGET_FILE:LLVMAArch64Disassembler>
        $<TARGET_FILE:LLVMAArch64Info>
        $<TARGET_FILE:LLVMAArch64Utils>

        # ARM target
        $<TARGET_FILE:LLVMARMAsmParser>
        $<TARGET_FILE:LLVMARMCodeGen>
        $<TARGET_FILE:LLVMARMDesc>
        $<TARGET_FILE:LLVMARMDisassembler>
        $<TARGET_FILE:LLVMARMInfo>
        $<TARGET_FILE:LLVMARMUtils>

        # RISCV target
        $<TARGET_FILE:LLVMRISCVAsmParser>
        $<TARGET_FILE:LLVMRISCVCodeGen>
        $<TARGET_FILE:LLVMRISCVDesc>
        $<TARGET_FILE:LLVMRISCVDisassembler>
        $<TARGET_FILE:LLVMRISCVInfo>
        $<TARGET_FILE:LLVMRISCVTargetMCA>

        # PowerPC target
        $<TARGET_FILE:LLVMPowerPCAsmParser>
        $<TARGET_FILE:LLVMPowerPCCodeGen>
        $<TARGET_FILE:LLVMPowerPCDesc>
        $<TARGET_FILE:LLVMPowerPCDisassembler>
        $<TARGET_FILE:LLVMPowerPCInfo>

        # MIPS target
        $<TARGET_FILE:LLVMMipsAsmParser>
        $<TARGET_FILE:LLVMMipsCodeGen>
        $<TARGET_FILE:LLVMMipsDesc>
        $<TARGET_FILE:LLVMMipsDisassembler>
        $<TARGET_FILE:LLVMMipsInfo>

        # SPARC target
        $<TARGET_FILE:LLVMSparcAsmParser>
        $<TARGET_FILE:LLVMSparcCodeGen>
        $<TARGET_FILE:LLVMSparcDesc>
        $<TARGET_FILE:LLVMSparcDisassembler>
        $<TARGET_FILE:LLVMSparcInfo>

        # SystemZ target
        $<TARGET_FILE:LLVMSystemZAsmParser>
        $<TARGET_FILE:LLVMSystemZCodeGen>
        $<TARGET_FILE:LLVMSystemZDesc>
        $<TARGET_FILE:LLVMSystemZDisassembler>
        $<TARGET_FILE:LLVMSystemZInfo>

        # WebAssembly target
        $<TARGET_FILE:LLVMWebAssemblyAsmParser>
        $<TARGET_FILE:LLVMWebAssemblyCodeGen>
        $<TARGET_FILE:LLVMWebAssemblyDesc>
        $<TARGET_FILE:LLVMWebAssemblyDisassembler>
        $<TARGET_FILE:LLVMWebAssemblyInfo>
        $<TARGET_FILE:LLVMWebAssemblyUtils>

        # AMDGPU target
        $<TARGET_FILE:LLVMAMDGPUAsmParser>
        $<TARGET_FILE:LLVMAMDGPUCodeGen>
        $<TARGET_FILE:LLVMAMDGPUDesc>
        $<TARGET_FILE:LLVMAMDGPUDisassembler>
        $<TARGET_FILE:LLVMAMDGPUInfo>
        $<TARGET_FILE:LLVMAMDGPUTargetMCA>
        $<TARGET_FILE:LLVMAMDGPUUtils>

        # NVPTX target
        $<TARGET_FILE:LLVMNVPTXCodeGen>
        $<TARGET_FILE:LLVMNVPTXDesc>
        $<TARGET_FILE:LLVMNVPTXInfo>

        # BPF target
        $<TARGET_FILE:LLVMBPFAsmParser>
        $<TARGET_FILE:LLVMBPFCodeGen>
        $<TARGET_FILE:LLVMBPFDesc>
        $<TARGET_FILE:LLVMBPFDisassembler>
        $<TARGET_FILE:LLVMBPFInfo>

        # Hexagon target
        $<TARGET_FILE:LLVMHexagonAsmParser>
        $<TARGET_FILE:LLVMHexagonCodeGen>
        $<TARGET_FILE:LLVMHexagonDesc>
        $<TARGET_FILE:LLVMHexagonDisassembler>
        $<TARGET_FILE:LLVMHexagonInfo>

        # LoongArch target
        $<TARGET_FILE:LLVMLoongArchAsmParser>
        $<TARGET_FILE:LLVMLoongArchCodeGen>
        $<TARGET_FILE:LLVMLoongArchDesc>
        $<TARGET_FILE:LLVMLoongArchDisassembler>
        $<TARGET_FILE:LLVMLoongArchInfo>

        # AVR target
        $<TARGET_FILE:LLVMAVRAsmParser>
        $<TARGET_FILE:LLVMAVRCodeGen>
        $<TARGET_FILE:LLVMAVRDesc>
        $<TARGET_FILE:LLVMAVRDisassembler>
        $<TARGET_FILE:LLVMAVRInfo>

        # XCore target
        $<TARGET_FILE:LLVMXCoreCodeGen>
        $<TARGET_FILE:LLVMXCoreDesc>
        $<TARGET_FILE:LLVMXCoreDisassembler>
        $<TARGET_FILE:LLVMXCoreInfo>

        # Lanai target
        $<TARGET_FILE:LLVMLanaiAsmParser>
        $<TARGET_FILE:LLVMLanaiCodeGen>
        $<TARGET_FILE:LLVMLanaiDesc>
        $<TARGET_FILE:LLVMLanaiDisassembler>
        $<TARGET_FILE:LLVMLanaiInfo>

        # MSP430 target
        $<TARGET_FILE:LLVMMSP430AsmParser>
        $<TARGET_FILE:LLVMMSP430CodeGen>
        $<TARGET_FILE:LLVMMSP430Desc>
        $<TARGET_FILE:LLVMMSP430Disassembler>
        $<TARGET_FILE:LLVMMSP430Info>

        # SPIRV target
        $<TARGET_FILE:LLVMSPIRVAnalysis>
        $<TARGET_FILE:LLVMSPIRVCodeGen>
        $<TARGET_FILE:LLVMSPIRVDesc>
        $<TARGET_FILE:LLVMSPIRVInfo>

        # VE target
        $<TARGET_FILE:LLVMVEAsmParser>
        $<TARGET_FILE:LLVMVECodeGen>
        $<TARGET_FILE:LLVMVEDesc>
        $<TARGET_FILE:LLVMVEDisassembler>
        $<TARGET_FILE:LLVMVEInfo>

        # lld
        $<TARGET_FILE:lldCommon>
        $<TARGET_FILE:lldCOFF>
        $<TARGET_FILE:lldELF>
        $<TARGET_FILE:lldMachO>
        $<TARGET_FILE:lldWasm>
        $<TARGET_FILE:LLVMLibDriver>
        $<TARGET_FILE:LLVMLTO>
        $<TARGET_FILE:LLVMDTLTO>
)
