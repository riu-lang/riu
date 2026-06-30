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

`yux <file>.yux` 单文件编译模式已移除（v0.18），编译必须走项目模式（`yux build`）。

### 可执行文件清单

所有 exe 放在 `build/windows/x64/<mode>/`，**互相独立 —— 构建一个不会自动编译其他**。需要多个时推荐一次性构建全部：

```powershell
xmake                           ; 构建所有 target（全部 exe + 静态库）
xmake build yux                 ; 仅构建主编译器
xmake build yux-lsp             ; 仅构建 LSP
```

| exe | xmake target | 用途 | 依赖 LLVM | 用户直接调用 |
|-----|-------------|------|-----------|-------------|
| `yux` | `yux` | 主编译器：`build`(项目编译)、`test`(运行测试)、`format`(格式化) | 是 | 是 |
| `yux-lsp` | `yux-lsp` | LSP 服务器，编辑器插件通过 stdio 接入；构建后自动复制为 `yux-lsp-claude` | 否 | 否（插件自动启动） |
| `yux-ast` | `yux-ast` | 转储 ANTLR4 parse tree，仅词法+语法，不做 AST/语义/codegen | 否 | 是 |
| `yux-check` | `yux-check` | 快速语义检查（阶段 0）：parse→AST→SemaPass，0 LLVM；支持单文件 + `test` 子命令批量诊断 | 否 | 是 |
| `yux-test-runner` | `yux-test-runner` | 测试运行器，由 `yux test` 内部 spawn 加载 DLL 执行 #Test | 否 | 否（`yux test` 自动调） |

各 exe 用法：

```powershell
# yux（主编译器）
yux build [<name>]              ; 项目编译（需在含 yux.toml 的目录）
yux build [<name>] --emit-ir    ; 同时输出 .ll
yux build [<name>] --test       ; 构建测试 DLL（yux test 内部自动加此参数）
yux build [<name>] --test --test-mod yux.core.array  ; 只编译指定模块的测试
yux test                        ; 运行当前项目所有 #Test
yux test --test-mod yux.core.array  ; 只运行指定模块的测试
yux test --threads 4 --verbose
yux format <file>               ; 格式化源码
yux format <file> -i            ; 原地格式化
yux format --stdin              ; 从 stdin 读取并格式化

# yux-ast
yux-ast <input.yux>             ; parse tree 多行打印到 stdout
yux-ast <input.yux> -o <file>   ; 写入文件
yux-ast <input.yux> --oneline   ; 单行紧凑形式

# yux-check
yux-check <input.yux>           ; 单文件快速 sema（退出码 0=无误）
yux-check test <dir>            ; 批量诊断测试（; check: EXXXX 注解）
yux-check test <dir> -r         ; 递归子目录
```

## 测试

**新测试默认 `yux test`**（DLL + 多子进程并行，每 DLL 独立进程）。诊断用 `yux-check test`，格式化/extern/项目输出用 `xmake test`。

```powershell
# yux test（项目内，*.test.yux 的 #Test）
# 流程：yux build --test → 并行 spawn yux-test-runner 子进程
yux test                        ; 当前项目所有 #Test
yux test --test-mod yux.core.array  ; 只编译/运行指定模块的测试
yux test --threads 4            ; 指定并行子进程数（默认 CPU 核数）
yux test --verbose              ; 打印每个测试捕获的 stdout/stderr
yux test -d                     ; 调试输出传给 yux build --test
cd sdk/yux && yux test          ; 主测试集

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
- **摘要行**：每个 DLL 输出末尾必有一行 `<dll>: X passed, Y failed, [T]`。如果某个 DLL 输出末尾**没有**摘要行，就是进程意外退出（崩溃/被强制终止/死循环），不要以为是测试还在跑
- `yux-check test`：诊断用例在 `tests/check-cases/`，行尾 `; check: EXXXX` 注解，错误码 + 行号精确匹配
- `xmake test`：仅保留三组 — `tests/cases/format_*`（格式化）、`tests/cases/extern_*`/`ptr_*`（extern 边界）、`tests/projects/`（项目输出 + expected.txt）。其余编译+运行用例已全量迁到 SDK `yux test`，诊断用例已全量迁到 `yux-check test`
- `yux test` 自动运行 `yux build --test`（复用缓存，只重编变化的文件），然后并行 spawn `yux-test-runner` 子进程——每个子进程加载一个 DLL、顺序跑其中测试、SEH 包裹异常
- 构建缓存（`PkgCacheRegistry`）：基于编译器指纹 + 源文件 mtime/size 判断 obj 是否新鲜，`yux build` 与 `yux test` 共用同一套缓存，**无需手动删除**——编译器重编后指纹变化自动全体作废
- 新增 SDK 测试**新建或追加**对应主题的 `.test.yux` 文件，不加到 `xmake test`

## 写 yux（易错）

**不要用 Rust/C++/Go 语义去套 yux。**

下面每条都是"不看就会写出错误代码"的硬规则，不是风格建议。

### 空格与换行是语法

- 关键字后、二元运算符两边、`,` 后**必须**有空格；`()` `[]` 内部**不能**有空格。
- 块 `{ ... }` 强制换行——`{` 后和 `}` 前必须是换行，不能压成单行 `{ stmt }`。
- 单表达式体用 `= expr` 省去块换行（`fn foo() i32 = 42`），但 `=` 和表达式必须在同一行。
- 注解 `#Anno` 放在 fn/let 的**上一行**（顶行堆叠），不是同一行。
- 注释是 `;`（不是 `//` 或 `#`），行首或 `空格 + ;` 开头。

