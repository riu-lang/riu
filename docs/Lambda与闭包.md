# Lambda 与闭包

本文档介绍 yux 中的函数类型、lambda 字面量与闭包。

> 规范层条款见 `docs/spec/03-类型系统.md` §3.11、`docs/spec/04-表达式.md` §4.11、`docs/spec/06-函数.md` §6.5.5、`docs/spec/08-所有权与引用.md` §8.7.6。闭包捕获模型的完整决议见 `docs/spec/draft/DRAFT-closure-capture.md`（v0.16 已落地）。

## 概览

yux 把"函数"视作一等值：

- **函数类型** `Function<P..., Ret>`：与 `i32` / `Rc<T>` 并列的特殊泛型。
- **lambda 字面量**：以 `=>` 标记的匿名函数表达式，求值得到一个函数值。
- **闭包**：lambda 体引用外层变量时，编译器自动收集为捕获，无需显式列表。

## 函数类型

```yux
; 变量持有函数值
#Mut let f Function<i32, i32> = (x i32) i32 => x + 1
println(f(41)) ; 42

; 类型别名（透明 alias）
Predicate = Function<String, bool>

fn first_match(arr Array<String>, p Predicate) String {
  ; ...
}
```

写法约定（详见 spec §3.11）：

- `Function` 是内置名（不是新关键字），末位类型实参永远是返回类型。
- `Function<()>` = 0 参、返回 unit；`Function<i32>` = 0 参、返回 `i32`；`Function<i32, bool>` = `(i32) -> bool`。
- 可空走标准 `?`：`Function<i32, i32>?`；返回可空是 `Function<i32, i32?>`。
- `Weak<Function<...>>` **禁**：函数值不是堆句柄。

类型相等是**结构等同**。文档用别名，不在类型位写形参名。

```yux
Function<i32, i32, i32>            ; (i32, i32) -> i32
Callback = Function<String, bool>  ; 别名即文档
```

## Lambda 字面量

四种形态（详见 spec §4.11.1）：

```yux
; 表达式形（多参，需括号）
let add = (a i32, b i32) i32 => a + b

; 单参省括号（裸 single）
let inc = (x i32) i32 => x + 1   ; 完整形
let f Function<i32, i32> = x => x + 1     ; 裸 single，类型由上下文反推

; 块形（≥ 1 参，=> 分隔参列与体）
let sum = { a i32, b i32 =>
  let s = a + b
  ret s
}

; 0 参块（无 =>；必须多行真块，单行 { expr } 不是 lambda 而是 expr-lambda 的位置）
; 体内末位无 `;` 的表达式作 tail-return；返回类型由上下文反推
let once Function<i32> = {
  42
}
```

### 形参类型推断

实参 / 字段值 / 别名右侧位置可省去形参类型：

```yux
fn op(f Function<i32, i32, i32>) i32 = f(1, 2)

; { ... } 内的 a / b 由 op 的形参类型反推为 i32
let r = op({ a, b => a + b })
```

无上下文则需显式标注：

```yux
let f = (x i32) i32 => x + 1   ; ✅ 显式
let f = x => x + 1              ; ❌ 编译错（无上下文）
```

### 返回类型规则（spec §4.11.3）

表达式形 lambda 可在 `=>` 前写显式返回类型；块形 lambda **无**返回类型标注位，始终由上下文推断。

| 形态 | 返回类型 |
|---|---|
| `x => expr` | 上下文推断 |
| `(x i32) => expr` | **默认 void**（不写就是 void） |
| `(x i32) i32 => expr` | 显式 `i32` |
| `{ a, b => body }` | 上下文推断（块形无显式返回类型位） |
| `{ body }` | 0 参块；无 `=>`；多行真块；上下文推断 |

**裸 vs 括号差异化的理由**：括号形态是"完整声明形"，不写返回类型即视作刻意 void；裸形态是"轻量推断形"，留空让上下文驱动。

### 实参 / 字段值位置必须括起来

```yux
; ✅ 多参 lambda 在 args 位置必带括号
op({ a, b => a + b })           ; 块形，类型由 op 形参反推
op((a, b) => a + b)             ; 表达式形

; 单参裸 single 仍可（apply 由用户定义，签名 Function<i32, Function<i32, i32>, i32>）
let r = apply(7, x => x * 2)
```

> Array v1 暂未提供 `map` / `filter` / `fold` 等高阶方法；上面示例用一个用户自定义 `apply` 演示单参裸 lambda 在实参位置的写法。批量遍历用 `each`（见 §尾随 lambda 调用糖）或 `for ... in arr`。

### 表达式体可含 `if` / `match`

```yux
let abs = (x i32) i32 => if x < 0 { -x } else { x }

let name = (k Kind) String => match k {
  Kind.A => "a"
  Kind.B => "b"
}
```

## 尾随 lambda 调用糖

仅块形 lambda 可作"尾随实参糖"：

```yux
fn each<T>(arr Array<T>, body Function<T, ()>) {
  ; ...
}

; 标准调用
each(arr, { x => println(x) })

; 糖：尾随 lambda 移到 (...) 之后
each(arr) { x =>
  println(x)
}

; 糖：唯一实参时省 (...)
each(arr) { x => println(x) }
```

详见 spec §4.8.4。

## 闭包

lambda 体引用了**非形参 / 非全局**的标识符，编译器自动收集为捕获，无需显式 `[move x]` 列表。

### 捕获模式（自动推断）

