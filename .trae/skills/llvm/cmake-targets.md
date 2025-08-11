# LLVM CMake 构建配置

本文档记录LLVM和LLD的CMake目标和选项，帮助配置和构建LLVM项目。

## LLVM CMake 选项

### 构建类型与优化

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `CMAKE_BUILD_TYPE` | STRING | Release | 构建类型：Debug/Release/RelWithDebInfo/MinSizeRel |
| `LLVM_ENABLE_ASSERTIONS` | BOOL | Debug=ON, 其他=OFF | 启用断言检查 |
| `LLVM_ENABLE_EXPENSIVE_CHECKS` | BOOL | OFF | 启用昂贵检查（需要断言） |
| `LLVM_ENABLE_LTO` | STRING | OFF | 链接时优化：OFF/Thin/Full |
| `LLVM_ENABLE_FATLTO` | BOOL | OFF | 使用-ffat-lto-objects构建 |
| `LLVM_OPTIMIZE_SANITIZED_BUILDS` | BOOL | ON | Sanitizer构建时使用-O1 |

### 目标架构

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_TARGETS_TO_BUILD` | STRING | all | 要构建的目标架构列表 |
| `LLVM_EXPERIMENTAL_TARGETS_TO_BUILD` | STRING | "" | 实验性目标架构列表 |
| `LLVM_TARGET_ARCH` | STRING | host | JIT目标架构 |
| `LLVM_DEFAULT_TARGET_TRIPLE` | STRING | host triple | 默认目标三元组 |

**支持的目标架构**：
- 核心目标：AArch64, AMDGPU, ARM, AVR, BPF, Hexagon, Lanai, LoongArch, Mips, MSP430, NVPTX, PowerPC, RISCV, Sparc, SPIRV, SystemZ, VE, WebAssembly, X86, XCore
- 实验性目标：ARC, CSKY, DirectX, M68k, Xtensa

### 项目与运行时

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_ENABLE_PROJECTS` | STRING | "" | 启用的项目列表 |
| `LLVM_ENABLE_RUNTIMES` | STRING | "" | 启用的运行时列表 |
| `LLVM_EXTERNAL_PROJECTS` | STRING | "" | 外部项目列表 |

**可用项目**：bolt, clang, clang-tools-extra, flang, libc, lld, lldb, mlir, openmp, polly

**可用运行时**：libc, libunwind, libcxxabi, libcxx, compiler-rt, openmp, llvm-libgcc, offload, flang-rt, libclc, libsycl, orc-rt

### 库构建选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `BUILD_SHARED_LIBS` | BOOL | OFF | 构建共享库而非静态库 |
| `LLVM_BUILD_LLVM_DYLIB` | BOOL | OFF | 构建libLLVM动态库 |
| `LLVM_LINK_LLVM_DYLIB` | BOOL | OFF | 工具链接到libLLVM动态库 |
| `LLVM_BUILD_LLVM_C_DYLIB` | BOOL | MSVC=ON, 其他=OFF | 构建LLVM-C动态库 |
| `LLVM_DYLIB_COMPONENTS` | STRING | all | libLLVM包含的组件 |
| `LLVM_ENABLE_PIC` | BOOL | ON | 构建位置无关代码 |
| `LLVM_ENABLE_PLUGINS` | BOOL | 取决于平台 | 启用插件支持 |

