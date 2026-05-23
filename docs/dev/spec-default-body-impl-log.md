# spec 默认方法体 + fall-through 实施日志

> 草案 [`DRAFT-spec-default-body.md`](../spec/draft/DRAFT-spec-default-body.md)（2026-05-18 锁形态，2026-05-22 落地）。本日志记录 Phase 1 → Phase 5 的落地过程；面向后续相关草案（`spec-reflect` / `extension-blocks` / 显式消歧调用语法 v0.X+1）的参考资料。
>
> 范围：spec body 内方法允许带默认体（`fnExprBody` / `fnBlockBody`），未覆盖时 fall-through 到实现类型；多 spec 默认体冲突 → 实现方显式覆盖消歧（E3132）。**不引入** `#Derive(Spec)`；**永不引入** 按字段递归自动 derive。Phase 6 spec 回写 + Phase 7 显式消歧调用语法（`a.SpecA::m()`）由后续日志承接。

## 阶段总览

- **Phase 1** AST 承载 + 解锁拒收。`ast_builder_struct.cpp` 移除对 spec body 方法带 body 的拒收；`SpecDeclNode` 签名槽位增加可选 `body` 与 `hasDefaultBody()`；错误码 `E1139` 物理退役（保留编号注释，禁止复用）；`spec_default_body_parse_*` AST dump 用例落地。
- **Phase 2** sema 占位校验。`src/sema/sema_pass.cpp` 对带默认体的 spec 签名做"占位符号校验"：`$` 绑 Self 抽象 type var，`$.foo()` / 裸 `foo()` 必须命中本 spec 内某条签名；引用本 spec 不存在的方法 → `E1140`。**0 LLVM** 严守；新增 AST 节点按 `sema-codegen.md` 在 `visitExpr` 加 dynamic_cast 分支。`spec_default_body_sema_*` 诊断用例落地。
- **Phase 3** fall-through 实现（方案 B：不真克隆）。`src/analyzer/spec_impl_checker.cpp` validateImpl 改为：未命中 spec 方法时若 spec 提供默认体 → 登记 `StructImplNode::InheritedDefault` + 把 fall-through 方法注册到 file fnSymbol 表，不抛 E1101；既无实现也无默认体 → E1101（合并掉草案曾估的 E1136 新码）。`sigEquivalent` 引入 `subst["Self"] = TypeInfo(typeBare)`，`TypeSelfNode.getType()` 空 structName 返回 `TypeInfo("Self")` 让 substitute 命中。`Compiler::compileInheritedDefaults` 常规方法编完后对每条登记记录**临时 patch TypeSelfNode + `$` / Self 形参符号**，走 compileMethod，编完原样还原。`spec_default_fallthrough_basic`（= expr 体）/ `spec_default_fallthrough_block`（{ } 体）端到端用例落地。
- **Phase 4** 组合冲突 E3132。`spec_impl_checker.cpp` validateImpl 改两轮：先按 (name, arity) 聚合所有 spec 的签名条目；二轮判定：impl 命中→OK；全无默认体→E1101；多个默认体→E3132；单一默认体→fall-through 沿用 Phase 3 路径。"一种默认体 + 另一种纯抽象签名" 走 `withDefault.size()==1` 路径放行（默认体覆盖所有 spec）。`E3132` 注册到 `include/error_code.h`。`spec_combine_conflict_E3132_basic` / `spec_combine_default_plus_abstract` 用例落地。
- **Phase 5** SDK 5 件套迁移 + 跨文件 fall-through 修补。`sdk/yux/base.yux`：`Ord.{lt,le,gt,ge}` 由 `cmp` 默认体推、`Eq.ne` 由 `eq` 推；`ToJson.to_json` / `Eq.eq` / `Clone.clone` / `Ord.cmp` / `ToString.to_string` 维持纯抽象签名（永不引入按字段递归 derive）。`Compiler::compileInheritedDefaults` 临时把 spec 默认体 `body->parentScope` 重指向 user `_file`，使 `findNearestScope→walk` 能在 user file 找到本 impl 的方法符号（如 `N.cmp`），编完原样还原 —— SDK 定义 spec、user 文件 `#Impl` 的核心组合得以工作。`spec_default_fallthrough_ord_sdk` / `spec_default_fallthrough_eq_sdk` 端到端用例落地。
- **BUG5 修复** 非泛型严格匹配时跳过 spec-bound 校验（`38dbd76`）。Phase 3 引入 `sigEquivalent` 的 Self subst 后，非泛型 impl 的严格签名匹配路径在边界判定上意外触发 spec-bound 校验，误伤无 bound 形态；定位后改为非泛型路径直接跳过 bound 校验。
- **Phase 6** spec 回写（2026-05-22）。`docs/spec/12-spec.md` 新增 §12.10「默认方法体（默认实现 + fall-through）」整段；§12.1.1.1 / §12.4.1.3 翻转拒收条款；§12.8 不在范围 item 2 标"已落地"；Open Issues 删除对应条目。CHANGELOG 顶部追加 2026-05-22 条目。附录 D 新增 `E1140` / `E3132`，标注 `E1139` 退役、`E1137`/`E1138` 既有占位补行。DRAFT 头部加"已落地"批注。g4 不动。

