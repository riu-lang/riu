# 18 Drop spec / 默认无析构

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-19 |
| 旧档 | 无。现行析构：§7.4 / §6.1.2 `fn ~()`；平凡判定 §7.4.2；字段级 release `compiler_destructor.cpp`。相关：[16 分离 impl](16-impl-split.md)（形态）；§12.10 默认体 / `@Spec`；§11.13 `#NoCopy`（**对照**：Copy 传染、Drop **不**传染）；§12.8 项 15 |

不绑版本。改 `riu.bnf`（删 `fnClean`）须本条到 `待实施` 后再停下来拍板。不引入 Rust 的 `Drop`/`Copy` 互斥、`ManuallyDrop`、`forget`。不把 `Drop.drop` 做成静态 / 自由函数。

## 提议

把「要不要析构」从编译器硬编码的平凡 / 非平凡表，收成语言里的 **`Drop` spec**。`Drop` **只是契约**：给人和编译器看「这类型自己有一份 `drop`」；`fn drop()` **不进用户可调方法集**，只能由编译器发出。

类型默认无析构、**`Drop` 不传染**。含 `String` / `Rc` 的用户 struct **不必** `#Impl(Drop)`。离开时编译器做**字段级**自动 drop：碰到 `Drop` 就调该实例的 `drop`，否则往字段里走——`a.rc.drop()`、`a.b.str.drop()`。只有自己有额外清理（关句柄等）才显式 `#Impl(Drop)`。

```riu
#Spec
struct Drop {
  fn drop() { }      ; 空默认体，走 §12.10 fall-through；IR 在 codegen 填
}

; 不必 #Impl(Drop)：离开时编译器发 a.count.drop()
struct Holder {
  count Rc<i32>
}

struct Outer {
  inner Holder
  name String
}
; 离开 outer → outer.inner.count.drop()、outer.name.drop()

; 空 #Impl(Drop)：fall-through 空默认体，外层看 OpaqueBox 是一次 drop，不再拆 count
#Impl(Drop)
struct OpaqueBox {
  count Rc<i32>
}

#Impl(Drop)
struct A {
  count Rc<i32>

  fn drop() {
    println("gone")  ; 此处 $ 仍有效
    $.drop@Drop()    ; 必须：顶层、恰好一次；此后 $ 已 move
  }
}
```

`#NoCopy` 仍然传染（漏标 double-free，E4032）。`Drop` 漏标只是少一次自定义体，字段里的 `Rc` 仍会被字段级 drop 掉，所以不强制承认。

### 字段级自动 drop（不传染）

编译器在寿命终点对一个**值** `v : T` 做：

1. `T` 实现了 `Drop`（根类型或 `#Impl(Drop)`）→ 调 `T.drop`（覆盖体或 fall-through 默认体）。**到此为止**，不再从外层拆 `T` 的字段（避免 double-drop）。
2. 否则 → 按字段声明**逆序**，对每个字段递归本规则。非 `Drop` 的中间 struct 是透明的：`a.b.str.drop()`。
3. 标量 / `T&` / `Ptr` / 无 `Drop` 可达字段 → no-op。

「需要 drop 胶水」≠「实现了 `Drop`」。`Holder` 需要胶水，但不是 `Drop`；作用域帧对需要胶水的槽发 IR，只有真正的 `Drop` 实例才走 `drop()`。

`drop@Drop` 就是对 `$` 跑第 2 步（再 `move($)`）。空 `#Impl(Drop)`（不写 `fn drop()`）走既有默认体 fall-through，该类型对外是一次 `drop`，外层不再拆字段。

enum / `T?` / `[T * N]`：编译器按 payload / 内层走同一规则（tag dispatch、`_has` 守门），它们不因此变成用户 `#Impl(Drop)`。

### 默认体：空 `{}`，走现有 fall-through；胶水在 codegen 填

`base.ut` 写空块默认体，**不**写 `= $.drop@Drop()`（会按 §12.10.8 递归）：

```riu
#Spec
struct Drop {
  fn drop() { }
}
```

sema / `#Impl` 穷尽性 / fall-through / `$.drop@Drop()` 一律走既有 §12.10 流程（空 `#Impl(Drop)` 也走这条）。字段级 IR **不**在 AST 里展开，和泛型一样推迟到 codegen：编该类型的 `drop` / `drop@Drop` 时按单态后的字段生成行走（现行 `callFieldDestructor`）。无 `Drop` 可达字段则为空操作，仍 `move($)`。

