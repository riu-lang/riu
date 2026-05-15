; 草案：yux 所有权与引用模型

状态：**草案 / 决议已收齐，待固化进文档与实施**。日期：2026-04-30。
作用：把"传参 / 借用 / 堆栈分配 / 构造与析构"这一组决策固化为单一规范，作为修改 `docs/类型系统.md` `docs/函数.md` `docs/结构体.md` 与 `CURRENT.md` 实施计划的依据。

> 本草案的所有规则均已逐条决议（见末尾"决议日志"）。后续若有反复，请在日志里追加修订记录，不要直接覆盖正文。

---

## 1. 目标

- 简单（kotlin 思维），无借用检查器、无生命周期标注
- 一定的内存安全：编译期消除悬垂引用与堆→栈反向引用
- 小结构体保留栈上分配的性能
- FFI 友好（与 C 互操作零摩擦）
- 与现有 SDK / 测试 / 文档迁移成本可控

## 2. 类型档位

四档，规则各自统一：

| 档位 | 写法 | 存储 | 传参 | 备注 |
|---|---|---|---|---|
| **值类型** | `i32` / `f64` / `bool` / 用户 struct / ... | 栈 / 寄存器 | 按值复制 | 用户 struct 默认即按值复制；编译器可在 ABI 层用指针传大结构体（不可观测优化） |
| **堆句柄** | `Rc<T>` / `Weak<T>` / `Array<T>` / `String` | 堆（RC 头 + payload） | 句柄复制 + RC 调整 | 共享语义；要独立缓冲走显式 `.clone()`（`String` 不可变，无 clone） |
| **借用** | `T&` | 指向栈或堆 payload | 传指针 | 不可逃逸，仅用于函数参数 / 局部变量 / `&` 表达式结果 |
| **FFI 指针** | `Ptr` | 任意 | 裸指针 | 等价 C 的 `void*`；**无泛型**；仅 FFI 边界使用；用户层只允许 yux→Ptr 单向转换 |

要点：

- "用户结构体按值复制" **是终态**，取消现状 `compiler_call.cpp:207` 的"结构体通过指针传递"
- 堆句柄类型是封闭集合：`Rc`、`Weak`、`Array`、`String`，不允许用户自定义新的"堆句柄"
- 自引用 / 双向引用结构体**必须经堆**（`Rc<Self>?` / `Weak<Self>?`），栈上自引用直接被语法拒绝（无办法表达）

## 3. 借用 `T&`

`T&` 用语法替代了原草案的 `Ref<T>`，在 `src/yux.g4` 层就限定了出现位置。

### 3.1 出现位置（白名单）

- 函数参数类型：`fn some(a i32&)`
- 局部变量类型：`var d i32& = &c`
- `&` 表达式的结果类型

### 3.2 出现位置（语法层禁止）

- 结构体字段
- `Array<T>` 元素 / `Rc` 内层 / `Weak` 内层
- 全局变量 / `cval`
- 函数返回值类型
- 嵌套：`T& &` 直接禁

由 `yux.g4` 的产生式直接拒绝，不进入语义阶段。

### 3.3 绑定与赋值

- **rebind 禁止**：`T&` 局部变量声明处**必须初始化**，之后赋值改的是被引对象
  ```yux
  var c = 1
  var d i32& = &c   ; d 绑到 c
  d = 3             ; ✅ c 变 3
                    ; ❌ d 不能重指向别的变量
  ```
- **拷贝绑定**：从一个 `T&` 复制绑定不写 `&`：
  ```yux
  var d2 i32& = d   ; ✅ d2 与 d 绑同一目标
  ```
- **`&d` 报错**：对 `T&` 再取址结果会是 `T& &`，由语法拒绝

### 3.4 取地址表达式 `&`

`&` 只能作用于**左值**：

