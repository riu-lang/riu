# 任务与 bug 记录

仓库里有几类 markdown 文件，分工与是否入库各不相同，**不要混用**。

| 文件 | 内容 | 入 git |
|------|------|--------|
| `CURRENT.md` | 当前正在进行的多步骤任务、分阶段计划与勾选 | 否（本地） |
| `CURRENT-*.md` | 并行的其它多步任务 | 否（本地） |
| `BUGS.md` | 开发过程中**新发现**、与当前任务无关、需大量排查或临时绕过的 bug | 否（本地） |
| `MILESTONE.md` / `TARGETS.md` | 里程碑与短期目标 | 是 |
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

### 测试用例命名必须沿用已有前缀

`tests/xmake.lua` 按文件名前缀把用例分到 `yux/<cat>` 分组（`borrow_*` → `yux/borrow`、`diag_*` → `yux/diag` 等，全表见 `yux-lang-dev` 技能的「测试」章节）。

落不到任何前缀的用例会进 `yux/misc`，**使分组失效**。优先用前缀命名；确实没有自然前缀的（算术 / 类型 / 控制流家族），把名字加进 `categorize` 中对应的白名单表。

### 完成一个版本后归档实施记录

把 `CURRENT.md` 里某个大任务（如所有权 v0.1）的 Phase 列表精简后落到 `docs/dev/<topic>-impl-log.md`。

归档时**剔除本地化指代**（人名 / 私人路径 / 邮箱）、剔除测试计数与具体行号（易腐烂），保留：

- 核心决策
- Block layout
- ABI 协议
- 关键文件与函数名
- 跨 Phase 的 TODO 汇总

### 实施日志中引用 BUGS.md 的位置改写为 TODO

`docs/dev/` 入库，`BUGS.md` 不入库，所以日志里"详见 BUGS.md 第 X 条"会变成悬挂引用。改写为该 TODO 本身的简述（如"TODO：Array 声明拷贝漏 retain"），具体诊断与修复进度仍在本地 `BUGS.md` 维护。
