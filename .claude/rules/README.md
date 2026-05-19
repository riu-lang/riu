# yux-lang 规则模块

本目录是 yux-lang 项目的**规则事实源**，按主题拆分；`.trae/rules` 通过 `sync-deps` 建立 junction 指向这里，Claude Code 与 Trae 共享同一份规则。

开始任务前先看[「怎么用」](#怎么用)；规则与 [`.claude/skills/yux-lang-dev`](../skills/yux-lang-dev/SKILL.md)（怎么构建 / 测 / 写）配套。

## 模块

| 文件 | 内容 |
|------|------|
| [behavior.md](behavior.md) | Claude / agent 通用行为约束（不改 g4、信息查证、写 yux 顺序、多步任务） |
| [tasks-and-bugs.md](tasks-and-bugs.md) | `CURRENT.md` / `BUGS.md` 约定、测试前缀分组、实施日志归档 |
| [spec-writeback.md](spec-writeback.md) | 语言面变更回写流程、`docs/spec/` 与 `docs/spec/draft/` 处理 |
| [sema-codegen.md](sema-codegen.md) | Sema / Codegen 两段分离协议（写 C++ 时遵守） |
| [yux-syntax.md](yux-syntax.md) | yux 语法速查（简版），编辑 `*.yux` 时自动附加；细节去 `docs/` 搜 |
| [directory.md](directory.md) | 仓库目录结构 |

## 怎么用

- 接到任务前，**至少看 [behavior.md](behavior.md) 与 [tasks-and-bugs.md](tasks-and-bugs.md)**；按任务性质再翻其余模块。
- 想"按惯例推测"的内容，**先来这里或技能中核对**；这里没写清楚的，用 Read/Grep 查实际源码和脚本，**不要凭目录名、文件名或命名习惯推断行为**。
- 文档（`docs/`）为中文参考，权威是 `src/yux.g4` + 编译器源码；冲突时更新文档，不要反过来。

## 维护

- 规则文件**入 git**，与 spec 同等约束力；改前先与用户对齐。
- 行为类规则（"该 / 不该做什么"）放 [behavior.md](behavior.md)；流程类（"按什么顺序写什么文件"）放对应主题模块。
- 不要在这里写"怎么构建 / 测 / 写代码"，那是技能 `yux-lang-dev` 的事。
