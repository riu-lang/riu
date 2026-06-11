# draft（接口/约束）v0.5 实施日志

> 注：本日志使用旧名 Box / box；当前等同 Rc<T>。

本文件归档 v0.5 `draft` + `#DraftLike` + `<T : D>` 边界实现的核心决策、ABI、关键代码点。是后续回答"为什么 v1 没有 dyn Draft"、"`#DraftLike` 为何只贴在 draft 声明处"、"Box<T> forward 为什么不需要独立机制"等问题的事实来源。

- 规范条款见 `docs/spec/12-draft.md` §12 全章 + §7.8 / §6.4.4 / §11.4 / 附录 A/B/C/D
- 设计草案沉淀见 `docs/spec/draft/`（v0.5 收口的 DRAFT-draft.md 已按模板末尾归宿处理）
- 前序 v0.4 借用模型见 `docs/dev/borrow-impl-log.md`

---

## Phase 1 — DRAFT-draft.md 决议收齐

A 组（声明形态）：draft 体内只允许方法签名（无默认体）；实现块仅以「合并块 + 穷尽且不多余」形态写；`#DraftLike` 标注位置仅 draft 声明处。

B 组（边界语法）：内联 `<T : D1 + D2>`，不引入 `where`；与 §8.6.7 衔接 —— T 仍 owned；T& 自动可调；Box forward 等价归一为 §8.6.7.3 自动解引用；T& 不可作实现位点；调用 ABI 为纯单态化静态分发，mangling 规则给未来 dyn Draft 留位。

C 组（匹配规则）：签名等价按 §12.3.1；draft 自身可泛型；体内 fn 不得再带本地泛型；按需匹配（不强求全签名一致）。显隐优先级 —— 同包共存即 error，跨包结构化匹配自动命中。多包同名 draft 按完全限定名独立判断。`#DraftLike` 默认严格 + 4 条硬性误用诊断（贴非 draft / 默认体 / 体内本地泛型 / 与显式实现块共存）。orphan 规则：Type 包或 D 包二者其一才能写实现，否则 E1120。

D 组（内置）：`ToString` 入 base.yux，**不**贴 `#DraftLike`（强契约，复用 `#Builtin` 让方法体可由编译器内嵌）；`Box<T>` forward 范围无独立机制，等价于现有 §8.6.7.3 归一并澄清 5 条边界；内置 `#DraftLike draft Any { }` 让所有类型自动满足（含微补：允许空 draft body）；新增 builtin `copy_of:<T>(x T&) T` 提供 T&→T 显式拷贝。

E 组（不在范围）：12 条决议 v1 不实施（dyn Draft / where 子句 / 默认方法体 / 关联类型 / 父 draft / blanket impl 等），全部记入 §12 Open Issues。

## Phase 2 — spec 回写

§7.8（结构体实现块）：明文 `Type : D1 + D2 { ... }` 形态。

§6.4.1.2 / 新增 §6.4.4：函数泛型形参可写 `<T : D1 + D2>` 边界，编译期单态化校验。

新增 §12 draft 专章（独立文件 `docs/spec/12-draft.md`，§12.1–§12.8 + Open Issues）：声明 / 实现 / 等价判定 / `#DraftLike` 结构匹配 / orphan / Box forward / 内置 / 错误码全覆盖。

§11.4 新增 `#DraftLike` 条款；§11.2.3 builtin 清单追加 `as_ref` / `copy_of` / `to_string`；原 §11.4/§11.5 顺延为 §11.5/§11.6。

附录 A 关键字表加 `draft` + 注解表加 `#DraftLike`；附录 B 加 `draftDecl` / 扩展 `structImpl` / `genericDef` → `typeParam` + `draftBound` 规则形态；附录 C §C.6a 新增 11 条术语；附录 D 段位表加 E11xx + §D.3.7 占位错误码表。

`docs/spec/CHANGELOG.md` 顶部追加 v0.5 一条（draft + `#DraftLike` + `<T : D>` 边界 + `copy_of`）。

## Phase 3 — 编译器实现

### 3.0 — 语法解锁

