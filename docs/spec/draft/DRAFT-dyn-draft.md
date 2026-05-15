# 草案：yux `Dyn<D>` / `Dyn<D&>`（运行时多态 draft）

状态：**已落地，见 [`docs/spec/12-draft.md`](../12-draft.md) §12.9**（2026-05-11）。本文件保留以维持决议日志追溯。
原日期：2026-05-11。
作用：把"draft 的运行时多态形态"这一组决策固化为单一规范，作为修改 `docs/spec/12-draft.md`（含 §12.8 项 1 退出"不在范围"）、附录 A/B/D、`CURRENT.md` 实施计划的依据。

涉及章节（预估）：§12.1 / §12.2 / §12.3 / §12.6（新增 / 重排）/ §12.8 / 附录 B（语法）/ 附录 D（错误码）。

---

## 1. 目标

- draft 引入**运行时多态**形态，与现有单态化 `<T : D>` 并存、互不替代
- **零隐式 boxing**：从具体类型到 `Dyn<D>` 一律显式（与 yux 整体"无隐式转换"基调一致）
- 调用分派**单次间接 + 单次取址**（vtable[i] 调用），编译期可见 vtable 槽位、无 RTTI 查表
- **对象安全（object safety）**沿用现有 draft 体限制，第一轮不引入 `Self` / draft-name 在返回位置——`fn clone() Self` 这类自反方法**不可 dyn**（E11xx）
- `Dyn<D>` / `Dyn<D&>` 是 sized 类型（16 字节 fat pointer），可作局部变量 / 形参 / 返回值 / 字段 / `Array<>` 元素
- v1 仅承诺**单线程**；多线程下 `Dyn<D>` 的 vtable 全局静态，data 端 RC 与 §8 RC 模型一致

## 2. 类型档位

| 名称 | 写法 | 语义 / 存储 | 备注 |
|---|---|---|---|
| owned dyn | `Dyn<D>` | `{ vtable: ptr, data: ptr }`，data 指向 `[RC head \| 实例]` | 与 `Rc<U>` 同源；release 走标准 RC，refcount=0 调 `vtable[0]` |
| 借用 dyn | `Dyn<D&>` | `{ vtable: ptr, data: ptr }`，data 借自栈或堆 | 不动 RC；不调 vtable[0]；按 §8.6 借用栈追踪 |

要点：

- `Dyn<D>` 与 `Rc<U>` 内存布局兼容（data ptr 都指向 `[RC head \| 实例]`）；二者差异仅在 fat pointer 多出 vtable_ptr
- `Dyn<D&>` 与 `Dyn<D>` 不可互转（owned ↔ 借用），与 `T` ↔ `T&` 同理
- 同一具体类型 `U` 对不同 draft `D1` / `D2` 有**独立** vtable，互不复用
- 不引入 `dyn` 关键字（沿用 `Dyn` 类型名）；`Dyn` 是编译器内置类型，类型表预定义，**不**写在 `base.yux`

## 3. 子特性 A — 语法形态

### 3.1 出现位置（白名单）

`Dyn<D>` 出现在：

- 函数参数 / 返回值类型
- 局部变量类型标注（带或不带初值）
- 结构体字段类型
- `Array<Dyn<D>>` 元素类型

`Dyn<D&>`（借用形态）仅在**带借用的类型位**（`typeWithRef` 产生式）出现：

- 函数参数 / 返回值类型
- 局部变量类型标注（**仅**带初值形式 `val d Dyn<D&> = ...`）
- **不**进结构体字段（字段不持借用）
- **不**进 `Array<...>` 元素（Array 是 owned 容器）

不允许嵌套 `Dyn<Dyn<...>>`（语义无意义，编译器拒绝）。

**g4 现状**：`genericDefWithRef` 实参允许内嵌 `T&`，`genericDef` 不允许；上述限制由现有 g4 自然落实，**不需要**改语法。

### 3.2 出现位置（语法层禁止）

