# yux 编译器开发指南

独立编译器，单二进制完成全流程（解析 → LLVM IR → LLD 链接 → exe），无外部汇编器/链接器调用。

**不要改语法文件 `src/yux.g4`**。有问题先暂停，列出需要修改的点交给用户决定。

> 本文件是项目唯一的开发规范来源。凡是想"按惯例推测"的内容，**先来这里核对**；这里没写清楚的，用 Read/Grep 查实际源码和脚本，**不要凭目录名、文件名或命名习惯推断行为**。文档（`docs/`）为中文参考，权威是 `src/yux.g4` + 编译器源码；冲突时更新文档，不要反过来。

## 任务与 bug 记录

本仓库有几类 markdown 文件，分工与是否入库各不相同，**不要混用**。

| 文件 | 内容 | 入 git |
|------|------|--------|
| `CURRENT.md` | 当前正在进行的多步骤任务、分阶段计划与勾选 | 否（本地） |
| `BUGS.md` | 开发过程中**新发现**、与当前任务无关、需大量排查或临时绕过的 bug | 否（本地） |
| `MILESTONE.md` / `TARGETS.md` | 里程碑与短期目标 | 是 |
| `docs/dev/*.md` | 已完成版本的实施日志归档（见下） | 是 |
| `docs/spec/*.md` | 语言规范（草案中） | 是 |
| `docs/spec/draft/DRAFT-*.md` | 跨章节的语言面设计草案、决议讨论；骨架范本见 `docs/spec/draft/_模板.md` | 是 |
| `docs/*.md` | 语言用户教程（中文） | 是 |

**使用规则**：

- **接到多步任务**：先在 `CURRENT.md` 写入分阶段计划（参照该文件现有条目），读后开干，阶段完成就地更新；整个任务完成后删除该条目。单步小修不必写。
- **新发现 bug**：按 `BUGS.md` 模板填写，暂停相关任务向用户说明。**进度→`CURRENT.md`，bug→`BUGS.md`，两者不混用**。
- **完成一个版本后归档实施记录**：把 `CURRENT.md` 里某个大任务（如所有权 v0.1）的 Phase 列表精简后落到 `docs/dev/<topic>-impl-log.md`。归档时**剔除本地化指代**（人名 / 私人路径 / 邮箱）、剔除测试计数与具体行号（易腐烂），保留：核心决策、Block layout、ABI 协议、关键文件与函数名、跨 Phase 的 TODO 汇总。
- **实施日志中引用 BUGS.md 的位置改写为 TODO**：`docs/dev/` 入库，`BUGS.md` 不入库，所以日志里"详见 BUGS.md 第 X 条"会变成悬挂引用。改写为该 TODO 本身的简述（如 "TODO：Array 声明拷贝漏 retain"），具体诊断与修复进度仍在本地 `BUGS.md` 维护。
- **`docs/spec/draft/DRAFT-*.md` 入库，但不等于规范**：草案用来沉淀跨章节设计的讨论与决议，**不一定会实施**；以 `docs/spec/` 正文为准。草案与 spec 冲突时，以 spec + `src/yux.g4` + 编译器源码为准；spec 未收口前，草案仅供参考、不构成实现承诺。新建草案从 `docs/spec/draft/_模板.md` 复制骨架；定型后按模板末尾「定型与归宿」拆分迁入 spec 正文与 CHANGELOG，原 DRAFT 文件删除或在头部标注「已落地，见 §N.M」保留为历史档。
- **改动语言面，必须回写规范**：凡是新增 / 修改 / 删除语言特性、语法形态、用法语义、ABI 协议、内置类型行为等"涉及标准"的变更（无论是先改 spec 再实现，还是先实现再补 spec），落地前**先与用户确认条款措辞**，确认后同步更新：
  1. `docs/spec/` 对应章节（条款 §N.M.K + Open Issues）；
  2. `docs/spec/CHANGELOG.md` 顶部追加一条（上新下旧，记录日期 / 摘要 / 影响章节）；
  3. 如改动了语法形态，附录 A / B 同步对齐 `src/yux.g4`；
  4. 如改动了术语，附录 C 同步增删。
  纯实现细节（性能、缓存、错误信息措辞、内部 helper 重命名等）不触发回写。判断标准：**用户写 yux 代码时能否观察到差别？**能则回写，不能则免。

## 环境与工具链