### 并行构建控制

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_PARALLEL_COMPILE_JOBS` | STRING | "" | 最大并行编译任务数 |
| `LLVM_PARALLEL_LINK_JOBS` | STRING | "" | 最大并行链接任务数 |
| `LLVM_PARALLEL_TABLEGEN_JOBS` | STRING | "" | 最大并行TableGen任务数 |
| `LLVM_RAM_PER_COMPILE_JOB` | STRING | "" | 每个编译任务的内存(MB) |
| `LLVM_RAM_PER_LINK_JOB` | STRING | "" | 每个链接任务的内存(MB) |

### 外部依赖

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_ENABLE_ZLIB` | STRING | ON | 使用zlib压缩 |
| `LLVM_ENABLE_ZSTD` | STRING | ON | 使用zstd压缩 |
| `LLVM_ENABLE_LIBXML2` | STRING | ON | 使用libxml2 |
| `LLVM_ENABLE_CURL` | STRING | OFF | 使用libcurl HTTP客户端 |
| `LLVM_ENABLE_FFI` | BOOL | OFF | 使用libffi调用外部函数 |
| `LLVM_ENABLE_LIBPFM` | BOOL | ON | 使用libpfm性能计数器 |
| `LLVM_ENABLE_THREADS` | BOOL | ON | 启用线程支持 |
| `LLVM_ENABLE_Z3_SOLVER` | BOOL | 取决于Z3 | 启用Z3约束求解器 |

### 工具链选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_USE_LINKER` | STRING | "" | 指定链接器（lld/gold/mold） |
| `LLVM_ENABLE_LLD` | BOOL | OFF | 使用lld作为链接器 |
| `LLVM_CCACHE_BUILD` | BOOL | OFF | 启用ccache构建 |
| `LLVM_USE_INTEL_JITEVENTS` | BOOL | OFF | 使用Intel JIT API |
| `LLVM_USE_OPROFILE` | BOOL | OFF | 使用OProfile JIT接口 |
| `LLVM_USE_PERF` | BOOL | OFF | 使用perf JIT接口 |

### 安装选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `CMAKE_INSTALL_PREFIX` | PATH | /usr/local | 安装前缀 |
| `LLVM_LIBDIR_SUFFIX` | STRING | "" | 库目录后缀（如64） |
| `LLVM_INSTALL_UTILS` | BOOL | OFF | 安装工具二进制 |
| `LLVM_INSTALL_TOOLCHAIN_ONLY` | BOOL | OFF | 仅安装工具链文件 |
| `LLVM_TOOLS_INSTALL_DIR` | STRING | bin | 工具安装目录 |

### 测试与文档

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_INCLUDE_TESTS` | BOOL | ON | 生成测试目标 |
| `LLVM_BUILD_TESTS` | BOOL | OFF | 构建单元测试 |
| `LLVM_INCLUDE_DOCS` | BOOL | ON | 生成文档目标 |
| `LLVM_BUILD_DOCS` | BOOL | OFF | 构建文档 |
| `LLVM_ENABLE_SPHINX` | BOOL | OFF | 使用Sphinx生成文档 |
| `LLVM_ENABLE_DOXYGEN` | BOOL | OFF | 使用Doxygen生成文档 |
| `LLVM_INSTALL_GTEST` | BOOL | OFF | 安装LLVM gtest库 |

### 调试选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_ENABLE_BACKTRACES` | BOOL | ON | 崩溃时启用回溯 |
| `LLVM_ENABLE_CRASH_OVERRIDES` | BOOL | ON | 启用崩溃覆盖 |
| `LLVM_ENABLE_DUMP` | BOOL | OFF | 禁用断言时启用dump函数 |
| `LLVM_ENABLE_DEBUGLOC_COVERAGE_TRACKING` | STRING | DISABLED | DebugLoc覆盖跟踪 |
| `LLVM_ABI_BREAKING_CHECKS` | STRING | WITH_ASSERTS | ABI破坏检查 |

### Sanitizer选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_USE_SANITIZER` | STRING | "" | 使用的Sanitizer类型 |
| `LLVM_UBSAN_FLAGS` | STRING | ... | UBSan编译标志 |

### 其他选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLVM_ENABLE_RTTI` | BOOL | OFF | 启用RTTI |
| `LLVM_ENABLE_EH` | BOOL | OFF | 启用异常处理 |
| `LLVM_ENABLE_MODULES` | BOOL | OFF | 启用C++模块 |
| `LLVM_ENABLE_BINDINGS` | BOOL | ON | 构建绑定 |
| `LLVM_OPTIMIZED_TABLEGEN` | BOOL | OFF | 优化构建TableGen |
| `LLVM_BUILD_EXAMPLES` | BOOL | OFF | 构建示例程序 |
| `LLVM_BUILD_BENCHMARKS` | BOOL | OFF | 构建基准测试 |

