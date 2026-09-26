# 24 spec 方法 `T ! E`

| | |
|---|---|
| 状态 | 提议 |
| 开 | 2026-09-26 |
| 旧档 | 无。`T ! E` 已落地 [DRAFT-fallible-sig.md](../draft/DRAFT-fallible-sig.md)（§6.7）；泛型 spec subst 已落地 [#15](15-spec-generic.md)。跨类型包装 / `Result` → [#1](1-error-v2.md)，**本条不做**。 |

不绑版本。不改 `riu.bnf`。stdlib（`riu.stream` 等）不开提议、不进本条。

## 提议

`#Spec` 方法与普通方法共用 `fnHeader`，语法已允许 `fn m() T ! E` / `fn m() ! E`。§12.3.1 等价表**没有**失败通道这一维；穷尽性比对把 spec 形参 subst 进参数 / 返回，**不** subst `! E`。

后果：`#Spec struct D<E> { fn m() ! E }` 对上 `#Impl("D<IoErr>")` 的 `fn m() ! IoErr` 会 E1101（字面 `"E"` ≠ `"IoErr"`）。具体 `! IoErr` 对具体 `! IoErr` 碰巧能过，所以洞只在形参当错误类型时爆。

退出：用户能写错误类型参数化的 spec；`#Impl` 穷尽性、`<S : D<E>>` 里 `s.m()` 的失败通道、默认体 fall-through 登记，都按 subst 之后的完整类型比（与参数 / 返回同一套）。对不上仍 E1101。

## 评估

### 现状

| | 现行 |
|---|---|
| 语法 | `fnHeader` 已有 `( type ( '!' type )? \| '!' type )?`。spec body 就是 `fn`，能 parse |
| §12.1.1 伪 `fnSig` | 漏了 `! E`，与 bnf / 附录 B 不一致 |
| §12.3.1 | 表无失败通道 |
| `sigEquivalent` | 参数 / 返回 `getType().substitute(subst)`；`resolvedFallibleErr()` 字符串直接 `==` |
| 边界方法 | `typeParamBoundHasMethod` 只认方法名；失败通道靠符号上未 subst 的 `fallibleErrType` |
| 默认体 | codegen 对函数体 `applySubst` 失败类型；checker 登记符号仍抄未 subst 字符串 |
| Iter | `#2` 已定 `next()` **不是** `T ! E`，本条不翻 |

### 赞成

- 形态已在 `fnHeader` 和 #15 subst 模型里，补洞不是新语法。
- 参数 subst、失败通道不 subst：具体 enum 碰巧过、形参一律 E1101，比「明确不支持」更糟。
- 错误参数化的契约（读写 / 编解码）都卡在这一刀；语言侧收口后 stdlib 自己写，不进 spec。

### 反对

- 抄未 subst `fallibleErrType` 的不止 checker：边界调用、默认体符号登记、路径调用几处都要同一套 subst，漏一处失败通道会漂。

### 未决

（无）

## 决定

- 日期：
- 结论：
- 理由：

## 规范要点

待实施后回写 §12.1.1 / §12.3.1 / §12.10.4；附录 B 的 `fnHeader` 与 bnf 对齐（若仍漏 `! E`）。下面是拟收口的洞。

- 语法：不改 bnf。spec 方法 = 普通 `fn`（可无体，可带默认体）。回写时删 §12.1.1 那条漏后缀的 `fnSig`，改指向 `fnHeader`。
- 失败通道是签名维：有无 `! E` 必须一致；`E` 按 subst 后**完整类型**比（§6.7.1.1：`Box<String>` ≠ `Box<i32>`）。一侧有、一侧无 → 不等价。
- subst：spec 形参换成 `#Impl` / 边界实参，再比。`Self` 仍换实现者。实现：spec 侧 `fallibleErrTypeNode()->getType().substitute(subst)` 再 `fallibleErrKey`，与参数 / 返回同一套。
- 声明处 `! E` 的 `E` 可以是 spec 形参（此时还不是 enum）。单态后须是可见 enum，否则沿用 §6.7.1.1。
- 边界调用：`<S : D<A>>` 里 `s.m()` 的失败通道是 subst 后的 `A`，不是 spec 源码里的形参名。形参方法查找与符号上的 `fallibleErrType` 都要带这张表。
- 默认体 fall-through：合成属于 S 的方法时，失败通道走同一张 subst 表（与 §12.3.2.3 / §12.10.4 已写的参数 / 返回 / `Self` 一致）。
- 诊断：失败通道对不上 → **E1101**（与其它签名维同一码，不新开）。不引入「E 必须是 enum 形参」之类新边界。
- 不做：跨类型自动包装 / `Result`（#1）；把 Iter 改成 `T ! E`（#2）；`Dyn<D<T>>`（#15 已排除）；改 bnf；stdlib 条款。

## 落地
