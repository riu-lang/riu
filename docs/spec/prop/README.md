# 规范提议

本地 issue。不依赖 git server。**状态以各文件头部为准**；本表是目录，改状态时改头部并改表一行。

编号为递增正整数，不补零、不限位数。流程：复制 [`_模板.md`](_模板.md) → `N-短标题.md`（N = 表中最大编号 + 1）→ 填提议 → 评估 → `待实施` 或 `关闭`。

提案即全部范围，不绑实现版本。实施可跨多个版本：每切片写 `notes/<ver>.md` 并追加本文件「落地」；全部完成才改 `已落地` 并回写 spec 正文。

未到 `待实施` 不要改 spec 正文 / g4。改 g4 仍须先停下来拍板。

旧 `DRAFT-*` 已冻结，见 [`../archive.md`](../archive.md)。展开时把需要的结论抄进本提议「规范要点」，不要继续改旧草案。

状态：
- `提议`，未确定，还在修改评估
- `待实施`，已确定，还未开始
- `实施中`，已确定，正在修改代码
- `已落地`，已实施完成，归档
- `关闭`，提案至此关闭，本次不再做修改和实施。不影响新的相似提案

| # | 标题 | 状态 | 文件 |
|---|------|------|------|
| 1 | 错误模型 v2 | 提议 | [1-error-v2.md](1-error-v2.md) |
| 2 | `Iter<T, E>` / 泛型 for-in | 提议 | [2-iter.md](2-iter.md) |
| 3 | 泛型 enum | 已落地 | [3-generic-enum.md](3-generic-enum.md) |
| 4 | 反射补全 | 提议 | [4-reflect.md](4-reflect.md) |
| 5 | `format` 类 printf | 提议 | [5-format.md](5-format.md) |
| 6 | 条件编译 + 用户注解 | 提议 | [6-cond-compile.md](6-cond-compile.md) |
| 7 | 代码生成代码（派生） | 提议 | [7-codegen.md](7-codegen.md) |
| 8 | 异步 / 并发 | 提议 | [8-async.md](8-async.md) |
| 9 | 包管理与远程依赖 | 提议 | [9-pkg.md](9-pkg.md) |
| 10 | 多目标 | 提议 | [10-multi-target.md](10-multi-target.md) |
| 11 | YAML AST / 深拷会话 / 进程内并行 SemaPass | 关闭 | [11-yaml-ast.md](11-yaml-ast.md) |
| 12 | SDK：正则 / `Command` / JSON | 提议 | [12-sdk-surface.md](12-sdk-surface.md) |
| 13 | 工具链（LSP / 测试 / 文档） | 提议 | [13-toolchain.md](13-toolchain.md) |
| 14 | 性能 / layout | 提议 | [14-perf.md](14-perf.md) |
| 15 | 泛型 spec | 已落地 | [15-spec-generic.md](15-spec-generic.md) |
| 16 | 分离 impl | 提议 | [16-impl-split.md](16-impl-split.md) |
