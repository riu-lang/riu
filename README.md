# yux-lang

一个独立的编译器，类似于 clang，一个程序能完成所有编译流程。

## 项目简介

yux 是一门自举的编程语言，使用 ANTLR4 解析语法，LLVM 作为编译后端。编译器将 `.yux` 源文件编译为可执行文件。

### 语言特性

- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Box<T>, Ptr<T>, Array<T>）
- 自动内存管理（Box<T> 引用计数）
- 与 C/Windows API 互操作

### 示例代码

```yux
/ 注释，顶行/，可加空格缩进

fn add(a i32, b i32) i32 = a + b

fn main() {
  val a = add(1, 2)
  println(a)
  
  / if 表达式
  val max = if a > 0 { a } else { -a }
  println(max)
}
```

详细语法说明请参考 [语法.md](语法.md)。

## 开发

### 环境要求

- **编译器**: Clang
- **构建工具**: xmake
- **系统**: Windows
- `build/windows/x64/debug`的绝对路径添加到`PATH`，以便调用

### 依赖

项目依赖以下库（位于 `third_party/` 目录）：

| 依赖 | 说明 |
|------|------|
| LLVM | 编译器后端基础设施 |
| ANTLR4 | 语法解析器生成器 |
| utfcpp | UTF-8 编码处理 |
| zlib | 压缩库 |

### 目录结构

```
yux-lang/
├── src/                  # 源代码
│   ├── yux.g4            # 语法文件
│   ├── main.cpp          # 程序入口
│   ├── compiler.*        # LLVM IR 生成
│   ├── ast_builder.*     # AST 构建
│   └── node/             # AST 节点定义
├── include/              # 公共头文件
├── sdk/                  # 自举运行时库
├── gen/                  # ANTLR4 生成的代码
├── tests/                # 测试用例
├── third_party/          # 外部依赖
├── build/                # 编译输出
└── yux-vscode/           # VSCode 扩展
```

详细的项目结构说明请参考 [AGENTS.md](AGENTS.md)。

### 同步依赖

```powershell
./sync-deps.ps1
```

## 构建

```powershell
# 构建 yux 编译器
xmake build yux
```

## 使用

两种模式：

```powershell
# 单文件模式：产物扁平放在 <源文件目录>/build/
yux <input.yux>            # 生成 <srcDir>/build/<stem>.exe

# 项目模式：在项目根目录（含 yux.toml）执行
yux build <name>           # <name> 必须与 yux.toml 的 name 一致，入口取 toml 的 entry
                           # 产物落在 <projectRoot>/build/<name>/<name>.exe
```

### yux.toml（项目配置）

| 字段 | 说明 |
|------|------|
| `name` | 项目 / 可执行文件名；`yux build <name>` 的 `<name>` 必须与它匹配 |
| `entry` | 入口 `.yux`，相对项目根 |
| `version` | 版本号（当前仅记录） |

单文件模式完全忽略 yux.toml。

### 命令行参数

| 参数 | 说明 |
|------|------|
| `input` | 输入的 .yux 文件（单文件模式） |
| `build <name>` | 项目模式子命令 |
| `--emit-ir` | 输出 LLVM IR 到 .ll 文件 |
| `-d, --debug` | 输出编译 IR 调试信息（仅 Debug 构建） |

## 测试

测试用例位于 `tests/cases/` 目录，使用 xmake 原生测试机制驱动，不再依赖 googletest / CMake。

**用例结构：**

- `tests/cases/*.yux` + 同名 `*.expected`：编译应成功，运行产物的 stdout 需与 `.expected` 完全一致
- `tests/cases/error/err_*.yux` + 同名 `*.expected`：编译应失败（`.expected` 内容仅作占位）

测试运行器以**单文件模式**调用 `yux`（`yux <case.yux>`），每个用例的产物落在 `tests/cases/build/<stem>.exe`（错误用例在 `tests/cases/error/build/`）。项目级测试暂未纳入此 harness，将来单独写一套。

语法以 [`src/yux.g4`](src/yux.g4) 和 [语法.md](语法.md) 为准，用例需符合这两者；不一致时以 g4 / 语法.md 为准。

**运行方式：**

```powershell
# 先构建编译器（测试会通过 xmake 依赖自动构建，但显式构建便于定位编译期错误）
xmake build yux

# 发现并运行全部用例
xmake test

# 详细日志
xmake test -v

# 单独运行某个用例（xmake 的语法：<target>/<test-name>）
xmake test yux_tests/basic_types.yux
xmake test yux_tests/error_err_val_reassign.yux
```

测试逻辑定义在 [tests/xmake.lua](tests/xmake.lua) 的 `yux_tests` target，通过 `add_tests` + 自定义 `on_run` 完成「编译 → 运行 → 比对输出」。

## License

Copyright (c) 2025. Yin-Jinlong@github