| 形式 | 允许 | 结果类型 | 备注 |
|---|---|---|---|
| `&局部变量` | ✅ | `T&`（变量类型为 T） | |
| `&函数参数` | ✅ | `T&` | |
| `&self.field` | ✅ | `FieldType&` | |
| `&box`（box 是 `Rc<T>` 局部变量） | ✅ | `Rc<T>&` | 借用**句柄本身**，不是 payload；详见 §4.3 |
| `&box.field` | ✅ | `FieldType&` | Rc 自动解引用后取字段，借用绑到 box 句柄存活期 |
| `&任意 RC 字段链` | ✅ | 终点字段类型的 `&` | 只要每一层都是字段访问 |
| `&arr[i]` | ❌ | —— | `[]` 调用 `get` 成员函数，结果是值；要修改用 `arr.set(i, ...)` |
| `&f()` / `&任何方法调用` | ❌ | —— | 函数返回值是右值临时 |

### 3.5 寿命检查

**唯一规则**：`T&` 变量的初始化来源（追到 `&` 的根对象 / 形参 `T&`）所在**声明作用域** 必须 ⊇ 该 `T&` 变量自身的**声明作用域**。

是 O(1) 静态检查，无数据流分析、无借用检查器。

```yux
fn foo(b Rc<Pair>&) {
  var r i32& = &b.field   ; ✅ b 是参数，作用域 ⊇ r
}

fn bar() {
  var b Rc<Pair> = ...
  var r i32& = &b.field   ; ✅ 同作用域
  if cond {
    var r2 i32& = &b.field ; ✅ b 作用域 ⊇ r2
  }
  ; ❌ var r3 i32& = if cond { &b.field } else { &c }
  ;    cond 分支里如果引入了更窄作用域的源对象，就违反规则
}
```

附加约束：在 `T&` 的存活区间内，其根对象不可被赋值 / 移交。Rc 句柄的"赋值"等价 release-旧 + retain-新，借用期内禁此操作即可（纯本地检查）。

### 3.6 调用约定

- **实参取址显式**：`fn f(a i32&) { ... }` 调用方写 `f(&x)`，**不自动加 `&`**
- **receiver 例外**：方法 receiver `$` 永远隐式 `Self&`，调用方写 `obj.method(args)`。**不引入 UFCS**（`T::method(self, args)` 风格），避免风格混乱
- LLVM 层 `T&` / receiver `$` 落成非空指针

## 4. `Rc<T>` 堆句柄

### 4.1 语义

- **持有**：拥有 RC 头 + payload `T` 的堆 block 的强引用句柄
- **不可空**：`Rc<T>` 永远指向有效对象；可空写 `Rc<T>?`
- **共享**：句柄复制 = retain；多个 Rc 句柄共享同一 block

### 4.2 访问形态（T&-风）

`Rc<T>` **没有自己的方法**——所有 `box.field` / `box.method(args)` 都自动解引用到 payload `T` 上分发：

```yux
var p Rc<Point> = ...
p.x                ; 等同 (*p).x
p.distance(q)      ; 等同 (*p).distance(q)
```

无显式 deref 语法，不需要 `*box` / `box[]`。

### 4.3 与 `T&` 的关系

- **`&box` → `Rc<T>&`**（借用 Rc **句柄本身**，不是 payload）。`Rc<T>` 是单字句柄值，`&box` 就是对该值取址，得到 `Rc<T>&`。可传给 `fn f(b Rc<T>&)` 这类接受句柄借用的函数；callee 通过自动解引用访问 payload
- **`&box.field` → `FieldType&`**（借用 payload 的某个字段）。`box.field` 自动解引用到 payload 后取字段，再 `&` 取址——结果绑到 box 句柄存活期；详见 §3.4 / §3.5
- **没有直接得到"整个 payload `T&`"的语法**。要把 Rc 的 payload 整体借给接收 `T&` 的函数，有两条路：
  1. 把函数签名改为接收 `Rc<T>&`（推荐，调用方写 `f(&box)`，零开销；callee 用自动解引用访问字段）
  2. 把函数拆成接收具体字段引用，调用方写 `f(&box.field)`
- **FFI 走 Ptr**：`ptr_of:<Rc<T>>(box)` 直接给到 payload 首地址（跳过 RC 头），见 §9.4。这是用户层唯一能拿到"裸 payload 指针"的口子，且仅 FFI 边界使用

## 5. `Weak<T>` 弱引用

### 5.1 范围（v1）

- **`Weak<T>` 仅配 `Rc<T>`**：`Weak<Array<T>>` / `Weak<String>` v1 不支持
- TODO 后续按需扩展到其他堆句柄类型

