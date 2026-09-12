# 单字段结构体字面量位置填充

`Type{ expr }` / `Self{ expr }`：单行、位置填充到唯一实例字段。命名多行 `Type { \n .f = expr \n }` 不变。

- 规范：§7.3.2.4 / §7.10.3.4、附录 B / D、E3129
- g4：`exprStructLit` 在 `{` 后不是换行时吃一条 `expr`
- AST：`ExprStructLitNode::positional()`
- sema / const-eval / codegen：恰好 1 个实例字段则赋给该字段；否则 E3129
- `{` 后 `(` 不走简写，避免 `f { () => e }` 尾随 lambda 被吃成字面量
- 回归：`sdk/yux/src/yux/core/struct_lit_positional.test.yux`，`tests/check-cases/diag_struct_lit_positional_*.yux`
