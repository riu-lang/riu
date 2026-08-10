# RULES

本项目规则入口，每会话自动加载。不要擅自决定/改动任务方向，遇到问题停下来。

## 规则来源

本项目的事实源拆为两类，开始任何任务前**都要看**，不要凭文件名或 README 推测项目约定：

- [rules/README.md](rules/README.md) —— **硬性规则**，每会话通过本文件加载。[rules/behavior.md](rules/behavior.md) 是核心决策框架——什么情况该做什么、什么时候必须停下来；[rules/engineering.md](rules/engineering.md) 是工程环境与测试流程。
- 手册（`rules/manuals/` + `rules/yux-syntax.md` 等）—— 按需读，触发条件见 behavior.md 强制触发表。

## 核心工作文件

这几个文件是工作核心，不随会话中断消失。项目初期 BUG 多、会话一换任务就丢，走文件才可靠：

| 文件 | 内容 | 入 git |
|------|------|--------|
| `CURRENT.md` | 当前多步任务的分阶段计划与进展 | 否（本地） |
| `CURRENT-*.md` | 并行的其它多步任务 | 否（本地） |
| `BUGS.md` | 开发中发现的 bug（与当前任务无关 / 需大量排查 / 临时绕过） | 否（本地） |
| `MILESTONE.md` | 里程碑，当前稳定版目标与已完成目标 | 是 |

### 多步任务 → `CURRENT.md`

接到多步任务，先在 `CURRENT.md` 写入分阶段计划（参照现有条目），每完成一个阶段就地更新；整个任务完成后删除该条目。单步小修不必写。

### 新发现 bug → `BUGS.md`

按 `BUGS.md` 内模板填写，暂停相关任务向用户说明。**进度 → CURRENT.md，bug → BUGS.md，两者不混用**。

### 版本日志 → `docs/dev/`

`docs/dev/<topic>-impl-log.md` 只用于标准 / 语言面变更（spec 条款、AST 节点 / 注解形态、ABI 协议、内置类型语义等触发 spec-writeback 的工作）。纯工程交付不写 impl-log，进度归到 MILESTONE.md。

实施日志中引用 BUGS.md 的位置改写为 TODO 简述（如"TODO：Array 声明拷贝漏 retain"），因为 BUGS.md 不入库。

## 最低限度阅读

开始任务前至少看：
- [rules/behavior.md](rules/behavior.md) — 决策框架（强制）
- [rules/directory.md](rules/directory.md) — 目录结构

## 其他参考

- [docs/index.md](docs/index.md) — 语言文档索引（中文）
- [yux/ast/yuxParser.g4](yux/ast/yuxParser.g4) / [yux/ast/yuxLexer.g4](yux/ast/yuxLexer.g4) — 权威 ANTLR4 语法（**只读**）
- [README.md](README.md) — 面向用户的概述
