# 函数类型 `Function<P..., Ret>` 实施日志

2026-08-25：函数类型从紧凑字面量 `fn(T)R` / `fn?(T)R` 改为内置特殊泛型 `Function<P..., Ret>`。

- 规范：`docs/spec/03-类型系统.md` §3.11、附录 B.2b、CHANGELOG 2026-08-25
- 语法：`yux/ast/yuxParser.g4` 删除 `typeFn` / `fnTypeParams`；`Function<...>` 走 `typeGeneric`
- 教程：`docs/Lambda与闭包.md`

## 决议

1. 旧 `fn(...)` 类型字面量立刻从 g4 删除。函数声明 `fn foo(...)` 不动。
2. 末位类型实参永远是返回类型；`Function<()>` = 0 参 unit。
3. 可空 `Function<...>?` 走标准 `?` 后缀，运行时仍是 16 字节 fat-ptr（`fn_ptr == null`），不套 `Nullable` 外壳。
4. `genericDef` 实参不允许 `T&` 本次不改；含 `T&` 形参的 Function 只出现在 `typeWithRef` 位。

## 关键代码

- `TypeInfo(FnTag, ...)` / `TypeInfo("Function", args)`：末位拆为返回类型；`getFullName` / `getMangleName` 输出 `Function<P...,Ret>` / `Function<...>?`
- `TypeInfo("Nullable", {Fn})`：折叠为 `fnNullable` fat-ptr，避免 `{has, value}` 布局
- `ASTBuilder::makeFunctionType` / `applyNullableSuffix`
- `createCast`：透明别名 / 相同 LLVM 类型 no-op（`IntUnOp = Function<i32, i32>` 赋值）