**不采用** 静态 / 自由函数 `drop(obj T&)`：spec 禁 `#Static fn`；覆盖体需要 `$`；零参 `drop` 本就不进用户可调方法集。

### `$.drop@Drop()` 是线性语句

只允许出现在该类型自己的 `fn drop()` 块体里，且：

- **顶层直接语句**，不得包在 `if` / `match` / `loop` / `try` / 嵌套块 / 表达式里。
- **恰好一次**，不能遗漏。
- 其前：`$` 可读写。其后：`$` 视为 `move($)`，再读写 / 再调 → E4033 同类。
- 不得从该语句之前 `ret` / 跳出。

自定义逻辑只能写在它**前面**。

```riu
#NoCopy
#Impl(Drop)
struct FileInputStream {
  _handle Ptr

  fn drop() {
    _close_best_effort($._handle)
    $._handle = null
    $.drop@Drop()          ; Ptr 非 Drop，胶水空；仍必须写，且 $ 被消费
                           ; close 可阻塞，调用flush等阻塞的；drop 本身无副作用、只释放
  }
}
```

`Ptr` 不是 `Drop`，没有字段级可走，所以必须 `#Impl(Drop)` 才能在离开时关句柄。

### 现状（调研）

编译器**已经**对平凡类型不发射析构，且对无 `fn ~()` 的非平凡 struct 做的就是字段级递归 release（`generateDefaultDestructor` / `callFieldDestructor`）。本条等于：把这套行走收成规则，**不要**再让含 `Rc` 的类型自动变成 / 必须承认 `Drop`。

| | 现行 | 问题 |
|---|---|---|
| 判定 | §7.4.2 平凡 / 非平凡（硬编码句柄名单 + 递归字段） | 复制（retain）和析构（release）绑在同一个词上 |
| 用户入口 | 特殊语法 `fn ~()` | 不是契约 |
| 默认析构 | 非平凡且无 `fn ~()` → 字段逆序 release | 和「类型是不是 Drop」没分开 |
| 覆盖 + 字段 | 先用户体，再强制追加字段 release | 不能把字段释放当成一次 `move($)` |
| 文档 | `docs/结构体.md` 写「未定义则不自动生成」 | 与 §7.4.1.4 矛盾 |

SDK 显式 `fn ~()` 很少：`FileInputStream` / `FileOutputStream`（`Ptr`）+ 测试 `SimpleDtor`。含 `String` / `Rc` 的用户 struct 今天已经靠字段级默认析构、源码无标注——字段级不传染后，这一事实保持。

`Array.drop(n usize)` 带参、走重载，与零参 `Drop.drop` **不冲突**：后者不进用户可调方法集。

§12.8 项 15：永不按字段 derive `ToJson` 等。字段级 drop 是编译器释放规则，不是 spec 默认体 derive。

`#16` 落地后 `#Impl(Drop)` 会拆成独立块；本条按现行合一 `structDecl` 写。

### 改为

1. SDK：`#Spec struct Drop { fn drop() { } }`（空默认体）。`drop@Drop` / 未覆盖的 `drop` 走 §12.10；字段级 IR 在 codegen 填。
2. **根 `Drop`**（编译器认定）：`Rc<T>`、`Weak<T>`、`Array<T>`、`String`、`StringBuilder`、`Heap<T>`、`Heap<T>?`、`fn(...)R`、`Dyn<D>`。仍走 typed release / `HeapFree` / `_dyn_release`。
3. **不传染**：用户 struct 含 `Drop` 字段不必 `#Impl(Drop)`。离开 / 覆盖 / 字段释放时按「字段级自动 drop」行走。空 `#Impl(Drop)` 保留：fall-through 空默认体，该类型对外是一次 `drop`。自定义清理再覆盖 `fn drop()`。
4. **触发点**：作用域结束、赋值覆盖旧值、作为字段被外层胶水走到、`ret` 前非返回槽、临时寿命终点、`Dyn` vtable[0]。槽要不要发 IR 看「需要胶水」；调不调 `drop()` 看「是不是 Drop」。
5. **`Drop.drop` 仅编译器可调**。不进反射 `methods`；`o.drop` 不得当成方法值得到 `Function<()>`。用户写 `x.drop()` / `$.drop()` / `drop(x)` / 取 `Function` 一律诊断。覆盖体里只能写顶层 `$.drop@Drop()`。`Array.drop(n)` 不受影响。
6. `fn drop()` 替换 `fn ~()`（改 bnf，待实施时执行）。`$` 仍是 `Self&`，不受 `#Val` / `#Frozen` / `#Const`，不得 `! E`。
7. 「平凡 / 非平凡」不再当析构开关。复制 / `#NoCopy` / `copy_of` 不动。`#Impl(Drop)` 不隐含 `#NoCopy`。

