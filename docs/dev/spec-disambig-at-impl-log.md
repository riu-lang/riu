# spec 默认体消歧调用 `@SpecA` 后缀 实施日志

> 草案 [`DRAFT-spec-disambig-at.md`](../spec/draft/DRAFT-spec-disambig-at.md)（2026-05-22 锁形态，2026-05-23 落地）。前置 [`spec-default-body-impl-log.md`](spec-default-body-impl-log.md)（§12.10）。本日志记录 Phase 1 → Phase 5 的落地过程；面向后续 Dyn fall-through assert 修复、`@label`（break/ret）独立草案、统一路径形态（跨包 `@pkg.Spec`）等相关工作的参考资料。
>
> 范围：dot-call 方法名后可选 `@SpecA` 后缀，显式指向某 spec 的默认方法体（escape hatch）；填补 §12.10.5 E3132 消歧覆盖体内 delegate 到 spec 默认体的形态空缺。`@` 仅在 dot-call 出现，仅接 spec 单名。

## 阶段总览

- **Phase 1** g4 + AST 承载。`src/yuxLexer.g4` 加 `SymbolAt: '@';`；`src/yuxParser.g4` `exprDot` 在 `member+=ID` 后加可选 `(SymbolAt specQual=ID)`（去掉草案曾估的 `genericDef` 槽，单名足够）；重新生成 `gen/`。`ExprDotNode` 加 `_specQualifier` 字段 + `specQualifier()` / `hasSpecQualifier()` 访问器；`ASTBuilder::visitExprDot` 命中 `SymbolAt + specQual` 时填充。改 g4 经用户口头确认（按 `behavior.md` 例外）。
- **Phase 2** sema 解析。`src/sema/sema_pass.cpp` 在 ExprCallNode 分支顶部加 `dotCallee->hasSpecQualifier()` 校验：T 不在 `#Impl` 列表 → **E1101**；SpecA 无该方法 → **E1140**（"unknown method"）；SpecA 中 m 是纯抽象签名 → **E1140**（"no default body"）。E1140 模板升级为 3 参数 `(specName, methodName, contextSuffix)`，既有 caller 同步。0 LLVM 严守。`diag_spec_disambig_at_{not_impl,unknown_method,no_default_body}` 端到端用例落地。
- **Phase 3** codegen。提取 `Compiler::emitSpecDefaultBodyMethod(spec, sigIdx, structName, emitMethodName)` helper 复用 fall-through 路径的 TypeSelfNode patch + `body->parentScope` 重指向 + `$` / 形参符号表 patch + restore。`StructImplNode` 加 `SpecDisambigEmit` 数据结构 + accessor。`SpecImplChecker::validateImpl` 末尾循环：为每条 (spec, 带默认体的签名) 登记 SpecDisambigEmit 并注册 `S.m__at__<spec>` fnSymbol。`Compiler::compileInheritedDefaults` 同步发射 disambigs（即便 S 覆盖了 m，escape hatch 仍需要）。`Compiler::compileMethodCall` 在 `hasSpecQualifier` 时把 member 重写为 `m__at__<spec>`，后续 dispatch 复用现有路径。Mangler 不动（`__at__` 是合法 identifier 字串）。`spec_disambig_at_{basic,override_delegate,e3132_delegate}` 端到端用例全过。
- **Phase 4** Dyn 整合（sema 完成，codegen 阻塞端到端）。sema 端 `d.m@D()` 接受（匹配 dyn 的 spec D，走常规 dispatch，不重写 member）；`d.m@OtherSpec()` 拒（复用 E1101）。codegen `compileMethodCall` 在 `baseType.isDyn()` 时跳过 member 重写。Dyn 上调用 spec 默认体 fall-through 方法本身触发 LLVM bad-signature assert（与 `@` 形态无关，见 BUG 清单），端到端测试推迟至该 BUG 修复后补；@ 形态在 Dyn 上的 sema 形态校验已完整，codegen 兜底就位。
- **Phase 5** 规范回写（2026-05-23）。`docs/spec/12-spec.md` 新增 §12.10.8「消歧调用 `@SpecA` 后缀」；§12.10.5.3 措辞改指向 §12.10.8；§12.10.7 item 1 标"已落地"。附录 B `exprDot` 产生式同步。附录 D `E1101` / `E1140` 备注追加新触发点。CHANGELOG 顶部追加 2026-05-23 条目。DRAFT 头部加"已落地"批注。

