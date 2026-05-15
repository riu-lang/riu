# §12 draft（接口与约束）

本章规范 yux 的 **draft**（接口契约）机制：显式实现 `Type : D { ... }`、`#DraftLike` 结构化匹配、泛型边界 `<T : D1 + D2>`、跨包 orphan 限制，以及内置 `ToString` / `Any` / `copy_of`。设计决议详见草案 [`draft/DRAFT-draft.md`](draft/DRAFT-draft.md)（决议日志条目编号在本章以 `[#X.Y]` 给出，仅作可追溯）。

> 设计基调：**默认严格 / 显式优先 / 零运行时开销**。draft 默认是封闭契约，必须由 `Type : D { ... }` 显式实现；`#DraftLike` 是开放结构化匹配的开关。v1 一律单态化静态分发，无 vtable、无隐式签名表参数。

涉及交叉章节：§6.4（函数泛型）/ §7.8（结构体实现块）/ §8.6.7（借用与边界）/ §11.4（注解）/ §附录 D（错误码）。

## §12.1 概念

### §12.1.1 draft 声明

```
draftDecl   ::= buildAnno* 'draft' genericDef? ID '{'
                    ( fnSig | comment | codeLineEnd )*
                '}'

fnSig       ::= buildAnno* 'fn' ID '(' fnParams? ')' (retType=type)?
```