### 5.2 语义

- 持有指向某 block 的弱引用，不阻止 payload 析构
- **原生可空**：`Weak<T>` 自带"空 / 已失效"状态；`Weak<T>?` 双层可空，**语法层禁止**
- **`==` / `!=` v1 禁用**：要比对走 `same_ref`（§10）或先 `upgrade` 后比内容

### 5.3 API

| 操作 | 形式 | 说明 |
|---|---|---|
| 构造函数 | `Weak(box Rc<T>?)` | 传 `null` 得空 weak；传活 box 绑定并 retain weak 计数 |
| 内置函数 | `weak:<T>(box Rc<T>?) Weak<T>` | 编译器 baked，与构造函数等价 |
| 升级 | `upgrade(w Weak<T>) Rc<T>?` | 强引用还在则返回 Some，已释放或空 weak 返回 None |

### 5.4 字段 / 容器

允许在结构体字段、`Array` 元素、全局、返回值中出现——它是堆句柄，无 `T&` 的逃逸问题。

## 6. `Array<T>` 与 `String`

### 6.1 共同点（堆句柄类）

- 内部均为 `{ _handle: *Block }` 单一句柄
- `Block` 头部带 RC（`{ strong: u32, weak: u32 }`），见 §7
- 传值 = 句柄复制 + retain；多个变量共享同一 block
- 字段 / 容器 / 返回值 / 全局均可使用

### 6.2 `Array<T>`

- **可写**：通过 `Array<T>&` 借用允许 push / 修改元素
- **共享 + 可变**：多个句柄同视图；任一句柄修改对其他句柄可见——这是预期，不是 bug
- **realloc 安全**：因为 `_handle` 是单一指针指向 Block，Block 内部数据指针变化不影响外部句柄
- **深拷贝**：显式 `.clone()` 复制底层 Block

`Block = { rc, len, cap, data: *T }`（`data` 是指向元素数组的间接指针；扩容只换 `data`，Block 不动）

### 6.3 `String` 不可变（Java 风）

- **immutable**：所有"修改"方法（`to_upper` / `trim` / `+` / `substring` / `repeat` / ...）一律返回新 `String`
- **无 `clone()`**：不可变下复制 = 句柄共享，深拷贝无意义
- **mutable 用例**：v1 通过 `Array<u32>`（codepoint）+ `String(bytes Array<u32>)` 构造完成；TODO 后续加 `StringBuilder` 类型
- **字面量**：落 `.rodata` 段，RC 头哨兵 `0xFFFFFFFF`，retain/release 入口对比跳过（见 §7.5）

`Block = { rc, len_bytes, data[0..len] }`（不可变 → 不需要 `cap`，长度固定）

### 6.4 `==` 行为

- `Array<T> == Array<T>`：长度 + 元素逐个 `==`（递归）
- `String == String`：内容逐字节比对；可与字面量直接比

## 7. RC 与析构

### 7.1 RC 头

```
struct Block<T> {
  rc:  { strong: u32, weak: u32 }   ; 8 字节
  ...payload...
}
```

`strong` 计外部强引用数；`weak` 计 `(外部弱引用 + 1 if strong > 0)`——所有强引用整体算作一份 weak（标准 Rc 范式）。

### 7.2 retain / release 语义

- **Rc retain**：`strong++`
- **Rc release**：
  ```
  if (--strong == 0) {
    destruct payload;     ; 字段级 release（见 §7.4）
    if (--weak == 0) free(block);
  }
  ```
- **Weak 创建（绑定活 box）**：`weak++`，复制 handle
- **Weak retain**：`weak++`
- **Weak release**：`if (--weak == 0) free(block);`
- **upgrade**：`if (strong > 0) { strong++; return Rc(handle); } else null`
- **空 Weak**（handle == null）：所有计数操作 no-op

### 7.3 调用约定（callee-clean）

- 调用方在传参前 retain（计数 +1）
- 被调用方把堆句柄参数视为自己的局部变量，作用域结束时 release
- 优势：参数和局部变量走同一条析构路径，不需要"参数特例"

