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
; 注释，顶行 ;，可加空格缩进

fn add(a i32, b i32) i32 = a + b

fn main() {
  val a = add(1, 2)
  println(a.to_string())
  
  ; if 表达式
  val max = if a > 0 { a } else { -a }
  println(max.to_string())
}
```

详细语法说明请参考 [文档](docs/index.md)。

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
├── bin/                  # 二进制工具（antlr4 jar 等）
├── build/                # 编译输出
├── yux-vscode/           # VSCode 扩展
└── yux-idea/             # IntelliJ 插件（通过 LSP4IJ 接入 yux-lsp）
```

### 初始化项目

克隆仓库后，运行初始化脚本生成跨平台包装脚本：

```powershell
node init.js
```

此命令会在项目根目录生成 `sync-deps` 和 `gen-antlr` 的跨平台包装脚本（`.ps1`、`.sh`、`.cmd`）。

### 同步依赖

```powershell
./sync-deps.ps1      # Windows PowerShell
./sync-deps.sh       # Linux/macOS
sync-deps.cmd        # Windows CMD
```

此命令会下载 `third_party/` 下的源码依赖，以及 `bin/` 下的二进制工具（如 ANTLR4 jar）。

### 生成解析器代码

当修改 `src/yux.g4` 语法文件后，需要重新生成 C++ 解析器代码：

```powershell
./gen-antlr.ps1      # Windows PowerShell
./gen-antlr.sh       # Linux/macOS
gen-antlr.cmd        # Windows CMD
```

此命令使用 `bin/antlr-4.13.2-complete.jar` 从语法文件生成代码到 `gen/yux/` 目录。

## 构建

```powershell
# 构建 yux 编译器
xmake build yux
```

## 使用

在项目根目录（含 `yux.toml`）执行：

```powershell
yux build <name>           # <name> 必须与 yux.toml 的 name 一致，入口取 toml 的 entry
                           # 产物落在 <projectRoot>/build/<name>/<name>.exe
```

### yux.toml（项目配置）

| 字段 | 说明 |
|------|------|
| `name` | 项目 / 可执行文件名；`yux build <name>` 的 `<name>` 必须与它匹配 |
| `entry` | 入口 `.yux`，相对项目根 |
| `version` | 版本号（当前仅记录） |

最小示例：

```toml
name="test"
version="1.0.0"
entry="main.yux"
```

### 命令行参数

| 参数 | 说明 |
|------|------|
| `build <name>` | 项目构建子命令 |
| `--emit-ir` | 输出 LLVM IR 到 .ll 文件 |
| `-d, --debug` | 输出编译 IR 调试信息（仅 Debug 构建） |
| `lsp` | 以 stdio 启动语言服务器（供 [yux-vscode](yux-vscode/) / [yux-idea](yux-idea/) 等编辑器集成使用） |

## 编辑器支持

- [`yux-vscode/`](yux-vscode/) —— VSCode 扩展
- [`yux-idea/`](yux-idea/) —— IntelliJ 系插件，通过 [LSP4IJ](https://github.com/redhat-developer/lsp4ij) 接入 `yux-lsp`

## 测试

测试用例位于 `tests/cases/` 目录，使用 xmake 原生测试机制驱动，不再依赖 googletest / CMake。

**用例结构：**

- `tests/cases/*.yux` + 同名 `*.expected`：编译应成功，运行产物的 stdout 需与 `.expected` 完全一致
- `tests/cases/error/err_*.yux` + 同名 `*.expected`：编译应失败（`.expected` 内容仅作占位）

测试运行器当前以单文件模式在内部调用 `yux` 编译每个用例，产物落在 `tests/cases/build/<stem>.exe`（错误用例在 `tests/cases/error/build/`）。单文件模式本身已弃用，这里是最后一处内部使用，未来会替换为每用例一个小项目的 harness。

语法以 [`src/yux.g4`](src/yux.g4) 和 [文档](docs/index.md) 为准，用例需符合这两者；

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

[MPL-2.0](LICENSE.txt)

Copyright (c) 2025. Yin-Jinlong@github
