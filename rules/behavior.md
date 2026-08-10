# 决策框架

适用于 agent 在本仓库工作时。**硬性规则**，每条都必须遵守。

## 核心循环

```
接任务 → CURRENT.md 写计划 → 实现 → 构建 → 中途测试 → ... → 结束回归 → 提交
```

每一步出错都有对应的分叉，不要硬走。

### 中途测试

改完代码、构建通过后立即跑：

```
yux test
```

在 `sdk/yux/` 下运行。快速验证当前改动是否破坏已有行为。

### 结束回归（任务收尾）

任务全部完成、准备提交前跑完整回归：

```powershell
xmake build yux-check
yux-check test tests/check-cases/
xmake test
```

三项全部通过才算验证完毕。详细测试命令见 [engineering.md](engineering.md)。

## 异常分支

### 实现时撞到 bug

先判断是否当前相关，有可能之前引起的：

1. `git stash` 暂存当前改动+`xmake build`构建所有目标
2. 跑相关测试确认基线（`yux test --test-mod <M>` 或 `xmake test yux_tests/<N>`）
3. 基线也挂 → **已有 bug**，`git stash pop` 恢复，记入 `BUGS.md`，绕过继续
4. 基线通过 → **当前改动引入**，`git stash pop`，修掉

### 规范没有明确说可以 = 不允许

遇到规范未覆盖的设计分岔（如"导入符号是否允许覆盖"），**停，问用户**。不准自己拍板 + 补一行注释就当允许。

### 会话恢复 / 继续旧任务

1. 读 `CURRENT.md` + `BUGS.md`
2. `yux build` 确认基线能编译
3. 基线不干净 → 先修到编译通过，再继续

### 基线判定

任务开始前可以先检查一下git工作区（轻量。判断是继续的任务还是新的任务）

git 工作区干净 + `BUGS.md` 无记录 + `CURRENT.md` 无记录 → 上一任务已完结，test 全过。这是当前唯一的可靠基线。

### 改语法文件

`yux/ast/yux*.g4` **只读**。如果任务看起来需要改语法，立刻暂停，列出问题与可能方向，交给用户决定。**不要"先改一点试试"**。

## 强制触发

以下场景**必须先读对应文件再动手**，不读不准写：

| 场景 | 必读 |
|------|------|
| 编辑 `*.yux` | `rules/yux-syntax.md`（写前往下逐条勾） |
| 改 `yux/frontend/sema/` 或 `yux/yux/compiler/` | `rules/sema-codegen.md` |
| 改语言特性 / 语法 / ABI | `rules/spec-writeback.md` |
| 需要各 exe 参数 / 调试流程 | `rules/manuals/manual-yux.md` 等 |

遇到代码BUG需要排查->`rules/manuals/manual-yux.md`排查

> yux-check 小且独立（构建yux不会自动构建check）
> 需要多个exe/任务完成前的编译 -> `xmake build` 构建所有目标。xmake 只支持一次全部/单个目标，不能xmake build a b c

## 信息查证

涉及到测试名、目录路径、文件内容、命令参数时，用工具查实际文件，**不凭命名推断**。

当 docs、`yux/ast/yux*.g4`、编译器三者冲突时，以 `yux/ast/yux*.g4` 和编译器源码为准，随后更新 docs。

## 改完 C++

- 改完 C++ 后立即跑 `./format.ps1`，确保代码风格一致
- 提交前 `./lint.ps1`，必须 **0 warnings**
- 未完成 / 潜在 bug / 待验证 → 必须写 `// TODO:`，不假装没看见
- 注释用中文；`// ====` 分隔区域

## 原则

- **质量 > 速度**。项目复杂，不强制完成任务。不能为了关 CURRENT 而糊过去
- **规范没说 = 不允许**，灰色地带必须问
- **不要拿 Rust / C++ / Go 语义去套 yux**
