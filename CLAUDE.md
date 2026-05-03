# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 规则来源

本项目的事实源拆为两份，开始任何任务前**两份都要读**，不要凭文件名或 README 推测项目约定：

- [AGENTS.md](AGENTS.md) —— **项目规则与目录结构**（任务/bug 记录约定、规范回写流程、目录树）
- 技能 `yux-lang-dev`（[.claude/skills/yux-lang-dev/SKILL.md](.claude/skills/yux-lang-dev/SKILL.md)）—— **怎么构建、怎么测、怎么写**（环境工具链、命令、架构、构建输出布局、测试流程、yux/C++ 代码风格）。通过 Skill 工具调用 `yux-lang-dev` 加载。

其他参考：
- [docs/index.md](docs/index.md) — 语言文档索引（中文）
- [src/yux.g4](src/yux.g4) — 权威 ANTLR4 语法（**只读**）
- [README.md](README.md) — 面向用户的概述

## 仅针对 Claude 的行为约束

以下是 Claude Code 在本仓库工作时必须遵守的、超出 AGENTS.md 通用规则之外的附加行为：

- **不要自行修改 `src/yux.g4`**：如果任务看起来需要改语法，立刻暂停，列出遇到的问题与可能的修改方向，交给用户决定。不要"先改一点试试"。
- **多步任务必须先落到 `CURRENT.md`**：接到多步骤任务时，先在 `CURRENT.md` 写入分阶段计划（格式参照该文件现有条目），每完成一个阶段就地更新；整个任务完成后删除该条目。单步小修不需要写。
- **新发现的 bug 写入 `BUGS.md`**：指的是**与当前任务无关**、或需大量排查、或临时绕过的 bug。按文件里的模板填写，然后暂停相关任务并告知用户。不要把它和 `CURRENT.md` 混用。
- **信息不足时先查证，不要编造**：涉及到具体的测试名、目录路径、文件内容、命令参数时，用 Read/Grep/Glob 查实际文件，不要凭命名推断；当 docs、yux.g4、编译器三者冲突时以 `src/yux.g4` 和编译器源码为准，随后更新 docs，不要反过来。
- **写 yux 代码前按顺序读文档**：先 `docs/*.md`（中文），再 `src/yux.g4`，最后才看 `src/*.cpp`。不要拿 Rust / C++ / Go 的语义去套 yux。
