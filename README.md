# riu-lang

一个独立的编译器，类似于 clang，一个程序能完成所有编译流程。

## 项目简介

riu（发音通 U，非英语）是一门自举的编程语言，手写词法/语法分析，LLVM 作为编译后端。编译器将 `.ut`（U Text）源文件编译为可执行文件；模块声明缓存为 `.ud`。

### 语言特性

- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Rc<T>, Ptr<T>, Array<T>）
- 自动内存管理（Rc<T> 引用计数）
- 与 C/Windows API 互操作

### 示例代码

```riu
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

[组织架构](riu.md)

### 环境要求

- **编译器**: Clang
- **构建工具**: GN（PATH）；Ninja / cloc 由 `./sync-deps.ps1` 下载到 `bin/`（Python 3 供 GN 脚本；LLVM 用其自带 `llvm/utils/gn`）
- **系统**: Windows
- `build/windows/x64/debug/bin`的绝对路径添加到`PATH`，以便调用

### 依赖

项目依赖以下库（位于 `third_party/` 目录）：

| 依赖 | 说明 |
|------|------|
| LLVM | 编译器后端基础设施 |
| utfcpp | UTF-8 编码处理 |
| zlib | 压缩库 |

### 克隆与子模块

```powershell
git clone --recurse-submodules <repo-url>
# 若已克隆未拉子模块：
git submodule update --init scripts/ps-sync-deps
```

根目录 PowerShell 脚本（无需 `init`）：

| 脚本 | 作用 |
|------|------|
| `./sync-deps.ps1` | 按 `DEPS.json` 同步 `third_party/` 与 `bin/`（调用 `scripts/ps-sync-deps`） |
| `./build.ps1` | GN + Ninja 构建入口 |
| `./count-lines.ps1` | `cloc` 统计（可选 commit，默认 HEAD） |
| `./lint.ps1` | clang-tidy（默认 git 变动文件；`--all` / 路径参数） |
| `./format.ps1` | clang-format（默认 git 变动；`--all` / `--check` / 路径参数） |

### 同步依赖

```powershell
./sync-deps.ps1
./sync-deps.ps1 -DryRun          # 预览
./sync-deps.ps1 cli11 zlib       # 只同步指定项
```

若 `llvm` 源码 commit 有变，下次 `./build.ps1 riu`（或 `./build.ps1 llvm`）会按 stamp 自动重新 gn gen 并编译 LLVM（首次/升级可能很久）。

### 代码统计

使用 [cloc](https://github.com/AlDanial/cloc)（`bin/cloc-2.10.exe`，由 `sync-deps` 下载）：

```powershell
./count-lines.ps1           # HEAD
./count-lines.ps1 <commit>
```

排除 lock 文件，并用 `riu_lang_def.txt` 识别 riu。

### Lint / Format

```powershell
./lint.ps1                 # lint git 已变动文件
./lint.ps1 --all           # target 全量
./lint.ps1 src/foo.cpp     # 指定文件
./format.ps1               # 格式化 git 已变动文件
./format.ps1 --all         # 全仓
./format.ps1 --check       # 只检查不改，有差异退出码 1
```

## 构建

```powershell
# 构建 riu 编译器
./build.ps1 riu

# 可选：附属工具
./build.ps1 riu-lsp     # LSP 服务器（编辑器插件用）
./build.ps1 riu-ast     # 词法 / FlatAst 转储工具
./build.ps1             # 全部默认目标
```

## 使用

在项目根目录（含 `riu.toml`）执行：

```powershell
riu build                  # 本项目全部产物：先 [library]（若有），再按序全部 [[executable]]
riu build <name>           # 只编产出名为 <name> 的那条（exe 或库）
                           # 单产物且产出名缺省为项目名时，riu build / riu build <项目名> 都能用
                           # 产物落在 <projectRoot>/build/<产出名>.exe / .lib（动态库另有 .dll）