**返回值（move-return + 实现 i）**：编译器对 `ret box_expr` 一律生成 retain；所有分支汇聚后净效果是 +1 给调用方；callee 末尾的析构 release 自然抵消多余 retain。peephole 抵消 IR 上的 retain/release 对——TODO，先实现后优化。

### 7.4 字段级 retain / release（含 RC 字段的栈结构体）

- **平凡结构体**（递归地不含任何 `Rc` / `Weak` / `Array` / `String`，也不含含上述的子结构体）→ 按位 memcpy / 直接析构，快速路径
- **非平凡结构体** → 字段级处理：
  - 复制：逐字段处理，RC 字段 retain；嵌套非平凡子结构体递归；平凡子段 memcpy
  - 析构：构造逆序对每个 RC 字段 release，递归析构嵌套非平凡子段
- **赋值**（`s = expr`）：统一 **retain-then-release** 顺序，避免 `s = s` 或别名导致先 release 后 retain 读到已释放对象
- **字段读**：`s.field` 拿到的是字段值的"借用视角"，本身不 retain；只有当读出的值被存进新变量 / 参数时按复制语义 retain
- **字段写**（`s.field = expr`）：retain-then-release
- **Array 元素读 / 写**：与字段对称

**v1 IR 落地形态**：内联展开（与现有 `compiler_destructor.cpp` 风格一致）。TODO 后期改为派生 `__copy_S` / `__destroy_S` 独立函数以缩减代码体积。

### 7.5 字面量哨兵

`.rodata` 段的 Block：`strong = 0xFFFFFFFF`, `weak = 0xFFFFFFFF`。runtime 入口检查：

```c
static inline void __rc_inc(Block* b) { if (b->strong != UINT32_MAX) b->strong++; }
static inline void __rc_dec(Block* b) { if (b->strong == UINT32_MAX) return; ... }
```

主要服务于 `String` 字面量；`Array` / 用户 `Rc` 暂未走静态字面量路径，TODO 视需要扩展。

### 7.6 临时值寿命

- 每条**语句 / 顶层表达式 / 代码块**边界维护一个"未消费的堆句柄临时值"清单；边界处对仍在清单中的临时值统一 release
- **不消费临时**：receiver / 字段访问只是借用临时，不夺所有权；临时靠"边界 release"自然 keep-alive 直到访问完成
- **消费**（从清单移除）：作为参数传入 callee-clean 调用 / 绑定到 var / 字段 / 作为 ret 表达式 / 作为另一表达式被进一步消费
- **`T&` 不能绑定临时值**：由 §3.5 寿命规则自动拒绝（临时的"作用域"是当前语句/表达式，比任何 var 声明都小）
- **控制流分支**：每分支独立维护清单，phi 汇合后合并

### 7.7 循环引用

v1 接受泄漏。父子 / 双向引用必须用 `Weak` 打破。**不实现 cycle collector**。

### 7.8 线程模型

v1 单线程。RC 用普通 add / sub，不上原子操作。RC 头字段类型固定为 `u32`，后续切原子时不改 ABI。文档明确"多线程下行为未定义"。

## 8. 构造函数与定性赋值分析（DAA）

含 RC 字段的结构体在构造完成时若字段未初始化，析构会对 garbage 调 release，直接 UB。所以构造路径必须做编译期定性赋值分析。

### 8.1 构造形式

**两条路径并存**：

1. **结构体字面量** `Foo { f1: v1, f2: v2 }`：
   - **必须列出所有字段**（缺字段 / 多字段直接编译报错）
   - 各字段表达式按出现顺序求值，无字段间互相引用（每个 `vN` 只能引用外部变量、参数）

2. **构造函数**：用户定义的方法，receiver 是 `$` (`Self&` under construction)
   - 函数体内 `$.field = expr` 是字段写；可以多次写（重赋）
   - 函数返回 `Self`（按值），编译器在 `ret;` 处隐式按值复制 `*$`

### 8.2 DAA 规则（构造函数体）

