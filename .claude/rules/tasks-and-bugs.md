# 任务与 bug 记录

仓库里有几类 markdown 文件，分工与是否入库各不相同，**不要混用**。

| 文件 | 内容 | 入 git |
|------|------|--------|
| `CURRENT.md` | 当前正在进行的多步骤任务、分阶段计划与勾选 | 否（本地） |
| `CURRENT-*.md` | 并行的其它多步任务 | 否（本地） |
| `BUGS.md` | 开发过程中**新发现**、与当前任务无关、需大量排查或临时绕过的 bug | 否（本地） |
| `MILESTONE.md` | 里程碑 | 是 |
| `docs/dev/*.md` | 已完成版本的实施日志归档（见下） | 是 |
| `docs/spec/*.md` | 语言规范（草案中） | 是 |
| `docs/spec/draft/DRAFT-*.md` | 跨章节的语言面设计草案、决议讨论；骨架范本见 `docs/spec/draft/_模板.md` | 是 |
| `docs/*.md` | 语言用户教程（中文） | 是 |

## 使用规则

### 多步任务 → `CURRENT.md`

- 接到多步任务，先在 `CURRENT.md` 写入分阶段计划（参照该文件现有条目），读后开干，阶段完成就地更新；整个任务完成后删除该条目。
- 单步小修不必写。

### 新发现 bug → `BUGS.md`

按 `BUGS.md` 模板填写，暂停相关任务向用户说明。**进度→`CURRENT.md`，bug→`BUGS.md`，两者不混用**。

### 测试用例命名

SDK 测试（`sdk/yux/src/yux/core/*.test.yux`）按主题建文件，内部 `#Test fn` 名自由但必须 `test_` 前缀。

项目与格式化用例位于 `tests/projects/`，每目录一个 `yux.toml`，由 `tests/xmake.lua` 的 `categorize()` 按目录内文件类型自动分到 `yux/project`（含 `expected.txt`）或 `yux/format`（含 `expected_format`）分组，用例名统一加 `project_` 前缀。

诊断用例位于 `tests/check-cases/`（`diag_*.yux`），由 `yux-check test` 运行，行尾 `; check: EXXXX` 注解精确匹配错误码。

### 测试命令速查

新测试默认 `yux test`（DLL + 多子进程并行）。诊断用 `yux-check test`，项目编译/格式化输出用 `xmake test`。

```powershell
# yux test（项目内，*.test.yux 的 #Test）
# 流程：yux build --test → 并行 spawn yux-test-runner 子进程
yux test                          ; 当前项目所有 #Test
yux test --test-mod yux.core.array    ; 只编译/运行指定模块的测试
yux test --threads 4              ; 指定并行子进程数（默认 CPU 核数）
yux test --verbose                ; 打印每个测试捕获的 stdout/stderr
yux test -d                       ; 调试输出传给 yux build --test
cd sdk/yux && yux test            ; 主测试集

# IR 调试（yux build）
yux build --emit-ir               ; 构建同时输出 .ll
yux build --emit-ir --emit-ir-dir ir_out  ; 指定 .ll 输出目录

# yux-check test（诊断回归，; check: EXXXX 注解）
yux-check test tests/check-cases/   ; 批量诊断测试
yux-check <file>                    ; 单文件诊断（输出 file:line:col [EXXXX]）

# xmake test（项目编译+运行 / 格式化回归）
xmake test -g yux/project          ; 项目编译+运行（tests/projects/*，含 expected.txt）
xmake test -g yux/format           ; 格式化回归（tests/projects/*，含 expected_format）
xmake test yux_tests/project_<name> ; 跑单个用例
```

详细命令见 [docs/命令行工具.md](../../docs/命令行工具.md)。

### 完成一个版本后归档实施记录

`docs/dev/<topic>-impl-log.md` 只用于**标准 / 语言面变更**（spec 条款、AST 节点 / 注解形态、ABI 协议、内置类型语义等触发 `spec-writeback.md` 的工作）。纯工程交付（文件级拆分、工具链整顿、clang-tidy 警告清理之类）**不写 impl-log**，进度归到 `MILESTONE.md` 对应版本条目里即可。

归档时**剔除本地化指代**（人名 / 私人路径 / 邮箱）、剔除测试计数与具体行号（易腐烂），保留：

- 核心决策
- Block layout
- ABI 协议
- 关键文件与函数名
- 跨 Phase 的 TODO 汇总

### 实施日志中引用 BUGS.md 的位置改写为 TODO

`docs/dev/` 入库，`BUGS.md` 不入库，所以日志里"详见 BUGS.md 第 X 条"会变成悬挂引用。改写为该 TODO 本身的简述（如"TODO：Array 声明拷贝漏 retain"），具体诊断与修复进度仍在本地 `BUGS.md` 维护。
