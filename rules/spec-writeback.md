# 语言规范回写

提案和 spec **仅**覆盖：

- **词法**（§1 / `riu.bnf` 词法）
- **语法**（§2 及语言语义：类型、表达式、语句、`riu.bnf` 文法）
- **`riu.core`**（隐式 core 包公开面）

**不**覆盖 stdlib（`sdk/stdlib`：`riu.io` / `riu.time` / `riu.stream` / `riu.platform` 等）、net、工具链。那些以源码为准，不开 [`docs/spec/prop/`](../docs/spec/prop/README.md)，不回写 spec 正文 / CHANGELOG。

词法 / 语法 / core 的新条款只走 `prop/`：`提议` → `评估` → `待实施` 或 `关闭`。未到 `待实施` 不要改 spec 正文。旧 `DRAFT-*` 冻结，见 [`docs/spec/archive.md`](../docs/spec/archive.md)。

冲突以 spec 正文 + `riu.bnf` + 编译器为准（冲突时改 docs）。改 `riu.bnf` 必须先停下来拍板。

落地前先与用户确认措辞。提案不绑实现版本，实施可跨多个版本：

1. 切片：`notes/<ver>.md` 一条；提议「落地」追加一行；状态保持 `实施中`（同步目录表）
2. 全部完成：状态 → `已落地`
3. 用户可见语义：`docs/spec/` 对应章节 + `CHANGELOG.md` 顶部一条
4. 改语法 → 附录 A/B 对齐 `riu.bnf`；改术语 → 附录 C

用户写 riu 能观察到差别才回写 spec。纯实现细节不回写。新未决项开提议，不往章节 Open Issues 堆。
