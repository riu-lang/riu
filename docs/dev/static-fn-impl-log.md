# 静态函数 + 构造模型重构实施日志

`DRAFT-static-fn.md`（实质为 "construction-model" 重构）Phase 0-6E 的落地记录。范围从"加静态函数"扩展为"砍同名 ctor、构造唯一通道收敛到 `#Static fn` + `Self { ... }` 字段字面量"。

- 规范条款：`docs/spec/07-结构体.md` §7.10（静态函数 + `Self`）、§7.3.2（构造唯一通道改写）、§7.1.3（字段初始化由 `Self {}` 接管）、`11-编译期注解.md` §11.5.1 / §11.11（`#Static` 注解）、附录 A.1（`Self` 升至关键字）、附录 D §D.3.3（E3120-E3128 + E3130 + E3131）
- 草案归档：`DRAFT-static-fn.md` 头部"已落地（P1 + Phase 6A-E）"

---

## 核心决策

- **识别走注解 `#Static`**（[#0.A]），不走签名隐式（"有无 `$`"）。一致于 `#Frozen` / `#Const` 一通道；隐式识别让"加/删 `$` 一行"改变 fn 性质太脆。
- **调用语法 `Type::name(...)`**（[#0.B]），复用既有 `SymbolColonColon` token，与 enum 路径 `E::V` 同形——parser 不分叉，sema 按 LHS 类型分流。
- **AST 节点复用 `ExprEnumCtorNode` 改名为 `ExprPathCallNode`**（[#4.A]），承载 `enum::variant` 与 `Type::static_fn` 两条 sema 分流；enum 路径语义不动。
- **`Self` 升关键字**（[#1.A]），与 `null` / `true` / `false` 同档；lexer 新增 token；普通 ID 不得再用 `Self`。
- **`Self { .field = value }` 字段字面量仅在 `#Static fn` 体内合法**（[#8]），且 LHS 仅限 `Self`——`Other { ... }` 不合法，跨类型构造一律走 `Other::new(...)`。α 形态 `Self { LineEnd (fieldInit | LineEnd)* }`，`fieldInit ::= . name = expr LineEnd`，多行强制 + `.` 前缀强制（[#9]）。
- **砍同名 ctor**（[#7]）：`fn StructName(...)` 形态废除，构造唯一通道为 `#Static fn` 返回 `Self`，内部用 `Self { ... }` 产值。不留过渡期。E3130 兜底拦截。
- **静态 fn 与实例 fn 同名允许**（[#2.A]）：sema warning（不阻断），调用语法 `Type::foo` / `obj.foo` 天然分明；与 ctor（即与结构体同名）仍禁——废 ctor 后已自然消失。
- **`Self::name(...)` 实例方法体内调用糖 v1 启用**（[#3.A]），等同当前类型名做 LHS；含泛型时 `Self` 绑当前单态化实参。
- **ABI**：`#Static fn` 顶层 fn 不注入 `$` 形参；mangle 形参表前置 struct mangle 段（含泛型实参），与实例方法同前缀但无 receiver 槽位。
- **DAA 退化为字段字面量必须列全字段**：`#Val` / `#Frozen` 字段约束的"构造期"重定义为"`#Static fn` 体内通过字段字面量产出 `Self` 的那个表达式位"。

---

## Phase 0 — 草案对齐 + Phase 0.5 语法 probe

`yux-ast` 跑 `Self LHS + 双 turbofish + Self { ... }` 形态，验语法歧义（无）。

## Phase 1 — parser / AST

- **1a** `yuxParser.g4` 扩 `exprEnumCtor`：LHS 加 Self、双 turbofish（struct 级 + fn 级）；保留产生式名暂未改（兼容历史 AST）
- **1b** `ExprStructLitNode` + `FieldInitNode` 类落 `expr_node.{h,cpp}`；ast_builder visit 路径；sema/codegen E0000 占位（Phase 2/3 接管）
- **1c** `#Static` 进 `knownAnnos`；`FnHeaderNode` 加 `isStatic()` 便捷方法

## Phase 2 — sema

- `sema_pass.cpp`：`ID::ID` 分流——LHS enum → 原路径；LHS struct 且 callee `#Static` → 静态调用；LHS struct 但 callee 是实例 fn → E3122；LHS struct 找不到 → E3123
- `Self` 在静态 fn 体内绑定到所属结构体类型；体外用 `Self` → E3128
- `#Static fn` 体内禁写 `$` / `$.field` / `&$` → E3124
- `Self { .f = ... }` 仅在 `#Static fn` 体内合法（E3125 outside_static）；字段穷尽（E3126 missing_field）/ 未知字段（E3127 unknown_field）
- 新诊断码 E3120-E3128 同步 `kMigratedCodes`

## Phase 3 — codegen

- `mangler.cpp`：`#Static fn` 走 `Type::name` mangle 形态，无 receiver；与同名实例方法在 IR 层不冲突
- `compiler_call.cpp`：静态调用走顶层 fn 路径，不注入 `$`；与既有 `compileFunctionCall` 共代码
- 泛型 struct 的 `#Static fn` 跟随 struct 单态化（Phase 6E.4 B-E 才完整接通）

## Phase 4 — SDK 试点

- **4a** 句柄字段 retain 路径修复（BUGS #4） + 数组字面量目标类型透传 + `static_fn_handle.test.yux` 三例
- **4b** SDK 试点：`String::empty()` / `String::from(buf)`（与同名 ctor 并存）
- **4c** 砍 String 同名 ctor，构造唯一通道走 `String::from(buf)`（防御深拷贝）+ SDK-私有 `String::_take_buf(buf)`（共享 handle，零拷贝热路径）；修 `compiler_expr.cpp` path-call 静态调用点漏 retain 非 fresh 句柄实参导致 callee 析构释 caller 唯一 +1 的 use-after-free（probe `test_copy_then_mutate` SEH 0xc0000374 已消）
- `static_fn_string.test.yux` 4 例 + `tests/cases/diag_static_*.yux` 全套诊断回归

## Phase 5 — spec + docs 回写

- `07-结构体.md` §7.10 新增；§7.3.2 改写（受限 `Self { ... }`）
- `11-编译期注解.md` §11.5.1 表加 `#Static`；§11.11 新增
- 附录 A `Self` 从 A.2 提至 A.1 关键字
- 附录 D §D.3.3 新增 E3120-E3128
- `docs/结构体.md` 教程 "#Static + Type::name(...)" 小节；`docs/函数.md` 关键字表加 `Self`
- `DRAFT-static-fn.md` 头部标"已落地（P1）"
- 附录 B 整体重审本轮不动

## Phase 6 — 彻底砍 ctor

- **6A** sema 拦截 `fn TypeName(args)` 定义形态 → E3130（作驱动让 SDK / tests 集中爆错）
- **6B** SDK 全量迁：19 个 `sdk/yux/src/yux/core/*.yux` 的 ctor + 调用点全部走 `#Static fn make / from / new` + `Self { ... }`
- **6C** tests / examples / docs 迁：50 个文件全量替换
- **6D** C++ 砍剩余 ctor 代码：
  - 主提：`compileConstructorCall` / `Mangler::ctor` / `validateCtorCallShape` 等
  - 6D-tail：泛型 impl 同名方法 E3130 兜底、sema 多余跳过清除、LSP `structNames` ctor 着色清除、`const_mut_checker::isConstructor` 死分支清除
- **6E.1+6E.2** `#Static fn` 调用点接通灵活整数字面量目标类型化 + arity/类型校验 (E3131)
- **6E.3** 修 `Dyn<D>` 对象安全漏判 (E1134): `draftIsObjectSafe` 改走 TypeNode AST + createFile 清回 validate flag
- **6E.4A** turbofish 形态接入 AST: `ExprPathCallNode` 加 lhs/rhs typeArgs + ast_builder 接收
- **6E.4 B-E** codegen 接通泛型 struct + `#Static fn` + `Self {}`:
  - **B** path-call struct 分支若 `lhsTypeArgs` 非空 → `ensureStructInstance` + 压 `SubstFrame`；paramTypes / retType 走 `applySubst`；`getMethodFunction(effLhs, ...)` 走 mangled instance
  - **C** `compileLiteralExpr` ExprStructLitNode 分支查 `_structInstances` 拿 baseDecl；字段类型 `applySubst` 让 `isArrayGeneric` / `typeNeedsDestructor` 识别具体类型
  - **D** Self 返回类型由 applySubst 自动处理（TypeSelfNode 返回 bare `GH`, applySubst 经 SubstFrame 替换）；不写 `setResolvedType`（会触发 debug assert）
  - **E** 恢复 `sdk/yux/src/yux/core/struct.test.yux::test_struct_byval_generic_instance`
  - SubstFrame 用 try/catch 兜底，异常路径必 pop

---

## 改动面清单

### parser / AST

- `src/yuxParser.g4`：`exprEnumCtor` 扩 Self LHS + 双 turbofish；`structImpl` 体内允许 `Self { ... }` 字面量
- `src/yuxLexer.g4`：`SelfTok: 'Self'` 新增
- AST 新增：`ExprStructLitNode` / `FieldInitNode` / `TypeSelfNode` / `ExprPathCallNode`（前身 `ExprEnumCtorNode` 改名 + lhs/rhsTypeArgs 字段）
- `ast_builder.cpp`：`visitExprStructLit` / `visitTypeSelf` / `visitExprPathCall`（含双 turbofish 解析）

### sema / 借用 / mangler

- `src/sema/sema_pass.cpp`：path-call 分流（enum vs static）；Self 体内绑定；`#Static fn` 体内 `$` 禁用；`Self { ... }` 形态校验；E3120-E3128 + E3130 + E3131 抛点
- `src/sema/call_resolve.cpp`：static call overload resolution（无 receiver 版本 + 灵活整数字面量目标类型化）
- `src/analyzer/const_mut_checker.cpp`：`isConstructor` 死分支清除（ctor 通道砍后无意义）
- `src/ast/mangler.cpp`：`#Static fn` mangle（前置 struct 段 + 无 receiver 槽位）

### codegen

- `src/compiler/compiler_call.cpp`：path-call 调度（含泛型 struct + `lhsTypeArgs` 接通 ensureStructInstance + SubstFrame）；`compileConstructorCall` 移除
- `src/compiler/compiler_expr.cpp`：`compileLiteralExpr` 接 ExprStructLitNode（含字段 applySubst）；path-call retain 修
- `src/compiler/compiler_types.cpp`：`TypeSelfNode` 解析为当前 impl 块所属类型
- `src/compiler/compiler.h`：泛型 SubstFrame 栈

### SDK / 测试 / 工具链

- 19 个 `sdk/yux/src/yux/core/*.yux` 全量迁 ctor → `#Static fn make/from/new` + `Self { ... }`
- 50 个 tests / examples / docs 文件迁
- `static_fn_*.test.yux` / `diag_static_*.yux` 覆盖；`tests/cases/diag_struct_generic_arity.yux` 同步
- LSP `lsp_server.cpp` `structNames` ctor 着色清除

### Spec

- `07-结构体.md` §7.10 新增、§7.3.2 改写
- `11-编译期注解.md` §11.5.1 / §11.11 加 `#Static`
- 附录 A.1 加 `Self`
- 附录 D §D.3.3 加 E3120-E3128 + E3130 + E3131
- 用户教程 `docs/结构体.md` / `docs/函数.md` 同步

---

## 跨 Phase TODO / 未落

- **enum 上的 `#Static fn`**：enum v1 不带方法块，留后续草案
- **draft 实现块内 `#Static fn`**：扩展 §7.8.2.1 已禁；draft 契约仅含实例方法签名（[#1.C]）
- **关联常量** `Type::CONST`：需要在 `structImpl` 内支持 `cval` 声明，独立草案
- **任意 struct 名字面量** `Other { .x = ... }`：v1 仅放开 `Self { ... }`（[#8]）；跨类型构造走 `Other::new(...)`
- **多 structImpl 块跨文件汇总**：沿用 §7.2.1.1 既有约定不改
- **附录 B 语法汇总同步**：`structImpl` / `expr` 产生式同步留待整体重审

---

## 回归

最终态：
- `xmake test` 184/184 全绿
- `cd sdk/yux && yux test` 534/534 全绿
- 全仓 ctor 形态零残留（grep `fn $structName($` 仅命中已加 E3130 拦截测试）
