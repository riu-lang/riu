# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 规则来源

本项目的事实源拆为两份，开始任何任务前**两份都要读**，不要凭文件名或 README 推测项目约定：

- [AGENTS.md](AGENTS.md) —— **项目规则与目录结构**（任务/bug 记录约定、规范回写流程、目录树）
- 技能 `yux-lang-dev`（[.claude/skills/yux-lang-dev/SKILL.md](.claude/skills/yux-lang-dev/SKILL.md)）—— **怎么构建、怎么测、怎么写**（环境工具链、命令、架构、构建输出布局、测试流程、yux/C++ 代码风格）。通过 Skill 工具调用 `yux-lang-dev` 加载。

其他参考：
- [docs/index.md](docs/index.md) — 语言文档索引（中文）
- [src/yuxParser.g4](src/yuxParser.g4) — 权威 ANTLR4 语法（**只读**）
- [src/yuxLexer.g4](src/yuxLexer.g4) — 分词（**只读**）
- [README.md](README.md) — 面向用户的概述

## 仅针对 Claude 的行为约束

以下是 Claude Code 在本仓库工作时必须遵守的、超出 AGENTS.md 通用规则之外的附加行为：

- **不要自行修改 `src/yux*.g4`**：如果任务看起来需要改语法，立刻暂停，列出遇到的问题与可能的修改方向，交给用户决定。不要"先改一点试试"。
- **多步任务必须先落到 `CURRENT.md`**：接到多步骤任务时，先在 `CURRENT.md` 写入分阶段计划（格式参照该文件现有条目），每完成一个阶段就地更新；整个任务完成后删除该条目。单步小修不需要写。
- **新发现的 bug 写入 `BUGS.md`**：指的是**与当前任务无关**、或需大量排查、或临时绕过的 bug。按文件里的模板填写，然后暂停相关任务并告知用户。不要把它和 `CURRENT.md` 混用。
- **信息不足时先查证，不要编造**：涉及到具体的测试名、目录路径、文件内容、命令参数时，用 Read/Grep/Glob 查实际文件，不要凭命名推断；当 docs、yux.g4、编译器三者冲突时以 `src/yux*.g4` 和编译器源码为准，随后更新 docs，不要反过来。
- **写 yux 代码前按顺序读文档**：先 `docs/*.md`（中文），再 `src/yux*.g4`，最后才看 `src/*.cpp`。不要拿 Rust / C++ / Go 的语义去套 yux。

## Sema / Codegen 协议（写 C++ 时遵守）

项目正在长期推进 Sema/Codegen 两段分离（最终目标：`yux-check` 独立 exe，0 LLVM 依赖，与 `yux build` 错误覆盖等价）。当前是**半完成态**，写新代码必须注意：

- **`src/sema/` = 0 LLVM 依赖**：sema 相关代码进 `src/sema/sema_pass.{h,cpp}` 与 `src/sema/call_resolve.{h,cpp}`，由静态库 `yux_frontend` 提供。不要在这两个文件里 `#include "llvm/..."` 或调 `IRBuilder` / `_module` 等 codegen 状态。
- **新加 `throw YuxError` 时**：
  - 默认不强求 sema 镜像；写在 `src/compiler/compiler_*.cpp` 里照常即可。
  - 若顺手让 sema 接管（推荐对纯静态形态检查这么做），**必须同步更新 `src/sema/sema_pass.cpp` 顶部的 `kMigratedCodes` 白名单**。漏更新 = sema 抛了又被自身的 try/catch 吞掉，**无测试能捕获**。
  - sema 接管后 Compiler 端的原 inline throw 保留作"幂等防御性双跑"（不要删），加注释说明已被 sema shadow。
- **新加 AST 节点 / 表达式类**：必须在 `SemaPass::visitExpr` (`src/sema/sema_pass.cpp`) 加一条 dynamic_cast 分支，哪怕只是 `return;` 占位，否则 sema 会静默 skip 整个子树。
- **sema 当前缺口**（出错时不报，靠 codegen 兜底）：泛型 fn / impl 体、lambda 体、所有 statement、target-type 上下文驱动的类型检查、`compiler_types.cpp` 的 alias 环检测。在这些路径里加 throw 时 sema 镜像不会生效，省事写 Compiler 端即可。
- **快速诊断**：本地写 demo / 改代码后想快速跑诊断，用 `yux-check <file.yux>`（0 LLVM，编译几秒）；要完整覆盖才走 `yux build`。yux-check 报错是 `yux build` 报错的**子集**，不报错不代表无错。
