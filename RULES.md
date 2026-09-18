# RULES

每会话入口（`AGENTS.md` → 本文件）。不要擅自改任务方向；卡住就停下来问。规范没写 = 不允许。不要用 Rust / C++ / Go 语义套 riu。

`riu/ast/riu.bnf` **只读**。任务看起来要改语法 → 立刻停，列问题给用户。权威：`riu.bnf` + 编译器源码 > docs（冲突时改 docs）。路径 / 测试名 / 命令参数查实际文件，不凭命名猜。

## 工作文件

| 文件 | 用途 | git |
|------|------|-----|
| `CURRENT.md` / `CURRENT-*.md` | 多步任务计划 | 否 |
| `BUGS.md` | 与当前任务无关 / 需大量排查的 bug | 否 |
| `notes/<ver>.md` | 本版落地（加 / 修），一份记录 | 是 |
| `notes/_模板.md` | 切小版本时复制为 `notes/<ver>.md` | 是 |
| `MILESTONE.md` | 大版本目标（草稿）v1 / v2 | 是 |
| `docs/spec/prop/` | 新语法/规范提议（#N，不绑版本） | 是 |
| `docs/spec/CHANGELOG.md` | spec 条款 diff | 是 |

落地只追加 `notes/<ver>.md`（`ver` = `build/version.gni` 的 `x.y`）。切版复制 `notes/_模板.md`。新语法/规范复制 `docs/spec/prop/_模板.md`，状态写在提议文件头部（同步 `prop/README.md` 表）。提案不绑实现版本；切片写 notes，全部完成才改 `已落地`。不要再抄进 MILESTONE。不要再改 `docs/spec/draft/`。不要再往 `docs/dev/` 写新 impl-log。改 spec 才写 spec CHANGELOG。不切版就不要改 `build/version.gni`。

多步任务写入 `CURRENT.md`，阶段更新，完成简单保留；单步小修不写。新 bug 用 `BUGS.md` 模板。进度和 bug 不混。修完可把 BUGS 条删掉，回归路径写进 notes。

继续旧任务：读 CURRENT + BUGS，确认能编过。撞到可能是旧 bug：`git stash` → `./build.ps1` → 跑相关测试。基线也挂 → 记 BUGS.md 后绕过；基线过 → 当前引入，修掉。工作区干净 + CURRENT/BUGS 空 = 上一任务已完结。质量优先，不强制关 CURRENT。

## Git

提交：`type: 中文说明`，可 `type(scope):`。说明写结果/原因，不列文件。可选正文用 `- ` 条。一条一事。

| type | 何时 |
|------|------|
| `feat` | 新能力 |
| `fix` | 修 bug |
| `refactor` | 行为不变的结构调整 |
| `docs` | 文档 / 规则 / spec 文本 |
| `test` | 只加/改测试 |
| `perf` | 性能 |
| `chore` | 构建脚本、依赖、杂项 |
| `style` | 仅格式 |

不要 `WIP`、不要中文冒号（`feat：`）。

忽略：`CURRENT*.md` / `BUGS.md` / `build/windows/` / `third_party/` / `bin/` / `.cursor/`。文本 `* text=auto eol=lf`。`third_party/` 只走 `./sync-deps.ps1`。不要改 git config。

`AGENTS.md` → `RULES.md`（仓库里唯一符号链接）。junction（`mklink /J`）不要 `git add`。

## 按需（动手前读）

| 场景 | 文件 |
|------|------|
| 写 `*.ut` | [rules/riu-syntax.md](rules/riu-syntax.md) |
| 改 `riu/frontend/sema/` 或 `riu/riu/compiler/` | [rules/sema-codegen.md](rules/sema-codegen.md) |
| 改语言特性 / 语法 / ABI | [rules/spec-writeback.md](rules/spec-writeback.md) |
| CLI / 脚本参数 | `riu --help`、`riu build --help`、`./build.ps1 --help` 等，不维护手册 md |

## 环境 / 构建

