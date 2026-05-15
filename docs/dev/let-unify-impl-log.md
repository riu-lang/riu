# let-unify 实施日志

> 草案 [`DRAFT-let-unify.md`](../spec/draft/DRAFT-let-unify.md)。本日志记录 P1 / P2 / P3 实施过程的关键决策、踩坑、回归点；面向未来 const-mut 草案合并、字段段 let-unify 草案（若有）的参考资料。
>
> 范围：**只限局部 + 全局变量声明**形态统一。字段段保留 `var/val/cval f T` + `#Val`/`#Frozen` 字段注解不动。

## 阶段总览

- **P1.a** g4 加 `Let` token + `statementLet` / `letGlobal` 产生式；旧 `var/val/cval` 路径暂留并存。
- **P1.b** SDK + tests 全仓脚本迁移（`scripts/migrate_let.py`）；formatter `printer.cpp` 补 `statementLet` / `letGlobal` 分支；诊断回归同步。`#Mut let x T` 允许无 init（option A，保留旧 `var x T` 延后赋值形态）。
- **P1.c** 元组解构 `statementLetTuple`；删 `globalConst` / `statementDeclare` / `statementDeclareAssign` / `statementCvalDeclAssign` / `statementDeclareAssignTuple` 五条旧产生式；删 `Cval` / `DeclKey` 词法 token。
- **P2** AST 内 `enum class DeclareType` 一并删除（用户决议），`StatementDeclare(Assign|AssignTuple)Node` 改用 `bool isMut + bool isConst`；checker 注释补 let-unify §3；新增 `diag_let_reassign.yux`。
- **P3** spec §5.1 / §11.9 §11.10 / 附录 A 同步；用户教程 `docs/基础语法.md` 重写"变量"章；CHANGELOG 顶部条目；本日志。

## 关键决策记录

### 字段段不在范围（v3 决议反转）

草案早版本（v3 之前）曾打算把字段段也并入 `let`，因 SDK 字段 audit 工作量过大、字段块自身的 `var/val/cval/注解 + 字段` 形态已经清晰，最终决议**字段保留现状**。落地后字段层零改动；`#Val` / `#Frozen` 字段注解全部保留语义。

### `#Mut let x T` 允许无 init（option A）

ast_builder 一开始把"有 type 无 init"统一拒成 E3114。审视发现 SDK + tests 中存在 `var x T` 后延后赋值的合法形态（典型：`declare_no_init.test.yux`），迁移到 `#Mut let x T` 会被 E3114 拒。option A：在 ast_builder 层放开此例外（`hasType && !hasInit && !flags.isMut` 才报 E3114），并落到 `StatementDeclareNode` 节点（与旧 `var x T` 走的节点一致）。默认 / `#Cval` / `#Frozen` 仍强制 init（因不可变无初值等于死代码）。spec §5.1.1.3 / §11.9.1.2 同步。

### 注解形态：inline 与顶行皆接受（formatter 规范化）

`letAnno: SymbolHash name=ID LineEnd?` 在 g4 层兼容 inline / 顶行两种写法，formatter / printer 负责规范化（局部 / 参数 inline；fn / struct / 字段 / 全局 `letGlobal` 顶行）。spec 层只描述"权威形态"，不引入"形态错位"诊断（DRAFT §4.3 提到的"局部 let 走顶行报错"未落地：grammar 层已统一接受，由 formatter 负责美化）。

### DeclareType enum 删除

P2 阶段（用户决议）把 `enum class DeclareType { Var, Val, CVal }` 直接删，`Statement(Declare|DeclareAssign|DeclareAssignTuple)Node` 改用 `bool _isMut + bool _isConst`。原因：let-unify 落地后 AST 节点上的"枚举档位"和"符号 writeable / isConst"两路并存信息冗余；用 bool 更直接，删 enum 减少抽象层。`const_mut_checker.cpp` 唯一一处 `da->declareType() == DeclareType::CVal` 改为 `da->isConst()`。

### checker 不需要新错号

DRAFT §3.4 / §6.3 列出的 E3113 / E3114 实际由 ast_builder 在 parse 阶段抛（语法形态错），不进入 checker；checker 仅承接 §3.3 `#Cval` 局部初值约束（E3104）+ const-mut 既有路径（E3093 / E3106 / E3107 / E3109 / E3110 / E3111）。**E3093 路径自动覆盖局部 let 重赋**（ast_builder 把默认 `let` → SymbolInfo writeable=false → compiler_stmt.cpp 抛 E3093）。CURRENT.md 中 P2 原列"E3093 路径扩展到局部"实际不需写新代码。

## 踩坑清单

### 全局 `let` 必须 `#Cval`（E3116）

`letGlobal` 缺 `#Cval` 时 ast_builder 抛 E3116。早期实现把所有 let 都视为"可声明全局"，发现存在歧义：全局 `let x = 1` 是不可变还是常量？为避免引入"全局可变 / 全局浅不可变 / 全局深不可变"三档（v1 不需要），全局强制 `#Cval`。

### Python 迁移脚本 CRLF 问题

`scripts/migrate_let.py` 在 Windows 上默认按 CRLF 写文件，git 检测到行尾改动。修复：`read_text(encoding='utf-8', newline='')` + `write_text(newline='')` 显式跑 LF；执行后 `git add -u` 触发 stat 刷新。

### 跨 sdk / tests / projects 三处迁移

P1.b 初次只迁移 `sdk/` + `tests/cases/`，遗漏 `tests/projects/` 7 个项目用例，导致 P1.c-2 删旧 token 后回归触发 7 个项目级测试失败。教训：全仓迁移必须先 `git grep -l '^\s*\(var\|val\|cval\)\s' -- '*.yux'` 列全清单。