### 无隐式类型转换

yux **没有任何隐式类型转换**。整数不自动拓宽/收窄，`i8` 和 `i32` 是不同的类型，`i32` 和 `u32` 也是不同的。必须显式 `.to_<type>()`。

**例外**（编译器自动处理，不需要写 `.to_*()`）：

| 场景 | 自动行为 |
|------|---------|
| 标量运算符 `+ - * / % & \| ^ << >>` | ① 灵活整数自动收束到另一侧类型（`a_u8 + 1` → `1` 推断为 `u8`）② `T&` 自动解引用（`a_ref + b` → 取 `a_ref` 指向的值再算） |
| `== != < <= > >=` | 同上，灵活整数收束 + `T&` 自动解引用 |
| 内置 `T&` 风格用的方法（`a.plus(r)` 等运算符方法） | 编译器自动给值类型实参加 `&` 取址 |
| 测试断言 `assert_eq`/`assert_true`/`assert_false` | 灵活整数按另一实参/类型参数推断；`T&` 实参自动 load |
| `let a TYPE = VALUE` 声明 | 灵活整数 `VALUE` 按 `TYPE` 推断 |
| 方法调用 receiver 的 `Rc<T>` / `T&` | `Rc<T>` 自动解引用找到 `T` 的方法（`rc.method()` 等价 `rc.inner.method()`）；`T&` 同理 |

> **"标量操作符自动解包"**：`+ - * / %` 等标量运算符的两侧操作数如果是 `T&`，编译器自动取引用指向的值，不要求用户手动写 `copy_of` 或 `*`。这只在运算符位置生效，`let` 声明 / 函数实参 / `ret` 等位置不自动解引用。

如果不确定编译器会不会自动收束，**先写后缀**，编译通过后再尝试去掉。

### 灵活整数（unsuffixed integer literal）

- 无后缀的整数字面量（如 `42`）默认类型为 `i32`，但标记为"灵活"——编译器在能唯一确定目标类型时自动推断。
- 后缀写法：`42i8` `100u32` `0xFFu64`。格式：数字 + `i`/`u` + 位宽（8/16/32/64）。
- **自动推断生效的场景**：
  - `let a i64 = 100` — 声明类型已知
  - `assert_eq(a_i8, 42)` — 泛型 `T` 由第一实参推断
  - `take_i64(100)` — 非泛型、非重载函数，唯一匹配
  - `a_u8 + 1` — 运算符对侧类型已知
- **不生效的场景（仍需后缀）**：
  - 方法调用参数：`arr.get(0i64)` / `sb.append(72u8)` — 重载消歧不覆盖
  - 数组字面量元素：`[10u32, 20u32]` — 元素类型不从 `Array<T>` 反向推断
  - 泛型函数 + 非 `i32` 目标类型：`let a i64 = fns_identity(100i64)` — 泛型 T 由实参推断，不从目标类型反向传播
  - 无类型标注的 `let a = 255u8` — 类型完全由表达式决定
  - 超过 `i32` 范围的 `u32` 值：`4000000000u32` — 无后缀默认 `i32` 会溢出
  - 需要特定类型消歧的重载：`foo(42i8)` vs `foo(42i16)` — 只有一个匹配时自动推断才生效

### 实例引用用 `$`

当前实例用 `$`，**不是** `self` 或 `this`。`$.field` 读字段，`$.method()` 调方法。裸 `$` 表示当前实例本身。

### 堆句柄显式 turbofish

`Rc:<T>(...)`、`Heap:<T>(...)`、`Weak:<T>(...)`、`Array:<T>(...)` —— 类型参数必须写 `:<T>`，编译器不会从参数类型推导。

### 结构体字段 / 局部变量

- 结构体字段段**不加 `let`**，直接 `name Type`。
- 局部变量用 `let name Type = expr`；可变变量前加 `#Mut` 在上一行：`#Mut let x i32 = 0`。
- `#Cval`（编译期常量）、`#Frozen`（深不可变）等注解也是顶行堆叠。

### 其他关键差异

- 数组 `[T * N]`：`N` 必须是 INT 字面量，不能是表达式或变量。
- 移入赋值 `a <- b`：表达式，返回旧值，区别于 `a = b`（语句，不产生值）。
- 没有 `++` / `--` / `+=` / `? :` 等 C 风格运算符；用 `x = x + 1` / `if-else`。
- `&&` / `||` 是短路求值，不是位运算（位运算是 `and` / `or`）。

详细语法见 [`rules/yux-syntax.md`](../../../rules/yux-syntax.md)（按需手动读）。

## 写 C++（易错）

- **注释必须中文**；用 `// ====` 分隔区域
- 遇到问题/潜在 bug/未完成/待验证 → 必须写 `// TODO:`，不要假装没看见
- 改 `src/sema/` 或 `src/compiler/` 前，手动读 [`rules/sema-codegen.md`](../../../rules/sema-codegen.md)