§12.1.1.1 `draftDecl` 引入一个 **draft**：一组方法签名集合（[#A.1] / [#A.1 微补]）。draft 体内**只允许方法签名**，**不得**出现函数体。

§12.1.1.2 签名集**允许为空**（空 draft 合法，对应 §12.7.2 内置 `Any`）。

§12.1.1.3 draft 自身**可以**带泛型形参（[#B.1 修订]）；但 draft 体内单个 `fn` 签名**不得**再引入新的泛型形参（[#C.1]，参见 §12.4.2）。

§12.1.1.4 `draft` 是关键字（附录 A.1）。draft 名按 §10 名字解析；同模块内 draft 名与其它顶层符号共享命名空间。

§12.1.1.5 `#DraftLike` 注解**只能**标在 draft 声明上（§11.4 / §12.4）。

```yux
draft ToString {
  fn to_string() String
}

draft Any { }                ; 合法：空签名集
draft To<T> {                ; 合法：draft 自身泛型
  fn to() T
}
```

### §12.1.2 draft 名字解析

§12.1.2.1 在 `<T : D>` 边界、`Type : D { ... }` 实现块、`#DraftLike` 命中查询中，`D` **应当**按 §10 完全限定名解析。

§12.1.2.2 同名跨包 draft（如 `a.Show` 与 `b.Show`）按完全限定名独立判定，两者并存不冲突；import 名字冲突按 §10 已有规则处理（[#C.3]）。

## §12.2 显式实现 `Type : D { ... }`

### §12.2.1 形态

§12.2.1.1 实现块沿用 §7.8.1 的 `structImpl` 扩展：`Type : D1 + D2 { ... }`。

§12.2.1.2 `Type` **应当**为 owned 类型（值类型 / 用户 struct / 堆句柄类型 / `Ptr`）；**不得**为 `T&`、`T?` 或本地泛型形参（[#B.2] / §8.6.7.1 / §8.6.7.5）。

§12.2.1.3 `Type` 为泛型结构体时（如 `Foo<T>`），实现块需写出形参形态：`Foo<T> : D { ... }`（每实例化一份 IR，与 §6.4.2.3 单态化路径一致）。

### §12.2.2 穷尽性 + 不多余

§12.2.2.1 实现块内方法集**应当**与所列 draft 的签名集严格相等：

- 缺任一 D 签名 → 报 `E1101`（"未实现 draft 方法"）。
- 多任何非 D 签名 → 报 `E1102`（"draft 实现块中含非 draft 方法"）。

§12.2.2.2 同一 `Type : D` 不允许出现多次（同包跨包均报 `E1103`）。辅助方法 / 构造 / 析构走普通方法块（§7.2 / §7.8.2）。

### §12.2.3 实现块的方法分发

§12.2.3.1 对 `Type : D` 已实现的方法 `m`：

- `obj.m(args)`（`obj: T` 或 `T&`）按 §8.6.7.3 receiver 归一为 `Self&`，直接静态分发到该实现。
- `Rc<T>` 上 `box.m(args)` 按 §8.6.7.3 自动解引用 + 归一，等价于在 T 上调用，无需独立 forward 机制（[#D.3] / §12.6）。

§12.2.3.2 receiver `$` 在 draft 实现块的方法体内的语义与普通方法一致（§7.2.2.1，类型 `Self&`）。

## §12.3 签名等价

下列规则在两处使用：（a）`Type : D` 实现块的穷尽性检查（§12.2.2）；（b）`#DraftLike` 结构化匹配（§12.4）。

### §12.3.1 等价判定

| 维度 | 规则 |
|---|---|
| 方法名 | 完全相同（大小写敏感） |
| 参数个数 | 相同 |
| 参数类型 | 逐一按 §3.4 类型相等判定（不变；不协变） |
| 参数名 | 不参与判定 |
| 返回类型 | 相同（不协变） |
| receiver 形态 | 不参与（按 §8.6.7.3 归一为 `Self&`） |
| `T` vs `T&` 参数 | **必须一致**（不互通） |
| draft 体内 `fn` 的本地泛型 | **禁止**（参见 §12.3.2） |
| 注解 | 不参与 |
| 可见性 | 实现 / 候选方法**应当**按 §10 在 D 视角可见 |

### §12.3.2 禁止方法本地泛型

§12.3.2.1 draft 体内单个 `fn` **不得**再引入泛型形参：

```yux
draft Bad {
  fn map<U>(x U) U      ; ❌ E1104
}
```

§12.3.2.2 需要这种能力时，写在外层泛型函数 `fn map<T : Mappable, U>(...)` 的 `genericDef` 中。

§12.3.2.3 draft 自身允许 `genericDef`（§12.1.1.3）；不同实参（如 `Counter : To<String>` / `Counter : To<i32>`）视为不同实现（[#B.1 修订]）。

## §12.4 `#DraftLike` 结构化匹配

### §12.4.1 标注与生效

§12.4.1.1 `#DraftLike` **只能**标在 `draft` 声明上（[#A.3] / §11.4）；标在其它声明（fn / struct / impl 块 / global）报 `E1110`。

§12.4.1.2 一旦 draft 标 `#DraftLike`，该 draft 在 `<T : D>` 场景下允许**结构化匹配**：编译器在单态化期为给定的 owned 类型 T 按 §12.3 检查 T 的方法集是否包含 D 的全部签名；命中即视为 T 实现 D。

§12.4.1.3 未标 `#DraftLike` 的 draft 仅接受显式 `Type : D { ... }` 实现；调用方**不得**绕过。

§12.4.1.4 是否结构化匹配是 draft 的**语义属性**，**不得**在使用处（边界 / 参数 / 调用点）变更。

### §12.4.2 显隐冲突

§12.4.2.1 同包内同时存在"普通方法块定义 `m()` + draft 实现块也实现 `m()`（D 含 `m` 同签名）"**应当**报 `E1105`（**情形 A**），无论 D 是否标 `#DraftLike`：调用 `obj.m()` 会出现普通分发与 draft 分发歧义，不允许写出（[#C.2]）。

§12.4.2.2 跨包 `#DraftLike` 命中（D 在外部包并标 `#DraftLike`、Type 在本包或第三方包仅有同签名普通方法）**应当**自动命中，无 warning（**情形 B**）。

§12.4.2.3 跨包"显隐共存"（情形 C）由 §12.5 orphan 规则禁止而不存在。

### §12.4.3 滥用边界

§12.4.3.1 `#DraftLike` 是**开放结构化匹配**的开关，**应当**有明确的语义动机（典型：跨包契约、不便修改的第三方类型）。仅为"少写 `:` 实现"开放属反模式；编译器**不**强制阻止，本规范层标记为不推荐。

§12.4.3.2 编译器**应当**在以下场景报 `E1110` 段：

| 场景 | 错误码 |
|---|---|
| `#DraftLike` 标在 draft 之外的声明上 | `E1110` |
| `#DraftLike` 标在带默认体的 draft 上（与 §12.1.1.1 矛盾，前向兼容保留） | `E1111` |
| `#DraftLike` 标在含方法本地泛型的 draft 上（与 §12.3.2 矛盾） | `E1112` |

### §12.4.4 多包同名 draft

§12.4.4.1 不引入新规则（[#C.3]）：每个 `<T : D>` 按 D 完全限定名独立判断结构化匹配；同名跨包 draft 并存不冲突。普通成员调用 `x.m()` 始终走普通方法分发，**不**经 draft 结构化匹配。

## §12.5 跨包规则（orphan）

### §12.5.1 限制

§12.5.1.1 `Type : D { ... }` 实现块**应当**写在以下两处之一（[#C.5]）：

- `Type` 所在的包，或
- `D` 所在的包。

否则编译期报 `E1120`（"orphan：禁止为外部类型实现外部 draft"）。

### §12.5.2 设计动机（*informative*）

避免外部库类型被第三方"污染"实现而破坏可推断性；让 §12.4.2 情形 C 不存在；`#DraftLike` 是跨包绑定行为的唯一通道。

## §12.6 `Rc<T>` 与 draft

§12.6.1 v1 **不**为 `Rc<U>` 引入独立的 forward 机制；草案曾用的"Rc 自动 forward"在本规范层等价于 §8.6.7.3 自动解引用 + 方法分发归一（[#D.3]）。

§12.6.2 边界与 Rc 实参的关系：

| 调用形态 | T 实例化 | 要求 D 实现位点 |
|---|---|---|
| `show(box)` 给 `fn show<T : D>(x T)` | T = `Rc<U>` | **`Rc<U>` 自身**实现 D |
| `show(as_ref(box))` 给 `fn show<T : D>(x T&)` | T = U | U 实现 D（沿用 §8.6.7.4） |

§12.6.3 边界泛型形参的实参档位仍受 §8.6.7.1 限制：T **不得**为 `T&`；调用方需经 `as_ref`（§8.3.5.5）/ `copy_of`（§12.7.3）显式做借用 ↔ owned 转换。

§12.6.4 Rc forward **不**绕过 §12.5 orphan：`Rc<U>` 能 forward 的方法集 = U 已实现的 D 方法集。

§12.6.5 v1 draft 体内只有实例方法签名（§12.1.1.1），**不**存在关联函数 forward；其它堆句柄（`Array<T>` / `String` / `Weak<T>` / `StringBuilder`）**不**参与"自动解引用调元素方法"。

## §12.7 内置 draft 与 builtin

### §12.7.1 `ToString`

§12.7.1.1 `sdk/yux/src/yux/core/base.yux` 内置 draft：

```yux
draft ToString {
  fn to_string() String
}
```

**不**标 `#DraftLike`（[#D.1]）：`ToString` 是 v0.6 字符串模板 `"$expr"` 的"可插值"约束，属强契约；用户类型必须显式 `Type : ToString { ... }` 才会进入插值路径，避免 debug-string 被误命中显示文本。

§12.7.1.2 各内置类型在 `base.yux` 内以 `Type : ToString { ... }` 形态显式实现；方法体可走 `#CompilerInner`（§11.2）或直接 yux 实现，二者并存。当前实施：窄整型（i8/u8/i16/u16/i32/u32）/`f32` 委派宽类型 `to_string`；宽类型（i64/u64/f64）/`bool`/`String` 直接 yux 实现：

```yux
i64 : ToString {
  fn to_string() String { ... }   ; yux 实现
}

String : ToString {
  fn to_string() String { $ }     ; 恒等返回
}
; 同理覆盖 i8 / u8 / i16 / u16 / i32 / u32 / u64 / f32 / f64 / bool
```

§12.7.1.3 不引入新注解 `#Builtin`；MILESTONE / TARGETS 中的"`#Builtin`"措辞按 `#CompilerInner` 统一。

### §12.7.2 `Any`

§12.7.2.1 `base.yux` 内置：

```yux
#DraftLike
draft Any { }
```

§12.7.2.2 由 §12.3.1 穷尽匹配 + ∅ 签名集，**所有 owned 类型自动满足 `Any`**（[#D.4]）。v1 仅作"约束最弱形态"使用；**不**与运行时反射 / `is` / `as` 组合。

### §12.7.3 `copy_of`

§12.7.3.1 新增编译器 baked builtin：

| builtin | 签名 | 方向 | 章节 |
|---|---|---|---|
| `as_ref` | `as_ref:<T>(box Rc<T>) T&` | Rc → T& | §8.3.5.5 |
| `copy_of` | `copy_of:<T>(x T&) T` | T& → T | §12.7.3（本节） |

§12.7.3.2 `copy_of` 规则（[#D.5]）：

- T 受 §8.6.7.1 owned 限制；`x` 是 T 的借用，结果是 T 的栈上 owned 副本。
- 复制语义按 T 档位：
  - **值类型**（标量 / 用户 struct / `[T*N]`）：memcpy + 字段级 retain（§7.4.3 / §7.4.4）。
  - **堆句柄**（`Rc<U>` / `Array<U>` / `String` / `Weak<U>` / `StringBuilder`）：句柄复制 + RC retain，沿用 §8.5 callee-clean 协议。
- `x` 的借用根（如对应的 `box` / 局部变量）在 `copy_of` 调用语句结束后仍可正常使用（临时借用 + 立即释放，按 §8.8 临时帧）。
- 与 `as_ref` 不互锁：原 `x` 视图与返回的 owned 副本各自独立析构。
- **不**接受 `Ptr`；turbofish 可省，T 由实参推断。

§12.7.3.3 占位签名置于 `base.yux`，`#CompilerInner` 形态；编译器在调用点合成 IR（§11.2.3）。

```yux
fn use_owned<T : D>(x T) { ... }

fn caller(box Rc<MyType>) {
  use_owned(copy_of(as_ref(box)))   ; T& → T 显式拷贝后传入 owned 形参
}
```

## §12.8 不在范围

v1 / v0.5 **明确不做**（[#E.1]）：

1. ~~**`dyn Draft` / 运行时多态**（mangling 留位但不实现）。~~ → v0.5.x **已实现**，见 §12.9。
2. **draft 默认方法体**。
3. **draft 体内方法本地泛型**（draft 自身可泛型）。
4. **关联类型 / 关联常量**。
5. **跨外部包为外部类型实现外部 draft**（§12.5）。
6. **操作符 draft**（`Add` / `Eq` / `Index` 等语法糖绑定；留 v0.7+；当前运算符走 §7.2.3 重载）。
7. **运行时反射 / `is` / `as` 类型测试**。
8. **draft 继承 / super-trait**（需"D2 蕴含 D1"时直接在边界写 `<T : D1 + D2>`）。
9. **协变 / 逆变返回或参数**（§12.3.1 签名等价不变）。
10. **draft 上独立可见性修饰**（沿用 §10 `_` 前缀私有）。
11. **`<T : D>` 在调用点 turbofish 处回写边界**（仅声明位允许）。
12. **`#DraftLike` 部分匹配 / 子集匹配**（§12.3.1 必须穷尽匹配 D 全部签名）。
13. **`where` 子句、`T : D1 | D2` or 约束**（§6.4.4.2）。

## §12.9 `Dyn<D>` / `Dyn<D&>`（运行时多态形态）

§12.9 在 §12.1–§12.7 的**单态化 `<T : D>`** 之外，提供 draft `D` 的**运行时多态**形态：`Dyn<D>`（owned）与 `Dyn<D&>`（借用）。二者并存、互不替代。设计基调与决议详见草案 [`draft/DRAFT-dyn-draft.md`](draft/DRAFT-dyn-draft.md)。

### §12.9.1 类型档位

§12.9.1.1 两种形态均为 **fat pointer**，运行时表示为 `{ vtable_ptr, data_ptr }`（16 字节，sized）：

| 名称 | 写法 | data 端语义 | RC 行为 |
|---|---|---|---|
| owned dyn | `Dyn<D>` | 指向 `[RC head \| U 实例]`，与 `Rc<U>` 同源 | 标准 RC；强引用为 0 时调 `vtable[0]` 析构 |
| 借用 dyn | `Dyn<D&>` | 借自栈或堆，不持有所有权 | 不动 RC，按 §8.6 借用栈追踪 |

§12.9.1.2 `Dyn` 是编译器内置类型名（非关键字），**不**写在 `base.yux`；不引入 `dyn` 关键字（沿用 `Dyn<D>` 类型名形态，与 `Rc<T>` / `Weak<T>` 一致）。

§12.9.1.3 `Dyn<D>` 与 `Dyn<D&>` 不可互转，与 `T` ↔ `T&` 同理（§8.3）。同一 `U` 对不同 draft `D1` / `D2` 有**独立** vtable，互不复用。

### §12.9.2 出现位置

§12.9.2.1 `Dyn<D>` 可出现在：函数参数 / 返回类型、局部变量类型标注（带 / 不带初值）、结构体字段类型、`Array<Dyn<D>>` 元素类型。

§12.9.2.2 `Dyn<D&>` 仅出现在 `typeWithRef` 位（函数参数 / 返回类型、`val/var ... = ...` 初值形态）；**不**进结构体字段（字段不持借用，§7.4.4）、**不**进 `Array<...>` 元素（Array 为 owned 容器）。

§12.9.2.3 上述限制由现有 `genericDef` / `genericDefWithRef` 实参槽语法自然落实，**不**需要改 g4。

### §12.9.3 静态检查

§12.9.3.1 `Dyn<X>` 中 `X` **应当**解析到 `DraftDeclNode`；否则报 `E1131`。

§12.9.3.2 **不**允许嵌套：`Dyn<Dyn<...>>`、`Rc<Dyn<...>>`、`Weak<Dyn<...>>` 报 `E1132`。

§12.9.3.3 **不**允许 `Dyn<D>?`（nullable dyn）报 `E1135`（v1 不引入）。

§12.9.3.4 draft `D` **应当对象安全**（§12.9.4），否则报 `E1134`。

§12.9.3.5 上述规则在所有声明位 `TypeNode` 上检查（free fn / impl 方法 / draft 签名 / struct 字段 / enum payload / 顶层类型别名）。

### §12.9.4 对象安全（object safety）

§12.9.4.1 draft `D` 在以下任一条件成立时**不对象安全**：

- draft 体内**任一**方法签名在 **receiver 之外**的位置出现 `Self` 类型；
- draft 体内**任一**方法签名在返回位置出现 draft 自身名（如 `draft D { fn clone() D }`）。

§12.9.4.2 v1 第一轮**不**为自反方法（`Self` / draft-name 返回）生成 thunk；用 `<T : D>` 单态化路径替代。Thunk 解锁留 v0.X+1。

§12.9.4.3 §12.3.2 已禁止 draft 方法本地泛型，§12.8 项 4 已禁止关联类型 / 关联常量；二者**自动满足**对象安全无新规则。

### §12.9.5 构造

§12.9.5.1 构造形态走 **turbofish** 类型构造：

```yux
val b Rc<U>     = U(...)
val d Dyn<D>     = Dyn:<D>(b)        ; Rc<U> → Dyn<D>，移交 RC
val r Dyn<D&>    = Dyn:<D&>(ref)     ; U& 或 Rc<U> → Dyn<D&>，借用
```

§12.9.5.2 调用站语法**应当**带 `:`（`Dyn:<D>(x)`）；无 `:` 写法 `Dyn<D>(x)` 仅在**类型位**有效（§B.2 / §B.2a）。`:` 前缀见 `yuxParser.g4` `exprCall` 形态。

§12.9.5.3 构造检查：

- `Dyn<D>(x)`：`x` **应当**为 `Rc<U>` 且 `U` 已满足 `D`（显式 `Type : D { ... }` 或 `#DraftLike` 结构化匹配，与 §12.3.1 等价规则一致）；否则报 `E1133`。
- `Dyn<D&>(x)`：`x` **应当**为 `U&` 或 `Rc<U>`，`U:D`；结果为借用形态，按 §8.6 进借用栈。

§12.9.5.4 **不**走隐式 coercion，**不**引入 `as_dyn` builtin。

> *informative*：当前实施约束 — g4 `genericDef` 实参不允许 `Type&`，故 `Dyn:<D&>(...)` 调用站语法暂不可表达；`Dyn<D&>` 仅在形参 / 返回 / declareAssign 类型位出现。该限制不影响规范正文，待实施期解锁。

### §12.9.6 方法分派

§12.9.6.1 `d.m(args)`（`d : Dyn<D>` 或 `Dyn<D&>`）：

- D 中存在 `m` → 按 D 签名做参数 arity / 类型等价检查（§12.3.1）；缺失方法报 `E6016`，arity 不匹配报 `E6012`，参数类型不匹配报 `E6015`。
- 间接调用 `(ptr receiver, P1, ..., Pn) → R`，i 为 D 中 `m` 的声明序下标 + 1（跳过槽 0 的 dtor）。
- receiver：owned `Dyn<D>` 走 `data + sizeof(RCHeader)` 跳 RC 头；借用 `Dyn<D&>` 直接以 `data` 为实例指针。

§12.9.6.2 字段访问 `d.field` 在 dyn 形态下**不**暴露（dyn 不携带具体类型布局）。

§12.9.6.3 `==` / `same_ref` 等结构化相等 v1 **不**提供（用户契约自管）。

### §12.9.7 vtable 模型（*informative*）

§12.9.7.1 每个 `(具体类型 U, draft D)` 对生成一份静态 vtable：

```
__yux_vtable_<U_mangled>__<D_qualified_mangled>:
  [0] dtor:        fn(ptr) void          ; U 的类型特定析构
  [1] D.method_0:  fn(ptr, ...) -> R     ; 按 D 声明序
  ...
  [N] D.method_N-1
```

- 槽 0：`U` 需要析构时取 `~()`；否则 `null`。
- 槽 1..N：按 D 签名走 `Mangler::method`；找不到 fn 时 emit forward declare 占位。
- 内置 `U`（`i32` / `i64` / `bool` 等）receiver ABI 为 by-value，与 Dyn 调用站统一 `(ptr,...)` 派发不可调和；vtable 槽插入 `linkonce_odr` 适配 thunk：load primitive 后转发到 SDK fn。
- vtable 符号 `linkonce_odr`、`unnamed_addr`，每编译单元各发一份。

### §12.9.8 RC / 析构

§12.9.8.1 owned `Dyn<D>` 走 `_dyn_release(data, vtable)`：strong-- → 为 0 时 dispatch `vtable[0](data + sizeof(RCHeader))` → weak-- + free。借用 `Dyn<D&>` 释放 no-op。

§12.9.8.2 构造 `Dyn:<D>(b)` 的 RC 接管：源 `b` 是 fresh 临时（构造表达式直接消费）时偷取 +1；命名变量则 retain 拷 +1，源 `Rc` 仍按自身 scope 释放。Dyn 局部变量在 scope 退出走 `_dyn_release` 抵消。

### §12.9.9 调用约定 / ABI

§12.9.9.1 `Dyn<D>` / `Dyn<D&>` 作参数按 16 字节聚合传（与 `Rc<U> + ptr` 同形）；作返回值走 sret 形态。

§12.9.9.2 `Dyn<D&>` 借用与原 `U:D` 借用按 `data_ptr` 视作同一借用根，§8.6 借用栈复用。

### §12.9.10 FFI / `extern` 边界

§12.9.10.1 `Dyn<D>` / `Dyn<D&>` **不得**跨 `extern` 边界（vtable 布局是 yux 内部 ABI，不暴露给 C），报 `E1136`。错误码在 v1 已分配；强制点留实施期补足。

### §12.9.11 不在本节范围

承 §12.8：

- `Self` / draft-name 在返回位置的对象安全解锁（thunk 路径，留 v0.X+1）
- `Dyn<D>?` nullable 形态（§12.9.3.3 / `E1135` 占位）
- `Dyn<D>` ↔ `Rc<U>` 向下转型 / 反射 / `is` / `as`（§12.8 项 7）
- 多线程 vtable 跨线程引用（v1 单线程）
- 操作符 draft 的 dyn 化（§12.8 项 6）
- vtable 内联缓存 / devirtualization（性能任务）

## Open Issues

- 操作符 draft（`Add` / `Eq` / `Index` …）的引入窗口与现有 §7.2.3 运算符重载的对齐路径。
- 是否为内置 `#DraftLike Stringify` 引入"断言失败时自动追加 actual / expected" 路径（与 §11.3.5.4 联动）。
- 用户结构体相等约束 / `Eq` draft 的最小形态（与 §11.3.5 的 `assert_eq` 用户类型扩展联动）。
- 跨编译单元的边界 IR 共享（v1 与现有 `<T>` 一致：调用方需可见函数体）。