- `Dyn<T>` 中 `T` **应当**为 draft 名（解析后命中 `DraftDeclNode`）；命中类型 / 结构体 / 不存在 → `E1131`
- `Dyn<D>` / `Dyn<D&>` 不能再被 `Rc<>` / `Weak<>` 包裹（`Rc<Dyn<D>>` 等价于 `Dyn<D>` 本身，禁止重复包装 → `E1132`）

### 3.3 绑定 / 赋值 / 初始化

构造形态沿用 `Rc` 风：

```yux
val b Rc<MyType> = MyType(...)        ; Rc<T> 隐式从值构造（yux 现行写法，不是 Rc<T>(x)）
val d Dyn<D>      = Dyn:<D>(b)         ; Rc<U>(已实现 U:D) → Dyn<D>；turbofish `:` 前缀
val r Dyn<D&>     = Dyn:<D&>(ref)      ; 取借用 fat pointer（仅当 genericDef 实参支持 `&`）
```

**调用站语法注**：yux g4 中 turbofish 形是 `e:<T>(args)`（含 `:` 前缀，见 `yuxParser.g4:499`），
不是 `e<T>(args)`。无前缀写法会被解释为 `(e<T)>(args)` 比较序列。
`Dyn<D>` 仅在**类型位**（typeNormal / typeGeneric / typeWithRef）以无 `:` 写法出现。

构造检查（编译期）：

- `Dyn<D>(x)`：`x` 类型 **应当** 为 `Rc<U>` 且 `U` 已显式实现 `D`（或 `#DraftLike` 结构匹配 `D`）；否则 `E1133`（"类型不满足 draft，无法构造 Dyn"）
- `Dyn<D&>(x)`：`x` 类型 **应当** 为 `U&` 或 `Rc<U>`，`U:D`；构造结果为借用形态，按 §8.6 进借用栈

### 3.4 配套表达式 / 操作符

