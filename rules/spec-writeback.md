# 语言规范回写

`docs/spec/draft/DRAFT-*.md` 入库，但不等于规范（不一定实施）。冲突以 spec + g4 + 编译器为准。新建从 `docs/spec/draft/_模板.md` 复制；定型后迁入 spec 正文与 CHANGELOG。

改语言面（特性 / 语法 / 语义 / ABI / 内置类型）落地前先与用户确认措辞，然后：

1. `docs/spec/` 对应章节（§N.M.K + Open Issues）
2. `docs/spec/CHANGELOG.md` 顶部追加（日期 / 摘要 / 影响章节）
3. 改语法 → 附录 A/B 对齐 g4；改术语 → 附录 C

用户写 yux 能观察到差别才回写。纯实现细节（性能、缓存、报错措辞、内部重命名）不回写。