## LLVM CMake 目标

### 核心库目标

| 目标 | 说明 |
|------|------|
| `LLVMCore` | IR核心功能 |
| `LLVMSupport` | 基础支持库 |
| `LLVMIRReader` | IR读取器 |
| `LLVMBitReader` | Bitcode读取器 |
| `LLVMBitWriter` | Bitcode写入器 |
| `LLVMAnalysis` | 分析Pass |
| `LLVMTransformUtils` | 转换工具 |
| `LLVMScalar` | 标量优化Pass |
| `LLVMVectorize` | 向量化Pass |
| `LLVMipo` | 过程间优化Pass |

### 代码生成库目标

| 目标 | 说明 |
|------|------|
| `LLVMCodeGen` | 代码生成核心 |
| `LLVMSelectionDAG` | SelectionDAG |
| `LLVMGlobalISel` | GlobalISel |
| `LLVMMC` | MC层核心 |
| `LLVMMCParser` | MC解析器 |
| `LLVMMCDisassembler` | MC反汇编器 |
| `LLVMAsmPrinter` | 汇编打印器 |

### 目标后端库目标

| 目标 | 说明 |
|------|------|
| `LLVMX86CodeGen` | X86后端 |
| `LLVMAArch64CodeGen` | AArch64后端 |
| `LLVMARMCodeGen` | ARM后端 |
| `LLVMRISCVCodeGen` | RISC-V后端 |
| `LLVMNVPTXCodeGen` | NVPTX后端 |
| `LLVMAMDGPUCodeGen` | AMDGPU后端 |
| `LLVMWebAssemblyCodeGen` | WebAssembly后端 |

### IR相关库目标

| 目标 | 说明 |
|------|------|
| `LLVMAsmParser` | LLVM汇编解析器 |
| `LLVMIRPrinter` | IR打印支持 |
| `LLVMVerifier` | IR验证器 |
| `LLVMDemangle` | 符号解混淆 |

### LTO库目标

| 目标 | 说明 |
|------|------|
| `LLVMLTO` | LTO接口 |
| `LLVMLTOBackend` | LTO后端 |

### 工具目标（分类）

#### 编译与优化工具

| 目标 | 说明 |
|------|------|
| `llc` | LLVM静态编译器 |
| `opt` | LLVM优化器 |
| `llvm-as` | LLVM汇编到bitcode |
| `llvm-dis` | LLVM bitcode到汇编 |
| `llvm-link` | LLVM bitcode链接器 |
| `llvm-extract` | 提取函数/全局变量 |
| `bugpoint` | 减少测试用例工具 |

#### 分析工具

| 目标 | 说明 |
|------|------|
| `llvm-profdata` | Profile数据工具 |
| `llvm-profgen` | Profile生成工具 |
| `llvm-cov` | 代码覆盖率工具 |
| `llvm-dwarfdump` | DWARF调试信息转储 |
| `llvm-symbolizer` | 地址符号化工具 |
| `llvm-objdump` | 对象文件反汇编 |
| `llvm-readobj` | 对象文件读取器 |
| `llvm-nm` | 符号表列出工具 |
| `llvm-size` | 段大小查看工具 |
| `llvm-strings` | 字符串打印工具 |

#### 对象文件工具

| 目标 | 说明 |
|------|------|
| `llvm-ar` | 归档工具 |
| `llvm-ranlib` | 归档索引工具 |
| `llvm-objcopy` | 对象文件复制/修改 |
| `llvm-strip` | 符号剥离工具 |
| `llvm-install-name-tool` | 动态库install_name工具 |

#### LTO工具

| 目标 | 说明 |
|------|------|
| `llvm-lto` | LTO工具 |
| `llvm-lto2` | LTO工具（新接口） |
| `llvm-bcanalyzer` | Bitcode分析器 |