```

### riu.toml（项目配置）

顶层只写项目身份；产物写在 `[library]` / `[[executable]]`。

| 字段 | 说明 |
|------|------|
| `name` | 项目身份。**不是**默认输出文件名 |
| `version` | 版本号（当前仅记录） |
| `[[executable]].entry` | 入口 `.ut`，相对源根 |
| `[[executable]].name` | 输出文件名（无扩展名）；缺省 = 项目 `name` |
| `[library].lib_mod` | 库拥有的模块根 |
| `external_links` | 写在产物上：`//kernel32` 系统库，`./lib/foo` 本树文件；不写库后缀 |

最小示例：

```toml
name="test"
version="1.0.0"

[[executable]]
entry="main.ut"
```

### 命令行参数

| 参数 | 说明 |
|------|------|
| `build <name>` | 项目构建子命令 |
| `--emit-ir` | 输出 LLVM IR 到 .ll 文件 |
| `--emit-ir-dir <dir>` | 指定 .ll 输出目录（默认 build/） |
| `-d, --debug` | 输出编译 IR 调试信息（仅 Debug 构建） |
| `lsp` | 以 stdio 启动语言服务器（供 [riu-vscode](plugins/riu-vscode/) / [riu-idea](plugins/riu-idea/) 等编辑器集成使用） |

附属可执行文件（与 `riu.exe` 同目录）：

| 命令 | 说明 |
|------|------|
| `riu-ast <input.ut> [-o <file>] [--rd] [--rd-tokens]` | 转储 rd FlatAst（默认）或词法 token；仅词法 + 语法 |
| `riu-lsp` | 独立 LSP 服务器二进制 |

## 编辑器支持

- [`plugins/riu-vscode/`](plugins/riu-vscode/) —— VSCode 扩展
- [`plugins/riu-idea/`](plugins/riu-idea/) —— IntelliJ 系插件，通过 [LSP4IJ](https://github.com/redhat-developer/lsp4ij) 接入 `riu-lsp`

## 测试

测试分为三级，覆盖不同层面：

| 层级 | 命令 | 用例位置 | 说明 |
|------|------|---------|------|
| 项目编译+运行 | `./build.ps1 test` | `tests/projects/` | 每目录一个 `riu.toml` + `expected.txt`；编译产物并比对 stdout |
| 格式化回归 | `./build.ps1 test` | `tests/projects/` | `expected_format` 文件，比对外格式化输出 |
| 诊断回归 | `riu-check test` | `tests/check-cases/` | `diag_*.ut`，行尾 `; check: EXXXX` 注解精确匹配 |
| 单元/行为测试 | `riu test` | `sdk/core` + `sdk/stdlib` 的 `*.test.ut` | `#Test` 注解，DLL + 多子进程并行 |

**运行方式：**

```powershell
# 项目 / 格式化测试
./build.ps1 riu
./build.ps1 test                  # 全部（默认并行，jobs = CPU 核数）
./build.ps1 test -Jobs 1          # 强制串行
./build.ps1 test <name>           # 单个（tests/projects/<name>）
./build.ps1 test -Group format    # 只跑格式化

# 诊断回归
riu-check test tests/check-cases/

# SDK 单元测试（主测试集）
cd sdk/core && riu test
cd sdk/stdlib && riu test
riu test --verbose          # 打印每个测试 stdout/stderr
riu test --test-mod riu.core.array  # 只测指定模块
```

语法以 [`riu/ast/riu.bnf`](riu/ast/riu.bnf)、手写 parser 和 [文档](docs/index.md) 为准，用例需符合这三者。

测试逻辑：`./build.ps1 test` 定义在 [tests/run.ps1](tests/run.ps1)；`riu test` 流程为 `riu build --test` → 并行 spawn `riu-test-runner` 子进程加载 DLL 执行。

## License

[MPL-2.0](LICENSE.txt)

Copyright (c) 2025-2026. Yin-Jinlong@github