## 关键决策记录

### 不引入 `#Derive(Spec)`（[#1.AA] 承袭）

草案早版本（2026-05-15 初稿）打算用 `#Derive(Ord)` 标记"我要默认实现"，与 `#Impl(Ord)` 共存。审视后发现：`#Impl + 不写体` 就是 "采用默认体"，再加 `#Derive` 是冗余信号；且 `#Derive` 会引入"什么时候必须写、什么时候可省"的二义性。决议**不引入** `#Derive`：`#Impl(D) struct S { ... }` 不写 D 的某方法 ⇔ 采用默认体（若 D 中有默认体）；无默认体仍 E1101。

### 永不引入"按字段递归自动 derive"（[#1.AE] 承袭）

`ToJson.to_json` / `Eq.eq` / `Clone.clone` 这类按结构体字段递归填充的默认体形态（典型如 Rust `#[derive(Debug, Clone, PartialEq)]`）**永不引入**。理由：（1）yux 静态分发 + 单态化的代价模型不希望让"派生"成为隐形 IR 膨胀点；（2）spec 默认体只能引用本 spec 内的签名（§12.10.3.1），不开放对实现类型字段的反射访问；（3）反射 API + 实现者手写体足够覆盖典型场景（DRAFT-spec-reflect 落地后）。决议在 §12.10.2.2 与 §12.8 项 15 永久封口。

### 组合冲突方案 B：实现者手写覆盖，无 escape hatch（[#1.T] 承袭）

多 spec 提供同名默认体时三种可能：A）按 `#Impl(D)` 顺序优先；B）实现方手写覆盖消歧；C）引入 `use SpecA::method` 显式选边。决议 **方案 B**：冲突 → E3132 → 实现方写显式覆盖，无 escape hatch。理由：（A）让 `#Impl` 顺序成为隐式语义点，违背"显式优先"基调；（C）引入新语法形态，与现有 `Type::method()` 静态调用通道协同设计成本高，留 Phase 7 单独评估。Phase 7（v0.X+1）将考虑 `a.SpecA::m()` 形态作为实现者写覆盖时的内部调用通道。

### fall-through 不真克隆 AST，临时 patch + 还原

`Compiler::compileInheritedDefaults` 编实现类型 S 的方法时，对每条 fall-through 登记记录：
- 临时把 spec 默认体的 `TypeSelfNode` 改写为指向 `S`；
- 临时给 `$` / Self 形参符号绑到 `S&` / `S`；
- 跨文件场景（SDK 定义 spec、user 文件 `#Impl`）临时把 `body->parentScope` 重指向 user `_file`；
- 走 `compileMethod` 编完后**原样还原**所有临时改写。

不真克隆 AST 的原因：（1）spec 默认体 AST 节点在多个 impl 间共享，克隆会让节点身份难追；（2）临时 patch + 还原的代价模型在 v1 单线程编译里可控；（3）调试 / IR dump 时仍能看到 spec 默认体的原始位置（克隆会丢失 source 关联）。代价：默认体内 statement / expr 局部 `let x Self = ...` 类用法不在 patch 范围（Phase 5 base.yux 5 件套不触发，后续若需扩展再补递归 TypeNode 收集）。

### 跨文件 fall-through 的 parentScope 临时重指向

Phase 5 落 SDK 5 件套时发现：spec 定义在 `sdk/yux/base.yux`，user 文件 `foo.yux` 写 `#Impl(Ord) struct N { fn cmp(...) i32 { ... } }`，编译 user 文件时 fall-through 进的 `lt/le/gt/ge` 默认体内调 `$.cmp(other)`，`findNearestScope→walk` 从 spec 默认体节点出发走 parentScope，走到 SDK 文件 scope 找不到 user 的 `N.cmp` 方法符号 → 报"未定义方法"。修复：`compileInheritedDefaults` 临时把 spec 默认体 `body->parentScope` 重指向 user `_file`，让 `walk` 命中 user file 的 fnSymbol 表，编完原样还原。这是 fall-through 跨文件的核心 enabler。

### 错误码：复用 E1101，不引入新码