1. **入口**：所有字段标记 *未初始化*
2. **写 `$.field = expr`**：字段转 *已初始化*；`expr` 中允许引用其他**已初始化**字段或外部变量、参数
3. **读 `$.field`**（在 RHS、作为方法 receiver、作为 `&$.field`、作为另一字段写的依赖）：编译期检查 → 必须 *已初始化*，否则报错
4. **分支汇合**：字段在所有分支里都被赋值后，汇合点视为 *已初始化*；任一分支未赋视为 *未初始化*
5. **循环**：保守起见，循环体内的字段写入只让"循环结束后"视为 *已初始化*（同 if 全分支规则）；循环内读则要求该字段在进循环前已 *已初始化*
6. **函数出口**（`ret;` 或函数末尾）：所有字段必须 *已初始化*，否则报错
7. **重复赋值同一字段**：允许；首次写标记 first-init（直接 store，不 release 旧值，因为是 garbage），后续写按 §7.4 retain-then-release 标记 reassign

### 8.3 `$` 不可逃逸

构造函数中 `$` 是 `Self&`，受 §3.2 / §3.5 约束：

- `ret $;` —— 返回引用，禁
- `$` 不能作为 `T&` 字段被存（已被语法层禁——结构体无 `T&` 字段）
- 把 `$` 传入接收 `Self&` 的辅助函数：允许，但被调用方仍受 DAA 约束（保守：辅助函数视为读所有字段，要求全部 *已初始化*；TODO 是否做更细粒度分析）

### 8.4 与无隐式默认值的衔接

即使是 `Weak<T>` / `Nullable<T>?` / `i32` / `Array<T>`，构造时都必须显式赋值。"自然空值"（`Weak(null)` / `null` / `0` / `Array<T>()`）由用户写出，不偷偷给。

## 9. FFI 与 `Ptr`

### 9.1 `Ptr` 形态

- 等价 C 的 `void*`
- **无泛型**（写法就是 `Ptr`，不是 `Ptr<T>`）
- 仅 FFI 边界使用；用户代码风格不鼓励
- **Ptr 算术**（偏移 / 加减）**仅 SDK 内可用**——v1 用注解（候选 `@sdk_only`）或路径白名单识别；普通用户代码出现 Ptr 算术编译报错。TODO 落实标记机制

### 9.2 `extern fn` 签名允许的类型

| 位置 | 允许 | 不允许 |
|---|---|---|
| 参数 | 内置标量、`Ptr` | `T&` / `Rc<T>` / `Array<T>` / `String` / `Weak<T>` / 用户 struct |
| 返回值 | 同上 + `void` | 同上 |

理由：`T&` 是 yux 内部寿命概念；RC 头是 yux 内部约定，C 端无法正确管理。

**v1 暂不强制约束**——`extern` 在 v1 是危险但开放的接口，便于 SDK 落地。TODO 后期参照 Rust `unsafe` 收紧。

### 9.3 自动转换（仅 extern 边界）

`T&` / `Rc<T>` / `Weak<T>` / `Array<T>` / `String` 作实参传给 `Ptr` 形参时**自动转换**（隐式仅此一处例外，文档明确说明）：

```yux
extern fn memcpy(dst Ptr, src Ptr, n i64) Ptr
fn copy_buf(a Array<u8>&, b Array<u8>&, n i64) {
  memcpy(a, b, n)   ; ✅ 自动转 Ptr
}
```

### 9.4 显式转 Ptr：`ptr_of:<T>(obj)`

编译器 baked 内置函数（不进 SDK 代码）：

| 源类型 T | 结果 | 说明 |
|---|---|---|
| `U&` | 指向 U 的 Ptr | 短生命周期，C 端不可保存超出本次调用 |
| `Rc<U>` | 指向 U payload | **跳过 RC 头**；C 端只能读写 payload，不能 free |
| `Array<U>` | 指向元素首地址 | 同上 |
| `String` | 指向 UTF-32 字节首地址 | 不带 NUL 终止；要 NUL 终止走未来 `to_cstr()` |
| 内置标量 | **不允许** | `i32 → Ptr` 没意义 |

turbofish 与 §10 `same_ref` 同形。

### 9.5 文档警告

- `box.payload` 经 Ptr 流出后，C 端**不可保存超出本次调用**——yux 这边 release 时 payload 会析构
- `extern fn` 不做 mangle，保留 C ABI 名

## 10. 比较