### LSP 残留

P1.c-2 删旧产生式后，`src/lsp/document.cpp` `prog->globalConst()` 与 `src/lsp/semantic_tokens.cpp` 关键字 `Cval` / `DeclKey` 编译失败。LSP 不在 `xmake test` 路径上但同属 build target，CI 会拦。P2 一并修：document 改读 `letGlobal()`；semantic_tokens 关键字集去 `Cval/DeclKey` 加 `Let`。

### `diag_kw_as_param_name` 触发关键字切换

`syntax_error_listener.cpp` 的 `isYuxKeyword` 关键字集仍含 `cval/var/val`，导致 `diag_kw_as_param_name` 改用 `let` 后 hint 文案不出。一并更新集合：去 `cval/var/val`，加 `let`。

## 文件清单

### 编译器（`src/`）

- `src/yuxLexer.g4` —— 加 `Let : 'let'`；删 `Cval` / `DeclKey` token。
- `src/yuxParser.g4` —— 加 `letGlobal` / `statementLet` / `statementLetTuple` / `letAnno`；删 `globalConst` / `statementDeclare` / `statementDeclareAssign` / `statementCvalDeclAssign` / `statementDeclareAssignTuple`。
- `src/ast/ast_builder.cpp` —— 加 `visitLetGlobal` / `visitStatementLet` / `visitStatementLetTuple`；删 5 个旧 visit*；`visitProgram` 全局符号预登记改读 `letGlobal()`。
- `src/ast/ast_builder.h` —— visit 声明同步增删。
- `src/ast/node/statement_node.{h,cpp}` —— 删 `enum class DeclareType`；`StatementDeclare(Assign|AssignTuple)Node` 改 `bool _isMut + bool _isConst` + `isMut()` / `isConst()` 访问器。
- `src/analyzer/const_mut_checker.{h,cpp}` —— 注释补 let-unify §3；§3.3 cval 初值约束改读 `da->isConst()`。
- `src/tools/format/printer.cpp` —— 加 `LetGlobalContext` / `StatementLetContext` / `StatementLetTupleContext` 格式化分支；删旧 `globalConstDoc`。
- `src/tools/syntax_error_listener.cpp` —— `isYuxKeyword` 集去 `cval/var/val`，加 `let`。
- `src/lsp/document.cpp` —— `prog->globalConst()` → `prog->letGlobal()`。
- `src/lsp/semantic_tokens.cpp` —— 关键字 case 集去 `Cval/DeclKey`，加 `Let`。
- `include/error_code.h` —— 新错号 E3112–E3116（注解未知 / 缺 type+init / 缺 init / 注解互斥 / 全局缺 #Cval）。

### SDK / 测试

- `scripts/migrate_let.py` —— 一次性迁移脚本，含元组形态 + LF newline 模式。
- `sdk/yux/**/*.yux` —— 全量迁移。
- `tests/cases/let_basic.yux` / `let_mut_no_init.yux` —— 正例。
- `tests/cases/diag_let_no_init.yux` / `diag_let_no_type_no_init.yux` / `diag_let_anno_conflict.yux` / `diag_let_unknown_anno.yux` / `diag_let_global_no_cval.yux` / `diag_let_reassign.yux` —— 诊断回归。
- `tests/projects/**/*.yux` —— 全量迁移。
- `tests/cases/diag_kw_as_param_name.{yux,expected_err}` —— 关键字误作标识符触发码切到 `let`。
- 若干诊断列号修正（迁移导致源片段移位）：`diag_const_mut_cval_init_call` / `_ref_val` / `diag_multibyte_caret` / `lambda_type_weak_fn`。

### 文档

- `docs/spec/05-语句与控制流.md` §5.1 重写（含 `let-unify 落地说明` 块）。
- `docs/spec/11-编译期注解.md` 新增 §11.9 `#Mut` / §11.10 `#Cval`；§11.5.1 注解表同步。
- `docs/spec/附录A-保留字.md` 关键字表删 `cval/val/var`、加 `let`；A.6 预留段加旧关键字归档说明；A.3 注解表补 `#Const` / `#Frozen` / `#Val` / `#Mut` / `#Cval`。
- `docs/spec/CHANGELOG.md` 顶部条目。
- `docs/基础语法.md` "变量声明 / 局部 cval / 全局常量"三节重写；通篇 `val` / `var` 示例改 `let` / `#Mut let`。
- `docs/spec/draft/DRAFT-let-unify.md` —— 头部追加"已落地"标记（与 const-mut 草案同步留作历史参考）。

## 测试结果（最终阶段）

- `xmake test`：167/167（含 `diag_let_reassign`）。
- `yux test` (`sdk/yux/`)：492/492。

## 归档建议

后续若启动"字段段 let-unify"独立草案（DRAFT §3.3 / §6 字段层留口），需要参考的关键决策：

1. 字段块本身已经多行，注解走顶行更整齐（const-mut P1 落地形态保留）。
2. 默认字段不可变（与局部一致）会让 95%+ SDK 字段需 audit 加 `#Mut`，工作量大；若启动需先评估字段位 `#Mut` 注解形态（inline vs 顶行）与字段类型推断兼容性。
3. AST 节点上的 `bool _isMut + bool _isConst` 模式可直接复用到 `StructFieldNode`（当前 `StructFieldNode` 已经用类似 bool 字段表达 `#Val` / `#Frozen` / `var/val/cval` 三档，重命名 + 收敛即可）。
