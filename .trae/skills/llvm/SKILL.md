---
name: "llvm"
description: "LLVM compiler infrastructure assistant. Invoke when user asks about LLVM, Clang, compiler optimization, IR generation, or working with llvm-project codebase."
---

# LLVM Skill

LLVM is a modular compiler infrastructure project. This skill helps navigate and work with the LLVM codebase.

## 项目结构

LLVM项目是一个模块化的编译器基础设施，包含多个子项目：

### 核心组件

| 目录 | 说明 |
|------|------|
| `llvm/` | 核心LLVM库，包含IR、转换、分析、代码生成等 |
| `clang/` | C/C++/Objective-C前端编译器，将源码编译为LLVM IR |
| `lld/` | 模块化跨平台链接器，支持ELF、COFF、MachO、WASM格式 |
| `mlir/` | 多级中间表示，用于构建可复用的编译器基础设施 |

### 前端与语言支持

| 目录 | 说明 |
|------|------|
| `flang/` | Fortran前端编译器（原f18项目） |
| `clang-tools-extra/` | 额外的Clang工具（clangd、clang-tidy、include-cleaner等） |

### 运行时库

| 目录 | 说明 |
|------|------|
| `libc/` | LLVM C库实现 |
| `libcxx/` | C++标准库实现 |
| `libclc/` | OpenCL C库实现，符合OpenCL 1.1规范 |
| `libsycl/` | SYCL支持库 |
| `openmp/` | OpenMP运行时库 |
| `orc-rt/` | ORC JIT运行时 |
| `flang-rt/` | Flang运行时库 |

### 优化与分析工具

| 目录 | 说明 |
|------|------|
| `bolt/` | 后链接优化器，基于执行profile优化代码布局 |
| `polly/` | 多面体优化框架 |

### 调试与开发工具

| 目录 | 说明 |
|------|------|
| `lldb/` | LLVM调试器 |
| `offload/` | 加速器卸载支持（CPU、GPU、FPGA等） |

### 其他

| 目录 | 说明 |
|------|------|
| `cmake/` | 共享CMake模块 |
| `third-party/` | 第三方依赖（googletest、benchmark等） |
| `cross-project-tests/` | 跨项目测试 |

## 常用目录导航

- IR相关代码：`llvm/lib/IR/`
- 优化Pass：`llvm/lib/Transforms/`
- 代码生成：`llvm/lib/CodeGen/`
- 目标后端：`llvm/lib/Target/`
- Clang AST：`clang/lib/AST/`
- Clang语义分析：`clang/lib/Sema/`
- LLD ELF链接器：`lld/ELF/`
- MLIR方言：`mlir/lib/Dialect/`

## LLVM IR 文档

LLVM IR（Intermediate Representation）是LLVM编译器基础设施的核心。以下是详细的IR文档：

### 核心概念

- [IR核心概念](ir-core.md) - Module、Function、BasicBlock等核心类
- [IR类型系统](ir-types.md) - Type、Value、User等类型系统
- [IR指令系统](ir-instructions.md) - Instruction及其子类
- [IR构建与操作](ir-builder.md) - IRBuilder、Pass管理等

### 快速导航

