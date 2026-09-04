# Fallible 签名 `T ! E` 实施日志（已归档）

> F1–F6 完工（2026-09-04）。`#Fallible(E)` 注解已删除；失败声明统一为签名后缀 `T ! E`。

## 阶段摘要

| Phase | 内容 | 状态 |
|---|---|---|
| F1 | 规范草稿 `DRAFT-fallible-sig.md` | ✅ |
| F2 | g4 + AST 双轨解析 | ✅ |
| F3 | TypeInfo / mangle / cast | ✅ |
| F4 | LLVM 符号 `!E` mangle | ✅ |
| F5 | `#Fallible` 弃用 E7020 + 测试迁移 | ✅ |
| F6 | 删除 `#Fallible` → E2005；spec 回写 | ✅ |
| F7 | lambda / fn-value 闭合 | 见 CURRENT.md |

## F6 要点

- `knownAnnos` / `argAnnos` 移除 `Fallible`；`collectAnnos` 对 `#Fallible` 专报 E2005 + hint。
- `resolvedFallibleErr()` 仅读 `_fallibleErrType`（`T ! E` 后缀）。
- 删除 E7020；E7019 改为重复 `! E` 后缀（lambda 双写路径）。
- 测试：`diag_throw_e2005_fallible_removed.yux`；删除 `diag_throw_e7019_dual_fallible.yux`。

规范：`docs/spec/06-函数.md` §6.7、`11-编译期注解.md` §11.5.1、`CHANGELOG` 2026-09-04 条目。
