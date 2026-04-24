# yux 编译器开发指南

独立编译器，单程序完成全流程（解析→IR→链接→exe）。

**不要改语法文件 `src/yux.g4`**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

## 任务与 bug 记录

- **`CURRENT.md`**：正在进行的多步骤任务。**接到多步骤任务先写入 `CURRENT.md` 追踪**（列出分阶段计划），开始前先读，阶段完成后就地更新；整个任务完成后删除该条目。
- **`BUGS.md`**：开发新功能时**新发现**的 bug（非本次修改引起、需要大量排查、或暂时规避的问题）。按模板填写，暂停相关任务并向用户说明。
- 两者分工明确：多步任务进度 → `CURRENT.md`；新发现的 bug → `BUGS.md`。不要混用。

## 硬性规则

- 环境：Windows PowerShell + Clang（无 MSVC 环境变量）
- LLVM 工具链在 PATH（`llvm/bin`）
- `build/windows/x64/debug` 默认在 PATH，构建后可直接调用 `yux`
- 调试工具：Windows 兼容的 `head`、`tail`

## 常用命令

```powershell
# 同步依赖（克隆后执行一次）
./sync-deps.ps1

# 构建编译器
xmake build yux

# 项目模式：必须在项目根目录（含 yux.toml）执行
yux build <name>             # <name> 必须与 yux.toml 的 name 一致；入口取 toml 的 entry
                             # 产物落在 <projectRoot>/build/<name>/<name>.exe
yux build <name> --emit-ir   # 同时生成 .ll 文件
yux build <name> -d          # 调试 IR 输出（信息量大，配合 tail 使用）

# 简易验证（以仓库内 examples/main 为例，yux.toml 里 name="test"）
cd examples/main && yux build test && ./build/test/test.exe
```

`yux.toml` 字段（详见 [docs/模块系统.md](docs/模块系统.md)）：`name`（项目 / exe 名）、`entry`（入口 .yux，相对项目根）、`version`。

单文件模式（`yux <file>.yux`）二进制里仍保留，也是测试 harness 目前内部驱动编译的方式，但已弃用、未来会移除——新增代码、示例、文档一律走项目模式，不要再用 `yux` 直接编译单个 `.yux` 文件。

**测试流程：**
1. **简易测试**：构建并运行 `examples/main`（或随手建一个小项目）快速验证
2. **完整测试**：简易测试通过后，`xmake test` 验证所有用例

测试运行器（`tests/xmake.lua`）当前以单文件模式调用 `yux` 编译每个 `tests/cases/*.yux`（这是单文件模式最后一处内部使用，后续会替换成每用例一个小项目的 harness），比较 stdout 与配对的 `.expected`；`error/err_*.yux` 期望编译失败。每个用例的产物放在 `tests/cases/build/<stem>.exe`（错误用例在 `tests/cases/error/build/`）。当测试用例与语言规范冲突时，更新测试用例——`src/yux.g4` 和编译器是权威规范，`docs/` 为参考。

## 架构

编译器单二进制（`yux.exe`）完成全流程，无外部汇编器/链接器：

```
.yux → ANTLR4 Lexer/Parser → ASTBuilder → 语义分析
     → Compiler (LLVM IR) → LLVM codegen → LLD link → .exe
```

**源码边界（`src/`）：**
- `main.cpp` — CLI 入口、参数解析
- `yux.cpp/h` — 编译器主类，编排流水线
- `ast_builder.cpp/h` — ANTLR 解析树 → AST 节点（`src/node/` 下 `expr_node`, `fn_node`, `struct_node`, `statement_node`）
- `compiler.cpp/h` — AST → LLVM IR
- `build_cache.cpp/h` — 源文件 mtime+size 缓存，时间戳和大小都匹配时跳过编译
- ANTLR 生成代码在 `gen/`（非 `src/`）

`sdk/` 是自举运行时（yux 自身编写），编译为 `build/sdk.ll` / `build/sdk.obj`，链接到每个 yux 程序。

## 构建输出布局

`build/` **由 xmake 和 yux 编译器共用**：

- xmake 输出：`build/windows/x64/debug/` 及点开头目录（`.objs/`, `.deps/`, `.build_cache/` 等）
- yux 单文件模式：`<srcDir>/build/*.exe`, `*.ll`, `*.obj`, `*.obj.cache`；多段模块 `A.B.C` 展开为 `<buildDir>/A/B/C.obj`
- yux 项目模式：主模块 + 单段导入落在 `<projectRoot>/build/<projectName>/`；多段模块仍按点分路径展开

**清理：**
- 安全清理：删除 `build/*.exe build/*.ll build/*.obj build/*.obj.cache`
- 完全清理：`xmake clean -a`
- **不要直接删除整个 `build/` 目录**

## 编写 yux 代码

生成或修改 yux 源码时，必须遵循以下规则：

**文档优先级：**
1. **先读文档** — 编写 yux 代码前必须查阅 `docs/*.md`（尤其是 `基础语法.md`、`类型系统.md`、`函数.md`、`结构体.md`、`控制流.md`）
2. **其次语法** — 文档不足时参考 `src/yux.g4` 获取精确语法规则
3. **最后编译器** — 仅在必要时阅读编译器源码（`src/*.cpp`、`src/node/*.cpp`）理解行为
4. **禁止参考 Rust** — yux 是独立语言，语义与 Rust 不同，不要假设 Rust 风格的行为

**代码风格要求：**
- **注释**：行注释以 `;` 开头（可缩进）；尾随注释用 ` ;`
- **空格**：关键字后必须有空格，二元运算符两边必须有空格，`,` 后有空格，`()` `[]` 内部无空格
- **强制尾随 `;`**：
  - `ret;` 用于 void 函数提前返回，必须尾随 `;`
  - `break;` 用于 loop 循环跳出，必须尾随 `;`
  - 表达式尾随 `;` 表示空返回，否则返回表达式值
- **无隐式转换** — 必须显式使用 `.to_<类型>()` 方法