| 形式 | 允许 | 结果类型 | 备注 |
|---|---|---|---|
| `d.m(args)` 当 `d : Dyn<D>` | ✅ | 按 `D::m` 签名 | vtable[i] 调用 |
| `d.m(args)` 当 `d : Dyn<D&>` | ✅ | 同上 | vtable 同 |
| `&d` 当 `d : Dyn<D>` | ✅ | `Dyn<D&>` | fat pointer 复制，不动 RC |
| `Dyn<D>(box)` | ✅ | `Dyn<D>` | 构造（§3.3） |
| 字段访问 `d.field` | ❌ | —— | dyn 形态不暴露原类型字段 |
| `dyn1 == dyn2` | ❌ | —— | v1 不提供（[#H]） |

### 3.5 静态检查规则

**唯一规则**：进入 `Dyn<D>` / `Dyn<D&>` 的类型 `U` **应当** 满足 D（§12.3.1 等价 + `#DraftLike` 结构化匹配适用），且 D **应当** 是**对象安全（object-safe）** 的（§4）。

是 O(1) 静态检查（draft 实现表已由 `DraftImplChecker` 索引；object safety 是 draft 声明的属性，构建一次缓存）。

### 3.6 调用约定 / ABI

- `Dyn<D>` / `Dyn<D&>` 作参数：按 16 字节聚合（在 x64 Windows ABI 下走两寄存器或栈，与 `Rc<U> + ptr` 同形）
- `Dyn<D>` 作返回值：sret 形态（16 字节 sret 槽），与现有非平凡返回一致
- `Dyn<D>` 作字段：内嵌 16 字节
- `Dyn<D&>` 借用语义：dyn 借用与原 `U:D` 借用按 `data_ptr` 视作同一借用根（§8.6.x 扩展）

## 4. 子特性 B — 对象安全（object safety）

draft `D` **应当对象安全**才能进入 `Dyn<D>` / `Dyn<D&>`，否则 `E1134`。

### 4.1 自动满足（无需新规则）

- draft 体内方法本地泛型已被 §11.4.2.3 / E1104 禁止 → 自动满足"无方法泛型"
- yux 无关联类型 / 关联常量（§12.8 项 4）→ 自动满足
- 操作符 draft 不引入（§12.8 项 6）→ 不存在"运算符要求 Self 参数"问题

### 4.2 v1 第一轮显式禁止

- draft 体内**任一**方法签名出现 `Self` 类型（receiver 之外）→ 该 draft 不对象安全；从具体类型构造 `Dyn<D>` 时报 `E1134`，声明 `Dyn<D>` 类型时同样报
- draft 体内**任一**方法签名出现 draft 自身名作返回类型（如 `draft D { fn clone() D }`）→ 同上禁止；当前 yux 语法层不区分"`D` 作返回 = Self"，本草案在语义层显式拒绝（[#Z]）

### 4.3 第二轮可解锁（**不在本草案范围**）

`Self` 返回 / draft-name 返回 / `Self` 形参的解锁需要为每个 `(U, D, 含 Self 方法)` 生成 thunk（sret 槽 16 字节、内部调具体 impl 得 `U`、`Rc+vtable` 包成 `Dyn<D>`），单独 Phase 引入。

## 5. 运行时 / vtable 模型

### 5.1 vtable 布局

每个 `(具体类型 U, draft D)` 生成一份**静态** vtable，全局只读：

```
vtable_U_D:
  [0]  dtor:        fn(ptr) void          ; U 的类型特定析构（释字段 RC / 调 U 的 dispose 等）
  [1]  D.method_0:  fn(<sigs of D.m0>...) ; 按 D 声明顺序
  [2]  D.method_1:  fn(<sigs of D.m1>...)
  ...
  [N]  D.method_N-1
```

- 槽 0 始终是 `dtor`，签名固定 `fn(ptr) void`；调用对象是 data_ptr 指向的实例（不含 RC head）
- 槽 1..N 是 D 的方法按**声明序**（与 `DraftDeclNode` 的 fnSig 顺序一致）
- 方法 fn ptr 签名 = receiver 视作 `Self&`（即首个隐式参数 `ptr`），其余按 D 签名；含 owned 非平凡返回时走 sret
- vtable 符号名：`__yux_vtable_<U_mangled>_<D_qualified_mangled>`，linkonce_odr，每编译单元各发一份

### 5.2 操作语义（伪代码）

```
; 构造 owned dyn
Dyn<D>(box_u):
  result.vtable = &__yux_vtable_<U>_<D>
  result.data   = box_u.data         ; 移交 RC 所有权（句柄复制 + 不 retain，因为 box_u 被消费）
  ; 单态 Rc<U> 析构在调用方：box_u 已被构造站消费

; 构造借用 dyn from U&
Dyn<D&>(u_ref):
  result.vtable = &__yux_vtable_<U>_<D>
  result.data   = u_ref              ; 借用 ptr
  ; 不动 RC；result 进借用栈

; 调用
d.m(args):
  fn_ptr = d.vtable[i]               ; i 是 D 中 m 的声明序下标 + 1（跳过 dtor 槽）
  return fn_ptr(d.data, args...)

; release owned dyn
release(d):
  if d.data == null: return
  dec_rc(d.data)
  if rc(d.data) == 0:
    d.vtable[0](d.data)              ; 跑类型特定析构
    free(d.data - sizeof(RCHeader))

; release 借用 dyn
release(Dyn<D&>): no-op
```

### 5.3 调用约定

- `Dyn<D>` 作 owned 形参：调用方按 §8 的 owned 移交语义；callee 负责 release
- `Dyn<D>` 作返回值：sret 形态，调用方持有结果，需 release
- `Dyn<D&>` 形参：借用，调用方负责保活源；callee 不 release

### 5.4 边界情况

- **同一 U 多个 D**：每对独立 vtable；切换 dyn 形态需重新 `Dyn<D2>(box)`
- **`Rc<U>` ↔ `Dyn<D>` 互转**：`Rc<U>` → `Dyn<D>` 由 `Dyn<D>(box)` 构造（消费 box，移交 RC）；`Dyn<D>` → `Rc<U>` v1 **不支持**（向下转型需 RTTI / type id，留 §12.8 反射话题）
- **空 dyn**：v1 **不引入** `Dyn<D>?`（nullable dyn）；要可空走 `Dyn<D>?` 第二轮（错误码占位 E1135）
- **循环引用**：`Dyn<D>` 内嵌 RC，沿用 §8 周期问题（用 `Weak`）；v1 **不**为 dyn 引入独立的 Weak 形态

## 6. 构造与初始化

`Dyn<D>(x)` 不引入 DAA 规则——构造表达式语义 = "把 x 的 data 接管 / 借用，写入 fat pointer 两槽"。

## 7. FFI / `extern` 边界

| 位置 | 允许 | 不允许 |
|---|---|---|
| 参数 | `Dyn<D>` / `Dyn<D&>`（按 16 字节聚合传） | 嵌套 |
| 返回值 | `Dyn<D>` sret | 同上 |

- v1 **不**允许跨 extern 边界传 `Dyn<D&>`（vtable 布局是 yux 内部 ABI，不暴露给 C）→ `E1136`
- `Dyn<D>` 跨 extern 也**不**允许（同上）

## 8. 比较 / 相等性

- `==` 不重载（v1 不引入操作符 draft）
- 不提供 `same_ref:<Dyn<D>>` 之类 builtin
- 用户需要相等性自己写方法（属于 D 的契约即可）

## 9. 不在范围

- `Self` / draft-name 在返回位置的对象安全解锁（[#Z]）
- `Dyn<D>?` nullable 形态
- `Dyn<D>` ↔ `Rc<U>` 向下转型（需 RTTI）
- 反射 / `is` / `as` 类型测试（沿用 §12.8 项 7）
- 多线程下 vtable 跨线程引用（v1 单线程）
- `Dyn<D>` 作泛型边界（沿用 `<T : D>` 单态化路径；二者并存即可）
- 操作符 draft 的 dyn 化
- vtable 内联缓存 / devirtualization 优化（编译器后期路径，性能任务）

## 10. 迁移面（粗估）

### 10.1 编译器（`src/`）

- `src/ast/node/`：新增 `DynTypeNode`（或扩 `TypeNormalNode` 一支）；构造表达式 `ExprDynCtorNode`（或复用 `ExprCallNode` 走 `Dyn<D>(...)` 形态）
- `src/ast/ast_builder.cpp`：解析 `Dyn<...>` 类型 + 构造表达式
- `include/types.h` / `src/`：`TypeInfo` 加 `isDyn()` / `isDynRef()` 谓词 + draft ref 携带
- `src/analyzer/draft_impl_checker.{h,cpp}`：新增 object safety 谓词 `draftIsObjectSafe(DraftDeclNode*)`；构造检查 `typeSatisfiesDraftForDyn(...)`
- `src/compiler/`：
  - `compiler_type.cpp`：`getLLVMType(Dyn<D>)` = `{ ptr, ptr }`
  - `compiler_expr.cpp`：`compileDynCtor` / `compileMethodCall` 分派到 vtable[i]
  - `compiler.cpp` / 新文件 `compiler_dyn_vtable.cpp`：按 `(U, D)` 对生成静态 vtable，linkonce_odr
  - `compiler_call.cpp`：方法调用 receiver 是 `Dyn<D>` / `Dyn<D&>` 时走 vtable 间接
  - dtor 槽 fn 生成：复用现有 `releaseAtPtr` 路径，把 `U` 的 per-field release 包成 `fn(ptr) void`
- `src/runtime/`：标准 RC dec_rc 已存在，复用即可；dyn release 路径在 codegen 端生成
- `src/analyzer/borrow_checker`：`Dyn<D&>` 进借用栈（按 data_ptr 视作借用根）
- `src/lsp/`：新增 `Dyn` token 高亮、补全
- 新错误码：E1131..E1136

### 10.2 SDK / runtime（`sdk/`）

- 本轮**不**为内置类型预绑 `Dyn` 形态；用户写 `Dyn<ToString>(box_i32)` 这种应自然工作（i32:ToString 已存在）
- 验证用例：构造 `Dyn<ToString>(Rc<i32>(42))` 并调 `to_string()`

### 10.3 语法（`src/yux*.g4`）

> 改语法需先确认。本节列要改什么，不动手。

- `yuxParser.g4`：`typeNormal` / `typeGeneric` 已能识别 `Dyn<X>`（`Dyn` 是普通 ID + 泛型实参）→ **语法层可能无需改动**，由语义层在 `Dyn` 名上做特殊解释
- 待确认：`Dyn<D&>` 中 `D&` 是否能进类型实参位置？当前 `typeGeneric` 实参语法是否允许 `Type&`（后缀借用）？需查
- 若不允许，需扩 `typeGeneric` 实参支持 `Type&`，或改用其它写法（如 `DynRef<D>` 单独类型名）

### 10.4 测试（`tests/`）

- `tests/cases/dyn_*.yux`（成功用例）：构造 / 调用 / Array<Dyn<D>> / Dyn<D&> 借用 / 跨函数传 / 字段
- `tests/cases/diag_dyn_*.yux`（诊断）：非 draft 名作 `Dyn<X>`、`Dyn<Dyn<...>>`、非对象安全 draft、`Rc<Dyn<...>>` 重复包装、构造类型不满足 D
- `sdk/yux/src/yux/core/dyn.test.yux`：`Dyn<ToString>` 端到端

### 10.5 规范文档（`docs/spec/`）

- `12-draft.md`：
  - §12.8 项 1 删除 / 改写为"已实现，见 §12.9"
  - 新增 §12.9 `Dyn<D>` / `Dyn<D&>`（语法 / 布局 / 对象安全 / 构造 / 调用分派 / 错误码）
- 附录 A：`Dyn` 不进关键字表（保持类型名身份）
- 附录 B：`Dyn` 类型形态条目
- 附录 D：E1131..E1136 入表
- `CHANGELOG.md` 顶部追加一条

### 10.6 用户教程（`docs/`）

- spec 落定后再写；草案期不动 `docs/*.md`

---

## 决议日志

- **[#1.A]** fat pointer 第一槽 = per-(Type, Draft) vtable_ptr（Rust 风格），不引入类型描述符 / Go interface 风格的 type id；零运行时查表与 yux"零运行时开销"基调一致。
- **[#1.B]** owned `Dyn<D>` 内部模型 = "Rc 同源"：data_ptr 指 `[RC head | 实例]`，标准 RC + vtable[0] dtor，二者职责分离。
- **[#2.A]** 不引入 `dyn` 关键字；沿用 `Dyn<D>` / `Dyn<D&>` 类型名形态，与 `Rc<T>` / `Weak<T>` 风格一致。
- **[#3.A]** 构造形态 = `Dyn<D>(x)` 类型构造，与 `Rc<T>(x)` 一致；**不**走隐式 coercion，**不**引入 `as_dyn` builtin。
- **[#4.A]** 第一轮允许 `Array<Dyn<D>>` / 字段，因为 `Dyn<D>` 是 16 字节 sized 类型，自然进入类型档位。
- **[#5.A]** owned `Dyn<D>` ↔ `Rc<U>` 关系：构造接管 RC，析构走标准 RC + vtable[0]；**不**为 dyn 引入独立 release 路径。
- **[#Z]** 对象安全：v1 第一轮**禁止** `Self` / draft-name 出现在 draft 体的返回位置；解锁需要 per-(U, D, 含 Self 方法) thunk 生成，单独 Phase。**理由**：thunk 路径非平凡，且对 `iter` / `clone` 等自反方法的需求可以先用 `<T : D>` 单态化路径满足；不阻塞 `Dyn<D>` 主线落地。
- **[#H]** v1 不提供 `Dyn<D>` 的 `==`、`same_ref` 等结构化相等；用户契约自管。

---

## 定型与归宿

草案定型后：

1. §12.8 项 1 改写；新增 §12.9 全章节；引用本草案条目编号保留追溯
2. CHANGELOG 顶部追加一条
3. 附录 B / D 同步
4. `CURRENT.md` Phase 列表从 §10 派生
5. 全部落地后归档到 `docs/dev/dyn-draft-impl-log.md`；本 DRAFT 文件可保留并在头部标注「已落地，见 §12.9」
