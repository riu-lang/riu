---
name: yux-lang-dev
description: Use at the start of any task in the yux-lang compiler repo — quick onboarding for build commands, test workflow, compiler architecture, build output layout, and the mandatory yux / C++ code style. Read this together with .claude/rules/ (which holds the project rules and directory structure).
---

# yux-lang 开发上手

yux 是自举编译器：`.yux → ANTLR4 → AST → LLVM IR → LLD → exe`。项目规则见 [`.claude/rules/`](../../rules/README.md)，xmake API 见 [`AGENT-XMAKE.md`](../../../AGENT-XMAKE.md)。

## 环境

- Windows + **Clang（无 MSVC）**，LLVM 在 PATH
- `build/windows/x64/debug` 在 PATH，构建后直接 `yux ...`
- C++ lint/format：`./lint.cmd`（clang-tidy）、`./format.cmd`（clang-format），**必须带 `./`**（不在 PATH）。提交时 `./lint.cmd` 必须 **0 警告**
- 不要假设有 `npm run lint` / `make fmt`

## 构建

```powershell
./sync-deps.ps1              ; 首次克隆后
./gen-antlr.ps1              ; 改 g4 后
xmake build yux              ; 编译
xmake build yux-lsp          ; LSP 服务
yux build                    ; 项目模式（必须在含 yux.toml 的目录）→ <root>/build/<name>.exe
yux build --emit-ir          ; 同时输出 .ll
```

**单文件模式 `yux <file>.yux` 已弃用**，一律走项目模式。

## 测试

**新测试默认 `yux test`**（JIT 进程内，快）。诊断用 `yux-check test`，格式化/extern/项目输出用 `xmake test`。

```powershell
# yux test（项目内，*.test.yux 的 #Test）
yux test                     ; 当前项目所有 #Test
yux test yux.core            ; 前缀匹配
yux test -d                  ; 调试输出，主要是出参入参
cd sdk/yux && yux test       ; 主测试集

# yux-check test（诊断回归，; check: EXXXX 注解）
yux-check test tests/check-cases/   ; 批量测试
yux-check test tests/check-cases/ -r ; 递归子目录

# xmake test（format / extern / 项目输出，tests/cases/ + tests/projects/）
xmake test -g yux/format      ; 格式化（format_*）
xmake test -g yux/extern      ; extern 边界（extern_* / ptr_*）
xmake test -g yux/project     ; 项目模式用例（tests/projects/）
xmake test yux_tests/<name>   ; 跑单个用例
```

- `yux test`：测试写在 `*.test.yux`（**不能**挂在普通 `.yux`），断言 `assert_eq`/`assert_true`/`fail`。SDK 测试集在 `sdk/yux/src/yux/core/*.test.yux`
- `yux-check test`：诊断用例在 `tests/check-cases/`，行尾 `; check: EXXXX` 注解，错误码 + 行号精确匹配
- `xmake test`：仅保留三组 — `tests/cases/format_*`（格式化）、`tests/cases/extern_*`/`ptr_*`（extern 边界）、`tests/projects/`（项目输出 + expected.txt）。其余编译+运行用例已全量迁到 SDK `yux test`，诊断用例已全量迁到 `yux-check test`
- 新增 SDK 测试**新建或追加**对应主题的 `.test.yux` 文件，不加到 `xmake test`

## 写 yux（易错）

**不要用 Rust/C++/Go 语义去套 yux。**

- 注释 `;` 开头，不是 `//`
- 空格是语法：关键字后、二元运算符两边、`,` 后必须有空格；`()` `[]` 内无空格
- 当前实例用 `$`，不是 `self`
- 堆句柄必须显式 turbofish：`Heap:<T>(...)`、`Rc:<T>(...)`
- 数组 `[T * N]`，N 必须整数，不能是表达式
- 块体强制换行，单表达式可用 `= expr`
- 无隐式转换，必须 `.to_<type>()`
- 结构体字段**不加 `let`**；局部变量可变用 `#Mut let`

详细语法见 [`rules/yux-syntax.md`](../../../rules/yux-syntax.md)（按需手动读）。

## 写 C++（易错）

- **注释必须中文**；用 `// ====` 分隔区域
- 遇到问题/潜在 bug/未完成/待验证 → 必须写 `// TODO:`，不要假装没看见
- 改 `src/sema/` 或 `src/compiler/` 前，手动读 [`rules/sema-codegen.md`](../../../rules/sema-codegen.md)
