---
name: yux-lang-dev
description: Use at the start of any task in the yux-lang compiler repo — quick onboarding for build commands, test workflow, compiler architecture, build output layout, and the mandatory yux / C++ code style. Read this together with .claude/rules/ (which holds the project rules and directory structure).
---

# yux-lang 开发上手

yux 是自举编译器，单二进制完成 `.yux → ANTLR4 解析 → AST → LLVM IR → LLD → exe` 全流程。本技能覆盖**怎么构建、怎么测、怎么写**；**项目规则和目录结构在 [`.claude/rules/`](../../rules/README.md)**（索引在 `README.md`，至少看 `behavior.md` + `tasks-and-bugs.md` + `directory.md`），开任务前两份都要看。

## 环境与工具链

当前主要面向 Windows 开发，跨平台后续再加。

- 操作系统：Windows
- 编译器：**Clang（无 MSVC 环境变量）**
- 构建：xmake（API 文档索引见 [`AGENT-XMAKE.md`](../../../AGENT-XMAKE.md)）
- LLVM 工具链在 `PATH`（`llvm/bin`）
- `build/windows/x64/debug` 在 `PATH`，构建后可直接 `yux ...`
- 改完 C++ 跑两条本地包装器（`init.js` 生成的 `./lint.cmd|sh|ps1`、`./format.cmd|sh|ps1`，不在 PATH 故必须带 `./`）；默认只作用于 git 已变动 / 未跟踪文件，加 `--all` 切全仓，加位置参数指定文件，`--check`（仅 format）只检查不改
  - `./format.cmd` —— `clang-format -i`（规则见仓库根 `.clang-format`，含 #include 块内排序）；PostToolUse hook 已对每次 Edit/Write 自动跑过，本包装器主要给批量场景（`--all` / 多文件 / pre-commit `--check`）
  - `./lint.cmd` —— clang-tidy（包根 `.clang-tidy` + `gen/.clang-tidy` 关掉 ANTLR 生成代码）；**提交时必须 0 警告**（细则与例外见 [behavior.md](../../rules/behavior.md)）
- 包装器属于"克隆即用"集合，由 `node init.js` 重新生成；新增脚本要同步加进 `init.js` 的 `SCRIPTS`
- 不要假设存在 `npm run lint` / `make fmt` 之类命令；所有 C++ 风格 / 静态检查只走上面两个包装器
- 会话 shell 可能是 bash 或 PowerShell；下方命令示例按 PowerShell 写，bash 下将 `./sync-deps.ps1` 换成 `./sync-deps.sh`、路径用正斜杠

## 常用命令

```powershell
; 同步第三方依赖（克隆后执行一次；下载 third_party/ 内容和 bin/ 下的二进制工具）
./sync-deps.ps1

; 生成 ANTLR4 解析器代码（修改 src/yux.g4 后执行）
./gen-antlr.ps1

; 构建编译器
xmake build yux
xmake f -m release && xmake build yux   ; release 构建（-d 调试 IR 输出仅 debug 可用）

; 附属二进制（与 yux.exe 同目录, 按需单独构建）
xmake build yux-lsp                     ; LSP 服务器（编辑器插件用）
xmake build yux-ast                     ; ANTLR parse tree 转储工具
                                        ;   yux-ast <file.yux> [-o <out>] [--oneline]
                                        ;   仅词法 + 语法, 不构造 AST / 不做语义 / 不调 LLVM
                                        ;   语法错时仍输出含 <error> 节点的树, 适合排查 g4

; 项目模式：必须在项目根目录（含 yux.toml）执行
yux build                    ; <name> 可省略，默认取 yux.toml 的 name；入口取 toml 的 entry
yux build <name>             ; 显式给出时必须与 yux.toml 的 name 一致
                             ; 最终产物：<projectRoot>/build/<name>.exe（exe）或 .lib（静态库）
                             ; 中间产物镜像 src 路径：build/<src-rel>.obj
                             ; 每目录一份 <dirname>.cache（增量缓存，含编译器指纹）
yux build [<name>] --emit-ir            ; 同时生成 .ll
yux build [<name>] --emit-ir-dir <dir> ; IR 输出到指定目录（默认 build/）
yux build [<name>] -d                  ; 编译期 IR 调试输出（仅 Debug 构建；量大，用 tail 过滤）

; 冒烟测试（仓库内 examples/test 的 yux.toml 里 name="test"）
cd examples/test && yux build && ./build/test.exe
```

