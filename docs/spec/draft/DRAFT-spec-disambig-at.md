; 草案：yux spec 默认体消歧调用语法（`@SpecA` 后缀）

# 草案：yux `$.m@SpecA()` —— spec 默认体消歧调用

状态：**已落地（2026-05-23），见 [`docs/spec/12-spec.md` §12.10.8](../12-spec.md#12108-消歧调用-speca-后缀)**。保留为历史档；后续若有反复请到正式条款做修订并在 CHANGELOG 追加一条。日期：2026-05-22。
作用：把 §12.10 已落地的"默认方法体 + fall-through" 留口的**显式消歧调用语法**（DRAFT-spec-default-body Phase 7 / §12.10.5.3）固化为单一规范，作为修改 `docs/spec/12-spec.md` §12.10 续节与 CURRENT.md 实施计划的依据。

> 本草案的所有规则均已逐条决议（见末尾「决议日志」）。后续若有反复，请在日志里追加修订记录，不要直接覆盖正文。

涉及章节（预估）：§12.10（续节）/ §B.2（exprDotCall 产生式）/ 附录 D（错误码复用）。

前置：[`DRAFT-spec-default-body.md`](DRAFT-spec-default-body.md) 已落地（2026-05-22，§12.10）。

---

## 1. 目标

- 为 §12.10.5 多 spec 默认体冲突（E3132）提供**显式消歧通道**：实现者写覆盖时可在体内 delegate 到任一 spec 的默认体。
- 形态最小：复用 dot-call 的方法直觉，不引入新关键字、不另起 `Type::m($, args)` UFCS 形态分裂调用入口。
- `@` 作为统一"标签 / 限定符"通道（与未来 `break@label` / `ret@label` 同源），一次定型，后续 label 形态独立草案。
- g4 改动局部，仅在 `exprDotCall` 的方法名后加可选 `('@' ID genericDef?)` 槽位；AST `CallNode` 加 `optional<string> specQualifier`。
- 不解决跨包 / 全限定形态——单名足够覆盖当前需求；全限定留待后续"路径形态"专项一次性铺。

## 2. 形态全景

| 写法 | 含义 | 备注 |
|---|---|---|
| `$.m()` | 常规方法调用 | dispatch 走 receiver 类型的方法表（spec 默认体已 fall-through） |
| `$.m@SpecA()` | 显式指向 SpecA 的默认方法体 m | 即便 receiver 类型覆盖了 m，仍走 spec 默认体 |
| `obj.m@SpecA()` | 同上，外部调用方场景 | receiver 任意（含 `Rc<S>` / `Heap<S>` 自动解引用） |
| `SpecA::m(obj)` | **不引入** | UFCS 形态分裂调用入口，与本草案目标冲突 |

要点：

- `@SpecA` 是**方法名上的标签**，不是新的调用入口；`m@SpecA` 整体作为一个 callable name 解析。
- spec 默认体在 fall-through 后形成 receiver 类型的方法实现；`@SpecA` 形态显式选择"未 fall-through 的 spec 默认体源符号"或"fall-through 后的方法符号"——两者在多数情况下等价（详见 §3.5）。
- `@` 后只接 spec 单名（带可选 `genericDef`，承载 `@To<i32>`）；**不**接路径前缀（同包 spec 名按 §10 解析）。

## 3. 语法 / 语义

### 3.1 g4 改动

```antlr
exprDotCall ::= expr '.' ID ('@' ID genericDef?)? '(' callArgs? ')'
```

仅在现有 `exprDotCall` 的 ID 与 `(` 之间插入可选 `('@' ID genericDef?)` 槽位。其它调用形态（free fn / Type::factory / 构造）**不**接受 `@` 后缀。

### 3.2 AST

`CallNode` 增加可选字段 `optional<string> specQualifier`（带可选 `genericDef` 承载形参实参时再扩展为 struct）。`ast_builder` 在 `visitExprDotCall` 命中 `@ID` 时填充该字段，缺省为空。

### 3.3 sema 解析规则

`$.m@SpecA(args)` / `obj.m@SpecA(args)` 在 sema 期按以下顺序判定：

1. **SpecA 必须是 receiver 类型 `T` 自身 `#Impl` 列表里的 spec**——若 T 未 `#Impl(SpecA)` → 报 **E1101**（"Type `T` does not implement spec `SpecA`"）。
2. **SpecA 必须含名为 m 的签名**——否则报 **E1140**（spec `SpecA` has no method `m`）。
3. **SpecA 中 m 必须带默认体**——纯抽象签名无法 disambiguate，报 **E1140**（spec `SpecA` has no default body for method `m`; cannot disambiguate via `@SpecA`）。

E1140 的错误消息按上下文区分（"unknown method" / "no default body"），不引入新错误码。

### 3.4 跨包 / 全限定

**v0.X 不引入**。`@` 后只接 spec 单名；同包内按 §10 名字解析。跨包消歧场景留待后续"统一路径形态"专项（同期铺 `@pkg.Spec` / turbofish `pkg.X<...>` / `#Impl(pkg.Spec)` 等位置）。

> 当前限制：若 receiver 类型 `T` 实现了来自两个不同包的同名 spec（如 `a.Show` + `b.Show`），单名 `@Show` 不可解析 —— 报 E1101（视为"未实现"）；用户须 import 时改 alias 或等待全限定形态。该场景极罕见，可接受。

### 3.5 与 fall-through / 覆盖的关系

- 选 A（**采纳**）：`$.m@SpecA()` 永远指向 **SpecA 的默认方法体**（即便 T 覆盖了 m）。
- 即 `@SpecA` 是 **escape hatch**：在 T 自己的 `fn m()` 覆盖体内，可通过 `$.m@SpecA()` delegate 到 spec 默认体（用于 E3132 消歧写覆盖时复用 spec 已有实现）。

```yux
#Impl(A)
#Impl(B)
struct S {
  fn m() {                  ; 显式覆盖以消歧 E3132
    $.m@A()                 ; delegate 到 A 的默认体
    $.m@B()                 ; 再 delegate 到 B 的默认体
  }
}
```

- 即便 T 未覆盖 m 且 fall-through 了 SpecA 的默认体，`$.m@SpecA()` 与 `$.m()` 行为一致（前者显式、后者由 fall-through 隐式选择）。

### 3.6 `Dyn<D>` 上的形态

- `d.m@D()` 合法且等价于 `d.m()`（dyn 携带 D 的 vtable，`@D` 仅作显式标注，**不**走 spec 默认体源符号 —— 仍按 vtable dispatch）。
- `d.m@OtherSpec()` 拒（dyn 只携带 D 的 vtable，无法 dispatch 到其它 spec）；报 **E1134**（沿用对象安全 / vtable 缺失语义类）或新增子诊断（实施期决定）。

### 3.7 调用约定 / ABI

- `$.m@SpecA()` 在 codegen 期直接打到 **SpecA 默认体在当前类型 T 上 fall-through 合成的方法符号**（mangling 与常规方法等价）。
- 若 T 未 fall-through SpecA 的 m（因为 T 覆盖了 m），编译期为 `@SpecA` 调用点**额外合成一份 SpecA 默认体 IR**（mangling 加 `@SpecA` 标签后缀，避免与 T 的覆盖体撞符号）。该合成仅在被实际 `@SpecA` 调用时触发，无人用则不发 IR。
- 调用约定与常规方法 + fall-through 完全一致；receiver 借用 / RC 行为按 §8.6 / §8.5 走。

## 4. 错误码

复用现有码，不引入新码：

| 码 | 触发 | 备注 |
|---|---|---|
| E1101 | receiver 类型未 `#Impl(SpecA)` | 沿用 §12.2.2.1 missing-impl 语义类 |
| E1140 | SpecA 无该方法 / 无默认体 | 沿用 §12.10.3.2 占位校验语义类，消息按上下文区分 |
| E1134 | `Dyn<D>` 上 `@OtherSpec` | 实施期复评，可能新增子诊断 |

## 5. 不在范围

- **`@` 后路径前缀 / 全限定形态**（`@pkg.SpecA`）—— 留"统一路径形态"专项。
- **free fn / 静态调用上的 `@` 后缀**（`SpecA::m@SpecB(...)`）—— 不引入；`@` 仅在 dot-call 出现。
- **`@label` 用于 `break` / `ret`** —— 同源 `@` 形态，留独立草案（`DRAFT-label-break.md` 或类似）；本草案不绑死 label 语义，避免一份草案担两件事。
- **`obj.m@SpecA::nested()` 嵌套形态** —— 不引入；`@` 后只接单 spec 标签。
- **运行时类型检查 / `@` 的反射形态** —— 留 spec-reflect 专项。

## 6. 迁移面（粗估）

### 6.1 编译器（`src/`）

- `src/yuxParser.g4` / `gen/`：`exprDotCall` 加可选 `('@' ID genericDef?)`（**改 g4，按 RULES.md 高风险，先与用户确认形态**）。
- `src/ast/ast_builder_expr.cpp`（或 callsite 对应文件）：`visitExprDotCall` 命中 `@ID` 时填 `CallNode::specQualifier`。
- `src/ast/node/call_node.{h,cpp}`：加 `optional<string> specQualifier`。
- `src/sema/sema_pass.cpp` / `src/sema/call_resolve.cpp`：dot-call 解析路径上加 `@SpecA` 分支：T 的 `#Impl` 列表查 SpecA → SpecA 签名表查 m → 默认体存在性校验。
- `src/compiler/compiler_call.cpp`（或对应文件）：codegen 期打到 spec 默认体在 T 上的 fall-through 合成符号；若 T 覆盖了 m，额外合成 `@SpecA` 专用符号。
- `src/ast/mangler.{h,cpp}`：`@SpecA` 专用符号 mangling 规则（建议加 `__yux_at<SpecA>` 后缀）。

### 6.2 SDK / runtime（`sdk/`）

- 无新增 SDK 改动。`base.yux` 5 件套不引入消歧用法（无组合冲突场景）。
- 可在 `sdk/yux/core/spec_disambig.test.yux` 加端到端用例验证。

### 6.3 测试（`tests/`）

- 新增 `tests/cases/spec_disambig_at_*`：基本 `$.m@SpecA()` / 外部 `obj.m@SpecA()` / E3132 消歧覆盖体内 delegate / fall-through 等价性 / `Dyn<D>` 上 `@D` 与 `@OtherSpec` / `@` 后未实现 spec E1101 / `@` 后无该方法 E1140 / `@` 后纯抽象签名 E1140。
- 跨包同名 spec 单名解析失败用例（如 `tests/projects/spec_disambig_cross_pkg/`）。

### 6.4 规范文档（`docs/spec/`）

- `docs/spec/12-spec.md` §12.10 续节（建议编号 §12.10.8 或 §12.11）「消歧调用 `@SpecA` 后缀」：形态 / sema 规则 / fall-through 关系 / Dyn / ABI / 错误码。
- §12.10.5.3 删除"留 v0.X+1"措辞，改为"详见 §12.10.8 / §12.11"。
- §B.2 `exprDotCall` 产生式同步。
- 附录 D：E1101 / E1140 备注追加新触发点。
- `docs/spec/CHANGELOG.md` 顶部追加一条。

### 6.5 用户教程（`docs/`）

- `docs/spec.md`（或 `docs/接口.md` 等中文教程）追加"消歧"小节，给 E3132 场景的典型例子。

## 7. 决议日志

按讨论顺序追加，标 `[#编号]`。每次反复或修订也追加新条目，不要覆盖。

- **[#1.A]** 选 `$.m@SpecA()` dot-call 形态，**不**选 `SpecA::m($)` UFCS 形态。理由：保留 method-style 直觉、receiver 不用塞首参、`@` 标签通道与未来 `break@label` / `ret@label` 同源。代价：g4 改动 + AST 字段 + sema 分支。
- **[#1.B]** `@` 后**单名**，只接 receiver 类型 `#Impl` 列表中的 spec。跨包 / 全限定形态留待后续"统一路径形态"专项一次性铺。
- **[#1.C]** `$.m@SpecA()` 永远指向 SpecA 默认方法体（escape hatch）；即便 T 覆盖了 m，`@SpecA` 仍走 spec 默认体。理由：消歧 / delegate 是主要使用场景，措辞直白。
- **[#1.D]** 错误码复用 E1101 / E1140，不引入新码。E1140 消息按上下文区分"unknown method" vs "no default body"。
- **[#1.E]** `@` 仅出现在 dot-call；free fn / `Type::factory` / 构造调用**不**接受 `@` 后缀。
- **[#1.F]** `Dyn<D>` 上 `d.m@D()` 合法等价于 `d.m()`；`d.m@OtherSpec()` 拒（vtable 缺失）。
- **[#1.G]** `@label` 用于 `break` / `ret` 与本草案**同源 `@` 形态但分草案**；本草案不绑死 label 语义。
- **[#1.H]** codegen 期 T 覆盖 m 时为 `@SpecA` 调用点额外合成一份 spec 默认体 IR（mangling 加 `@SpecA` 标签），无人用则不发。

---

## 定型与归宿

草案定型后按以下步骤拆分迁入正式文档：

1. `docs/spec/12-spec.md` §12.10 续节（建议 §12.10.8 / §12.11）按 §3 / §4 落条款；§12.10.5.3 改"详见 §X.Y"。
2. `docs/spec/CHANGELOG.md` 顶部追加 2026-MM-DD 条目。
3. 附录 D：E1101 / E1140 备注追加新触发点；附录 B `exprDotCall` 产生式同步。
4. `CURRENT.md` Phase 列表从 §6 派生；全部完成后归档到 `docs/dev/spec-disambig-at-impl-log.md`。
5. 本 DRAFT 文件头部加「已落地，见 §12.10.8 / §12.11」批注，保留为历史档。
