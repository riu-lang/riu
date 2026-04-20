# yux 编译器开发指南

独立的编译器，类似于clang，一个程序能完成所有。

**不要改语法文件**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

## 快速入门

### 项目概述

Yux 是一个自举的编程语言编译器，使用 LLVM 作为后端，支持：
- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Box<T>, Ptr<T>, Array<T>）
- 自动内存管理（Box<T> 引用计数）
- 与 C/Windows API 互操作

### 快速构建

```powershell
# 构建 yux 编译器
xmake build yux

# 测试编译
yux main.yux
```

## 开发环境

- 构建系统：`xmake` + `Clang`
- 运行环境：Windows PowerShell，无 MSVC 开发环境变量
- LLVM 工具链：系统 PATH 包含 llvm/bin
- 调试工具：Windows 兼容的 `head`、`tail`（使用 `-n 行数` 参数，始终返回 0）

## 项目结构

### 根目录

```
yux-lang/
├── src/                  # 源代码（详见下文）
├── include/              # 头文件
├── sdk/                  # 自举运行时库
├── gen/                  # ANTLR4 生成代码
├── third_party/          # 外部依赖
├── tests/                # 测试用例
├── build/                # 编译输出（详见下文）
├── .xmake/               # xmake 构建配置
├── .cache/               # clangd 缓存
├── yux-vscode/           # VS Code 插件
├── xmake.lua             # 构建脚本
├── compile_commands.json # 编译数据库（clangd）
├── main.yux              # 测试入口
├── AGENTS.md             # 本文档
└── 语法.md               # 语法详细说明
```

### 源代码目录 (src/)

```
src/
├── main.cpp              # 程序入口
├── yux.g4                # ANTLR4 语法文件（不要修改）
├── yux.cpp/h             # 编译器主类
├── compiler.cpp/h        # LLVM IR 生成
├── ast_builder.cpp/h     # AST 构建
├── build_cache.cpp/h     # 构建缓存管理
└── node/                 # AST 节点定义
    ├── expr_node.*       # 表达式节点
    ├── fn_node.*         # 函数节点
    ├── struct_node.*     # 结构体节点
    └── statement_node.*  # 语句节点
```

### 构建目录 (build/)

**重要：build 目录包含 xmake 和 yux 编译器的共用输出，清理时需谨慎！**

```
build/
├── build.cache           # yux 编译缓存（详见下文）
├── main.exe              # yux 编译的测试程序
├── main.ll               # 生成的 LLVM IR
├── main.obj              # 目标文件
├── sdk.ll                # SDK 的 LLVM IR
├── sdk.obj               # SDK 目标文件
│
├── windows/              # xmake 输出目录（共用）
│   └── x64/
│       └── debug/
│           ├── yux.exe   # yux 编译器可执行文件
│           ├── yux.pdb   # 调试符号
│           ├── antlr4_static.lib
│           └── zlib.lib
│
├── .build_cache/         # xmake 构建缓存
├── .deps/                # xmake 依赖信息
├── .gens/                # xmake 生成文件
├── .objs/                # xmake 目标文件
├── .xpack/               # xmake 打包输出
├── config/               # xmake 配置
├── toolchain/            # xmake 工具链信息
└── xpack/                # xmake 打包配置
```

**清理建议：**
- 安全清理：仅删除 `build/*.exe`、`build/*.ll`、`build/*.obj`、`build/build.cache`
- 完全清理：`xmake clean -a`（会清理所有 xmake 输出）

### 构建缓存 (build/build.cache)

yux 编译器使用构建缓存来避免重复编译未修改的文件：

**文件格式：**
```
文件路径
修改时间戳 文件大小
...
```

**示例：**
```
/abs/path/main.yux
1776603026 507
/abs/path/sdk/sdk.yux
1776596925 5068
```

**工作原理：**
- 编译前检查源文件的修改时间和大小
- 如果缓存中记录的时间戳和大小都匹配，则跳过编译

### 测试目录 (tests/)

```
tests/
├── xmake.lua             # 测试构建脚本
├── test_base.cpp         # 测试框架
├── CMakeLists.txt        # CMake 配置（备用）
└── cases/                # 测试用例
    ├── *.yux             # 测试源文件
    ├── *.expected        # 期望输出
    └── error/            # 错误测试用例
        ├── err_*.yux     # 应该编译失败的测试
        └── err_*.expected
```

### 外部依赖 (third_party/)

```
third_party/
├── antlr4/               # 解析器生成器
├── llvm/                 # 编译器后端
│   ├── llvm/             # LLVM 源码
│   └── lld/              # 链接器源码
├── googletest/           # 测试框架
├── utfcpp/               # UTF-8 处理
└── zlib/                 # 压缩库
```

### 编译流程

```
源代码 (.yux)
    ↓
词法分析 (ANTLR4 Lexer)
    ↓
语法分析 (ANTLR4 Parser)
    ↓
AST 构建 (ASTBuilder)
    ↓
语义分析 (符号表、类型检查)
    ↓
IR 生成 (Compiler → LLVM IR)
    ↓
目标代码生成 (LLVM)
    ↓
链接 (LLD)
    ↓
可执行文件 (build/*.exe)
```

## 调试

### 编译命令

```bash
yux input.yux                    # 编译
yux --emit-ir input.yux          # 生成 IR
yux -d input.yux                 # 调试模式
```

### 输出说明

- 编译成功后在工作目录输出到 `build` 目录，返回 `0`
- `--emit-ir` 会在 `build` 目录生成 `.ll` 文件
- `-d` 输出编译 IR 调试信息，信息量大，建议配合 `tail` 或其他过滤工具使用

## 测试

[main.yux](main.yux)

## 语法参考

详细语法说明请参考 [语法.md](语法.md)

### 编译器开发注意事项

1. **注释规则**：
   - 行注释：顶行或缩进，匹配 `^\s*/.*`
   - 尾随注释：用 ` ;`，代码行或空行后，不能用 `/` 尾随

2. **空格规则**：
   - 关键字后必须有空格
   - 二元运算符两边必须有空格
   - `()` `[]` 内部无空格
   - `,` 后有空格

3. **类型系统**：
   - 无隐式转换，所有类型必须显式转换
   - 使用 `.to_类型()` 方法进行类型转换
   - 无后缀整数字面量默认 `i32`，但可按上下文自动推断：二元运算的另一侧类型、带显式类型的声明/赋值、函数返回类型、唯一匹配的函数重载参数。若多个重载均可自动匹配则报错，需用类型后缀消歧（如 `2u8`）。

4. **关键字**：
   - `fn`, `var`, `val`, `cval`
   - `if`, `elif`, `else`
   - `ret`, `break`
   - `null`, `true`, `false`
   - `loop`, `struct`
