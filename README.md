# yux-lang

一个独立的编译器，类似于 clang，一个程序能完成所有编译流程。

## 项目简介

yux 是一门自举的编程语言，使用 ANTLR4 解析语法，LLVM 作为编译后端。编译器将 `.yux` 源文件编译为可执行文件。

### 语言特性

- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Rc<T>, Ptr<T>, Array<T>）
- 自动内存管理（Rc<T> 引用计数）
- 与 C/Windows API 互操作

### 示例代码

```yux
; 注释，顶行 ;，可加空格缩进

fn add(a i32, b i32) i32 = a + b

fn main() {
  let a = add(1, 2)
  println(a.to_string())
  
  ; if 表达式
  let max = if a > 0 { a } else { -a }
  println(max.to_string())
}
```

详细语法说明请参考 [文档](docs/index.md)。

## 开发

[组织架构](yux.md)

### 环境要求

- **编译器**: Clang
- **构建工具**: xmake
- **系统**: Windows
- `build/windows/x64/debug/bin`的绝对路径添加到`PATH`，以便调用

### 依赖

项目依赖以下库（位于 `third_party/` 目录）：

| 依赖 | 说明 |
|------|------|
| LLVM | 编译器后端基础设施 |
| ANTLR4 | 语法解析器生成器 |
| utfcpp | UTF-8 编码处理 |
| zlib | 压缩库 |

### 初始化项目

克隆仓库后，运行初始化脚本生成跨平台包装脚本：

```powershell
node init.js
```

此命令会在项目根目录生成 `sync-deps`、`gen-antlr`、`count-lines`、`lint`、`format` 的跨平台包装脚本（`.ps1`、`.sh`、`.cmd`）。

### 同步依赖

```powershell
./sync-deps.ps1      # Windows PowerShell
./sync-deps.sh       # Linux/macOS
sync-deps.cmd        # Windows CMD
```

此命令会下载 `third_party/` 下的源码依赖，以及 `bin/` 下的二进制工具（如 ANTLR4 jar）。

### 生成解析器代码

当修改 `yux/ast/yux*.g4` 语法文件后，需要重新生成 C++ 解析器代码：

```powershell
./gen-antlr.ps1      # Windows PowerShell
./gen-antlr.sh       # Linux/macOS
gen-antlr.cmd        # Windows CMD
```

此命令使用 `bin/antlr-4.13.2-complete.jar` 从语法文件生成代码到 `yux/ast/gen/yux/` 目录。

### 代码统计

使用 [cloc](https://github.com/AlDanial/cloc) 统计代码行数：

```powershell
./count-lines           # 统计 HEAD
./count-lines <commit>  # 统计指定 commit
```

此命令会自动排除 lock 文件（如 `package-lock.json`），并使用 `yux_lang_def.txt` 配置识别 yux 语言。

### Lint / Format

`lint` 跑 clang-tidy，`format` 排 `#include` 块；默认只作用于 git 已变动 / 未跟踪文件，加 `--all` 切全仓，加位置参数指定文件：

```powershell
./lint.ps1                 # lint git 已变动文件
./lint.ps1 --all           # 三个 target 全量
./lint.ps1 src/foo.cpp     # 指定文件
./format.ps1               # 排 git 已变动文件 #include 块
./format.ps1 --all         # 全仓
./format.ps1 --check       # 只检查不改, 有差异退出码 1
```

## 构建

```powershell
# 构建 yux 编译器
xmake build yux

# 可选：附属工具
xmake build yux-lsp     # LSP 服务器（编辑器插件用）
xmake build yux-ast     # 仅 ANTLR parse tree 转储工具
```

## 使用

在项目根目录（含 `yux.toml`）执行：

```powershell
yux build                  # 等价于 yux build <toml-name>；当前每个项目仅一个目标
yux build <name>           # 显式给出时 <name> 必须与 yux.toml 的 name 一致
                           # 入口取 toml 的 entry，产物落在 <projectRoot>/build/<name>/<name>.exe
```

### yux.toml（项目配置）

| 字段 | 说明 |
|------|------|
| `name` | 项目 / 可执行文件名；`yux build` 默认取它，显式 `yux build <name>` 必须与它匹配 |
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
| `--emit-ir-dir <dir>` | 指定 .ll 输出目录（默认 build/） |
| `-d, --debug` | 输出编译 IR 调试信息（仅 Debug 构建） |
| `lsp` | 以 stdio 启动语言服务器（供 [yux-vscode](plugins/yux-vscode/) / [yux-idea](plugins/yux-idea/) 等编辑器集成使用） |

附属可执行文件（与 `yux.exe` 同目录）：

| 命令 | 说明 |
|------|------|
| `yux-ast <input.yux> [-o <file>] [--oneline]` | 转储 ANTLR parse tree；仅词法 + 语法，遇到语法错也输出含 `<error>` 节点的树 |
| `yux-lsp` | 独立 LSP 服务器二进制 |

## 编辑器支持

- [`plugins/yux-vscode/`](plugins/yux-vscode/) —— VSCode 扩展
- [`plugins/yux-idea/`](plugins/yux-idea/) —— IntelliJ 系插件，通过 [LSP4IJ](https://github.com/redhat-developer/lsp4ij) 接入 `yux-lsp`

## 测试

测试分为三级，覆盖不同层面：

| 层级 | 命令 | 用例位置 | 说明 |
|------|------|---------|------|
| 项目编译+运行 | `xmake test` | `tests/projects/` | 每目录一个 `yux.toml` + `expected.txt`；编译产物并比对 stdout |
| 格式化回归 | `xmake test` | `tests/projects/` | `expected_format` 文件，比对外格式化输出 |
| 诊断回归 | `yux-check test` | `tests/check-cases/` | `diag_*.yux`，行尾 `; check: EXXXX` 注解精确匹配 |
| 单元/行为测试 | `yux test` | `sdk/yux/src/yux/core/*.test.yux` | `#Test` 注解，DLL + 多子进程并行 |

**运行方式：**

```powershell
# 项目 / 格式化测试
xmake build yux
xmake test                  # 全部
xmake test yux_tests/<name> # 单个

# 诊断回归
yux-check test tests/check-cases/

# SDK 单元测试（主测试集）
cd sdk/yux && yux test
yux test --verbose          # 打印每个测试 stdout/stderr
yux test --test-mod yux.core.array  # 只测指定模块
```

语法以 `yux/ast/yux*.g4` 和 [文档](docs/index.md) 为准，用例需符合这两者。

测试逻辑：`xmake test` 定义在 [tests/xmake.lua](tests/xmake.lua) 的 `yux_tests` target；`yux test` 流程为 `yux build --test` → 并行 spawn `yux-test-runner` 子进程加载 DLL 执行。

## License

[MPL-2.0](LICENSE.txt)

Copyright (c) 2025-2026. Yin-Jinlong@github