`yux.toml` 字段（详见 [docs/模块系统.md](../../../docs/模块系统.md)）：

- `name` —— 项目 / exe 名。`yux build` 默认取它；显式 `yux build <name>` 必须与之一致
- `entry` —— 入口 `.yux`，相对项目根
- `version` —— 目前仅记录，编译器不校验

单文件模式 `yux <file>.yux` 在二进制中仍保留，测试 harness 的部分用例还在用，但**已弃用**。新增代码、示例、文档一律走项目模式，不要教用户直接编译单个 `.yux`。

## 架构

```
.yux → ANTLR4 Lexer/Parser → ASTBuilder → 语义分析
     → Compiler (LLVM IR) → LLVM codegen → LLD link → .exe
```

源码边界（`src/`）：

- `main.cpp` —— CLI 入口、参数解析
- ANTLR 生成代码在 `gen/`（**不要手改**）

运行时位于 `sdk/yux/`（独立的 yux 项目，`yux.toml` 含 `[lib] type="static"`），源码在 `sdk/yux/src/yux/core/`（`base.yux` / `math.yux` / `pkg` 文件等），由 yux 自身编写并编译为静态库 `sdk/yux/build/yux/yux.lib`，链接进每个 yux 程序。

## 构建输出布局

- xmake 输出：`build/windows/x64/{debug,release}/`、以及点开头目录（`.objs/`、`.deps/`、`.build_cache/` 等）
- yux 项目模式：
  - 最终产物：`<projectRoot>/build/<name>.exe` 或 `<name>.lib`（直接落在 `build/` 下，无 `<name>/` 子层）
  - 中间产物：obj / IR 镜像源文件相对项目根的路径，落在 `<projectRoot>/build/<src-rel>.obj` / `.ll`（典型 `build/src/<rel>.obj`）
  - 增量缓存：每个目录视为一个**包**，包内所有 `.yux` 的元信息聚合到 `<dir>/<dirname>.cache`；首行存当前 `yux.exe` 的 mtime+size 作为编译器指纹，重编 `yux` 后整 cache 自动失效
- yux 单文件模式（已弃用）：obj 落在 `<srcDir>/build/.tmp/yux-<pid>/` 临时目录，链接完即清；IR 落在 `<srcDir>/build/<basename>.ll`；exe 留在 `<srcDir>/build/<basename>.exe`，**不进缓存**
- SDK 自构建仍走特例：`sdk/yux/build/yux/{core.obj,yux.lib}`（旧路径，依赖问题以后再说）
- sdk 链接搜索：`build/windows/x64/sdk` => `sdk`

清理规则：

- 安全清理：`rm -rf build/<projectName>.exe build/src/`（带走所有 obj + 缓存）
- 单包清理：删 `build/src/<dir>/` 即可（obj 与该目录的 `<dirname>.cache` 一起没了）
- 完全清理：`xmake clean -a`
- **不要 `rm -rf build/`**，会一起干掉 xmake 的工作目录

## 测试

有两条相互独立的测试通道，**新写测试**默认走 `yux test`，仅当与 RC/借用/诊断/extern 紧耦合或需要 `expected_err` 时才落到 `xmake test`。

### `yux test`（首选）—— `#Test` 函数 + JIT 进程内执行

由 `yux test` 子命令递归扫描项目下 `*.test.yux`，把每个 `#Test fn` 用 ORC LLJIT 在进程内编译执行。不落 `.obj`/`.exe`、跳过 LLD，单 suite 跑完通常 2–3 秒，适合纯逻辑回归（算术 / 类型 / 控制流 / 字符串等值 / 字面量 / 泛型调用 …）。