当前主要面向`windows`开发，核心开发完成后会加入各种跨平台。

- 操作系统：Windows
- 编译器：**Clang（无 MSVC 环境变量）**
- 构建：xmake
- LLVM 工具链在 `PATH`（`llvm/bin`）
- `build/windows/x64/debug` 在 `PATH`，构建后可直接 `yux ...`
- 当前未配置 lint / formatter / clang-tidy，不要假设存在 `npm run lint` / `make fmt` 之类命令
- 会话 shell 可能是 bash 或 PowerShell；下方命令示例按 PowerShell 写，bash 下将 `./sync-deps.ps1` 换成 `./sync-deps.sh`、路径用正斜杠

## 常用命令

```powershell
# 同步第三方依赖（克隆后执行一次；下载 third_party/ 内容和 bin/ 下的二进制工具）
./sync-deps.ps1

# 生成 ANTLR4 解析器代码（修改 src/yux.g4 后执行）
./gen-antlr.ps1

# 构建编译器
xmake build yux
xmake f -m release && xmake build yux   # release 构建（-d 调试 IR 输出仅 debug 可用）

# 项目模式：必须在项目根目录（含 yux.toml）执行
yux build <name>             # <name> 必须与 yux.toml 的 name 一致；入口取 toml 的 entry
                             # 产物：<projectRoot>/build/<name>/<name>.exe
yux build <name> --emit-ir   # 同时生成 .ll
yux build <name> -d          # 编译期 IR 调试输出（仅 Debug 构建；量大，用 tail 过滤）

# 冒烟测试（仓库内 examples/test 的 yux.toml 里 name="test"）
cd examples/test && yux build test && ./build/test/test.exe
```

`yux.toml` 字段（详见 [docs/模块系统.md](docs/模块系统.md)）：
- `name` —— 项目 / exe 名。`yux build <name>` 的 `<name>` 必须与之一致
- `entry` —— 入口 `.yux`，相对项目根
- `version` —— 目前仅记录，编译器不校验

单文件模式 `yux <file>.yux` 在二进制中仍保留，测试 harness 的部分用例还在用，但**已弃用**。新增代码、示例、文档一律走项目模式，不要教用户直接编译单个 `.yux`。

## 测试

测试运行器：[tests/xmake.lua](tests/xmake.lua) 的 `yux_tests` target，走 xmake 原生 `xmake test`（无 googletest / CMake）。三类用例：

| 类别 | 位置 | 对照文件 | 命名要求 | xmake test 名 |
|------|------|---------|---------|---------------|
| 单文件成功用例 | `tests/cases/*.yux` | 同名 `.expected` | —— | `yux_tests/<basename>`（不带 `.yux`） |
| 项目模式用例 | `tests/projects/<case>/` | 同目录 `expected.txt` | 目录内必须有 `yux.toml`、入口源文件、`expected.txt` | `yux_tests/project_<dirname>` |

只测能编译运行的用例（不写错误用例）。单文件用例内部仍以单文件模式调用 `yux <file>`，比较 stdout 与 `.expected`；项目用例以该目录为 CWD 调用 `yux build <case>`，运行 `build/<case>/<case>.exe` 并比对 `expected.txt`（项目用例跑完会清掉 `build/`）。

```powershell
xmake build yux                          # 测试会自动依赖构建，但显式先构建便于定位编译错误
xmake test                               # 全部用例
xmake test -v                            # 失败时打印 stdout / stderr / errors
xmake test yux_tests/basic_types         # 单个用例（注意：不带 .yux 后缀）
xmake test yux_tests/project_imports_struct
xmake test "yux_tests/*"                 # 通配符
```

测试用例与语言规范冲突时，**更新用例**（`src/yux.g4` + 编译器为准）；不要通过修改规范去迁就用例。

开发流程建议：
1. 先改 `examples/test` 或随手建项目做冒烟验证
2. 冒烟过后 `xmake test` 全量回归

## 架构

```
.yux → ANTLR4 Lexer/Parser → ASTBuilder → 语义分析
     → Compiler (LLVM IR) → LLVM codegen → LLD link → .exe
```

源码边界（`src/`）：

