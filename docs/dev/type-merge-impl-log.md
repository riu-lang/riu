# 合并 `type` / `typeWithRef`（2026-09-10）

`T&` 仍不能始终持有：形参 / 局部可装，字段不行。本轮不新增寿命分析，只改产生式、E4037 位置和返回白名单。

## 语法

`yuxParser.g4` 删除 `typeWithRef`，唯一 `type` 尾部可选 `&`。`genericDef` / `genericDefWithRef` 不合并。声明头 `fn f<T&>` 不支持，sema 继续 E4037。

## 位置

- **临时位**（形参 / `let` / loop 初值 / 返回）：`T&`、`Array<T&>`、`[T& * N]` 合法；`Dyn<D&>` 仍只在临时位（E4038）。
- **持有位**（字段 / 别名 / 全局 / enum payload）：裸 `T&` / `Array<T&>` / `[T& * N]` → **E4039**。
- `Rc<T&>` / `Weak<T&>` / `Heap<T&>` / 用户 `Foo<T&>` / `f:<i32&>` 继续 **E4037**（所有位置）。
- `Function<…>` 里的 `T&` 是 owned fat-ptr，可作字段。
- 剥 `U&` 时沿用当前位：临时位的 `[Field& * N]&` 合法（反射 `Type::fields`）。

## 返回

- 实例方法：根 ∈ `{ $ , $rodata }`（`Self&`、`&$.x`）。
- 自由函数 / lambda：只允许 `$rodata`。不再把唯一 T& 形参当返回源 → `fn alias(p T&) T& = p` → E4020。
- 有 T& 形参但 `ret &G` 仍合法；≥2 个 T& 形参不再在函数头报 E4021。

下一档：lambda 套住实例字段 `T&`、自由函数 `ret &s.field`、返回 `Array<T&>` 容器、声明头 `fn f<T&>`。
