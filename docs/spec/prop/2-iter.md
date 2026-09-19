# 2 `Iter<T, E>` / 泛型 for-in

| | |
|---|---|
| 状态 | 已落地 |
| 开 | 2026-09-13 |
| 旧档 | 无。现 `for-in` 只 lower Array / `[T*N]`（§5.5.4）。草案 `#Spec struct Iter<T> { fn next() T? }` 见 [DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) §3.1 / [#1.R]；**本条不用那份签名**。 |

前置：[15 泛型 spec](15-spec-generic.md)（已落地）；[3 泛型 enum](3-generic-enum.md) **简单切片**（已落地）。本条不发明关联类型。相关糖：[17 类型别名 `type`](17-type-alias.md)（块内短名）；**不挡**本条。

## 提议

for-in 从「只认数组」扩成两套协议；Map / Set 可直接 `for`；Array 上已落地的急切 `filter` / `map` **保持急切**，不改惰性。

Iter 的 `next()` 返回三态值枚举，**不**走 `T ! E`。结束不是失败；手写 `loop { match it.next() }` 与 `for-in` 同一条 `next`。不用 `T?` 当结束（`T = U?` 时是双层可空）。

### 选项（E 怎么挂）

非内置 Iter：手写 `next` 用 `match`。Array / Indexed **不**走失败通道，也没有 `IterItem`。

| | 形态 | 结束 | 网络等业务失败 | 要 #3 | 手写 loop | for-in |
|---|---|---|---|---|---|---|
| a1 | `next() T ! E`，E 全放开 | E 的某个 variant；编译器不认识 | 与结束同一条 E | 否 | 每轮 try | 整段 `catch E`：结束 / 失败 / **同型循环体失败** 混在一起 |
| a2 | `next() T ! End` | `End::End` | 没有 | 否 | 每轮 try | 编译器可内吃 `End` |
| a3 | `next() T ! IterErr<E>` | `IterErr::End` | `IterErr::Fail(E)` | 是 | 每轮 try（End 在失败通道） | 内层只包 `next`：吃 End，透传 Fail |
| **b** | `next() IterItem<T, E>` 值 | `IterItem::End` | `IterItem::Error(E)` | 是 | `match` 三臂，不必 try | 认三个 variant：Item 进 body；End 离开；Error 让这条 `for` 在 **E** 上可失败 |
| c | 两套 spec：无 E 的 Iter + TryIter | 分轨 | 只 TryIter | 是 | 看哪套 | 分轨 |

不选 a1：lowering 若把整个 `for` 放进用户 `try`，循环体里同型 `T ! E` 会和结束撞。不选 a2：网络流写不进去。不选 a3：结束走 `!`，手写 loop 每轮必须 try（E7006）。不选 c：不拆基名。不选「`next() T?`」：`T = U?` 双层可空。

## 评估

- 赞成：
  - Map/Set 用 Indexed 直接 `for`，不 clone `keys()`，也不让编译器去认 `_keys` 私有布局。
  - Iter 留给计数器 / 生成器 / 消费型适配器，状态在自身字段。
  - Array 特判保留，写穿和长度拍照不变。
  - `IterItem`：空值可以是合法 item；结束与 `Error(E)` 不共用 `null`；手写与 `for` 同一套值。
  - 急切 `filter`/`map` 已有测试与 §9.2.3.5；惰性本条不做。
- 反对：
  - 两套 spec，用户要分清 `T&` 与 `T`。
  - `next` 不走 `T ! E`：结束 / 迭代失败是值；`for` 遇上 `Error(e)` 才把 **E** 接到已有失败通道。这是有意的分流，不是第二套 try。
  - #15 单 Impl：同一类型对 `Iter` 只能一条 `#Impl(Iter<…>)`。
- 未决：无（2026-09-17 已收：b；不拆 c；`E = End` 的 `for` 不必空 catch）。

## 决定

- 日期：2026-09-17
- 结论：实施
- 理由：手写 `loop { match it.next() }` 与 `for-in` 同一等。结束不是业务失败，不进 `!`。不拆 `TryIter`。`E = End` 且实现不 `Error` 时 `for` 不必写空 `catch End`。

## 规范要点

已回写 §5.5.4 / §9.2.3.6 / §10.4.1.5 / §12.7.6 / 附录 C/D。

### spec / SDK

```riu
enum End {
  End
}

enum IterItem<T, E> {
  Item(T)
  End
  Error(E)
}

#Spec
struct Indexed<T> {
  fn len() usize
  fn at(i usize) T&
}

#Spec
struct Iter<T, E> {
  fn next() IterItem<T, E>
}
```

- 只有签名。不在 spec 上放 `filter` / `map` / `collect`（E1104）。
- `at` 不叫 `get`：Map 已有 `get(key K&) V?`。
- `next` 写游标，返回值枚举：`ret IterItem::Item(v)` 交出 `T`；`ret IterItem::End` 结束；`ret IterItem::Error(e)` 业务失败。`next` 本身**不是** `T ! E`（成功类型就是 `IterItem<T, E>`）。
- `E` **应当**是 enum（与 §6.7.1.1 同一档：`for` 透传 / 手写 `ret e` 要进失败通道）。无业务失败时用 SDK `End`。
- `enum End` 与 variant `IterItem::End` 不同名空间：前者是占位错误类型，后者是「没有下一项」。
- 关联类型 / `IntoIter`：**不做**。短名走 [17](17-type-alias.md) 的文件 / 块内 `type`，不是 `Iter::Item`。

### for-in 语义（扩 §5.5.4）

形态不变。仍不是表达式。`expr` 入口求一次。剥一层 `T&` 后按序：

1. `Array<T>` / `[T*N]`：现状。长度拍照。`item` = `T&`。无 `!`。
2. 否则若 `#Impl(Indexed<U>)`：隐藏索引 `loop`，`item` = `at(i)` 的 `U&`。入口拍一次 `len`。无 `!`。
3. 否则若 `#Impl(Iter<U, E>)`：隐藏 `#Mut` 槽。每轮 `match` **`next` 的返回值**（不要把 `next` 或整段 body 推进用户 `try`）。
   - `IterItem::Item(v)`：`item` 类型 `U`。
   - `IterItem::End`：离开循环（不是失败）。
   - `IterItem::Error(e)`：这条 `for` 语句在 **E** 上可失败。外层须 `try` / 同型 `!`（E7006）。
   - `E` 为 SDK `End`：这条 `for` **不可失败**（与 Array / Indexed 同），不必空 `catch End`。实现不该 `ret IterItem::Error`；若写出，按离开循环处理。
   - 可寻址左值就地调（写字段要 `#Mut`，§7.5.1.1）；临时物化；`#NoCopy` 左值 move 进槽。
4. 否则 **E3160**（文案扩到 Indexed / Iter）。

`break` / `continue` / label / 析构序与现状相同。Iter：`continue` 保留迭代器槽，拆本轮 `item`。

同一类型 Indexed + Iter：走 Indexed。

手写 `it.next()`：得到 `IterItem<U, E>`，用 `match`。要进失败通道时在 `Error` 臂 `ret e`（外层须 `! E`）。**不**因调用 `next` 而 E7006。

### SDK / 例子

- `Array` / `[T*N]`：**不** `#Impl(Iter<…>)`。Indexed 不必写。
- `Map<K, V>`：`#Impl(Indexed<K>)`。`at(i)` = `$._keys[i]`。
- `Set<T>`：`#Impl(Indexed<T>)`；`at` 转发 `$._items.at(i)`（不碰 Map 的 `_keys`，E3042）。

```riu
#Impl(Iter<i32, End>)
struct Counter {
  #Mut
  i i32
  end i32

  fn next() IterItem<i32, End> {
    if $.i >= $.end {
      ret IterItem::End
    }
    #Mut let v i32 = $.i
    $.i += 1
    IterItem::Item(v)
  }
}

#Impl(Iter<u8, NetErr>)
struct NetStream {
  fn next() IterItem<u8, NetErr> {
    ; eof → ret IterItem::End
    ; 网络失败 → ret IterItem::Error(e)
    ; 否则 ret IterItem::Item(b)
  }
}

; 手写
loop {
  match it.next() {
    IterItem::Item(v) {
      ; 用 v
    }
    IterItem::End { break; }
    IterItem::Error(e) { ret e }
  }
}
```

`#Impl(Iter<i32?, End>)` 合法：`null` 是 item。

### 明确不做

- 惰性 `filter` / `map`；`for (a, b) in`；`0..n`；`while`；C 风格 `for(;;)`。
- `Dyn<Iter<T, E>>` 作为 for-in 目标。
- 编译器按字段名特判 Map 布局。
- `next() T?` / 嵌套 Nullable 当结束。
- a1 / a3：结束走 `T ! E`。
- c：第二基名 `TryIter`。
- 关联类型。

## 落地

- 2026-09-19（`notes/0.23`）：2.1 SDK `Indexed<T>`；for-in 第二档 `#Impl(Indexed<U>)` 隐藏索引 loop，`item = U&`，入口拍 `len`。回归：`for_in_indexed_*` / `diag_for_in_not_indexed`、`tests/projects/for_in_indexed`、`control_flow.test` Indexed。
- 2026-09-19（`notes/0.23`）：2.2 `Map` `#Impl(Indexed<K>)` 加 `at`；`Set` 转发 `$._items.at(i)`；E3160 文案扩 Indexed。回归：`map.test` / `set.test` 直接 for。Iter / spec 回写未做。
- 2026-09-19（`notes/0.23`）：2.3 SDK `End` / `IterItem<T, E>` / `Iter<T, E>`；for-in 第三档 match `next`。`E = End` 无 `!`；其它 E 须 try / 同型 `!`。回归：`for_in_iter_*`、`tests/projects/for_in_iter`、`control_flow.test` Iter。spec 回写未做。
- 2026-09-19（`notes/0.23`）：2.4 三套回归；回写 §5.5.4 / §9.2.3.6 / §10.4.1.5 / §12.7.6 / 附录 C/D；提案标已落地。