- `main.cpp` —— CLI 入口、参数解析
- `yux.cpp/h` —— 编译器主驱动，编排流水线
- `ast_builder.cpp/h` —— ANTLR 解析树 → AST 节点
- `compiler.cpp/h` —— AST → LLVM IR
- `build_cache.cpp/h` —— 源文件 mtime + size 缓存；缓存写为 `<objPath>.cache`，mtime 和 size 都命中则跳过该文件的重新编译
- `mangler.cpp/h` —— 名字重整（模块路径、结构体成员等）
- `node/` —— AST 节点定义：`node`、`file_node`、`expr_node`、`fn_node`、`struct_node`、`statement_node`、`literal_node`、`global_const_node`、`type_node`
- ANTLR 生成代码在 `gen/`（**不要手改**）

运行时位于 `sdk/yux/`（独立的 yux 项目，`yux.toml` 含 `[lib] type="static"`），源码在 `sdk/yux/src/yux/core/`（`base.yux` / `math.yux` / `pkg` 文件等），由 yux 自身编写并编译为静态库 `sdk/yux/build/yux/yux.lib`，链接进每个 yux 程序。

## 目录结构

```
yux-lang/
├── src/              编译器 C++ 源码（见「架构」）
│   └── node/         AST 节点
├── gen/              ANTLR4 生成代码，不要手改
├── include/          公共 C++ 头（types.h）
├── sdk/yux/          自举运行时（独立 yux 项目，编为静态库 yux.lib），链接到每个 yux 程序
├── docs/             语言参考文档（中文）；入口 docs/index.md
│   ├── spec/         语言规范（草案中）
│   │   └── draft/    跨章节设计草案（`DRAFT-<特性>.md` + `_模板.md`），入库但不等同规范
│   └── dev/          已完成版本的实施日志归档（如 ownership-impl-log.md），内容为重大/重要变更实现，其它在git提交记录
├── examples/test/    示例项目，用作快速冒烟测试
├── tests/
│   ├── cases/        单文件用例 + .expected（仅成功用例）
│   ├── projects/     项目模式用例（每目录一个 yux.toml + expected.txt）
│   └── xmake.lua     测试运行器（yux_tests target）
├── third_party/      依赖：antlr4, cli11, llvm, toml11, utfcpp, zlib（由 sync-deps 拉取）
├── bin/              二进制工具：antlr-4.13.2-complete.jar（由 sync-deps 拉取）
├── build/            xmake 与 yux 共用产物目录，详见「构建输出布局」
├── yux-vscode/       VSCode 语法高亮插件
├── yux-idea/         IntelliJ 插件（通过 LSP4IJ 接入 `yux lsp`，含语法高亮 / 配色 / 代码风格）
├── xmake.lua         顶层构建脚本
├── yux.toml          仓库自身的 dogfood 项目配置
├── CURRENT.md        当前多步任务追踪，本地（不入 git）
├── BUGS.md           新发现的 bug 清单，本地（不入 git）
├── TARGETS.md        短期目标，次于里程碑
├── MILESTONE.md      里程碑，当前稳定版目标和已经实现的目标
└── AGENTS.md / CLAUDE.md / README.md
```

## 构建输出布局

- xmake 输出：`build/windows/x64/{debug,release}/`、以及点开头目录（`.objs/`、`.deps/`、`.build_cache/` 等）
- yux 单文件模式：`<srcDir>/build/*.exe`、`*.ll`、`*.obj`、`*.obj.cache`；多段模块 `A.B.C` 展开为 `<buildDir>/A/B/C.obj`
- yux 项目模式：主模块 + 单段导入落在 `<projectRoot>/build/<projectName>/`；多段模块按点分路径在 `build/` 下展开

清理规则：
- 安全清理：`rm build/*.exe build/*.ll build/*.obj build/*.obj.cache`
- 完全清理：`xmake clean -a`
- **不要 `rm -rf build/`**，会一起干掉 xmake 的工作目录

## 编写 yux 代码

**文档优先级（yux 是自有语言，不要类比任何其他语言的语义）：**

1. **先读文档**：`docs/index.md` 是索引，当前含 `基础语法.md`、`类型系统.md`、`内置类型.md`、`函数.md`、`结构体.md`、`控制流.md`、`模块系统.md`、`构建注解.md`、`安装指南.md`（均为中文）
2. **其次语法**：文档不足时查 `src/yux.g4`（权威）
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
  // 处理 Box<T> 类型 (智能指针)
  // Box 需要减少引用计数，如果计数为 0 则释放内存
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