- `src/yux.g4`：`typeParam` + `draftBound` 形态（`<T : D1 + D2>`）；`draftDecl` 规则；ANTLR 重新生成
- `#DraftLike` 注解加入白名单
- 错误码段位 E11xx + E2015（turbofish/类型引用位置写 bound）登记进 `src/diag.cpp`

### 3.1 — AST

新增节点：
- `DraftDeclNode`（draft 声明）
- `StructImplNode._draftRefs`（实现块声明的 D1+D2 列表）
- `FnHeaderNode._typeParamBounds`（每个泛型形参对应的 D 列表）
- `FileNode._draftDecls`

`ast_builder.cpp`：`visitDraftDecl` 实装；`visitStructDecl/Impl` 适配 `structType` 子规则；turbofish/类型引用位置写 bound → E2015；`#DraftLike` 仅 draft 声明位置（其它位置 E1110）；draft 体内 fn 引入本地泛型 → E1104（draft 自身贴了 `#DraftLike` 时升 E1112）。

### 3.2 — 语义校验（draft_impl_checker）

`src/draft_impl_checker.{h,cpp}` 集中以下校验：

- `DraftRegistry`：跨文件按完全限定名索引 draft；`resolve(bareName, visibleFrom)` 走 file → wildcardImports → parent(sdk) 链，返回 `(qualifiedName, decl, ownerFile)`；挂在 `Yux` 上懒构建
- `validateImpl`：`StructImpl._draftRefs` 解析为 `(typeQualified, draftQualified) → impl` 表；§12.2.2 穷尽性 / 不多余 / 重复 → E1101 / E1102 / E1103；§12.5 orphan → E1120；签名等价按 §12.3.1
- `checkExplicitImplicitConflict`：同 Type 普通方法块的 `m()` 与 `Type:D{m()}` 等价 → E1105
- `typeSatisfiesDraft(typeBareName, draft, draftTypeArgs)`：`#DraftLike` 结构化匹配 helper；遍历 SDK + 用户文件中该类型所有 impl 块（含普通方法块与 draft impl 块），对每个 draft 签名按 §12.3.1 等价比对；仅返回 bool，让边界匹配点自己转诊断

校验入口挂到 `Compiler::compile()` 起始；`Yux::validateDraftImpls()` 用 `_draftImplValidated` flag 守一次性，避免多文件 compile 重入。

### 3.3 — 边界单态化

`Yux` 持有长生命期 `DraftImplChecker`（`yux.h` / `yux.cpp`）。

`DraftImplChecker::boundSatisfied(typeArg, draft, draftQualified, draftTypeArgs)` 先查 `_seen` 显式 impl 表，未命中再走 `#DraftLike` 结构匹配。

入口：`Compiler::compileGenericFunctionCall` 在 typeArgs 推断完毕、`ensureFnInstance` 之前，对每个 `<T : D...>` 解析 D 限定名并调 `boundSatisfied`，未命中报 E1106。

### 3.4 — 调用分发

显式 `Type : D { ... }` 实现块的方法在 `ast_builder` 阶段就和普通方法块一起 `registerFnSymbol` 注册成 `<module>.<Type>.<member>`；`compileStructImpls` 也无差别 emit。无运行时签名表，无独立分发路径。

`compileMethodCall`：在拿到 baseType 后追加 `applySubst(baseType)` —— 实例化期把泛型形参 `T` 还原成具体类型，复用现有 builtin / struct method 分发路径。

`ExprDotNode::getType`：当 receiver 是泛型形参 T 且不在普通符号表里时，回退查 enclosing fn 的 `typeParamBounds`，按每个 D 在 file → SDK 链查 `DraftDeclNode`，命中签名后返回 `fn() <ret>`。这样 `val s = x.show()` 在 AST 阶段就能拿到具体返回类型，避免把 `s` 当 T 用导致后续 `println(s)` 退化成 ExternalFunctionCall（裸名）。

`ExprCallNode::getType`：把 `lookupSymbol(type.name)` 命中 `TypeParam` 的情况从 E3095 抛出豁免 —— T 在调用 callee 链路里只是占位，不应判定为非函数符号。

