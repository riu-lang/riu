# extern CName / 跨模块重复声明

日期：2026-09-06。v0.21 收口。

## 行为

- yux 层：`extern fn` 与普通 `fn` 一样按模块分区（`yux.core.GetStdHandle` / `mymod.GetStdHandle`）。
- LLVM：只声明 C ABI 名（`#CName` 或声明名）；同链接名同签名合并，不 mangle。
- 无限定名：本模块声明优先于 `use` / SDK 注入的同 C 签名副本（不再 E6014）。
- 同一 C 链接名但形参 / 返回不同 → E2036。
- 扁平 SDK 成员可 `yux.core.fn`（包孩子 `core` 指向 SDK 壳）。

## 实现

- `FileNode::collectFnOverloads`：本模块符号先入；同 `externLinkName` + 同 C 签名视为语义重复。
- `SemaPass::validateExternFns`：可见 extern 按链接名对签名。
- `getOrCreateExternFunction`：已有 LLVM 函数类型不符则 E2036。
- `registerSdkModulePaths`：`packageChild("yux", "core")` → SDK 壳。