- **`==` 内容相等**：覆盖所有类型
  - 内置标量：值相等
  - 用户 struct：字段递归 `==`（编译器派生）
  - `Rc<T>`：解引用后比 payload 内容
  - `Array<T>`：长度 + 元素逐个 `==`
  - `String`：内容逐字节
  - `Weak<T>`：v1 禁用 `==` / `!=`
- **地址相等**：内置 `same_ref:<T>(a, b) bool`，编译器 baked
  - 接受 `Rc<T>` / `Weak<T>` / `T&` / `Array<T>` / `String`
  - 比对底层 handle 指针
  - 空 Weak 与任何活句柄不等

## 11. 不在范围

- 借用检查器、生命周期标注
- 默认移动语义（已否决）
- 跟踪 GC / Arena 分配器
- 多线程 / 闭包
- cycle collector
- 字段级默认值语法（`field T = expr` 在 struct 定义处给默认值）

## 12. 迁移面（粗估）

### 12.1 编译器

- `src/compiler_call.cpp:207` 周边：取消"结构体通过指针传递"，按 §2 分发（值 / 堆句柄 / `T&`）
- `src/compiler_*.cpp` 加：
  - `T&` 来源 / 寿命检查（§3.5）
  - 字段级 retain / release 派生（§7.4）
  - 临时值清单（§7.6）
  - move-return 的 retain 注入（§7.3）
  - 构造函数 DAA（§8.2）
  - `ptr_of` / `same_ref` builtin（§9.4 / §10）
  - extern 边界自动转 Ptr（§9.3）
- `src/compiler_destructor.cpp`：扩展为字段级 RC retain/release 派生
- 字面量哨兵的 runtime 入口（§7.5）
- `Array<T>` / `String` layout 重写为 `{ _handle: *Block }`（§6.1 / §6.2 / §6.3）
- `String` 改 immutable，移除 `clone`，所有"修改"方法改返回新 String
- `yux.g4`：
  - `T&` 出现位置白名单（§3.1 / §3.2）
  - `T& &` 嵌套禁
  - `&` 仅作用于左值（§3.4）
  - `Weak<T>?` 禁

### 12.2 SDK / runtime

- `sdk/yux/src/yux/core/base.yux`：`Array<T>` / `String` / `Rc<T>` / `Weak<T>` 语义和方法签名重写
- `Weak(box Rc<T>?)` 构造、`upgrade` builtin、`weak` builtin
- 字面量哨兵相关 `__rc_inc` / `__rc_dec` 入口

### 12.3 测试

- `tests/cases/array_passing_copy.*` 重写为 `array_passing_share.*`：覆盖共享 / clone / 借用三场景
- `tests/cases/string_methods.*` 改为 immutable 语义
- 新增覆盖：
  - `T&` 寿命检查（合法 / 非法用例）
  - 字段级 RC（含 `Rc` 字段的 struct 复制 / 析构）
  - Rc 共享（多句柄看到同一对象）
  - Weak / upgrade / 循环引用打破
  - 构造函数 DAA（缺字段、读未初始化字段、分支汇合）
  - `same_ref` / `ptr_of`
  - extern 边界自动转 Ptr

### 12.4 文档（中文，`docs/`）

- `docs/类型系统.md`：四档体系；`T&` / `Rc<T>` / `Weak<T>` / `Array<T>` / `String` / `Ptr` 各自语义重写
- `docs/函数.md`：调用约定（callee-clean、move-return、receiver 隐式 / 实参显式 `&`、临时值寿命）
- `docs/结构体.md`：构造（字面量 + 构造函数）、字段级 RC 派生、DAA、自引用
- `docs/内置类型.md`：`Array` / `String` 方法表（immutable String）

---

## 决议日志

按讨论顺序追加，标 `[#编号]`。每次反复或修订也追加新条目，不要覆盖。