### 3.5 — Box<T> 自动 forward

按 §12.6.1 v1 决议：不引入独立 forward 机制。`box.m()` 走 §8.6.7.3 自动解引用 + 方法分发归一，复用 `compileMethodCall` 中既有的 `baseType.isBox()` → `actualType = boxElementType` 路径；draft impl 块的方法已与普通方法块同名注册，无需新代码。

### 3.6 — 诊断错误码

E1101–E1106 / E1110 / E1112 / E1120 全部抛点：

- E1101 穷尽性缺失 / E1102 不多余即多余成员 / E1103 重复实现 — `draft_impl_checker::validateImpl`
- E1104 draft 体内 fn 本地泛型 / E1105 显隐冲突 — `ast_builder` + `draft_impl_checker::checkExplicitImplicitConflict`
- E1106 边界单态化未命中 — `compiler_call::compileGenericFunctionCall`
- E1110 `#DraftLike` 误标在非 draft 位置 / E1112 `#DraftLike` draft 内方法本地泛型 — `ast_builder`
- E1120 orphan — `draft_impl_checker::validateImpl`

E1111（`#DraftLike` 默认方法体）在 grammar 层即被堵：`draftDecl` 仅允许 `fnHeader`（无函数体），构造不出触发样本，留待将来文法扩展默认体后再补。

## Phase 4 — SDK + 测试

`sdk/yux/src/yux/core/base.yux`：内置 `ToString` draft + `#DraftLike Any` + 各内置类型 `Type : ToString { ... }` 显式实现。窄整型（i8/u8/i16/u16/i32/u32）通过 `to_string()` 委派给宽类型；i64/u64/f64/bool/String 走 yux 实现；f32 委派 f64。spec §12.7.1.2 措辞同步订正：方法体 `#Builtin` 与 yux 实现并存为合法形态；CHANGELOG 顶部新条目记录此迁移。

SDK 端到端测试位于 `sdk/yux/src/yux/core/`：

- `draft_explicit_impl.test.yux`：用户类型 `Counter : ToString` 显式实现 + 内置整数 / 布尔 / 字符串 `to_string`
- `draft_bound_match.test.yux`：`fn show_via_bound<T : ToString>(x T) String` 走多种类型 + `<T : Any>` 空 draft 匹配
- `draft_box_forward.test.yux`：`Box<Bag>.to_string()` 走 §8.6.7.3 自动解引用归一

`tests/cases/diag_draft_*.{yux,expected_err}` 覆盖 E1101–E1106 / E1110 / E1112 单文件诊断。

`tests/projects/draft_explicit_impl/` 与 `tests/projects/draft_box_forward/` 作为项目模式回归。

## ABI / 关键代码点

- draft 声明：`DraftDeclNode`（含成员签名列表 + `#DraftLike` flag）
- 实现块：`StructImplNode._draftRefs`（解析后挂 `(typeQualified, draftQualified) → impl` 表）
- 边界：`FnHeaderNode._typeParamBounds`，单态化期 `boundSatisfied(...)` 校验
- 注册中心：`Yux::draftRegistry()` + `Yux::draftImplChecker()`，长生命期 + 一次性 validate flag
- 分发：方法符号统一注册为 `<module>.<Type>.<member>`，无独立 vtable / 签名表；`#Builtin` 内嵌方法体走原有 baked dispatch
- 解引用归一：`compileMethodCall` 中 `baseType.isBox() → actualType = boxElementType`（§8.6.7.3 既有路径，draft 复用）

## TODO（v0.5 未覆盖，记入后续版本）

- 跨包 orphan 用例（E1120）：单文件 `implMod` 为空走豁免，需多包项目用例覆盖；统筹进 v0.6+
- E1111（`#DraftLike` 默认方法体）：等文法扩展支持默认体后补样本
- dyn Draft / 关联类型 / 父 draft / blanket impl 等 E 组 12 条 Open Issues：v1.x 重新评估
- ToString 在 v0.6 字符串模板"可插值约束"中的接入点：spec 已点出（强契约，未标 `#DraftLike`），实现待 v0.6