按外层变量类型选择语义：

| 外层类型 | 捕获语义 |
|---|---|
| 标量（`i32` / `f64` / `bool` 等） | **值复制** |
| `Rc<T>` / `Weak<T>` / `Array<T>` / `String` | **句柄复制 + retain**（共享底层对象） |
| `T&`（借用） | **借用透传** |
| `$`（方法体内） | 视作隐式 `Self&` 形参，按 `T&` 处理 |
| `Heap<T>?`（可空堆） | **B 档 move**：outer slot 写 null，env 独占所有权 |
| `Heap<T>`（非空堆） | **禁止捕获** → E4024（非空不可 move） |

```yux
fn make_adder(n i32) Function<i32, i32> {
  ret (x i32) i32 => x + n   ; 捕获标量 n（值复制）
}

fn main() {
  let add5 = make_adder(5)
  let add10 = make_adder(10)
  println(add5(1))   ; 6
  println(add10(1))  ; 11
}
```

### 捕获变量在 lambda 体内只读

```yux
fn bad() {
  #Mut let n = 0
  let f = () => n = n + 1   ; ❌ 编译错 E2030：lambda 不可对捕获变量赋值
}
```

理由：标量按值复制后赋值仅影响 captures 副本，对外层静默无效，与用户直觉冲突。要修改外层状态走"堆对象 mutator 方法"或"`T&` 形参显式传入"。

```yux
struct Counter {
  v i32

  #Static
  fn make() Counter {
    ret Self {
      .v = 0
    }
  }

  fn inc() {
    $.v = $.v + 1
  }
}

fn good() {
  #Mut let c Rc<Counter> = Counter::make()
  let f = () => c.inc()    ; ✅ 走 mutator 方法，外层状态受影响
  f()
  f()
  println(c.v) ; 2
}
```

### 含 `T&` 捕获：不可逃逸

捕获了 `T&` 的 lambda 自身按"广义 `T&`"处理 —— 不可逃逸出借用源 scope：

```yux
fn make_reader(r i32&) Function<i32> {
  ret () i32 => r              ; ❌ E4022：含 T& 捕获的 lambda 不能 ret 出去
}

fn use_reader(r i32&) {
  let read = () i32 => r       ; ✅ 在本 frame 内消费
  println(read())
}
```

### 方法体内 lambda 引用 `$`

`$` 视作隐式 `Self&` 形参，按 `T&` 行处理 —— 同样触发不可逃逸。

```yux
struct Greeter {
  msg String

  fn greet_all(names Array<String>) {
    each(names) { n =>
      println($.msg + " " + n)   ; ✅ 捕获 $，方法 frame 内消费
    }
  }
}
```

### 返回 `T&` 的允许源

lambda 返回 `T&` 时，允许源集 = lambda 自身**形参**为 `T&` 者；**捕获来的 `T&` 不进允许源集**：

```yux
let pick = (a i32&, b i32&) i32& => a   ; ✅ a 是形参 T&

fn outer(c i32&) {
  let bad = () i32& => c   ; ❌ E4020：c 是捕获，不在允许源
}
```

与 spec §8.6.10 fn 返回 `T&` 溯源规则同构。

### Heap 捕获

`Heap<T>?`（可空堆）捕获时走 **B 档 move**：outer slot 写 null，env 独占所有权，lambda 析构时释放。

```yux
fn make_handler(h Heap<Data>?) Function<()> {
  ret () => {
    ; h 被捕获，outer slot 变 null
    ; lambda 析构时释放 env 中的 Heap 句柄
  }
}
```

非空 `Heap<T>` **禁止**捕获（E4024）——非空形态不可 move，lambda 创建即"取走 outer 所有权"违反约束。改用 `Heap<T>?` 声明。

## FFI 边界

v1 显式不支持函数类型跨 FFI 边界：

```yux
extern {
  fn my_callback(cb Function<()>) ; ❌ E2031：extern fn 不接受 Function<...> 类型
}
```

含捕获 lambda 的内部布局（fat-ptr）与 C 函数指针 ABI 不兼容；零捕获 lambda 与 C ABI 互通推 v0.x+1。当前 C API 中需要 callback 的接口（如 Win32 `EnumWindowsProc`）在 v1 无法直接对接。

## 不在范围

- 泛型 lambda 字面量（`<T>(x T) => x`）：v1 不支持，推 v0.x+1。多数泛型需求由"外层泛型 fn + 内层单态 lambda" + 泛型类型别名 `Predicate<T> = Function<T, bool>` 已覆盖。
- `it` 隐式参数名。
- 函数值 `==` / 地址相等比较。
- `#Inline` / `#CallOnce` 注解：v1 不引入；上线后将解锁 lambda 内对外层 `#Mut let` 变量的赋值直通。

## 交叉引用

- [函数](函数.md)：具名 fn 声明、参数组糖、泛型函数。
- [docs/spec/03-类型系统.md §3.11](spec/03-类型系统.md)：函数类型字面量规范。
- [docs/spec/04-表达式.md §4.11](spec/04-表达式.md)：lambda 字面量规范。
- [docs/spec/08-所有权与引用.md §8.7.6](spec/08-所有权与引用.md)：闭包捕获与所有权规则。
- [docs/spec/draft/DRAFT-closure-capture.md](spec/draft/DRAFT-closure-capture.md)：闭包捕获完整决议草案（三档模式、layout、RC 协议、静态检查）。
