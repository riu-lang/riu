# const-eval 编译期求值扩展实施日志

`DRAFT-const-eval.md` Phase 1–5 的落地记录。范围：全局 `#Cval` 初始化器从 `literal` 升为常量表达式、struct 字面量 LHS/位置放宽、`#Const fn` 纳入 sema 期编译期求值。

- 规范条款：`docs/spec/05-语句与控制流.md` §5.1.4 / §5.1.5（`#Cval let` 初值约束扩展）、`docs/spec/07-结构体.md` §7.3.2 / §7.10.3（struct 字面量 LHS + 位置放宽）、`docs/spec/11-编译期注解.md` §11.6.4 / §11.10（`#Const` const-eval 通路 + `#Cval` 初值措辞）、附录 B §B.1 / §B.6（产生式同步）、附录 D §D.3.3（E3140–E3144）
- 草案归档：`DRAFT-const-eval.md` 头部"已落地（Phase 1–5，2026-05-30）"
- 实施分支：`dev-const-eval`（从 d7f7a2f 之前切出，与 `dev` 分支反射 Phase 5+ 并行）

---

## 核心决策

- **ConstantValue 自定义 variant**（0 LLVM 依赖）：`int64_t` / `double` / `bool` / `null` / `struct{vector<ConstantValue>}`。保持 sema/codegen 分离协议不破（[sema-codegen.md](../../rules/sema-codegen.md)）。
- **求值阶段在 sema 期**：`ConstEvaluator` 输入 `ExprNode*` + const 上下文，递归求值输出 `std::optional<ConstantValue>`；codegen 端翻为 `llvm::Constant` emit 全局，不走 IRBuilder runtime 路径。
- **复用 `#Const` 注解**（[#1.B]）：自由/静态成员位叠加 const-eval 通路，等价 C++ `constexpr`；成员位语义不变（不改 `$` / 不调非 `#Const`）。同一注解、两个使用面，不引入新注解。
- **整数溢出 trap（E3143）**（[#1.H]）：sema 期可算出的算术错直接编译失败，与运行期 wrap 形成"一致失败"而非"一致 wrap"。
- **错误处理禁**（[#1.H]）：`#Const fn` body 禁 `try / catch`（E3141），编译期"必定成功 or 编译失败"二态。
- **String 形参/返回 v1 禁**（[#1.I]）：`String` 含 Block 头不能进 rodata；长期解 `DRAFT-str-view.md`。
- **控制流白名单最小集**：`= expr` / 单 ret 块体 / 顺序 const let / `exprOneLineIfElse` / `exprIfElsePreValue` / if-statement 两分支 ret。不含 `for` / `while` / `match`。
- **struct 字面量 LHS 放宽**：从 `Self` 到 `typeName`，从仅 `#Static fn` 体内到任意 expr 位置。字段约束不变（全公开、全填、顺序无关）。
- **E3142 占位**：v1 无 `#Private` 修饰，E3142 无触发场景，先注册码 + 占位测试。

---

## Phase 1 — sema-期 const-eval 求值器骨架（0 LLVM）

- **新增** `src/sema/const_eval.{h,cpp}`：`ConstantValue` variant + `ConstEvaluator` 类
- 支持节点：字面量 / 名字引用（`#Cval` 全局 + 局部 const let）/ 一元 / 二元算术 / 位 / 比较 / 逻辑
- 本阶段不接入 ast_builder 通路，仅独立求值器

## Phase 2 — g4 改动 + 全局 `#Cval let` 接 expr

- **g4 改动**：`letGlobal` RHS `literal` → `expr`（经用户确认）
- `ast_builder_decl.cpp::visitLetGlobal` 接收 expr，调 ConstEvaluator；非 const → E3140
- 错码 E3140 / E3143 注册到 `include/error_code.h`
- codegen：从 ConstantValue 翻 `llvm::Constant` emit 全局（仅标量 / bool / null）
- 测试：`const_eval_global_arith` / `diag_const_eval_E3140` / `diag_const_eval_E3143`（E3143 测试占位，当前所有算术失败统一报 E3140）

## Phase 3 — 控制流白名单 + `#Const fn` body 校验扩展

- `src/analyzer/const_mut_checker.cpp`：扩 `#Const fn` body 校验（自由 / 静态成员位）
- 控制流白名单 + E3141 注册
- 测试：`diag_const_eval_E3141`

## Phase 4 — `#Const fn` 调用纳入 const-eval

- `ConstEvaluator` 加 `ExprCallNode` 分支：callee 必须 `#Const`，args 全 const-evaluable，按入参绑定形参局部环境，递归求 body
- 形参 / 返回类型白名单校验（标量 / const struct / `#Builtin` rodata）；违反 → E3144
- 错码 E3144 注册
- 测试：`const_eval_const_fn_basic` / `const_eval_const_fn_nested` / `diag_const_eval_E3144`

## Phase 5 — struct 字面量扩展 LHS / 位置 + ConstantStruct 求值

- **g4 改动**：struct 字面量 LHS `Self` → `typeName`（经用户确认）
- `ast_builder` 放行 `Point { .x = e <NL> .y = e }` 形态在任意 expr 位
- `ConstEvaluator` 加 `ExprStructLitNode` 分支：每字段递归求 const → `ConstantValue::Struct`
- codegen：runtime 上下文走现有 IRBuilder 路径；const 上下文从 `ConstantValue::Struct` 翻 `llvm::ConstantStruct`
- 字段约束：全字段必须填全 + 全公开（v1 无 `#Private` 故事实通过）
- 测试：`const_eval_global_struct_literal` / `const_eval_struct_runtime` / `const_eval_nested_struct`

---

## 跨 Phase TODO

- **E3143 分流**：当前所有算术失败（含溢出 / 除零）统一报 E3140；`const_eval.h` 已留 `lastError` 字段设计，后续启用即可触发 `diag_const_eval_E3143` 测试
- **const-eval 缓存**：同参多次调用 `#Const fn` 无 memoize；缓存可后置加，不影响正确性
- **浮点求值精度**：当前用 host `double`；交叉编译时与目标行为有分歧风险，后续应按目标三元组对齐
- **`match` 在 const-eval**：待 `DRAFT-枚举.md` 收口后讨论
- **Phase 6（reflect 反哺）**：`ensureReflectTypeGlobal` 迁到 const-eval 通路，需与 reflect dev 分支协调 rebase 时机
- **Phase 8（用户文档同步）**：`docs/结构体.md` / `docs/基础语法.md` / `docs/构建注解.md` 示例代码同步