- 测试位置：旁置 `<name>.test.yux`（**不能**在普通 `.yux` 里挂 `#Test`）；`yux build` 递归扫描时跳过 `*.test.yux`，不会进 lib/exe。
- 测试函数：`#Test fn name(): void { ... }` —— 无参、无返回类型；与 `#CompilerInner` 互斥。
- 断言：`assert_eq(a, b)` / `assert_true(b)` / `assert_false(b)` / `fail(msg)`，支持整型 / 浮点 / bool / String；String 还有 `assert_contains` / `assert_starts_with`（详见 `docs/spec/§11.3.5`）。失败走 SEH `0xE0FA17ED` → runner 翻译为 `(SEH ASSERT_FAILED 0xe0fa17ed)`。
- 隔离：默认 in-process + Windows SEH 包裹每个测试函数；`--isolate=process` 给每个测试起一个子进程兜底（借用 / RC 类、yux 助手 fail 路径走这条；详见 BUGS.md「yux test JIT SEH 跨帧」known-issue）。
- 输出：每个测试 `RUN <module>#<fn>` / `OK` 一行；失败时 `FAIL ... (<reason>)` 后面带 `  | ` 缩进的 stdout/stderr 回放（`-v` 时所有测试都回放）；末尾 `<n> passed, <m> failed`。

```powershell
; 项目模式（必须在含 yux.toml 的目录执行；yux.exe 已在 PATH，直接 `yux` 即可，别写 `./build/.../yux.exe`）
yux test                                 ; 当前项目所有 *.test.yux 中的 #Test
yux test yux.core                        ; 模块前缀匹配
yux test yux.core.string.test#test_eq    ; <module>#<fn> 精确匹配
yux test yux.core.string yux.core.array  ; 多 selector（任一命中即收）；前缀 / <mod>#<fn> 可混用
yux test --isolate=process               ; 每个测试独立子进程
yux test -v                              ; 详细模式（即便 OK 也回放 stdout/stderr）
```

主战场：[sdk/yux/src/yux/core/](../../../sdk/yux/src/yux/core/arithmetic.test.yux) 下的 `*.test.yux` —— SDK 自身就是个项目，`cd sdk/yux && yux test` 是当前最大套（121 用例）。新增逻辑用例放这里。

### `xmake test` —— 单文件 `.expected` 用例 + 项目模式回归

走 [tests/xmake.lua](../../../tests/xmake.lua) 的 `yux_tests` target。两类用例：

| 类别 | 位置 | 对照文件 | xmake test 名 |
|------|------|---------|---------------|
| 单文件成功用例 | `tests/cases/*.yux` | 同名 `.expected` | `yux_tests/<basename>` |
| 单文件诊断用例 | `tests/cases/diag_*.yux` | 同名 `.expected_err` | `yux_tests/<basename>` |
| 项目模式用例 | `tests/projects/<case>/` | 同目录 `expected.txt` | `yux_tests/project_<dirname>` |

单文件成功用例：调用 `yux <file>`、运行产物 exe，比 stdout 与 `.expected`。
诊断用例：调用 `yux <file>` 必须以非零退出码结束，逐行子串匹配 `.expected_err`（行首 `;` 注释，空行忽略）。
项目用例：以该目录为 CWD 调用 `yux build <case>`，运行 `build/<case>/<case>.exe` 并比对 `expected.txt`。

```powershell
xmake build yux                          ; 测试会自动依赖构建，但显式先构建便于定位编译错误
xmake test                               ; 全部用例
xmake test -v                            ; 失败时打印 stdout / stderr / errors
xmake test yux_tests/diag_undefined_var  ; 单个用例（不带 .yux 后缀）
xmake test yux_tests/project_imports_struct
xmake test "yux_tests/*"
xmake test -g yux/diag                   ; 只跑某一分组（见下表）
```

#### 用例分组（按文件名前缀）

`tests/xmake.lua` 的 `categorize(name)` 把每个 `add_tests` 分到 `yux/<cat>` 分组。
**新增用例必须沿用对应前缀**，否则会落入 `yux/misc`。

| 分组 | 前缀 / 命名规则 | 典型用例 |
|------|---------------|---------|
| `yux/diag` | `diag_*.yux` + `*.expected_err` | 诊断 / 错误提示回归 |
| `yux/borrow` | `borrow_*` + `.expected_err` | 借用诊断（合法路径已迁 `yux test`） |
| `yux/extern` | `ptr_of`、`extern_ptr_auto` | extern fn / Ptr 边界（JIT 链接不到） |
| `yux/project` | `tests/projects/<dir>/`（自动加 `project_` 前缀） | 项目模式 |
| `yux/misc` | 兜底 | 其余 |

