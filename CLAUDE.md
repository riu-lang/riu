# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 规则来源

本项目的事实源拆为两类，开始任何任务前**都要看**，不要凭文件名或 README 推测项目约定：

- [.claude/rules/](.claude/rules/README.md) —— **项目规则**（行为约束、任务/bug 记录、spec 回写、sema 协议、目录结构），按主题分模块。`.trae/rules` 通过 `sync-deps` junction 指向同一份。
- 技能 `yux-lang-dev`（[.claude/skills/yux-lang-dev/SKILL.md](.claude/skills/yux-lang-dev/SKILL.md)）—— **怎么构建、怎么测、怎么写**（环境工具链、命令、架构、构建输出布局、测试流程、yux/C++ 代码风格）。通过 Skill 工具调用 `yux-lang-dev` 加载。

最低限度先看 [`.claude/rules/behavior.md`](.claude/rules/behavior.md) 与 [`.claude/rules/tasks-and-bugs.md`](.claude/rules/tasks-and-bugs.md)；按任务性质再翻其余模块（[索引](.claude/rules/README.md)）。

**编辑 `*.yux` 时**：手动读 [`rules/yux-syntax.md`](rules/yux-syntax.md)（简版速查），细节去 `docs/` 搜。

其他参考：
- [docs/index.md](docs/index.md) — 语言文档索引（中文）
- [src/yuxParser.g4](src/yuxParser.g4) / [src/yuxLexer.g4](src/yuxLexer.g4) — 权威 ANTLR4 语法（**只读**）
- [README.md](README.md) — 面向用户的概述
- [组织架构](yux.md)
