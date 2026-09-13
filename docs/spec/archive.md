# 旧草案归档

`docs/spec/draft/` 已冻结，**不要再改、不要再按旧模板开新草案**。新语法/规范走 [`prop/`](prop/)。

本表是旧档结论（文件头可能过时，**以本表为准**）。后面要展开某主题：打开对应 `DRAFT-*`，把仍有效的决议抄进新提议的「规范要点」，然后只维护提议文件。

| 旧文件 | 主题 | 结论 | spec |
|--------|------|------|------|
| [DRAFT-所有权与引用.md](draft/DRAFT-所有权与引用.md) | 所有权 / 借用 | 已落地 | §8 |
| [DRAFT-draft.md](draft/DRAFT-draft.md) | `draft` + `#DraftLike` | 已落地（文件头仍写待固化） | §12 |
| [DRAFT-枚举.md](draft/DRAFT-枚举.md) | enum + match | 已落地 v0.7（文件头仍写讨论中）。泛型 enum → [#3](prop/3-generic-enum.md) | §3 / §5 |
| [DRAFT-错误.md](draft/DRAFT-错误.md) | 错误模型 v1 | 已落地（文件头仍写待固化）。v2 → [#1](prop/1-error-v2.md) | §4.12 / §6.7 |
| [DRAFT-fallible-sig.md](draft/DRAFT-fallible-sig.md) | `T ! E` | 已落地（文件头仍写 F1 草稿） | §4.12 / §6.7 |
| [DRAFT-lambda.md](draft/DRAFT-lambda.md) | lambda / 函数类型 | 已落地 | §3.11 / §4.11 |
| [DRAFT-closure-capture.md](draft/DRAFT-closure-capture.md) | 闭包捕获 | 已落地 v0.16 | §4.11.6 |
| [DRAFT-let-unify.md](draft/DRAFT-let-unify.md) | `let` 统一 | 已落地 | §5.1 |
| [DRAFT-const-mut.md](draft/DRAFT-const-mut.md) | 可变性 / 常量 | 已落地 | §5.1 / §11.6–11.8 |
| [DRAFT-const-eval.md](draft/DRAFT-const-eval.md) | 编译期求值 | 已落地 | §5.1.4 / §11.10 |
| [DRAFT-static-fn.md](draft/DRAFT-static-fn.md) | `#Static` / `Type::fn` | 已落地 | §7.10 |
| [DRAFT-static-vars.md](draft/DRAFT-static-vars.md) | 静态变量 | 已落地 | §5.1.4 / §7.11 |
| [DRAFT-static-ref.md](draft/DRAFT-static-ref.md) | `T&` 返回 / 静态引用 | 已落地 | §8.6.10 |
| [DRAFT-heap-types.md](draft/DRAFT-heap-types.md) | Heap / Rc | 已落地 | §3.3 / §8.3a / §9.5a |
| [DRAFT-dyn-draft.md](draft/DRAFT-dyn-draft.md) | `Dyn<D>` | 已落地 | §12.9 |
| [DRAFT-spec-unify.md](draft/DRAFT-spec-unify.md) | spec 形态合一 | v1 已落地 | §12 |
| [DRAFT-spec-default-body.md](draft/DRAFT-spec-default-body.md) | spec 默认方法体 | 已落地（正文头已注；状态行过时） | §12.10 |
| [DRAFT-spec-disambig-at.md](draft/DRAFT-spec-disambig-at.md) | `@Spec` 消歧 | 已落地 | §12.10.8 |
| [DRAFT-spec-reflect.md](draft/DRAFT-spec-reflect.md) | 编译期反射 | v1 已落地。补全 → [#4](prop/4-reflect.md) | §13 |