## 关键决策记录

### 选 `$.m@SpecA()` dot-call，不选 `SpecA::m($)` UFCS（[#1.A] 承袭）

草案早版本（spec-default-body Phase 7 占位）打算引入 `a.SpecA::m()` 形态，与既有 `Type::m()` 静态调用通道协同。审视后改 dot-call + `@` 后缀，理由：（1）保留 method-style 直觉，receiver 不用塞首参；（2）`@` 标签通道与未来 `break@label` / `ret@label` 同源，一次定型；（3）`Type::m()` 已是"静态调用"通道，重载它表达"实例调用消歧到具体 spec"在语义上分裂。代价：g4 改动（exprDot 加可选槽）+ AST 字段 + sema 分支 + codegen 额外符号合成。

### `@` 后单名，不接路径前缀（[#1.B] 承袭）

`@` 后只接 spec 单名（按 §10 名字解析），不接 `@pkg.SpecA` 形态。理由：跨包消歧场景留待"统一路径形态"专项（同期铺 `pkg.X<...>` turbofish、`#Impl(pkg.Spec)` 等位置），避免现在引入半成品形态。代价：若 T 实现了来自两个不同包的同名 spec，单名 `@Show` 不可解析 → 报 E1101（视为"未实现"）；该场景极罕见，可接受。

### `@SpecA` 是 escape hatch，永远指向 spec 默认体（[#1.C] 承袭）

`$.m@SpecA()` 即便 T 覆盖了 m 仍走 SpecA 的默认方法体。理由：消歧 / delegate 是 `@` 形态的**主要使用场景**——E3132 冲突时实现者写覆盖，体内需要 delegate 到任一 spec 的已有默认体；若 `@SpecA` 在 T 覆盖时退化为 T 的覆盖体，escape hatch 形态消失。代价：codegen 期 T 覆盖 m 时仍需为 `@SpecA` 调用点合成 spec 默认体 IR，无人用则不发。

### 错误码复用 E1101 / E1140，不引入新码（[#1.D] 承袭）

E1101 / E1140 触发面扩展承载新触发点，E1140 模板升级为 3 参数 `(specName, methodName, contextSuffix)` 让消息按上下文区分"unknown method" vs "no default body"。理由：与"实现者漏 spec 方法" / "spec 默认体引用未知方法名"语义同类，新码反而让"未实现"概念分裂；Dyn 上 `@OtherSpec` 同复用 E1101（vtable 缺失 = 未实现）。

### codegen 额外合成 `m__at__<spec>` 符号

`StructImplNode::SpecDisambigEmit` 登记 (spec, 带默认体签名) → `Compiler::compileInheritedDefaults` 同步发射 `S.m__at__<spec>`（沿用 fall-through 的 TypeSelfNode + parentScope patch helper）；调用点 `compileMethodCall` 把 member 名重写为 `m__at__<spec>` 走常规 dispatch。理由：（1）即便 T 覆盖了 m，escape hatch 仍要求能调到 spec 默认体——必须另起一份符号避免与 T 的覆盖体撞符号；（2）`__at__` 是合法 identifier 字串，沿用 `Mangler::method` 不引入新 mangling 规则；（3）该额外符号仅在 `S` 实际参与 spec 实现宣告校验时合成，无 spec 默认体则不发。

### Dyn 上 `@` 形态不重写 member（Phase 4）

sema 端 `d.m@D()`（D 匹配 Dyn 的 spec）走常规 dispatch；codegen `compileMethodCall` 在 `baseType.isDyn()` 时**跳过 member 重写**——dyn 携带的是 D 的 vtable，无 `m__at__<spec>` 槽位，重写后会查不到符号。`d.m@OtherSpec()` 在 sema 阶段已拒（E1101），codegen 不会进入。

## 关键文件 / 函数

