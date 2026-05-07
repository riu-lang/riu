# yux 编译器开发规则

独立编译器，单二进制完成全流程（解析 → LLVM IR → LLD 链接 → exe），无外部汇编器/链接器调用。

**不要改语法文件 `src/yux*.g4`**。有问题先暂停，列出需要修改的点交给用户决定。

> 本文件只保留**项目规则**与**目录结构**两类内容，作为唯一事实源。**怎么构建、怎么测、怎么写**（环境、命令、架构、测试流程、yux/C++ 代码风格）放在技能 `yux-lang-dev` 的 SKILL.md，开任务前两份都要看。
>
> 凡是想"按惯例推测"的内容，**先来这里或技能中核对**；这里没写清楚的，用 Read/Grep 查实际源码和脚本，**不要凭目录名、文件名或命名习惯推断行为**。文档（`docs/`）为中文参考，权威是 `src/yux.g4` + 编译器源码；冲突时更新文档，不要反过来。

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
- **新增测试用例必须沿用已有前缀**：`tests/xmake.lua` 按文件名前缀把用例分到 `yux/<cat>` 分组（`borrow_*` → `yux/borrow`、`diag_*` → `yux/diag` 等，全表见 `yux-lang-dev` 技能的「测试」章节）。落不到任何前缀的用例会进 `yux/misc`，使分组失效。优先用前缀命名；确实没有自然前缀的（算术/类型/控制流家族），把名字加进 `categorize` 中对应的白名单表。
- **完成一个版本后归档实施记录**：把 `CURRENT.md` 里某个大任务（如所有权 v0.1）的 Phase 列表精简后落到 `docs/dev/<topic>-impl-log.md`。归档时**剔除本地化指代**（人名 / 私人路径 / 邮箱）、剔除测试计数与具体行号（易腐烂），保留：核心决策、Block layout、ABI 协议、关键文件与函数名、跨 Phase 的 TODO 汇总。
- **实施日志中引用 BUGS.md 的位置改写为 TODO**：`docs/dev/` 入库，`BUGS.md` 不入库，所以日志里"详见 BUGS.md 第 X 条"会变成悬挂引用。改写为该 TODO 本身的简述（如 "TODO：Array 声明拷贝漏 retain"），具体诊断与修复进度仍在本地 `BUGS.md` 维护。
- **`docs/spec/draft/DRAFT-*.md` 入库，但不等于规范**：草案用来沉淀跨章节设计的讨论与决议，**不一定会实施**；以 `docs/spec/` 正文为准。草案与 spec 冲突时，以 spec + `src/yux.g4` + 编译器源码为准；spec 未收口前，草案仅供参考、不构成实现承诺。新建草案从 `docs/spec/draft/_模板.md` 复制骨架；定型后按模板末尾「定型与归宿」拆分迁入 spec 正文与 CHANGELOG，原 DRAFT 文件删除或在头部标注「已落地，见 §N.M」保留为历史档。
- **改动语言面，必须回写规范**：凡是新增 / 修改 / 删除语言特性、语法形态、用法语义、ABI 协议、内置类型行为等"涉及标准"的变更（无论是先改 spec 再实现，还是先实现再补 spec），落地前**先与用户确认条款措辞**，确认后同步更新：
  1. `docs/spec/` 对应章节（条款 §N.M.K + Open Issues）；
  2. `docs/spec/CHANGELOG.md` 顶部追加一条（上新下旧，记录日期 / 摘要 / 影响章节）；
  3. 如改动了语法形态，附录 A / B 同步对齐 `src/yux.g4`；
  4. 如改动了术语，附录 C 同步增删。
  纯实现细节（性能、缓存、错误信息措辞、内部 helper 重命名等）不触发回写。判断标准：**用户写 yux 代码时能否观察到差别？**能则回写，不能则免。

## 目录结构

```
yux-lang/
├── src/              编译器 C++ 源码
│   └── node/         AST 节点
├── gen/              ANTLR4 生成代码，不要手改
├── include/          公共 C++ 头（types.h）
├── sdk/yux/          自举运行时（独立 yux 项目，编为静态库 yux.lib），链接到每个 yux 程序
├── docs/             语言参考文档（中文）；入口 docs/index.md
│   ├── spec/         语言规范（草案中）
│   │   └── draft/    跨章节设计草案（`DRAFT-<特性>.md` + `_模板.md`），入库但不等同规范
│   └── dev/          已完成版本的实施日志归档（如 ownership-impl-log.md），内容为重大/重要变更实现，其它在 git 提交记录
├── examples/test/    示例项目，用作快速冒烟测试
├── tests/
│   ├── cases/        单文件用例 + .expected（仅成功用例）
│   ├── projects/     项目模式用例（每目录一个 yux.toml + expected.txt）
│   └── xmake.lua     测试运行器（yux_tests target）
├── third_party/      依赖：antlr4, cli11, llvm, toml11, utfcpp, zlib（由 sync-deps 拉取）
├── bin/              二进制工具：antlr-4.13.2-complete.jar（由 sync-deps 拉取）
├── build/            xmake 产物目录（布局详见 yux-lang-dev 技能）
├── plugins/          编辑器 / 客户端插件（同时是 Claude Code marketplace 根，含 .claude-plugin/marketplace.json）
│   ├── yux-vscode/       VSCode 语法高亮插件（LSP 客户端走 `yux-lsp`）
│   ├── yux-idea/         IntelliJ 插件（通过 LSP4IJ 接入 `yux-lsp`，含语法高亮 / 配色 / 代码风格）
│   └── yux-claude-code/  Claude Code LSP 插件（marketplace 名 `yux-lang-lsp@yux-lang`，对接独立可执行 `yux-lsp-claude`）
├── .claude/skills/   本仓库专属 Claude Code 技能（含 yux-lang-dev 上手指南）
├── xmake.lua         顶层构建脚本
├── yux.toml          仓库自身的 dogfood 项目配置
├── CURRENT.md        当前多步任务追踪，本地（不入 git）
├── CURRENT-*.md      其它任务，本地（不入 git）
├── BUGS.md           新发现的 bug 清单，本地（不入 git）
├── TARGETS.md        短期目标，次于里程碑
├── MILESTONE.md      里程碑，当前稳定版目标和已经实现的目标
└── AGENTS.md / CLAUDE.md / README.md
```