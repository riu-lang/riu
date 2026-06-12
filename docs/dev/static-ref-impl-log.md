# static-ref 静态引用实施日志

`DRAFT-static-ref.md` Phase 1–2 的落地记录。范围：`&global_var` / `&cval` 取全局/静态引用、返回 T& 的函数以全局引用为源、借用检查器 immortal 哨兵放行。

- 规范草案：`docs/spec/draft/DRAFT-static-ref.md`（已标注"已落地"）
- spec 回写：§8.6（借用 T&）新增静态借用子条款（§8.6.1.3–§8.6.1.4）、§8.6.10（返回引用的溯源约束）修订允许源集纳入 $rodata、§8.9（禁忌一览）删除"函数返回值 T&"条目（E2009 已在 v0.15 移除）→ ✅ 已收口（v0.16 收尾）
- 实施分支：`dev`（直推，未切特性分支）

---

## 核心决策

- **不引入新类型修饰**（[#1]）：`T&` 就是 `T&`，根来自形参还是全局是编译器内部信息，不暴露到类型层。
- **复用 immortal 哨兵 `"$rodata"`**（[#2]）：borrow_checker 已有反射 rodata / static data 的 `"$rodata"` 路径（`registerBorrow` line 147），全局/静态引用走同一条——永不过期。
- **单源约束放宽**（[#3]）：E4021 从"恰好 1 个 T& 形参"放宽为"最多 1 个"——0 T& 形参时允许源集仍含 `$rodata`，仅静态借用合法。
- **调用站溯源**：自由函数返回 T& 且无 T& 实参 → 溯源到 `$rodata`（函数定义侧 E4020 已保证源合法）。
- **不动 g4**：`exprGetRef` 语法已可匹配全局 ID，不需改语法文件。

---

## Phase 1 — `compileGetRefExpr` 全局变量 fallback

**文件**：`src/compiler/expr/expr_unary.cpp`

`compileGetRefExpr` 原逻辑仅查 `_localVarPtrs`，不命中即抛 E3031。全局变量不在 `_localVarPtrs` 中（它们由 `compileGlobalVars` / `compileGlobalConsts` 创建为 LLVM GlobalVariable），导致 `&GLOBAL` 在 codegen 层就报错。

**改动**：`_localVarPtrs.find(objName)` 失败后不直接抛 E3031，增加全局变量路径：

1. 通过 `_currentFnNode->lookupSymbol(objName)` 查文件级符号表
2. 若找到，用 `Mangler::global(ownerMod, objName, globPriv)` 构造 LLVM 名
3. `_module->getGlobalVariable(mangledName, true)` 获取 LLVM GlobalVariable
4. 返回 GlobalVariable 指针（字段链 GEP 与局部路径同款）
5. 若 ALSO 找不到 → 保留原 E3031/E3030 错误

```cpp
// 局部路径（现有）
auto it = _localVarPtrs.find(objName);
if (it != _localVarPtrs.end()) {
    currentPtr = it->second;
    ...
} else {
    // —— 全局变量路径（新增） ——
    sym = _currentFnNode->lookupSymbol(objName);
    if (!sym) throw E3030;
    string mangledName = Mangler::global(ownerMod, objName, globPriv);
    auto globalVar = _module->getGlobalVariable(mangledName, true);
    if (!globalVar) throw E3031;
    currentPtr = globalVar;
}
```

---

## Phase 2 — BorrowChecker 识别全局 → $rodata

**文件**：`src/analyzer/borrow_checker.cpp`

### 2a. 给 BorrowChecker 加 `_fn` 成员

用于 `_fn->lookupSymbol(name)` 判断名字是否文件级全局符号（非局部/非 T& 形参但在符号表中存在 → 即全局变量）。

### 2b. `rootFromRefInit` 全局变量识别

`ExprGetRefNode` 路径：若 `resolveRoot(name)` 返回 name 自身且 name 不在 `_declared`（非局部）中，查 `_fn->lookupSymbol(name)`——命中 → 返回 `"$rodata"`。

```cpp
if (auto getRef = dynamic_cast<ExprGetRefNode*>(expr)) {
    auto name = getRef->obj().getText();
    auto resolved = resolveRoot(name);
    if (resolved == name && _declared.find(name) == _declared.end()) {
        if (_fn && _fn->lookupSymbol(name)) {
            return "$rodata";
        }
    }
    return resolved;
}
```

`registerBorrow` 已有 `$rodata` immortal 路径（不检查 `_declared`，不递增 `_activeBorrows`），无需改动。

### 2c. T& 返回允许源集始终含 `$rodata`

在 `run()` 中设定返回源集时，无论方法还是自由函数，追加 `_returnAllowedSources.insert("$rodata")`。

### 2d. E4021 放宽

自由函数：`refParams.size() > 1`（原 `!= 1`），0 T& 形参 → 仅静态借用合法。
Lambda：同规则，`lamRefParams.size() > 1`，0 形参时 `_returnAllowedDesc = "global/static reference"`。

### 2e. 调用站零参 T& 函数溯源

`rootFromRefInit` 的 `ExprCallNode` 分支：若函数返回 T& 且无显式 T& 实参可溯源（既非 dot method 也非 T& 实参），返回 `"$rodata"`——函数定义侧 E4020 已保证 ret 表达式根在允许源集中。

---

## 测试

**新文件**：`sdk/yux/src/yux/core/static_ref.test.yux`

```
✓ test_ref_global_val          — &G_VAL 读
✓ test_ref_global_mut          — &G_MUT 读
✓ test_ref_global_struct_field — &ORIGIN.x 读
✓ test_return_ref_global       — fn get_g_val_ref() i32& = &G_VAL → 调用站
```

全量回归：`yux test` 590/590、`xmake test` 24/24、`yux-check test` 140/140，lint 0 warnings。

---

## 与预估的差异

| 预估（DRAFT §6） | 实际 |
|---|---|
| `sema_pass.cpp` E2009 条件调整 | E2009 已在 v0.15 移除，无需改动 |
| `ast_builder_expr.cpp` `&global` AST 复核 | 路径已通，无需改动 |
| `yux.g4` 语法文件改动 | 不需改 g4 |
| `checkReturnRef` 新增函数 | 未新增函数，逻辑内联在现有 `run()` / `rootFromRefInit` / `rootFromRetExpr` 中 |

实施比预估简单——核心改动仅两个文件、约 70 行增量。

---

## 跨 Phase TODO

- **`&Type::STATIC_FIELD`**（如 `&Counter::DEFAULT`）：语法层暂不支持——`exprGetRef.obj` 不接受 `Type::ID` 路径。先做 `&global_var` / `&cval`，struct 静态字段引用后续单独 MR。
- **spec 回写**：§8.6 / §8.6.10 / §8.9 + 附录 D 同步更新 → ✅ 已收口（v0.16 收尾）。
- **struct 含 T& 字段**：推 v2（需生命周期标注 / pinned 语义）。
- **≥2 T& 形参单源约束放松**：推 v2。