| 主题 | 说明 | 文档 |
|------|------|------|
| Module | 模块，IR顶层容器 | [ir-core.md#module模块](ir-core.md#module模块) |
| Function | 函数 | [ir-core.md#function函数](ir-core.md#function函数) |
| BasicBlock | 基本块 | [ir-core.md#basicblock基本块](ir-core.md#basicblock基本块) |
| Type | 类型系统 | [ir-types.md#typetype类型](ir-types.md#typetype类型) |
| Value | 值基类 | [ir-types.md#value值](ir-types.md#value值) |
| Instruction | 指令基类 | [ir-instructions.md#instruction指令](ir-instructions.md#instruction指令) |
| IRBuilder | IR构建器 | [ir-builder.md#irbuilderir构建器](ir-builder.md#irbuilderir构建器) |
| PassManager | Pass管理器 | [ir-builder.md#pass管理器](ir-builder.md#pass管理器) |

## 代码生成文档

代码生成将LLVM IR转换为目标机器码，是编译器的后端核心。

### 核心概念

- [代码生成](codegen.md) - MachineInstr、SelectionDAG、GlobalISel、寄存器分配
- [MC层](mc-layer.md) - MCInst、MCStreamer、对象文件生成
- [Target后端](target-backend.md) - TargetMachine、指令选择、TableGen
- [LLD链接器](lld-linker.md) - ELF链接流程、符号解析、重定位

### 快速导航

| 主题 | 说明 | 文档 |
|------|------|------|
| MachineInstr | 机器指令 | [codegen.md#machineinstr-机器指令](codegen.md#machineinstr-机器指令) |
| SelectionDAG | 选择DAG | [codegen.md#selectiondag-选择dag](codegen.md#selectiondag-选择dag) |
| GlobalISel | 全局指令选择 | [codegen.md#globalisel-全局指令选择](codegen.md#globalisel-全局指令选择) |
| 寄存器分配 | 活跃区间与分配算法 | [codegen.md#寄存器分配](codegen.md#寄存器分配) |
| MCInst | MC指令 | [mc-layer.md#mcinst-机器码指令](mc-layer.md#mcinst-机器码指令) |
| MCStreamer | 输出流 | [mc-layer.md#mcstreamer-输出流](mc-layer.md#mcstreamer-输出流) |
| AsmPrinter | 汇编打印器 | [mc-layer.md#asmprinter-汇编打印器](mc-layer.md#asmprinter-汇编打印器) |
| TargetMachine | 目标机器 | [target-backend.md#targetmachine-目标机器](target-backend.md#targetmachine-目标机器) |
| TargetLowering | 目标Lowering | [target-backend.md#targetlowering-目标lowering](target-backend.md#targetlowering-目标lowering) |
| ELF链接流程 | 链接器工作流程 | [lld-linker.md#elf链接器架构](lld-linker.md#elf链接器架构) |
| 符号解析 | 符号表与解析规则 | [lld-linker.md#symbol-符号](lld-linker.md#symbol-符号) |

## 自定义编译器流程

对于自定义编译器（IR→obj→exe），完整的编译流程：

```
源码 → Clang/Flang → LLVM IR → 优化Pass → 代码生成 → 目标文件 → LLD链接 → 可执行文件
                                    ↓
                          SelectionDAG/GlobalISel
                                    ↓
                              MachineInstr
                                    ↓
                              MC层 (MCInst)
                                    ↓
                              对象文件 (.o)
```

### 关键组件

| 阶段 | 组件 | 文档 |
|------|------|------|
| IR生成 | Clang前端 | 项目结构中的clang/目录 |
| IR优化 | Transforms | [ir-builder.md](ir-builder.md) |
| 指令选择 | SelectionDAG/GlobalISel | [codegen.md](codegen.md) |
| 寄存器分配 | RegAlloc | [codegen.md#寄存器分配](codegen.md#寄存器分配) |
| 代码发射 | MC层 | [mc-layer.md](mc-layer.md) |
| 目标文件 | ELF/COFF | [mc-layer.md#对象文件生成流程](mc-layer.md#对象文件生成流程) |
| 链接 | LLD | [lld-linker.md](lld-linker.md) |
| 目标后端 | Target | [target-backend.md](target-backend.md) |

## CMake 构建配置

LLVM使用CMake构建系统，支持丰富的配置选项和构建目标。

### 核心文档

- [CMake目标与选项](cmake-targets.md) - LLVM和LLD的CMake目标、选项及常用构建示例

### 快速导航

| 主题 | 说明 | 文档 |
|------|------|------|
| 构建选项 | CMake配置选项 | [cmake-targets.md#llvm-cmake-选项](cmake-targets.md#llvm-cmake-选项) |
| 库目标 | LLVM库构建目标 | [cmake-targets.md#llvm-cmake-目标](cmake-targets.md#llvm-cmake-目标) |
| 工具目标 | LLVM工具构建目标 | [cmake-targets.md#工具目标分类](cmake-targets.md#工具目标分类) |
| LLD选项 | LLD链接器配置 | [cmake-targets.md#lld-cmake-选项](cmake-targets.md#lld-cmake-选项) |
| 构建示例 | 常用构建命令 | [cmake-targets.md#常用构建示例](cmake-targets.md#常用构建示例) |

## Common Tasks

- Search IR-related code in `llvm/lib/IR/`
- Find optimization passes in `llvm/lib/Transforms/`
- Explore Clang AST in `clang/lib/AST/`
- Learn LLVM IR concepts from [ir-core.md](ir-core.md)
- Explore code generation in `llvm/lib/CodeGen/`
- Find target backends in `llvm/lib/Target/`
- Study LLD linker in `lld/ELF/`
- Configure build with [cmake-targets.md](cmake-targets.md)
