# RULES

每会话入口。不要擅自改任务方向；卡住就停下来问。规范没写 = 不允许。不要用 Rust / C++ / Go 语义套 yux。

`yux/ast/yux*.g4` **只读**。任务看起来要改语法 → 立刻停，列问题给用户。权威：g4 + 编译器源码 > docs（冲突时改 docs）。路径 / 测试名 / 命令参数查实际文件，不凭命名猜。

## 工作文件

| 文件 | 用途 | git |
|------|------|-----|
| `CURRENT.md` / `CURRENT-*.md` | 多步任务计划 | 否 |
| `BUGS.md` | 与当前任务无关 / 需大量排查的 bug | 否 |
| `MILESTONE.md` | 稳定版目标 | 是 |

多步任务写入 `CURRENT.md`，阶段更新，完成后删条目；单步小修不写。新 bug 用 `BUGS.md` 模板。进度和 bug 不混。语言面变更才写 `docs/dev/<topic>-impl-log.md`（里面把 BUGS 改写成 TODO 简述）；纯工程进度归 MILESTONE.md。

继续旧任务：读 CURRENT + BUGS，确认能编过。撞到可能是旧 bug：`git stash` → `./build.ps1` → 跑相关测试。基线也挂 → 记 BUGS.md 后绕过；基线过 → 当前引入，修掉。工作区干净 + CURRENT/BUGS 空 = 上一任务已完结。质量优先，不强制关 CURRENT。

## 按需（动手前读）

| 场景 | 文件 |
|------|------|
| 写 `*.yux` | [rules/yux-syntax.md](rules/yux-syntax.md) |
| 改 `yux/frontend/sema/` 或 `yux/yux/compiler/` | [rules/sema-codegen.md](rules/sema-codegen.md) |
| 改语言特性 / 语法 / ABI | [rules/spec-writeback.md](rules/spec-writeback.md) |
| CLI / 脚本参数 | `yux --help`、`yux build --help`、`./build.ps1 --help` 等，不维护手册 md |

## 环境 / 构建

Windows + Clang（无 MSVC 作编译器；仍需 VS 的 Windows SDK / STL）。`build/windows/x64/debug/bin` 在 PATH。`yux-check` 独立 exe，`./build.ps1 yux` 不会编它。`yux` 只有 `build` / `test` / `format`，没有 `yux file.yux`；仓库根没有 `yux.toml`，`yux build` / `yux test` 不能在仓库根跑。

| 改动 | 重编 |
|------|------|
| `yux/yux/compiler/` | `./build.ps1 yux` |
| `yux/frontend/` | `./build.ps1 yux yux-check` |
| `yux/ast/` | `./build.ps1 yux yux-check yux-ast` |
| `yux/lsp/` | `./build.ps1 yux-lsp` |
| `yux/test-runner/` | `./build.ps1 yux-test-runner` |
| `yux/ast/yux*.g4` | `./gen-antlr.ps1` → 上面全部 |

## 验证

| 做什么 | cwd | 命令 |
|--------|-----|------|
| 单文件诊断 | 任意 | `yux-check <file.yux>` |
| 诊断回归 | 仓库根 | `yux-check test tests/check-cases/`（子集 `diag_*` / `**/*`；`*` 不跨目录，递归用 `**`，无 `-r`） |
| SDK `#Test` | `sdk/yux/` | `yux test`（`--verbose` / `--test-mod <M>` / `--threads N`） |
| 项目回归 | 仓库根 | `./build.ps1 test`（`-Jobs 1` 串行） |

中途：改了什么跑什么。收尾：先按「改动 / 重编」把 exe 编好，再跑上表三套回归。

测试崩溃：DLL 无摘要行 → 在 `sdk/yux/` 下 `--verbose` → `--test-mod` → `yux build --test -d`。

改完 C++ 立刻 `./format.ps1`；完成修改+测试通过后 `./lint.ps1` **0 warnings**。注释中文；`// ====` 分区；未完成 / 待验证写 `// TODO:`。

## 目录

```
yux/                 构建子系统（各自 BUILD.gn）
  yux/compiler/      LLVM IR（yux.exe）
  yux/cli/           build / test / format
  rt/                C99 运行时 yuxrt.lib
  ast/               ANTLR4 + AST；gen/ 与 yux*.g4 不要手改
  analyzer/          语义分析器
  frontend/          sema + tools + formatter（0 LLVM）
  check/             yux-check
  lsp/               yux-lsp
  test-runner/       yux test 内部 spawn
sdk/yux/src/yux/core/  自举 runtime + *.test.yux
docs/spec/           语言规范；docs/dev/ 实施日志
tests/projects/      项目回归；tests/check-cases/ 诊断用例
build/               GN；plugins/ 编辑器；third_party/ 依赖
rules/               按需规则（syntax / sema / spec-writeback）
*.ps1                build / sync-deps / gen-antlr / lint / format
```
