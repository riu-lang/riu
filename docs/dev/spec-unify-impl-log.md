# spec-unify v1 实施日志

> 草案 [`DRAFT-spec-unify.md`](../spec/draft/DRAFT-spec-unify.md) v1 收紧版（2026-05-18）。本日志记录 Phase a → b.1 → c → d → b.2 + BUGS#4 修复的落地过程；面向后续独立草案（`spec-default-body` / `spec-reflect` / `spec-fields` 等）的参考资料。
>
> 范围：**仅替代 `draft` 关键字**——`#Spec struct` 描述接口、`#Impl(Spec)` 顶行注解声明实现、方法体并入 struct 声明体、删 `#DraftLike`、删 `Any` spec。默认方法体 / 编译期反射 / 异构按字段递归自动 derive 不在范围。

## 阶段总览

- **Phase a** g4 改造（commit `726931b`）。删 `Draft` token / `draftDecl` / `draftType` / `structImpl` 产生式；`structDecl` 合并方法块 = `(filedDecl|LineEnd)* fnClean? (fn|LineEnd)*`；`buildAnno` arg 升级到 `arg=ID genericDef?` 以承载 `#Impl(Spec<T>)` turbofish。
- **Phase b.1** ast_builder 接管（commit `726931b` 同期）。`visitStructDecl` 重写为 3 路分发：`#Spec` → `DraftDeclNode`（仅签名）/ 普通 struct → `StructDeclNode`（+ `StructImplNode` 若有方法/析构/`#Impl`）/ 字段后的方法段融合。下游 AST 节点类型沿用原名（v1 内部不改）。E1137 / E1138 / E1139 三个新错码登记。
- **Phase c** SDK / tests 全仓迁移（commit `c0553ca`）。`tmp_migrate.py` 脚本一次跑完 64 个 `*.yux` 文件：`draft Foo { ... }` → `#Spec struct Foo { ... }`；`T { ... }` 方法块 + `T : Spec { ... }` 实现块合并进 struct 主体 + 顶行 `#Impl(Spec)`；删 `Any` spec 与 `#DraftLike` 注解；内置类型（i8..u64 / f32 / f64 / bool / String）每个合并为单 `#Impl(ToString) #Builtin struct` 块。期间发现并修两个隐 bug，见踩坑清单。
- **Phase d** 文档迁移（commit `e840571`）。`12-draft.md` → `12-spec.md` 整章重写；§07 结构体 + §11 注解 + 附录 A/B + CHANGELOG 全部同步；用户向 `docs/{内置类型,函数,枚举与匹配}.md` 同步措辞。
- **Phase b.1** SDK / tests 文件名重命名（commit `23a4341`）。纯 `git mv`：`sdk/yux/src/yux/core/draft_*.test.yux` → `spec_*`；`tests/cases/diag_draft_e110{1,4,6}_*.{yux,expected_err}` → `diag_spec_*`；`tests/projects/draft_{rc_forward,explicit_impl}/` → `spec_*`（含 `yux.toml` 的 `name=` 字段同步）。
- **BUGS#4 修复**（与 b.2 同 commit `e6c7bdb`）。Phase b.1 文件重命名暴露 `SpecImplChecker::buildTypeOwnerMap` 的 latent bug——见踩坑清单。
- **Phase b.2** C++ 源码重命名 + 错误消息文本迁移（commit `e6c7bdb`）。5 个 src 文件 `git mv` 到 `spec_*`；~40 个 C++ 标识符全量重命名（`DraftDeclNode` → `SpecDeclNode` 等，sed 批处理）；`error_code.h` 中 E1101 / E1104 / E1106 / E1120 / E1131 / E1134 / E2015 "draft" → "spec"；E1110-1112 保留 "draft / DraftLike" 字面量（绑死被弃但仍存在的 `#DraftLike` 注解）。
- **`build`** `.gitattributes` 修复（commit `788afe1`）。原 `* auto eol=lf` 语法错误（`auto` 是 `text` 属性的值，不是独立属性），改为 `* text=auto eol=lf`。仓库 index 内文件早已是 LF，无文件内容差异。