Windows + Clang（无 MSVC 作编译器；仍需 VS 的 Windows SDK / STL）。`build/windows/x64/debug/bin` 在 PATH。`riu-check` 独立 exe，`./build.ps1 riu` 不会编它。`riu` 只有 `build` / `test` / `format`，没有 `riu file.ut`；仓库根没有 `riu.toml`，`riu build` / `riu test` 不能在仓库根跑。

| 改动 | 重编 |
|------|------|
| `riu/riu/compiler/` | `./build.ps1 riu` |
| `riu/frontend/` | `./build.ps1 riu riu-check` |
| `riu/ast/` | `./build.ps1 riu riu-check riu-ast` |
| `riu/lsp/` | `./build.ps1 riu-lsp` |
| `riu/test-runner/` | `./build.ps1 riu-test-runner` |

新 `.cpp` / `.h` 写入对应 `BUILD.gn` 的 `sources`，否则 ninja 编不到。`frontend` / `check` / `lsp` / `ast` / `analyzer` 不加 LLVM。

## 验证

| 做什么 | cwd | 命令 |
|--------|-----|------|
| 单文件诊断 | 任意 | `riu-check <file.ut>` |
| 诊断回归 | 仓库根 | `riu-check test tests/check-cases/`（子集 `diag_*` / `**/*`；`*` 不跨目录，递归用 `**`，无 `-r`） |
| SDK `#Test` | `sdk/riu/` | `riu test`（`--verbose` / `--test-mod <M>` / `--threads N`） |
| 项目回归 | 仓库根 | `./build.ps1 test`（`-Jobs 1` 串行） |

中途：改了什么跑什么。收尾：先按「改动 / 重编」把 exe 编好，再跑上表三套回归。

测试崩溃：DLL 无摘要行 → 在 `sdk/riu/` 下 `--verbose` → `--test-mod` → `riu build --test -d`。

改完 C++ 立刻 `./format.ps1`；完成修改+测试通过后 `./lint.ps1` **0 warnings** （无打印的warning）。注释中文；`// ====` 分区；未完成 / 待验证写 `// TODO:`。新诊断码：`riu/include/error_code.h` 段内递增；用户能看到才按 spec-writeback 同步附录 D。

## 加测试

改了行为就加回归，放对套（`examples/` 给用户看，不是回归）：

| 测什么 | 放哪 | 形态 |
|--------|------|------|
| SDK / 运行时 | `sdk/riu/src/**/*.test.ut` | `#Test fn`；`sdk/riu/` 下 `riu test` |
| 诊断（sema，单文件） | `tests/check-cases/` | 行尾 `; check: E1234`（可多个码）；无错则无注解；习惯名 `diag_*` / `*_ok` |
| 整项目编跑 | `tests/projects/<name>/` | `riu.toml` + `expected.txt`（stdout） |
| 预期编译失败 | 同上 | `expected_fail.txt`（对照 stderr） |
| 格式化 | 同上 | `expected_format/` |

多文件 / 包边界 / 必须 LLVM 编出来才暴露的，走 `tests/projects/`。只要报错码、不跑 exe，走 check-cases。

## 目录

```
riu/                 构建子系统（各自 BUILD.gn）
  riu/compiler/      LLVM IR（riu.exe）
  riu/cli/           build / test / format
  include/           公共头（error_code.h 等）
  rt/                C99 运行时 riurt.lib
  ast/               rd Scanner/Parser + FileNode；riu.bnf 语法权威
  analyzer/          语义分析器
  frontend/          sema + tools + formatter（0 LLVM）
  check/             riu-check
  lsp/               riu-lsp
  test-runner/       riu test 内部 spawn
sdk/riu/src/riu/core/  自举 runtime + *.test.ut
docs/spec/           语言规范；docs/dev/ 旧实施日志（只读归档）
notes/               本版落地
examples/            用户示例（非回归）
tests/projects/      项目回归；tests/check-cases/ 诊断用例
build/               GN；plugins/ 编辑器；third_party/ 依赖
rules/               按需规则（syntax / sema / spec-writeback）
RULES.md             会话入口；AGENTS.md → RULES.md
*.ps1                build / sync-deps / lint / format
```
