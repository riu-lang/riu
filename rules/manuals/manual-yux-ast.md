# yux-ast 手册（最低频）

ANTLR4 parse tree 转储工具。0 LLVM 依赖，**只做词法 + 语法分析**，不做 AST 构建、语义分析或 codegen。

## 用法

```
yux-ast [OPTIONS] input
```

| 参数 | 说明 |
|------|------|
| `input`（必填） | 输入 .yux 文件 |
| `-o, --output TEXT` | 输出文件（默认 stdout） |
| `--oneline` | 单行输出（默认缩进多行 pretty print） |

## 适用场景

**语法有异议时**——当不确定 parser 怎么理解一段 yux 代码、或怀疑 parser 行为与预期不符时。parse tree 直接反映 ANTLR4 根据 `yux/ast/yuxParser.g4` + `yux/ast/yuxLexer.g4` 产出的结构，是最权威的语法裁决。

## 与 yux-check / yux build 的关系

- `yux-ast`：词法 + 语法 → parse tree（不检查语义，不建 AST）
- `yux-check`：parse → AST → SemaPass（语义检查，0 LLVM）
- `yux build`：全流程 parse → AST → sema → codegen → link

语法层面有分歧时，`yux-ast` 的输出是最直接的证据——比看 g4 源码快，比翻文档更权威。
