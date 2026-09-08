# Array 急切高阶方法实施记录

## 范围

- `Array<T>.any/all/filter` 接受 `Function<T&, bool>`。
- `Array<T>.map<U>` 接受 `Function<T&, U>`；支持显式 `map:<U>`，函数实参类型明确时支持反推 `U`。
- `any/all` 短路；`filter/map` 急切构造独立 `Array`，不引入 `Iter<T>`。

## 实现

- 内建方法表统一登记 arity、返回类型、函数形参与 lowering id。
- SemaPass 在访问 lambda 前下传 `Function<T&, ...>` 目标类型，并独立校验方法类型实参和实参类型。
- codegen 直接按函数值 `{ fn_ptr, captures }` ABI 遍历调用；`filter` 对非空输入一次预分配，命中元素沿用 Array 深拷贝规则。
- 内联调用仍遵守普通参数的 callee-clean 所有权：fresh 闭包转移，已有 `Function` 值 retain / release 配平。
- 修正 lambda `T&` 形参绑定：LLVM 指针实参直接登记为底层地址，不再额外创建指针槽。

## 验证

- `yux-check sdk/yux/src/yux/core/array.test.yux`：通过。
- `yux test --test-mod yux.core.array --threads 1`：71 passed。
- `yux-check test tests/check-cases/`：507 passed。
- `yux test`（`sdk/yux/`）：81 个 DLL 全部通过。
- `./build.ps1 test`：70 passed。