## 评估

- 赞成：
  - 和今天 `callFieldDestructor` 一致：含 `String` 的 struct 不用标，不破坏现有代码。
  - `#Impl(Drop)` 只出现在真正有自定义清理的类型上，契约短、可读。
  - 与 `#NoCopy` 分工清楚：Copy 漏标会 double-free 所以传染；Drop 漏标仍有字段级释放，所以不传染。
  - `drop@Drop` 线性语句把自定义体和字段释放收成一次 move。
  - 零参 `drop` 仅编译器调：`Array.drop(n)` 不必改名。
- 反对：
  - 线性顶层语句不能按分支 drop、不能 `ret` 跳过。这是刻意的。
  - 「需要胶水」和「是 Drop」两套判定，codegen / Dyn 槽 0 都要认（今天 `typeNeedsDestructor` 已接近前者）。
- 未决：无

已收：字段级自动 drop、**Drop 不传染**；空 `#Impl(Drop)` 保留（fall-through 空默认体）；`drop` 不进反射、`o.drop` 不得得到 `Function<()>`；默认空 `{}` 走 §12.10，字段 IR 在 codegen 填（像泛型）；覆盖体必须顶层恰好一次 `$.drop@Drop()`；不采用静态 `drop(obj T&)`；零参仅编译器可调；`Drop` 仅契约；不隐含 `#NoCopy`。

实施时源码加 `TODO:`：`drop` **不得有副作用**（阻塞、跨线程、异步等，细则后定）。IO 类走 `close`（允许阻塞）+ `drop` 只释放资源。避免释放阻塞，业务代码看不出问题在哪里

## 决定

- 日期：
- 结论：实施 / 关闭
- 理由：

## 规范要点

待实施后回写 §6.1.2 / §7.4 / §8.5 / §12.7 / §12.8 项 15（字段级 drop 不是 derive）/ §12.9.7 vtable[0]（认「需要胶水」）/ §12.10.8 / 附录 A/B（删 `fnClean`）/ 附录 C / 附录 D。不写 Drop 传染诊断。

- 语法：`#Spec struct Drop { fn drop() { } }`（空默认体）。空 `#Impl(Drop)` 合法（fall-through）。有自定义清理再覆盖 `fn drop()`。删 `fnClean` / `fn ~()`。`$.drop@Drop()` 走既有 `@Spec` 调用 +「仅 `fn drop` 顶层」checker。
- `Drop` 仅契约，**不传染**。零参 `drop` **不进**用户可调方法集、**不进**反射 `methods`；`o.drop` 不得得到 `Function<()>`。`Array.drop(n)` 照常重载。
- 字段级 drop：对值 `v:T`，是 `Drop` 则调 `T.drop`，否则逆序递归字段。默认体 / `drop@Drop` 的 IR 在 codegen 按具体类型生成（像泛型实例化）。
- `fn drop()` 覆盖体：`$.drop@Drop()` 顶层、恰好一次、不可包、不可漏；其后 `$` 已 move。
- 根类型由编译器认定。enum / `T?` / `[T*N]` 走同一行走，不要求用户 `#Impl`。
- 调用：仅编译器发出 `drop`。用户写 `x.drop()` / `$.drop()` / `drop(x)` / 取方法值诊断。
- `drop` 不得 `! E`、不受 const-mut；`drop@Drop` 之前字段写放行。
- `drop` 无副作用（阻塞 / 跨线程 / 异步等，细则后定）。IO：`close` 可阻塞，`drop` 只释放。实施时 `TODO:`。
- 复制 / `#NoCopy` / `copy_of` 不动。
- 不做：Drop 字段传染 / 强制 `#Impl`、静态 / 自由 `drop(obj T&)`、用户可调零参 `drop`、方法值 `Function<()>`、`ManuallyDrop`、`forget`、Pin、析构失败、enum 上用户 `#Impl(Drop)`、按字段 derive 其它 spec。

## 落地

（未实施。动手时：`Drop.drop` 默认体 codegen 填字段胶水；源码 `TODO:` drop 无副作用，IO 用 close+drop。）