DRAFT §6 表格曾估 `E1136`（"spec 仅签名无默认体 + 实现者没写"）作新码。落地评审认定：与 §12.2.2.1 既有"实现者漏 spec 方法"语义重合，新码反而让"未实现"概念分裂。决议合并进既有 `E1101`（spec_impl_checker 实际使用的码）。附录 D 备注两者同语义类，便于读者交叉理解。

## 关键文件 / 函数

- `src/ast/ast_builder_struct.cpp`：Phase 1 移除 spec body 内 fn 带 body 的拒收点（原 line 122-127 附近）。
- `src/ast/node/spec_decl_node.{h,cpp}`：`SpecDeclNode` 签名槽位增加 `body` + `hasDefaultBody()`。
- `src/sema/sema_pass.cpp`：Phase 2 spec 默认体占位符号校验 + `$` Self 抽象绑定 + `E1140` 抛出点；按 `sema-codegen.md` 在 `visitExpr` 加新 AST 节点的 dynamic_cast 分支。
- `src/analyzer/spec_impl_checker.cpp::validateImpl`：Phase 3/4 两轮聚合判定 + InheritedDefault 登记 + E1101 / E3132 抛出点；`sigEquivalent` 引入 `subst["Self"]`。
- `src/compiler/compiler_struct.cpp::Compiler::compileInheritedDefaults`：fall-through 默认体 typecheck + IR 生成；临时 patch TypeSelfNode + `$` / Self 形参符号 + `body->parentScope`，编完还原。
- `include/error_code.h`：`E1139` 退役注释、`E1140` 新增、`E3132` 新增。
- `sdk/yux/base.yux`：5 件套默认体迁移点（`Ord.{lt,le,gt,ge}` / `Eq.ne`）。

## ABI / 协议

- spec 默认体 fall-through 后的方法在 mangling / vtable / 直接调用形态上**与实现方手写方法等价**；外部不可观察是否走 fall-through。
- `Dyn<D>` vtable 槽（§12.9.7）：fall-through 来的方法走与手写方法相同的槽位填充路径；spec 默认体本身不直接进 vtable（vtable 绑死具体 (U, D) 对）。
- spec 默认体 IR 仅在被某具体 `#Impl(D) struct S` 实际触发 fall-through 时生成；无人 fall-through 的默认体不产生 IR（与"无人调用的泛型函数体不发"现状一致）。

## 跨 Phase TODO

- **Phase 7**：显式消歧调用语法 `a.SpecA::m(args)`。与 `::` 静态调用现有协议（§7.10）协同设计；g4 / call_resolve / 测试一并；E3132 错误消息追加"hint: use `a.SpecA::m(...)` to disambiguate（待 v0.X+1）"或类似措辞。
- **默认体内 statement / expr 局部 `let x Self = ...` 类用法**：Phase 5 base.yux 5 件套不触发，后续若需扩展再补递归 TypeNode 收集到 `compileInheritedDefaults` 的 patch 覆盖面。
- **跨编译单元的默认体 IR 共享**：v1 与现有 `<T>` 一致——调用方编译单元需可见 spec 默认体 body；如何与未来 incremental compile 协同留待 v0.7+ 评估。
- **`Dyn<D>` vtable 与 fall-through 协同的端到端测试**：Phase 5 用例覆盖 owned `S` + `<T : D>` 单态化路径，未覆盖 `Dyn<D>(rc_s)` 间接调用 fall-through 方法的端到端形态。`dyn-draft-impl-log` Phase 后续补。

## 踩坑清单

### BUG5：非泛型严格匹配误触发 spec-bound 校验

Phase 3 引入 `sigEquivalent` 的 `subst["Self"]` 后，非泛型 impl 的严格签名匹配路径意外触发 spec-bound 校验。表现：无 bound 的非泛型 `#Impl(D)` 在 validateImpl 严格匹配阶段误报 bound 不满足。定位：`sigEquivalent` Self subst 通用化后让原本非泛型 fast-path 也走 generic bound 检查路径。修复（commit `38dbd76`）：非泛型路径直接跳过 bound 校验。

### SDK fall-through 报 "$.cmp 未定义"

Phase 5 SDK 5 件套落地时表现：user 文件写 `#Impl(Ord) struct N { fn cmp(other Self&) i32 { ... } }`，编译报 `lt` 默认体内 `$.cmp` 未定义。根因：spec 默认体 `body->parentScope` 指向 SDK 文件 scope，`walk` 找不到 user 文件的 `N.cmp`。修复：`compileInheritedDefaults` 临时把 `body->parentScope` 重指向 user `_file`，编完原样还原（见上"关键决策"）。