历史上还有 `yux/array` / `yux/box` / `yux/ref` / `yux/weak` / `yux/nullable` / `yux/rc` / `yux/struct` 等行为分组，已全部迁到 `sdk/yux/src/yux/core/*.test.yux`，由 `yux test` 直接跑。`tests/cases/` 现在只留无法走 JIT 的两类：诊断（`diag_*` / `borrow_* + .expected_err`）和 extern fn 链接边界（`ptr_of` / `extern_ptr_auto`）。新加纯逻辑或行为用例直接写到 `sdk/yux/src/yux/core/<name>.test.yux`。

### 共同规则

- 测试用例与语言规范冲突时，**更新用例**（`src/yux.g4` + 编译器为准）；不要通过修改规范去迁就用例。
- 开发流程建议：
  1. 先改 `examples/test` 或随手建项目做冒烟验证。
  2. 改某子系统时优先 `xmake test -g yux/<相关分组>` 或 `yux test <prefix>` 局部回归。
  3. 提交前两条全量都过一遍：`cd sdk/yux && yux test` + `xmake test`。

## 编写 yux 代码

**文档优先级（yux 是自有语言，不要类比任何其他语言的语义）：**

1. **先读文档**：`docs/index.md` 是索引，当前含 `基础语法.md`、`类型系统.md`、`内置类型.md`、`函数.md`、`结构体.md`、`控制流.md`、`模块系统.md`、`构建注解.md`、`安装指南.md`（均为中文）
2. **其次语法**：文档不足时查 `src/yux*.g4`（权威）
3. **最后编译器**：必要时读 `src/*.cpp`、`src/node/*.cpp` 理解具体行为
4. **不要参考 Rust / Go / Swift**：语义不同，不要凭想象套用

**代码风格（强制）：**

- 注释：行注释以 `;` 开头（可缩进）；尾随注释用 ` ;`
- 空格：关键字后必须有空格；二元运算符两边必须有空格；`,` 后有空格；`()` `[]` 内部无空格
- 强制尾随 `;`：
  - `ret;` —— void 函数提前返回
  - `break;` —— loop 循环跳出
  - 表达式带尾随 `;` 表示空返回，不带 `;` 则返回表达式值
- **无隐式转换**：类型转换必须显式 `.to_<type>()`

## 编写 C++ 代码（编译器源码）

**注释规范（强制）：**

- **语言**：所有注释必须使用**中文**
- **文件头注释**：每个源文件开头必须有文件级注释，说明文件用途和主要功能
  ```cpp
  // 版权

  // 文件用途说明
  //
  // 本文件包含 XXX 相关的实现逻辑:
  // - 功能1
  // - 功能2
  // - 功能3
  ```
- **函数注释**：重要函数必须有注释说明其用途、参数、返回值
  ```cpp
  // 编译函数调用表达式
  // 处理类型推断、参数编译、分发到具体调用类型
  llvm::Value* compileCallExpr(p<ExprCallNode> node);
  ```
- **代码块注释**：复杂逻辑块前应有注释说明
  ```cpp
  // 处理 Rc<T> 类型 (智能指针)
  // Rc 需要减少引用计数，如果计数为 0 则释放内存
  ```
- **分隔注释**：使用 `// ====` 分隔不同功能区域
  ```cpp
  // ==================== 类型映射 ====================

  // ==================== 泛型结构体实例化 ====================
  ```

**TODO 规范（强制）：**

- **必须写 TODO 的情况**：
  1. **暂时无法解决的问题**：遇到问题但当前无法解决，必须写 TODO 说明
  2. **潜在问题**：发现可能存在的问题但不确定，必须写 TODO 标注
  3. **未完成功能**：部分实现的功能，必须写 TODO 说明剩余工作
  4. **性能优化点**：发现可优化但暂不处理的代码，写 TODO 标注
  5. **待验证逻辑**：不确定是否正确的逻辑，写 TODO 标注

- **TODO 格式**：
  ```cpp
  // TODO: 问题描述
  // 例如：
  // TODO: 支持嵌套成员访问
  // TODO: 优化数组填充性能，考虑使用 memset
  // TODO: 验证泛型参数约束是否正确
  ```

- **TODO 优先级**：
  - `// TODO:` —— 普通待办
  - `// TODO(high):` —— 高优先级，影响功能
  - `// TODO(low):` —— 低优先级，优化类

- **禁止行为**：
  - **不要忽略问题**：发现问题必须写 TODO，不能假装没看见
  - **不要删除 TODO**：除非问题已解决
  - **不要过度 TODO**：确定不需要处理的不要写 TODO