## 关键决策记录

### v1 范围收紧：只替代 `draft` 关键字（[#1.AD]）

草案早版本（2026-05-15 初稿）打算把 `#Spec` / `#Impl` / 反射 / 默认方法体 / `#Reflect` fall-through 一锅煮。审视后发现：默认方法体涉及 spec body 内的纯 yux 实现 + 编译期单态化二分支；反射需要 rodata emit + `Field.value` sema 改名；fall-through 需要静态调用解析逻辑改动。这三块各自都是独立工程，**不应该和"替代关键字"绑在一起**。v1 范围收紧仅做形态替换，三块独立 DRAFT 草案占位（`DRAFT-spec-default-body.md` / `DRAFT-spec-reflect.md`），实施顺序：spec-unify v1 → spec-default-body → spec-reflect。

### 删 `Any` spec（无 `#DraftLike` 后无意义）

`Any` spec 在 v0.5 设计里依赖 `#DraftLike` 自动派生（"所有 owned 类型自动满足 `Any`"）。v1 删 `#DraftLike` 后，`Any` 必须靠每个类型显式 `#Impl(Any)` 才能满足——这对内置类型 + 用户类型 + 泛型类型实例都得加，**完全不现实**。决议直接删除 `Any` spec；`<T : Any>` 边界形态也删；将来若需要 "universal bound" 由 `DRAFT-spec-default-body.md` 或 reflect 草案承接（具体方案待定，可能用 rodata 类型擦除 + downcast，见 `DRAFT-spec-reflect.md` §8a 占位）。

### 删 E1102 "不多余" 检查

旧 `T : D { ... }` 外置实现块语义里，块内方法**必须**全部是 D 签名集的实现，否则 E1102。v1 声明合一后，`#Impl(D) struct T { ... methods ... }` 的 methods 里既可能是 D 契约方法、也可能是 T 自身普通方法（同样合法），强分流"必须 D 契约"会拒掉所有内置类型（如 `i8` 有几十个方法、ToString 只契约一个 `to_string`）。决议 `SpecImplChecker::validateImpl` 删 E1102 检查；严格性回归推迟到 extension-blocks 草案落地后（届时显式 extension block 内的方法可以再次强分流契约）。对应 5 个过时 diag 用例（e1102 / e1103 / e1105 / e1110 / e1112）一并删除。

### 类名 / 文件名内部沿用 "draft"（Phase b.1/b.2 之前）

Phase a / b.1 / c 阶段先 ship "形态正确" 而不动 C++ 端的 `DraftDeclNode` / `DraftImplChecker` / `DraftRegistry` 类名 + `draft_*.cpp` 文件名。原因：Phase c 的 SDK 全仓迁移是热路径，类名改动 blast radius 大且与"形态正确"目标正交。直到 Phase b.2（c 之后）才统一 sed 批处理改名 + 错误消息文本同步。`isDraftLike` 方法名 + `"DraftLike"` 字面量保留（绑死被弃但仍在 g4 中的 `#DraftLike` 注解形态）。

### `#Impl(Spec1)` 多次声明 vs `#Impl(Spec1+Spec2)`

ast_builder 累积所有 `#Impl(D)` 顶行注解到 `StructImplNode::specRefs`。每个注解只接受单个 D；若要实现多个 spec 需多行 `#Impl(D1)` `#Impl(D2)` 堆叠。**不**支持 `#Impl(D1+D2)` 复合参数形态（buildAnno arg 是 `ID genericDef?`，不开放 `+` 运算符）。一行一 spec 简化解析 + 错误定位。

### `.gitattributes` 语法

原值 `* auto eol=lf` 不是合法属性语法（`auto` 是 `text` 的值，不是独立属性）。git 解析时把 `auto` 当成未知属性（无效化），但 `eol=lf` 仍生效——所以 commit 到 index 时确实做了 LF 归一化（这也是 `git add --renormalize .` 无额外变更的原因）。但 `text` 属性未设，autocrlf 模式下 Windows 工作树仍按 native CRLF 检出，导致每次 `git add` 都警告。修正为 `* text=auto eol=lf` 让 `text` 属性也显式设为 `auto`，autocrlf 完全绕过、工作树 LF + index LF 一致。

