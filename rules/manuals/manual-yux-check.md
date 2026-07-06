# yux-check 手册

快速语义检查工具。0 LLVM 依赖，编译快（几秒）。做 parse → AST → SemaPass。

## 单文件诊断

```
yux-check <file.yux>
```

输出 `file:line:col [EXXXX]: message` 格式。立即看语义错误，不等 codegen。

## 批量诊断测试

```
yux-check test tests/check-cases/
```

诊断用例在 `tests/check-cases/`（`diag_*.yux`），行尾 `; check: EXXXX` 注解精确匹配错误码 + 行号。

## 与 yux build 的关系

**`yux-check` 报错是 `yux build` 报错的子集**。不报错不代表无错——sema 当前缺口：泛型 fn/impl 体、所有 statement、target-type 类型检查、alias 环检测。这些走 `yux build` 完整 codegen 才能全覆盖。

适用场景：改代码后快速初筛，通过后再 `yux build` 完整验证。

## 注意

**`yux-check` 是独立 exe**，不在 `xmake build yux` 的产物里。改了 `yux/frontend/`（sema、AST）代码后，只 `xmake build yux` 不够——`yux-check` 链接同一个 `yux_frontend` 静态库但也需要重编自己的 main：

```powershell
xmake build yux-check    ; 改了 sema/AST 后必须单独编
```

判断该重编哪个：修改的文件在 `yux/frontend/` 或 `yux/ast/` 下 → `yux-check` 和 `yux` 都要重编；只改了 `yux/yux/compiler/` → 只需 `xmake build yux`（`yux-check` 0 LLVM，不碰 compiler 代码）。
