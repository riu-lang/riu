; 草案：yux 编译期反射（内置 spec Reflect + 静态字段）—— 已落地，见 §13

# 草案：yux 编译期反射

状态：**已落地（2026-06-09，见 [`../13-反射.md`](../13-反射.md)）**。日期：2026-05-18（草案）、2026-06-09（落地）。
作用：在 `DRAFT-spec-unify.md` v1 之上引入**内置 spec `Reflect`**——编译器隐式为每个类型 `#Impl(Reflect)`，通过 `#Static #Frozen` 字段（仅类型形访问）暴露类型 / 字段 / 方法 / 变体元数据；反射数据走 **runtime 数组**（普通 `for` 遍历）；`Field.value` 走 sema 期纯改名（编译期可定的具体 Field 引用），**无 `#Inline for` / 无 IR-before pass**（[#1.AE]）；`#Reflect` 注解控制 emit 强度防 DCE。

> 本草案从 `DRAFT-spec-unify.md` v1 收紧后剥离（[#1.AD]）。形态决议已在母草案锁住：[#1.G] 反射动机、[#1.H] runtime emit、[#1.W] `f.value` 字段访问形态、[#1.X] Field 唯一编译器内置、[#1.Y] Reflect spec 形态、[#1.Z] `#Static #Frozen` 字段子集、[#1.AB] 实例不能调静态成员、[#1.AE] 永不引入 inline-for / `Field.value` 走 sema 改名。本草案承接实施。

涉及章节（预估）：§12 spec / 新增 §13 反射 / §11 编译期注解（`#Reflect`）。

---

## 1. 目标

- 通过**内置 spec `Reflect`**暴露反射 API，编译器隐式 `#Impl(Reflect)`
- `#Static #Frozen` 字段子集：`type` / `fields` / `methods` / `variants`，仅类型形访问（`Counter::type` / `Self::fields`，[#1.AB]）
- 数据 emit 到 `.rodata`，默认 emit + `--gc-sections` DCE 兜底
- `#Reflect` 标的类型防 DCE 误删
- 反射数据走 **runtime 数组**：`Self::fields` 是普通 `[Field * N]` 定长值数组，可索引 / 取 len / `for` 遍历
- **`Field.value` sema 期改名**：当 `f` 是编译期可确定的具体 Field 引用（`Counter::fields[0]` / `Self::fields[N]` 下标位字面量）时，`f.value` 改写为 `<receiver>.<f.name>`；非编译期可定 → E3133
- **不引入** `#Inline for` / IR-before unroll pass / 异构按字段递归自动 derive（[#1.AE]）

## 2. `Reflect` spec 形态

```yux
; base.yux
#Spec
struct Reflect {                          ; 编译器隐式为每个类型 #Impl(Reflect)，自动填充
  #Static
  #Frozen
  type Type                               ; 值拷贝，仅含全限定类型名
  #Static
  #Frozen
  fields [Field * 0]                      ; 0 = 编译期占位，按 implementer 自动填长度
  #Static
  #Frozen
  methods [Method * 0]                    ; 值数组，无 &
  #Static
  #Frozen
  variants [Variant * 0]                  ; 仅 enum；非 enum 访问 → E3135
}
```

使用形态（**仅类型形**，[#1.AB]）：

| 场景 | 写法 |
|---|---|
| 拿 Type 节点 | `Counter::type` |
| 拿字段表 | `Counter::fields` / `Self::fields` |
| 方法体内 | `Self::type` / `Self::fields`（**不**写 `$.type` / `$.fields`，E1137）|

## 3. `Type` / `Field` / `Method` / `Variant` 数据类型

全部为 `#Builtin` struct，LLVM 布局由编译器硬编码。

```yux
#Builtin
struct Type {
  #Frozen
  name String                          ; 全限定类型名（如 "yux.core.Counter"）
  ; fields / methods / variants 不作为 Type 成员，通过 T::fields / T::methods /
  ; T::variants 独立静态路径访问（Reflect spec 上的 #Static 字段）。
  ; 全部按值 copy（rodata → stack），不引入 T&。
}

#Builtin
struct Method {
  #Frozen
  name String
}

#Builtin
struct Variant {
  #Frozen
  name String
}
```

**`Field` 唯一编译器内置**（[#1.X]）——`.value` 是 dependent-typed phantom（类型按 f 静态绑定的具体字段而定），yux 类型系统当前无法表达：

```yux
#Builtin
struct Field {
  #Frozen
  name String
  ; value <field 实际类型>             ; phantom：sema 期改名为 <receiver>.<f.name>
  ;                                    ; 仅当 f 编译期可定时可用（`Counter::fields[0].value` 等）
  ;                                    ; 详见 §6 / [#1.W] / [#1.AE]
  ; type / offset 暂不包含：使用时上下文已知遍历哪个 struct 的 fields，有需求时再加。
}
```

代价：LSP 补全 / hover / doc 三套硬编码 `.value` 字段。

## 4. `.rodata` emit 与 DCE

- 默认每类型 emit `Type` / `Field` 节点到 `.rodata`，linkonce_odr
- 链接期 `--gc-sections` 自动删未被引用的反射数据
- `#Reflect` 注解 → 防 DCE，强制保留即使源码未引用

```yux
#Reflect
struct User {
  name String
  age i32
}
```

`#Reflect` v1 零参；anno-struct 落地后（[#1.P]）可扩 named-args bool 字段（`#Reflect(name=true, fields=false)`）。

## 5. 实例形访问拦截（[#1.AB]）

`c.type` / `c.fields` / `$.type` / `$.fields` 等"实例上调静态成员" → **E1137**。提示用 `<Type>::field` / `Self::field`。

理由：静态属类型不属实例；避免 sema 引入"实例形 mirror"注入逻辑。

## 6. `Field.value` sema 期改名（[#1.W] / [#1.AE]）

**形态**：通过 `Field.value` 访问字段实际值，按普通字段访问规则解析，**无新语法**。

sema 规则：

| 上下文 | `f.value` 解析 |
|---|---|
| `f` 是编译期可确定的具体 `Field&`（`Counter::fields[0]` / `Self::fields[N]` 下标位字面量） | 改写为 `<receiver>.<f.name>`；类型 = 该字段实际类型 |
| `f` 是 runtime 变量（普通 `for f in Self::fields` 内的循环变量） | E3133"`Field.value` 要求 f 编译期可定"|
| 在普通方法体外（无 `$` receiver 上下文）| E3134"`Field.value` 改名无 receiver 绑定" |
| 写 `f.value = expr` | 等价 `$.<f.name> = expr`，受常规 const-mut 检查（`#Mut` 字段才允许写）|

实现路径：sema 期识别 `<staticFieldsExpr>[<intLit>].value` pattern → resolve 出该 Field 的 `name` 静态值 → 改写为 `<receiver>.<name>`；复用既有字段访问通路 + const-mut 检查，**无 IR-before pass / 无 unroll 机制**（[#1.AE]）。

显式 receiver 形态（可选扩展）：是否支持 `<other_recv>::fields[0].value` → `<other_recv>.<f.name>` 改写，留实施时拍板。最简实现仅 `$`（方法体内 receiver）。

副作用：异构按字段递归形态（`ToJson.to_json` 等"遍历所有字段"自动 derive）**永不引入**——`for f in Self::fields { f.value.to_json() }` 报 E3133（`f` 运行期才能定）。实现者用例：手写每字段访问（`Counter::fields[0].value.to_json() + Counter::fields[1].value.to_json() + ...`）或同质化形态（所有字段 `Dyn<ToString>`，普通 `for f in Self::fields { ... }` 但不用 `.value`，用其它运行期访问路径）。

## 7. `#Static #Frozen` 字段子集（[#1.Z]）

吸收进本草案的最小子集：

- `#Static` 字段段允许出现在 spec body（[#1.Q] 通用扩展）
- 字段类型可为 `T&`（runtime ref 类型）
- `Type::name` path-call 形态扩到无 paren 分支（已由 static-fn 落地）
- **不引入** `#Cval` / `#Mut` 静态字段等其它档位（独立 `DRAFT-data-struct.md` 全集另起）

## 8. 错误码

| 码 | 触发 |
|---|---|
| E1137 | 实例上调静态字段 / 静态函数（`c.type` 等，[#1.AB]）|
| E3133 | `Field.value` 改名时 f 非编译期可定（[#1.W] / [#1.AE]）|
| E3134 | `Field.value` 改名无 receiver 绑定（[#1.W] / [#1.AE]）|
| E3135 | `Reflect::variants` 在非 enum 上访问（[#1.Y]）|
| E3136 | ~~按值取 `Counter::type` 等 rodata 单例~~ — 设计消解：反射元数据统一按值 copy（rodata→stack），`Field` 不再含 `type`/`offset`（避免 T&），E3136 无需引入（[#1.Y]）|

## 9. 不在范围

- `#Inline for` unroll / IR-before unroll pass / 异构按字段递归自动 derive 默认体 → **永不引入**（[#1.AE]）
- spec 默认方法体 → `DRAFT-spec-default-body.md`（[#1.AD]）
- `#Static #Frozen` 之外其它静态字段档位 → `DRAFT-data-struct.md`
- `#NoReflect` 反向注解（强制不 emit）
- runtime 方法 dispatch（永不开放，沿用 `Dyn<Spec>`）
- 编译期方法 dispatch（"调一个 cval Type 上的方法"）

## 9. 迁移面

### 9.1 编译器

- `src/sema/`：`#Static` 字段段解析 + spec body 内 `#Static` 允许（[#1.Q] 例外）；`T::name` 静态字段访问（无 paren 分支已在）
- `src/codegen/`：每类型 emit `.rodata` Type / Field 节点；隐式 `#Impl(Reflect)` 自动填充
- 链接期 `--gc-sections` 验证
- **`Field.value` sema 期改名**：识别 `<staticFieldsExpr>[<intLit>].value` pattern → resolve Field.name 静态值 → 改写为 `<receiver>.<name>` → 复用既有字段访问通路；非编译期可定的 f 报 E3133
- LSP / hover / doc 硬编码 `Field.value`

### 9.2 SDK

- base.yux 写 `Reflect` spec + `Type` / `Method` / `Variant` 数据类型 + `Field` `#Builtin stub`
- `#Reflect` 注解识别

### 9.3 g4

- `#Static` 字段段产生式（buildAnno 已就绪，无 g4 新动作 / 仅 sema 接受）
- 不动 token

### 9.4 测试

- `Counter::type` / `Counter::fields[0].name` 等编译期访问
- `Counter::fields[0].value` → `$.<name>` sema 改名端到端
- 实例形 E1137 诊断
- 非编译期可定的 f：`for f in Self::fields { f.value }` → E3133
- 非 enum `Counter::variants` 访问 E3135
- 按值取 rodata 单例 E3136
- `#Reflect` 防 DCE 端到端
- runtime `for f in Self::fields` 普通遍历（不访问 `.value`）正常 codegen

### 9.5 spec 文档

- 新增 §13 反射 / `Reflect` spec / `#Static #Frozen` 字段子集 / `Field.value` sema 改名
- §11 编译期注解追加 `#Reflect`

---

## 决议日志

承接 `DRAFT-spec-unify.md` 决议：

- **[#1.G]** 反射 P1 动机
- **[#1.H]** `#Reflect` runtime emit
- **[#1.W]** `f.value` 字段访问形态（sema 改名）
- **[#1.X]** Field 唯一编译器内置反射类型
- **[#1.Y]** 内置 spec `Reflect` + 静态字段形态
- **[#1.Z]** `#Static #Frozen` 字段子集吸收
- **[#1.AB]** 实例不能调静态字段 / 静态函数（E1137）
- **[#1.AE]** 永不引入 inline-for / 反射走 runtime / Field.value 走 sema 改名

本草案启动时新增决议从 `[#3.A]` 起编号。

---

## 定型与归宿

参考 `DRAFT-spec-unify.md` 末尾定型流程。本草案依赖 `DRAFT-spec-unify.md` v1 + `DRAFT-spec-default-body.md`（启动顺序：default-body → reflect）。
