# 2 `Iter<T>` / 泛型 for-in

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-13 |
| 旧档 | 无。现 `for-in` 只 lower Array / `[T*N]`（§5.5.4）。spec 示例 `#Spec struct Iter<T> { fn next() T? }` 见 [DRAFT-spec-unify.md](../draft/DRAFT-spec-unify.md) §3.1 / [#1.R] |

前置：[15 泛型 spec](15-spec-generic.md)。本条不发明关联类型，不改 g4。

## 提议

for-in 从「只认数组」扩成两套协议；Map / Set 可直接 `for`；Array 上已落地的急切 `filter` / `map` **保持急切**，不改惰性。

riu 约束（不能按别的语言套）：

- 字段不能存 `T&`（E4039）。用户 struct 做不出「借着集合走」的迭代器对象。
- `Array` / `Map` / `Set` 是 `#NoCopy`，不能靠复制句柄共享底层缓冲。
- spec 方法不能再带自己的泛型（E1104），`Iter` 上写不出 `map<U>`。
- `T&?` 等于 `Nullable<T&>`，持有位非法。`next` 不能靠「可空借用」表示结束。
- 现 `for item in arr` 的 `item` 是 `T&`，`#Mut` 数组可写穿（§5.5.4.3）。走 `next() T?` 会丢掉写穿。

因此 for-in **两条 lowering，不混 item 档**：

| expr（剥一层 `T&`） | 协议 | `item` | 集合 |
|---|---|---|---|
| `Array<T>` / `[T*N]` | 现状索引 | `T&` | 不消费 |
| `#Impl(Indexed<T>)` | `len` + `at` 索引 | `T&` | 不消费 |
| `#Impl(Iter<T>)` | `next` 直到 `null` | `T`（值） | 推进游标；`#NoCopy` 则 move 进循环 |

Ambiguous（同一类型既 Indexed 又 Iter）：Indexed 优先，与数组行为对齐。

## 评估

- 赞成：
  - Map/Set 用 Indexed 直接 `for`，不 clone `keys()`，也不让编译器去认 `_keys` 私有布局。
  - Iter 留给计数器 / 生成器 / 消费型适配器，状态在自身字段。
  - Array 特判保留，写穿和长度拍照不变。
  - 急切 `filter`/`map` 已有测试与 §9.2.3.5；惰性要适配器类型 + `#NoCopy` 传染 + 绕开 E1104，本条不做。
- 反对：
  - 两套 spec，用户要分清 `T&` 与 `T`。
  - 依赖 #15；#15 未落地时本条不能实施。
- 未决：
  - String：共享 `Rc` 缓冲，Indexed 给出 `u32&` 会写穿字面量。倾向本条不做 `for c in s`；需要时另加 `chars()` 返回 `Iter<u32>`（`String` 可复制，适配器可持有 `String`）。
  - Map 值 / 键值对：本条 `for k in map` 只走键 `K&`。`values()` 仍克隆。`for (k, v) in` 要改 g4，**本条不做**。
  - 区间语法 `0..n`：改 g4，**本条不做**。SDK 可另加 `Range` 实现 `Iter<i32>`（后切片）。
  - 可失败 `next`（`T? ! E`）：依赖 #1 / #3，**本条不做**。结束只靠 `null`。

## 决定

- 日期：
- 结论：实施 / 关闭
- 理由：

## 规范要点

待 #15 落地且本条待实施后回写 §5.5.4 / §9 / §10.4.1.5 / §12.7 / 附录 C/D。

### spec（SDK `base.ut`）

```riu
#Spec
struct Indexed<T> {
  fn len() usize
  fn at(i usize) T&
}

#Spec
struct Iter<T> {
  fn next() T?
}
```

- 只有签名。不在 spec 上放 `filter` / `map` / `collect`（E1104；collect 可后切片做自由函数）。
- `at` 不叫 `get`：Map 已有 `get(key K&) V?`。
- `next` 写 `$` 的游标字段；耗尽返回 `null`。返回值是 `T`，不是 `T&`。
- 关联类型 / `IntoIter`：**不做**。能 `for` 的要么是特判容器，要么 expr 自己 `#Impl` 了上面某一个。

### for-in 语义（扩 §5.5.4）

形态不变：`(ID ':')? 'for' ID 'in' expr statementBlock`。仍不是表达式；仍无 item 类型标注、无元组解构。

求值：`expr` 入口求一次，寿命覆盖整段循环。剥一层 `T&` 后按序：

1. `Array<T>` / `[T*N]`：现状。长度拍照。`item` = 元素 `T&`。
2. 否则若 `#Impl(Indexed<U>)`：隐藏索引 `loop`，`item` = `at(i)` 的 `U&`。长度入口拍照（调一次 `len`）。body 里改集合长度不改变本轮次数；realloc 后旧 `T&` 失效仍属用户错误。
3. 否则若 `#Impl(Iter<U>)`：隐藏 `#Mut` 槽里推进 `next`。`item` 类型 `U`（从 `U?` 取出）。
   - 可寻址左值：就地调 `next`（写字段要求原绑定 `#Mut`，§7.5.1.1）。
   - 临时值：物化到槽。
   - `#NoCopy` 左值：move 进槽（E4031 的出口与 `move` 一致）。
4. 否则 **E3160**（文案扩到 Indexed / Iter）。

`break` / `continue` / label / 析构序与现状相同。Iter 路径：`continue` 保留迭代器槽，拆本轮 `item`。

同一类型两套都实现：走 Indexed。

### SDK

- `Array` / `[T*N]`：**不** `#Impl(Iter<T>)`（否则 `item` 从 `T&` 变成 `T`）。Indexed 不必写，编译器特判已经覆盖；若为统一要写，`at` 即现有 `get`。
- `Map<K, V>`：`#Impl(Indexed<K>)`。`len` 转发；`at(i)` = `$._keys[i]`（根 `$`，§8.6.10）。`for k in map` 得 `K&`，不 clone。
- `Set<T>`：`#Impl(Indexed<T>)`，转发到内嵌 Map。
- 用户例子（#15 之后）：

```riu
#Impl(Iter<i32>)
struct Counter {
  #Mut
  i i32
  end i32

  fn next() i32? {
    if $.i >= $.end {
      ret null
    }
    #Mut let v i32 = $.i
    $.i += 1
    v
  }
}
```

### 明确不做

- 惰性 `filter` / `map` 适配器；Array 急切方法不动（§9.2.3.5）。
- `for (a, b) in`、`0..n`、`while`、C 风格 `for(;;)`（后两项已决议）。
- `Dyn<Iter<T>>` 作为 for-in 目标。
- 编译器按字段名特判 Map 布局。

## 落地

- （未开始。须 #15 已落地。建议切片：Indexed + Map/Set for-in → Iter + 用户类型 + E3160 → 可选 Range / `collect`。）
