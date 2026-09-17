# 2 `Iter<T, E>` / 泛型 for-in

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-13 |
| 旧档 | 无。现 `for-in` 只 lower Array / `[T*N]`（§5.5.4）。草案 `#Spec struct Iter<T> { fn next() T? }` 见 [DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) §3.1 / [#1.R]；**本条不用那份签名**。 |

前置：[15 泛型 spec](15-spec-generic.md)（已落地）；[3 泛型 enum](3-generic-enum.md) **简单切片**（类型参数 + 单态 + match；改 g4 待拍板）。本条不发明关联类型。

## 提议

for-in 从「只认数组」扩成两套协议；Map / Set 可直接 `for`；Array 上已落地的急切 `filter` / `map` **保持急切**，不改惰性。

Iter 走已有失败通道 `T ! E`，不用 `T?` 当结束（#15 不限制 `T` 为非 Nullable；`T = U?` 时 `next() T?` 是双层可空，与禁 `Weak<T>?` 同类）。

### 选项（E 怎么挂）

非内置 Iter：手写 `next` 与 `for-in` 都要能处理失败（try 或外层 `!`）。Array / Indexed **不**走失败通道。

| | 形态 | 结束 | 网络等业务失败 | 要 #3 | for-in |
|---|---|---|---|---|---|
| a1 | `Iter<T, E>`，`next() T ! E`，E 全放开 | 只能是 E 的某个 variant；编译器不认识 | 与结束同一条 E | 否 | 整段 `catch E`：结束 / `next` 失败 / **同型循环体失败** 混在一起 |
| a2 | `Iter<T>`，`next() T ! End`（SDK 固定 `enum End { End }`） | `End::End` | 没有 | 否 | 编译器可内吃 `End`；要业务失败不够 |
| **a3** | `Iter<T, E>`，`next() T ! IterErr<E>` | `IterErr::End` 固定 | `IterErr::Fail(E)` | 是（`IterErr<E>`） | 内层只包 `next`：`End` 自己吃；`Fail(E)` 让这条 `for` 在 **E** 上可失败 |
| b | `next() IterResult<T, E>` 值 | variant | variant | 是 | 不走 `T ! E`，和已有失败通道叠两套 |
| c | 两套 spec：`Iter<T>` = a2；`TryIter<T, E>` = a3 | 分轨 | 只 TryIter | 是（仅 TryIter） | 无失败的用户 Iter 不必 try |

**倾向 a3。** 不选 a1：lowering 若把整个 `for` 放进用户 `try`，循环体里同型 `T ! E` 会和结束撞。不选 a2：网络流这种 `next` 中途失败写不进去。不选 b：结束 / 失败已经有 `!`，再做一套值枚举是第二通道。c 留给「Counter 连 `Fail` 的类型都不想看见」；a3 用 SDK `enum End { End }` 作 E 也能写 Counter，只是签名仍带 `IterErr<End>`。

## 评估

- 赞成：
  - Map/Set 用 Indexed 直接 `for`，不 clone `keys()`，也不让编译器去认 `_keys` 私有布局。
  - Iter 留给计数器 / 生成器 / 消费型适配器，状态在自身字段。
  - Array 特判保留，写穿和长度拍照不变。
  - `T ! IterErr<E>`：空值可以是合法 item；结束与 `Fail(E)` 不共用 `null`。
  - 急切 `filter`/`map` 已有测试与 §9.2.3.5；惰性本条不做。
- 反对：
  - 两套 spec，用户要分清 `T&` 与 `T`。
  - 依赖 #3 简单切片；`enumDecl` 要加 `genericDef?`（**改 g4，待拍板**）。
  - 现 `fallibleErrType` 多处按 **名字字符串** 存（E7008 / 透传）。`! IterErr<E>` 落地必须改成完整 `TypeInfo`，否则 `IterErr<NetErr>` 与 `IterErr<ParseErr>` 分不开。
  - #15 单 Impl：同一类型对 `Iter` 只能一条 `#Impl(Iter<…>)`。
- 未决：
  - String：本条不做 `for c in s`；需要时另加 `chars()` → `Iter<u32, End>`。
  - Map 值 / 键值对：`for k in map` 只走键 `K&`。`for (k, v) in` 改 g4，**本条不做**。
  - 区间语法 `0..n`：改 g4，**本条不做**。SDK 可后加 `Range`。
  - 无业务失败的用户 Iter 是否改走 c（第二基名），避免 `IterErr<End>`。倾向先 a3、不拆基名。
  - 用户是否坚持「凡非内置 `for` 都要手写 try」（即使编译器已吃掉 `End`）。倾向：只有 `Fail(E)` 需要外层处理 E；`E = End` 且实现不 `Fail` 时不必写空 `catch End`。

## 决定

- 日期：
- 结论：实施 / 关闭
- 理由：

## 规范要点

待 #3 简单切片落地且本条待实施后回写 §5.5.4 / §9 / §10.4.1.5 / §12.7 / 附录 C/D。

### spec / SDK

```riu
enum End {
  End
}

enum IterErr<E> {
  End
  Fail(E)
}

#Spec
struct Indexed<T> {
  fn len() usize
  fn at(i usize) T&
}

#Spec
struct Iter<T, E> {
  fn next() T ! IterErr<E>
}
```

- 只有签名。不在 spec 上放 `filter` / `map` / `collect`（E1104）。
- `at` 不叫 `get`：Map 已有 `get(key K&) V?`。
- `next` 写游标。`ret v` 成功交出 `T`；`ret IterErr::End` 结束；`ret IterErr::Fail(e)` 业务失败。`T` 与 `IterErr<E>` 不得等同（E7008）。
- `E` **应当**是 enum（§6.7.1.1）。无业务失败时用 SDK `End`。
- 关联类型 / `IntoIter`：**不做**。

### for-in 语义（扩 §5.5.4）

形态不变。仍不是表达式。`expr` 入口求一次。剥一层 `T&` 后按序：

1. `Array<T>` / `[T*N]`：现状。长度拍照。`item` = `T&`。无 `!`。
2. 否则若 `#Impl(Indexed<U>)`：隐藏索引 `loop`，`item` = `at(i)` 的 `U&`。入口拍一次 `len`。无 `!`。
3. 否则若 `#Impl(Iter<U, E>)`：隐藏 `#Mut` 槽。每轮 **只对 `next` 走失败路由**（不要把整段 body 推进同一个用户 `try`）。
   - 成功：`item` 类型 `U`。
   - `IterErr::End`：离开循环（不进用户 `catch`）。
   - `IterErr::Fail(e)`：这条 `for` 语句在 **E** 上可失败。外层须 `try` / 同型 `!`（E7006），与手写 `it.next()` 同一套 §6.7.2。
   - 可寻址左值就地调（写字段要 `#Mut`，§7.5.1.1）；临时物化；`#NoCopy` 左值 move 进槽。
4. 否则 **E3160**（文案扩到 Indexed / Iter）。

`break` / `continue` / label / 析构序与现状相同。Iter：`continue` 保留迭代器槽，拆本轮 `item`。

同一类型 Indexed + Iter：走 Indexed。

手写 `it.next()`：已有 `T ! E` 规则，必须 try 或 `!`，与是否在 `for` 里无关。

### SDK / 例子

- `Array` / `[T*N]`：**不** `#Impl(Iter<…>)`。Indexed 不必写。
- `Map<K, V>`：`#Impl(Indexed<K>)`。`at(i)` = `$._keys[i]`。
- `Set<T>`：`#Impl(Indexed<T>)`。

```riu
#Impl(Iter<i32, End>)
struct Counter {
  #Mut
  i i32
  end i32

  fn next() i32 ! IterErr<End> {
    if $.i >= $.end {
      ret IterErr::End
    }
    #Mut let v i32 = $.i
    $.i += 1
    v
  }
}

#Impl(Iter<u8, NetErr>)
struct NetStream {
  fn next() u8 ! IterErr<NetErr> {
    ; eof → ret IterErr::End
    ; 网络失败 → ret IterErr::Fail(e)
    ; 否则 ret 一个 u8
  }
}
```

`#Impl(Iter<i32?, End>)` 合法：`null` 是 item。

### 明确不做

- 惰性 `filter` / `map`；`for (a, b) in`；`0..n`；`while`；C 风格 `for(;;)`。
- `Dyn<Iter<T, E>>` 作为 for-in 目标。
- 编译器按字段名特判 Map 布局。
- `next() T?` / 嵌套 Nullable 当结束。
- b：`IterResult<T, E>` 值通道。
- a1：E 全放开且编译器不拆结束。

## 落地

- （未开始。须 #3 简单切片 + `! E` 的 E 改为完整类型。建议切片：#3 → Indexed + Map/Set for-in → Iter `T ! IterErr<E>` + E3160 / 失败路由 → 可选 Range / `collect`。）