- **[#1.A]** 类型档位锁定四档：值（含用户 struct，默认按值复制）/ `Rc<T>` / `T&` / `Ptr`（去泛型，等价 `void*`，仅 FFI 单向）
- **[#1.B]** 引用语法定为 `T&`（取代 `Ref<T>`）；出现位置白名单仅函数参数 / 局部变量 / `&` 表达式结果；其余语法层禁
- **[#1.C]** `T&` rebind 禁；声明处必须初始化；`d = 3` 改值；`var d2 T& = d` 拷绑定；`&d`（d 是 T&）禁
- **[#1.D]** `T&` 不能作返回值（D1）
- **[#1.E]** `&` 只作用于左值；`&arr[i]` / `&f()` 禁；`&局部 / &参数 / &self.field / &box.field` ✅
- **[#1.F]** `T&` 寿命：声明作用域 ⊇ 来源声明作用域；借用期内根对象不可重赋值
- **[#1.G]** receiver 永远隐式 `Self&`；不引入 UFCS（`T::fn(self, ...)` 风），统一 `obj.method(args)`
- **[#1.H]** `==` 改内容相等（覆盖原 D7"引用相等"建议）
- **[#1.I]** 自引用结构体必须经堆 `Rc<Self>?` / `Weak<Self>?`
- **[#1.J]** Rc 无自有方法，`box.field` / `box.method()` 一律解引用到 T 上分发
- **[#2.1]** RC 头 `{ strong: u32, weak: u32 }` 8 字节
- **[#2.2.a']** Weak 原生可空，`Weak<T>?` 禁；构造函数 `Weak(box Rc<T>?)`，builtin `weak:<T>(box)`；空 weak 写 `Weak(null)` / `weak(null)`
- **[#2.2.b]** Weak `==` / `!=` v1 禁用
- **[#2.3.A]** callee-clean：调用方传参前 retain，被调用方按局部变量析构 release
- **[#2.3.B]** move-return + 实现 i：编译器对 `ret box_expr` 一律生成 retain，让 callee 末尾析构 release 抵消；TODO peephole
- **[#2.3.C]** 字段级 retain/release：平凡 memcpy / 非平凡字段级；赋值 retain-then-release；v1 内联，TODO 派生独立函数
- **[#2.3.D]** 临时值寿命到语句 / 顶层表达式 / 代码块边界；不消费临时（receiver / 字段访问只借用）
- **[#2.3.E.1]** 标准 Rc 双计数（强引用合算一份 weak）；strong=0 析构 payload，可能保留 block 直到 weak=0
- **[#2.3.E.2]** Weak v1 仅配 Rc；Array / String 的 weak 不暴露但 layout 保留 weak 字段；TODO
- **[#2.3.E.3]** 字面量哨兵 `0xFFFFFFFF`，retain / release 入口对比跳过
- **[#3.A]** `Array<T>` / `String` 归堆句柄类，与 `Rc` / `Weak` 同规则
- **[#3.B]** API 约定：只读用 `T&`，持有按值，不为 Array / String 设特殊传参规则
- **[#3.D]** `String` 改不可变，无 `clone`；mutable 走未来 `StringBuilder`（基于 `Array<u32>`）
- **[#3.E]** 字面量常量段 + RC 哨兵；release / retain 跳过
- **[#A.1]** extern fn args / return 仅允许内置标量 / `Ptr`（v1 不强制约束 LLVM 落地，TODO 后期 Rust unsafe 风收紧）
- **[#A.2]** extern 边界 `T&` / 堆对象自动转 `Ptr`（隐式仅此例外）
- **[#A.3]** 显式转 Ptr 用 builtin `ptr_of:<T>(obj)`，编译器 baked，不进 SDK
- **[#A.4]** 堆对象 → Ptr 指向 payload，跳过 RC 头
- **[#A.5]** Ptr 算术仅 SDK 内可用；TODO 标记机制
- **[#B]** 地址相等用 builtin `same_ref:<T>(a, b)`，编译器 baked，无新操作符
- **[#C]** 自引用构造不加糖；`Weak<T>` 字段必须显式 `Weak(null)`
- **[#8]** 构造函数 DAA：`$` 是 `Self&`；首次写 / 重赋区分；分支汇合规则；出口全字段已初始化检查；`$` 不可逃逸
- **[#4.3 修订]** `&box` 是 `Rc<T>&`（句柄借用），不是 `T&`；没有"借用整个 payload 为 T&"的直写语法；要传 payload 给 T& 形参的函数，要么改签名为 `Rc<T>&`，要么走 `&box.field` 拆字段；裸 payload 指针仅 FFI 通过 `ptr_of:<Rc<T>>(box)` 获取