#### JIT工具

| 目标 | 说明 |
|------|------|
| `lli` | LLVM解释器/JIT |
| `llvm-jitlink` | JIT链接工具 |
| `llvm-jitlink-executor` | JIT链接执行器 |

#### 其他工具

| 目标 | 说明 |
|------|------|
| `llvm-config` | LLVM配置信息 |
| `llvm-tblgen` | TableGen工具 |
| `llvm-mca` | 机器代码分析器 |
| `llvm-diff` | IR差异比较 |
| `llvm-cat` | Bitcode连接工具 |
| `llvm-cxxfilt` | C++符号解混淆 |
| `llvm-ifs` | 接口存根工具 |
| `llvm-dwp` | DWARF打包工具 |
| `dsymutil` | dSYM工具（Darwin） |

### 元目标

| 目标 | 说明 |
|------|------|
| `all` | 构建所有默认目标 |
| `install` | 安装构建产物 |
| `install-strip` | 安装并剥离符号 |
| `check-all` | 运行所有测试 |
| `check-llvm-unit` | 运行LLVM单元测试 |
| `clean` | 清理构建文件 |
| `llvm-headers` | 安装头文件 |
| `llvm-libraries` | 安装库文件 |

### 动态库目标

| 目标 | 说明 |
|------|------|
| `LLVM` | libLLVM动态库 |
| `LLVM-C` | LLVM-C接口动态库 |

## LLD CMake 选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `LLD_BUILD_TOOLS` | BOOL | ON | 构建LLD工具 |
| `LLD_USE_VTUNE` | BOOL | OFF | 启用VTune任务跟踪 |
| `LLD_DEFAULT_LD_LLD_IS_MINGW` | BOOL | OFF | ld.lld默认使用MinGW后端 |
| `LLD_VENDOR` | STRING | "" | 版本信息中的供应商文本 |
| `LLD_TOOLS_INSTALL_DIR` | PATH | bin | 工具安装目录 |

## LLD CMake 目标

### 库目标

| 目标 | 说明 |
|------|------|
| `lldCommon` | LLD通用库 |
| `lldCOFF` | COFF链接器库 |
| `lldELF` | ELF链接器库 |
| `lldMachO` | Mach-O链接器库 |
| `lldMinGW` | MinGW链接器库 |
| `lldWasm` | WebAssembly链接器库 |

### 工具目标

| 目标 | 说明 |
|------|------|
| `lld` | LLD链接器入口 |
| `ld.lld` | ELF链接器 |
| `ld64.lld` | Mach-O链接器 |
| `lld-link` | COFF链接器 |
| `wasm-ld` | WebAssembly链接器 |

### 元目标

| 目标 | 说明 |
|------|------|
| `lld-headers` | 安装LLD头文件 |
| `lld-libraries` | 安装LLD库文件 |
| `LLDUnitTests` | LLD单元测试 |

## 常用构建示例

### 最小构建

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  -DLLVM_ENABLE_PROJECTS="" \
  ../llvm
```

### 完整构建（含Clang和LLD）

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  ../llvm
```

### 开发构建

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD=X86 \
  ../llvm
```

### 使用LLD链接

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_LINKER=lld \
  ../llvm
```

### 限制并行链接

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_PARALLEL_LINK_JOBS=2 \
  ../llvm
```

### 构建共享库

```bash
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_BUILD_LLVM_DYLIB=ON \
  -DLLVM_LINK_LLVM_DYLIB=ON \
  ../llvm
```

## 源码位置

| 组件 | 路径 |
|------|------|
| LLVM CMake配置 | `llvm/CMakeLists.txt` |
| LLVM CMake模块 | `llvm/cmake/modules/` |
| 共享CMake模块 | `cmake/Modules/` |
| LLD CMake配置 | `lld/CMakeLists.txt` |
| LLVM工具定义 | `llvm/tools/CMakeLists.txt` |
| LLVM库定义 | `llvm/lib/*/CMakeLists.txt` |