## 踩坑清单

### ANTLR `(fn|LineEnd)*` 无法识别 bodyless fn 后 BlockEnd

Phase a 落 g4 后跑 `yux-ast tmp.yux`：

```yux
struct Foo {
  fn bar() i32
}
```

报 `extraneous input 'fn' expecting {LineEnd, '#', '}', ID}`。结构体 body 用 `(filedDecl|LineEnd)* fnClean? (fn|LineEnd)*` 时，ANTLR adaptivePredict 在 `(filedDecl|LineEnd)*` 循环出口处不能识别 `Fn` token，把 struct body 截断为空——猜测是 buildAnno 共享 `#` 起始 + `fn` 自身允许 buildAnno 前缀的两路歧义导致 LL 预测失败。**修复**：仿照旧 `draftDecl` 的 `(fnHeader LineEnd)*` 风格，把 `(fn|LineEnd)*` 改为 `(fn LineEnd | LineEnd)*`——强制每个 fn 尾随 LineEnd，消除歧义。带 body 的 fn 也自然满足（fnBody 后必然 `\n`）。

### `visitProgram` 显式 structDecl 循环与 `visitChildren` 双重 visit

Phase b.1 把 `addStructDecl` / `addStructImpl` / `addDraftDecl` 调用搬进 `visitStructDecl` 内部（之前是 visitProgram 外部循环里调用）。结果每个 struct 被 visit 两次——一次显式 `for (auto sd : ctx->structDecl()) visit(sd)`，一次 ANTLR 自动的 `visitChildren(ctx)`。dup 注册触发 E1103 "Duplicate impl block"。**修复**：删 visitProgram 显式 structDecl 循环（fn / letGlobal 预扫保留，因要预登记符号；struct 全 visit 走 visitChildren 即可）。源码顺序遍历保留，符号注册 timing 不变。

### `SpecImplChecker::buildTypeOwnerMap` 跨文件同名 struct 误归属（BUGS#4）

Phase b.1 把 `sdk/yux/src/yux/core/draft_explicit_impl.test.yux` 改名为 `spec_explicit_impl.test.yux`，alphabetical order 从 `draft_*` < `lambda` 翻成 `lambda` < `spec_*`，`yux test` 全量跑突然在 `alias.test.yux:6:1` 报 E1120：

```
Cannot implement draft 'yux.core.ToString' for type 'yux.core.lambda.test.Counter':
both belong to external packages (orphan rule, spec §12.5)
```

报错位置（`alias.test:6:1`）与触发点（`spec_explicit_impl.test:6` 的 `#Impl(ToString)`）跨文件，限定名 `yux.core.lambda.test.Counter` 来自 `lambda.test`（该文件 Counter 并未 `#Impl(ToString)`）。**根因**：`buildTypeOwnerMap` 用 bareName 单层 map `_typeOwnerModule`，注释"同名跨文件冲突由其它阶段诊断, 这里取首次登记的 owner"——实际上 test 文件之间各自独立 module，没有别处诊断 struct 名冲突，map 把所有 `Counter` 折叠到首登记 file 的 moduleName；rename 改了字典序使 lambda.test 抢先登记，orphan 检查随后用错的 owner 触发 E1120。**修复**：`validateImpl` orphan 分支前加 `typeLocalToImplFile` 检测（遍历 `implFile->getStructDecls()` 找同名）——spec-unify v1 下 `#Impl(D) struct X` 必同 FileNode，本地命中即合法，绕过全局 map。`_seen` key / `boundSatisfied` 通路保留全局 map 语义不动（跨文件 bound 满足查询不受影响）。同类潜在冲突 `Holder` / `Point` 因都不 `#Impl` 仍沉默。

### `tmp_migrate.py` 自动合并产物前导空行