- `src/yuxLexer.g4`：`SymbolAt: '@';` token（Phase 1）。
- `src/yuxParser.g4`：`exprDot` 在 `member+=ID` 后加 `(SymbolAt specQual=ID)?`（Phase 1）。
- `gen/`：ANTLR4 重新生成（Phase 1）。
- `src/ast/node/expr_dot_node.{h,cpp}`：`_specQualifier` 字段 + `specQualifier()` / `hasSpecQualifier()` 访问器。
- `src/ast/ast_builder_expr.cpp::visitExprDot`：命中 `SymbolAt + specQual` 时填 `ExprDotNode._specQualifier`。
- `src/sema/sema_pass.cpp`：ExprCallNode 分支顶部 `@` 后缀三段校验（impl 列表 / spec 方法存在 / 带默认体）；E1140 模板 3 参数化（Phase 2）。
- `include/error_code.h`：E1140 模板升级 + caller 同步。
- `src/compiler/compiler_struct.cpp::Compiler::emitSpecDefaultBodyMethod`：提取的 helper，复用 fall-through 的 patch / restore 流程（Phase 3）。
- `src/ast/node/struct_impl_node.{h,cpp}`：`SpecDisambigEmit` 数据结构 + accessor。
- `src/analyzer/spec_impl_checker.cpp::SpecImplChecker::validateImpl`：末尾循环登记 SpecDisambigEmit + 注册 `S.m__at__<spec>` fnSymbol。
- `src/compiler/compiler_struct.cpp::Compiler::compileInheritedDefaults`：同步发射 disambig 符号。
- `src/compiler/compiler_call.cpp::Compiler::compileMethodCall`：`hasSpecQualifier` 时 member 重写为 `m__at__<spec>`；`baseType.isDyn()` 时跳过重写（Phase 4）。

## ABI / 协议

- 实现者类型 `S` 对每条 (spec, 带默认体的签名) 在 fall-through 之外**额外合成** `S.m__at__<spec>` 符号；调用约定 / 借用语义与常规方法 + fall-through 等价。
- `m__at__<spec>` 走 `Mangler::method`，不引入新 mangling 规则——`__at__` 是合法 identifier 字串嵌入方法名。
- 该额外符号仅在 `S` 实际参与 spec 实现宣告校验时合成；无人用 `@SpecA` 形态不影响其它编译产物。
- `Dyn<D>` 上 `@` 形态由 vtable dispatch 接管（`d.m@D()`），不进 `m__at__<spec>` 路径；`@OtherSpec` 在 sema 期已拒。

## 跨 Phase TODO

- **Dyn 上 spec 默认体 fall-through 方法的端到端测试**：阻塞于 LLVM bad-signature assert（见 BUG 清单），与 `@` 形态无关。BUG 修复后补 `spec_disambig_at_dyn_*` 用例。
- **`@label` 用于 `break` / `ret`**：同源 `@` 形态但分草案承担，独立 `DRAFT-label-break.md` 或类似。当前 `@` token 已通用化，label 形态落地时仅需在对应位置补 parser 分支。
- **跨包 / 全限定形态**：`@pkg.SpecA` 留"统一路径形态"专项（与同期 `pkg.X<...>` turbofish / `#Impl(pkg.Spec)` 等位置一并设计）。当前限制：T 实现了不同包的同名 spec 时单名 `@Show` 不可解析 → 报 E1101，用户须 import alias 或等待全限定形态。

## 踩坑清单

### Dyn 上调用 spec 默认体 fall-through 方法触发 LLVM bad-signature assert

Phase 4 落 Dyn 整合时表现：sema 端 `d.m@D()` 已接受 + codegen 跳过 member 重写，端到端用例编译时 LLVM 在 vtable dispatch 路径上触发 bad-signature assert。定位：与 `@` 形态无关——Dyn 上调用 spec 默认体 fall-through 方法本身（即便不带 `@` 后缀）即触发，根因在 vtable 槽填充时 fall-through 合成方法的签名与 spec 抽象签名 mismatch。**绕过**：本任务端到端用例不覆盖 Dyn 场景，sema 形态校验已完整。**已记录到 `BUGS.md`**："Dyn<D&> 上调用 spec 默认体 fall-through 方法触发 LLVM bad-signature assert"，留独立排查窗口。
