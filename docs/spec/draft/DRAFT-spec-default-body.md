; 草案：yux spec 默认方法体 + 实现者 fall-through

# 草案：yux spec 默认方法体

状态：**草案 / 占位（形态已锁，待 spec-unify v1 落地后启动）**。日期：2026-05-18。
作用：在 `DRAFT-spec-unify.md` v1 落地（仅替代 draft）之上，**追加** spec body 内方法可带 body 形态——实现者未覆盖时自动 fall-through 到默认体；不引入 `#Derive` 独立注解。

> 本草案从 `DRAFT-spec-unify.md` v1 收紧后剥离（[#1.AD]）。形态决议已在母草案锁住：[#1.E] 默认体动机、[#1.S] typecheck 时机、[#1.T] 组合冲突、[#1.AA] 砍 `#Derive`。本草案承接实施。

涉及章节（预估）：§12 spec（spec-unify v1 落地后的章节号待定）。

---

## 1. 目标

- spec body 内方法允许带 body，作"实现方可继承的默认实现"
- 实现者写同名 fn → 覆盖默认体；不写 → 默认体 fall-through
- spec 仅签名无默认体 + 实现者没写 → E1136"spec 方法未实现"
- 组合 spec 默认体冲突 → 实现者**必须显式覆盖以消歧**（E3132），不引入隐式优先级
- 不引入 `#Derive` 独立注解（永久决议，[#1.AA]）

## 2. spec body 内方法形态

```yux
#Spec
struct Ord {
  fn cmp(other Self&) i32                             ; 仅签名 → 必须实现
  fn lt(other Self&) bool = $.cmp(other) < 0          ; fnExprBody 默认体
  fn le(other Self&) bool {                           ; fnBlockBody 默认体
    $.cmp(other) <= 0
  }
}
```

v1（本草案落地后）允许的方法体形态：

| 形态 | 例 | 备注 |
|---|---|---|
| 仅签名 | `fn cmp(other Self&) i32` | 必须由实现者写体 |
| `= expr` 单表达式体 | `fn lt(other Self&) bool = $.cmp(other) < 0` | 与 fn 通用 `fnExprBody` 一致 |
| `{ ... }` 块体 | `fn le(other Self&) bool { $.cmp(other) <= 0 }` | 多行强制换行（feedback_yux_block_newline）|

## 3. 默认体 typecheck 时机（[#1.S]）

- sema 期：解析 + 占位符号校验（`$` 绑 Self 抽象 type var、调用其它 spec 方法签名识别），**不做完整 typecheck**
- 单态化时机：某具体类型 `#Impl(Spec)` 且未覆盖某方法 → 克隆默认体 AST + 替换 `Self` + 入该类型方法表 + 走常规 typecheck

后果：无人实现的 spec 默认体错误**不报**，与"无人调用的泛型函数体不报错"现状一致。

## 4. 组合默认体冲突（[#1.T]）

两个 spec 都带同名同 arity 方法 + 都带默认体时：

- 实现者必须显式覆盖以消歧
- fn body 内可写 `SpecA::default_method(...)` / `SpecB::default_method(...)` 选边
- 冲突未覆盖 → **E3132**
- 一种 spec 默认体覆盖另一种纯抽象签名**不冲突**

不引入隐式优先级 / 顺序 / `use SpecA::method` 机制。

## 5. base.yux 内置 5 件套

`ToString` / `ToJson` / `Eq` / `Ord` / `Clone` 改为带默认体的 spec。本草案能落地的形态：

- ✅ `Ord.lt / le / gt / ge` 由 `cmp` 推
- ✅ `Eq.ne` 由 `eq` 推
- ❌ `ToJson.to_json` / `Eq.eq` / `Clone.clone` 按字段递归形态自动 derive —— **永不引入**（[#1.AE]）；实现者手写体或用 `Counter::fields[0].value` 形态手动展开

## 6. 错误码

| 码 | 触发 | 来源 |
|---|---|---|
| E1136 | spec 仅签名无默认体 + 实现者没写 | spec-unify v1 已分配 |
| E1138 | spec 内方法带 body（v1 占位） | spec-unify v1 → 本草案落地后**取消** |
| E3132 | 组合 spec 默认体冲突 + 实现者未覆盖 | [#1.T] |

E1138 在本草案落地时移除（spec 内方法可带 body 正式生效）。

## 7. 不在范围

- spec 含字段段 / 关联类型 / extension blocks → 母草案 §9 列出的衍生草案
- `#Inline for` + 按字段递归默认体的自动 derive → **永不引入**（[#1.AE]）
- 反射 + `Field.value` sema 改名 → `DRAFT-spec-reflect.md`

## 8. 迁移面

### 8.1 编译器

- `src/sema/`：spec 默认体 sema 期解析 + 占位符号校验；单态化时机克隆 AST + 替换 `Self` + 走常规 typecheck
- E1138 占位拒收逻辑取消
- E3132 组合冲突检查

### 8.2 SDK

- base.yux 5 件套补默认体（可落地部分）
- 现有 spec / 实现按 fall-through 收口（实现者删冗余方法）

### 8.3 g4

- **不动**——`fnDecl` body 可选形态已就绪（spec-unify v1 g4 阶段已铺）

### 8.4 测试

- spec 默认体 fall-through 端到端
- 组合冲突 E3132 诊断
- 默认体内调其它 spec 方法
- spec 默认体内 `Self` 抽象变量替换

### 8.5 spec 文档

- spec 章节追加"默认方法体"段
- 附录注解 / 错误码同步

---

## 决议日志

承接 `DRAFT-spec-unify.md` 决议（不重复，仅列引用）：

- **[#1.E]** spec 内方法允许带 body 作默认实现
- **[#1.S]** 默认体 typecheck 推迟到单态化时机
- **[#1.T]** 组合默认体冲突 → 实现者显式覆盖消歧（E3132）
- **[#1.AA]** 砍 `#Derive(Spec)`；`#Impl + 不写体` 即 fall-through

本草案启动时如需新增决议，从 `[#2.A]` 起编号。

---

## 定型与归宿

参考 `DRAFT-spec-unify.md` 末尾定型流程；本草案归宿同步到 spec-unify v1 落地后的 spec 章节。