Phase c 脚本合并 `struct X { fields }` + `X { methods }` 时，python 实现把 body 部分简单 `\n`.join，导致结果形如 `struct X {\n\n  field ... }`（多一个空行）。早期 ast_builder 容忍，但导致 diag 用例的报错行号比预期偏 1 行。先批处理 `python -c` 删 `struct X {\n\s*\n` 模式，再统一刷 `diag_*.expected_err` 行号。教训：迁移脚本 join body 前 lstrip 第一段；或合并后单独 normalize 空行。

### `expected_err` 内容 + 文件名一起变 → git 失去 rename 检测

`tests/cases/diag_draft_e110{1,4,6}.expected_err` 同时改了文件名（`draft` → `spec`）和内容（首行模块限定名 + 后续 "draft method" → "spec method"）。git rename 检测阈值看 similarity，内容差太多就降级为 `D` + `A` 不显示 rename。无功能影响，但 git log 视图欠美观。后续可考虑 `git log --follow` 或迁移前先 commit 只改名再 commit 改内容（本轮没拆，b.1 commit 同时承担两件）。

## 后续相关 phase

- `DRAFT-spec-default-body.md` — spec 默认方法体 + fall-through ([#1.S] / [#1.T] / [#1.AA])；启动顺序：本 v1 → default-body → reflect。
- `DRAFT-spec-reflect.md` — 内置 spec `Reflect` + `#Static #Frozen` 字段子集 + `Field.value` sema 改名 + `#Reflect` 防 DCE。§8a 新增"类型擦除 + Any downcast"扩展占位（`erase<T>(T&) AnyRef` / `try_cast<T>(AnyRef) T&?`，rodata 单例 `same_ref` 比对；v1.x 可选纳入）。
- extension-blocks 草案（待立项）— 严格 spec impl 隔离回归（E1102 复活）+ 跨包扩展。
- 内置 5 件套（ToString / ToJson / Eq / Ord / Clone）的可推默认体（`Ord.lt/le/gt/ge` 由 `cmp` 推等）→ 等 default-body 落地；按字段递归自动 derive **永不引入**（[#1.AE]）。

## ABI / 关键代码点

- spec 声明：`SpecDeclNode`（含成员签名列表 + `#DraftLike` flag 沿用）；位置 `src/ast/node/spec_node.h`
- 实现块：`StructImplNode._specRefs`（每个 `#Impl(D)` 顶行注解解析后追加；同一 struct 多 `#Impl` 合到一个 StructImplNode）
- 注册中心：`Yux::specRegistry()` + `Yux::specImplChecker()`，长生命期 + 一次性 validate flag
- 分发：方法符号统一注册为 `<module>.<Type>.<member>`，无独立 vtable / 签名表；`#Builtin` 内嵌方法体走原有 baked dispatch
- 关键校验：`SpecImplChecker::validateImpl`（穷尽性 E1101 / orphan E1120 / 重复 E1103）— BUGS#4 修复后 orphan 优先看 implFile 本地声明
- 文件 / 类改名：`src/analyzer/spec_{registry,impl_checker}.{h,cpp}`、`src/ast/node/spec_node.h`；旧 `draft_*` 文件名全部 `git mv`
- 错误码：E1101 / E1104 / E1106 / E1120 / E1131 / E1134 / E2015 文本 "draft" → "spec"；E1110-1112 保留（绑死 `#DraftLike`）；E1137 / E1138 / E1139 新增（v1 占位 / 静态调用 / spec body 方法带 body）；E1102 删除

## 字面规模

- commits: `726931b` → `c0553ca` → `e840571` → `23a4341` → `788afe1` → `e6c7bdb`（共 6 个 commit；前 1 个含 Phase a + b.1，后 5 个为 Phase c / d / SDK rename / .gitattributes / b.2+BUGS#4）
- 测试覆盖：xmake test 179/179 + SDK yux test 532/532 全绿（剩 1 个 SEH/JIT flake 见 BUGS.md #1，与本草案无关）
- C++ 标识符改名约 40 项（sed 批处理）
- yux 端用户用例迁移 64 个 `.yux` 文件（脚本一次跑完）
