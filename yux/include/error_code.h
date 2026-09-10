// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 错误码与消息模板集中表
//
// 设计：每个 ErrorCodeDef 携带 (code, message_template)。throw 站点写
//   throw YuxError(line, col, ErrorCode::E3001, leftType.name, rightType.name);
// 其中 message 是 std::vformat 兼容的模板（{} 占位）。
//
// 分段：
//   E1xxx  词法（Phase 3 ANTLR Listener 接管，预留）
//   E2xxx  语法 / AST 结构
//   E3xxx  类型
//   E4xxx  所有权 / 借用
//   E5xxx  模块 / 包
//   E6xxx  内置 / 调用
//
// E0000 为兼容占位，迁移完成后应不再出现。新增码请保持段内号递增并同步附录 D。

#ifndef YUX_LANG_ERROR_CODE_H
#define YUX_LANG_ERROR_CODE_H

#include <cstdint>
#include <exception>
#include <map>
#include <string>

// 诊断严重等级
// Note < Warning < Error；后续 CLI --warn / --allow / --deny 可在不超过该上限范围内调整
// 默认 Error 的码为"硬错误"，CLI 不允许降级为 Warning/Note（仅允许 -Werror 升级 Warning→Error）
enum class DiagSeverity : uint8_t {
    Note,
    Warning,
    Error,
};

// 错误码定义：除 code/message 外携带默认严重等级
// defaultSev 决定该码在没有 CLI 覆盖时的呈现级别，也决定该码是否可被 CLI 降级
struct ErrorCodeDef {
    const char* code;
    DiagSeverity defaultSev;
    const char* message;
};

namespace ErrorCode {

namespace detail {
// 进程级注册表：code 字符串 → 默认严重等级
// 每个 DEF_ERR 通过一个 inline const 变量在静态初始化阶段注册。
inline std::map<std::string, DiagSeverity>& registry() {
    static std::map<std::string, DiagSeverity> m;
    return m;
}
struct CodeRegistration {
    CodeRegistration(const char* code, DiagSeverity sev) noexcept {
        try {
            registry()[code] = sev;
        } catch (...) {
            std::terminate();
        }
    }
};
} // namespace detail

// 按字符串查 code 的默认严重等级；未知 code 返回 nullptr
inline const DiagSeverity* lookupDefaultSeverity(const std::string& code) {
    auto& m = detail::registry();
    auto it = m.find(code);
    if (it == m.end()) return nullptr;
    return &it->second;
}

// 默认 Error 的常用宏
#define DEF_ERR(code, msg)                                                                                             \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Error, msg};                                        \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Error};
// 默认 Warning（当前还没有 Warning 类码，留作 Phase 5 引入未使用变量等场景）
#define DEF_WARN(code, msg)                                                                                            \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Warning, msg};                                      \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Warning};
// 默认 Note（信息性提示）
#define DEF_NOTE(code, msg)                                                                                            \
    inline constexpr ErrorCodeDef E##code{"E" #code, DiagSeverity::Note, msg};                                         \
    inline const ::ErrorCode::detail::CodeRegistration _reg_E##code{"E" #code, DiagSeverity::Note};

// ── 占位 ──────────────────────────────────────────────────────────────
DEF_ERR(0000, "")

// ── E1xxx 词法 / 文法（ANTLR 报告） ───────────────────────────────────
DEF_ERR(1001, "lexer error: {}")
DEF_ERR(1002, "syntax error: {}")

// ── E11xx spec / 接口（v0.5+, spec-unify v1） ─────────────────────────
// 附录 D §D.3.7
DEF_ERR(1101, "Type '{}' does not implement spec method '{}: {}' (impl block missing)")
DEF_ERR(1102, "Method '{}' in 'Type {} : D' impl block is not part of D's signature set")
DEF_ERR(1103, "Duplicate impl block 'Type {} : {}'")
DEF_ERR(1104, "spec method '{}.{}' must not introduce its own generic parameters")
DEF_ERR(1105, "Method '{}' on type '{}' is defined in both an ordinary impl block and a 'Type : {}' impl block")
DEF_ERR(1106, "Type '{}' does not satisfy spec bound '{}' for type parameter '{}'")
DEF_ERR(1110, "'#DraftLike' annotation is only allowed on 'draft' declarations")
DEF_ERR(1111, "'#DraftLike' draft '{}' must not declare default method bodies")
DEF_ERR(1112, "'#DraftLike' draft '{}' must not contain method-local generic parameters")
DEF_ERR(1120, "Cannot implement spec '{}' for type '{}': both belong to external packages (orphan rule, spec §12.5)")
// ── Dyn<D> / Dyn<D&> 运行时多态 (DRAFT-dyn-draft / 拟 §12.9 — Phase 2) ────
DEF_ERR(1131, "Type argument of `Dyn<...>` must be a spec name; `{}` is not a spec")
DEF_ERR(1132, "Nested `Dyn<...>` is not allowed: `{}` cannot wrap another `Dyn` / `Rc` of `Dyn`")
DEF_ERR(
    1133,
    "Cannot construct `Dyn<{}>` from `{}`: argument must be `Rc<U>` (owned) or `U&` (borrow) where `U` implements `{}`")
DEF_ERR(1134, "Spec `{}` is not object-safe: signatures contain `Self` or the spec's own name in non-receiver "
              "position; `Dyn<{}>` / `Dyn<{}&>` is not allowed")
DEF_ERR(1135, "`Dyn<D>?` (nullable dyn) is not supported in v1")
DEF_ERR(1136, "`Dyn<{}>` cannot cross `extern` boundary: vtable layout is internal ABI")
// ── E113x spec-unify v1（[#1.AD] / DRAFT-spec-unify.md）───────────────
DEF_ERR(1137, "Type `{}` does not implement spec method `{}` (declared in `#Impl({})`)")
DEF_ERR(1138, "Cannot access static member `{}` on instance of `{}`; use `{}::{}` instead")
// E1139 已退役（DRAFT-spec-default-body Phase 1 解锁 spec 默认体）
DEF_ERR(1140, "spec `{}` has no method `{}`{}")
DEF_ERR(1141, "Type `{}` does not implement spec static field `{}` (declared in `#Impl({})`)")

// ── E2xxx 语法 / AST 结构 ─────────────────────────────────────────────
DEF_ERR(2001, "Weak<T>? is forbidden: Weak is natively nullable (upgrade returns Rc<T>?)")
DEF_ERR(2002, "buildTypeWithRef: unknown typeWithRef alternative")
DEF_ERR(2003, "module `{}` is ambiguous: both `{}.yux` and `{}/` exist")
DEF_ERR(2004, "module alias `{}` conflicts with existing symbol")
DEF_ERR(2005, "Unknown build annotation `#{}`")
DEF_ERR(2006, "Function `{}` has no body; only `#Builtin` functions may omit the body")
DEF_ERR(2007, "Method `{}.{}` has no body; only `#Builtin` methods may omit the body")
DEF_ERR(2008, "wildcard alias `{}` is ambiguous, matched {}")
DEF_ERR(2009, "Function `{}` cannot return `T&`; only `#Builtin` baked builtins may have a reference return type "
              "(spec §8.9)")
DEF_ERR(2010, "Cannot call mutating method `Array.{}` on `{}`: it has an active borrow (spec §8.4.2.5)")
DEF_ERR(2011, "Build annotation `#{}` is not allowed on this declaration (only `fn` accepts it)")
DEF_ERR(2012, "`#Test` function `{}` must have signature `fn {}()` (no params, no return type, must have body)")
DEF_ERR(2013, "`#Test` and `#Builtin` cannot both be applied to function `{}`")
DEF_ERR(2014, "`#Test` is only allowed in `*.test.yux` files; `{}` is not a test file")
DEF_ERR(2015, "spec bounds (`: D`) are only allowed at declaration sites (fn/struct/spec generic params); not at type "
              "references or call-point turbofish")
DEF_ERR(2016, "Type alias `{}` forms a cycle (recursive without indirection)")
DEF_ERR(2017, "Type alias name `{}` conflicts with existing {} `{}`")
DEF_ERR(2018, "Duplicate variant `{}` in enum `{}`")
DEF_ERR(2019, "Unknown enum `{}` in constructor `{}::{}`")
DEF_ERR(2020, "Enum `{}` has no variant `{}`")
DEF_ERR(2021, "Enum variant `{}::{}` expects {} payload arg(s), got {}")
DEF_ERR(2022, "match scrutinee must be an enum type, got `{}`")
DEF_ERR(2023, "non-exhaustive match on enum `{}`: missing variant(s) {}")
DEF_ERR(2024, "duplicate variant `{}::{}` in match arms")
DEF_ERR(2025, "`else` arm must be the last arm in match")
DEF_ERR(2026, "match pattern for `{}::{}` expects {} binding(s), got {}")
DEF_ERR(2027, "duplicate binding `{}` in match pattern `{}::{}`")
DEF_ERR(2028, "lambda body references outer local `{}`: closures not yet supported (Phase 4)")
DEF_ERR(
    2029,
    "lambda capture of `{}` (type `{}`) not yet supported: Phase 4a / 4a-2 / 4c cover scalars / 8-byte heap handles "
    "(Rc / Weak / Array / String) / `T&`; structs / enums / fn / mixing `T&` with heap handles arrive in later phases")
DEF_ERR(2030, "lambda body cannot assign to captured variable `{}` (spec §6.2.1: captures are immutable in v1)")
DEF_ERR(2031,
        "extern fn `{}` cannot use Function<...> types in {} (function values are not ABI-compatible with C; spec §7)")
DEF_ERR(2032, "Enum variant `{}::{}` payload #{} type mismatch: expected `{}`, got `{}` (no implicit conversion; for "
              "`Rc<T>` payload, bind `var b Rc<T> = T(...)` first then pass `b`)")
DEF_ERR(2033, "invalid string escape sequence `{}`")
DEF_ERR(2034, "extern fn `{}` cannot use type `{}` in {} (not C ABI; spec §6.6.2)")
DEF_ERR(2035, "struct `{}` cannot appear in extern signature: field `{}` has type `{}` (not C-layout; spec §7.5.3)")
DEF_ERR(2036, "extern C symbol `{}` has incompatible signatures (spec §6.6.1)")

// ── E3xxx 类型 — 类型不匹配 ───────────────────────────────────────────
// E3001: 统一二元运算类型不匹配（原 E3001-E3004, E3075-E3077 合并）
DEF_ERR(3001, "Type mismatch in {} operation: left is {}, right is {}")
// E3005: 统一 if-else 分支类型不匹配（原 E3005-E3008 合并）
DEF_ERR(3005, "Type mismatch in if-else branches: {} vs {}")
// E3009: 统一数组类型不匹配（原 E3009-E3011, E3013 合并）
DEF_ERR(3009, "Array type mismatch: expected {}, got {}")
DEF_ERR(3012, "Array size mismatch: expected {}, got {}")
// E3014: 统一赋值/初始化类型不匹配（原 E3014-E3015, E3017, E3027-E3028 合并）
DEF_ERR(3014, "Type mismatch: expected {}, got {}")
DEF_ERR(3016, "Weak<{}> 仅支持从 Rc<{}> 或 Weak<{}> 构造")
DEF_ERR(3018, "T& copy-bind source type mismatch: '{}' is not {}&")
DEF_ERR(3019, "T& local initializer must be &expr or copy-bind from a T& variable")
// E3020 已退役 → E3014
// E3024: 统一 Nullable 操作符左侧类型要求（原 E3024 + E3025 合并）
DEF_ERR(3024, "Operator requires Nullable<T> on the left, got {}")
DEF_ERR(3025, "`{}`@label '{}' target not found: no enclosing loop with that label")
DEF_ERR(3022, "Duplicate loop label '{}': already used by an enclosing loop")
DEF_ERR(3026, "String template interpolation requires type implementing ToString, got '{}' (impl `Type : ToString {{ "
              "fn to_string() String {{ ... }} }}`)")
DEF_ERR(3027, "Type mismatch in match arms: expected {}, arm produces {}")
DEF_ERR(3028, "Heap<{}> constructor argument type mismatch: expected {}, got {}")

// ── E3xxx 类型 — 符号查找 ─────────────────────────────────────────────
// E3030: 统一符号查找（原 E3030-E3033 合并）
DEF_ERR(3030, "'{}' not found")

// ── E3xxx 类型 — 字段 / 结构体访问 ────────────────────────────────────
DEF_ERR(3040, "Struct {} has no field: {}")
// E3041: 统一字段/成员访问（原 E3041 + E3045 合并）
DEF_ERR(3041, "Cannot access field or member on non-struct type: {}")
DEF_ERR(3042, "Cannot access private field '{}' of struct '{}'")
DEF_ERR(3043, "Cannot find struct declaration for field access")
DEF_ERR(3044, "`?.` inner type {} has no struct decl")
DEF_ERR(3046, "Nested member access not yet supported")

// ── E3xxx 类型 — 泛型参数缺失 ─────────────────────────────────────────
// E3050: 统一泛型参数缺失（原 E3050-E3057 合并）
DEF_ERR(3050, "{} type requires element type")

// ── E3xxx 类型 — 数组操作 ─────────────────────────────────────────────
// E3060: 统一数组索引（原 E3060 + E3065 合并）
DEF_ERR(3060, "Array {} requires at least one index")
// E3061: 统一数组变量（原 E3061 + E3066 合并）
DEF_ERR(3061, "Array {} requires a variable")
DEF_ERR(3062, "Cannot index non-array type: {}")
DEF_ERR(3063, "Empty array literal not supported")
DEF_ERR(3064, "Array<T> initialization requires Array<T> expression or array literal")
// E3067: 统一数组填充注解（原 E3067 + E3068 合并）
DEF_ERR(3067, "Array fill expression requires array type annotation{}")

// ── E3xxx 类型 — 运算符 ───────────────────────────────────────────────
DEF_ERR(3070, "Cannot apply bitwise NOT to float type: {}")
DEF_ERR(3071, "Cannot apply logical NOT to non-bool type: {}")
DEF_ERR(3072, "Unknown unary operator")
DEF_ERR(3073, "Type '{}' does not support operator '{}' (method '{}' not found)")
DEF_ERR(3074, "Type '{}' does not support unary operator '{}' (method '{}' not found)")
// E3075-E3077 已退役 → E3001
DEF_ERR(3078, "Weak<T> does not support == / != (v1 does not expose handle comparison)")

// ── E3xxx 类型 — 字面量 ───────────────────────────────────────────────
// E3080: 统一不支持的 literal 类型（原 E3080-E3082 合并）
DEF_ERR(3080, "Unsupported literal type")

// ── E3xxx 类型 — 其他 ─────────────────────────────────────────────────
DEF_ERR(3090, "Unsupported dot expression")
DEF_ERR(3091, "Unknown expression type")
DEF_ERR(3092, "Unknown statement type")
DEF_ERR(3093, "Cannot assign to immutable variable: {}")
DEF_ERR(3094, "`{}` statement not within a loop")
DEF_ERR(3095, "Type {} is not a Function")
DEF_ERR(3096, "Cannot get LLVM type for '{}'")
DEF_ERR(3097, "Cannot determine type for reference expression: no scope")
DEF_ERR(3098, "Unknown type '{}' for field '{}' of generic struct '{}'")
DEF_ERR(3099, "{}\n  {}") // compiler_types 包装上下文（msg + ctx）
DEF_ERR(3100, "Tuple index {} out of range for type '{}' (size {})")
DEF_ERR(3101, "Tuple destructure expects type tuple, got '{}'")
DEF_ERR(3102, "Tuple destructure arity mismatch: {} names vs tuple size {}")
DEF_ERR(3103, "Integer literal '{}' out of range for type '{}'")
DEF_ERR(3104, "Local `cval` initializer must be a constant expression: {} (DRAFT-const-mut §3.3; allowed: "
              "numeric/bool/null/string literals, references to declared `cval`, and arithmetic / bitwise / comparison "
              "/ logical combinations thereof)")
DEF_ERR(3105, "Unknown parameter annotation '#{}' (only '#Frozen' is supported on parameters; DRAFT-const-mut §5.1)")
DEF_ERR(3106, "Cannot write field '{}' of `#Frozen` parameter '{}' (DRAFT-const-mut §5.3)")
DEF_ERR(3107, "Cannot pass `#Frozen` value '{}' to mutable parameter '{}'; use copy_of to obtain an owned copy "
              "(DRAFT-const-mut §5.4)")
DEF_ERR(3108, "Unknown or duplicate field annotation '#{}' (only '#Val' and '#Frozen' are supported on fields, "
              "mutually exclusive; DRAFT-const-mut §6.1)")
DEF_ERR(3109, "Cannot write field '{}' marked '#{}' outside the constructor of struct '{}' (DRAFT-const-mut §6.2)")
DEF_ERR(3110, "`#Const fn` '{}' cannot {}: {} (DRAFT-const-mut §4.2)")
DEF_ERR(3111, "`#Const fn` '{}' cannot call non-`#Const` function '{}' (DRAFT-const-mut §4.2.4)")
DEF_ERR(3112, "Unknown `let` annotation '#{}' (only '#Mut', '#Frozen', '#Cval', '#Inline' are supported on `let`; "
              "DRAFT-let-unify §3.4)")
DEF_ERR(3113, "`let {}` requires a type or initializer (DRAFT-let-unify §3.4)")
DEF_ERR(3114, "`let {} <type>` requires an initializer (use `#Mut let` for deferred assignment; DRAFT-let-unify §3.4)")
DEF_ERR(3115, "Annotations '#{}' and '#{}' are mutually exclusive on `let` (DRAFT-let-unify §3.4)")
DEF_ERR(
    3116,
    "Global `let {}` requires `#Cval`: only compile-time constants are allowed at global scope (DRAFT-let-unify §3)")
DEF_ERR(3117, "`#Inline` requires `#Cval` on `let` declaration (DRAFT-let-unify §3.4)")
DEF_ERR(3118, "Cannot take address of `#Inline` constant `{}`: inline constants have no storage address "
              "(like C `#define`; use a plain `#Cval` if an address is needed)")

// ── DRAFT-const-eval Phase 2: 全局 const-eval ─────────────────────────
DEF_ERR(3140, "Global `let {}` initializer is not a constant expression (DRAFT-const-eval §2; allowed: literals, "
              "references to declared `#Cval` globals, and arithmetic / bitwise / comparison / logical combinations "
              "thereof)")
DEF_ERR(3143, "Constant expression evaluation error in global `let {}` (overflow, division by zero, or unsupported "
              "operation; DRAFT-const-eval §4.7)")

// ── DRAFT-const-eval Phase 3: #Const fn body 控制流白名单 ─────────────
DEF_ERR(3141, "`#Const fn` '{}' body contains disallowed control-flow form: {} (DRAFT-const-eval §3; allowed: "
              "`= expr`, `{{ ret expr }}`, sequential `#Cval let`, `exprOneLineIfElse`, "
              "if-statement with `ret` in both branches)")

// ── DRAFT-const-eval Phase 4: #Const fn 调用纳入 const-eval ───────────
DEF_ERR(3144, "`#Const fn` '{}' cannot be invoked in a constant expression: {} type '{}' is not in the const-eval "
              "whitelist (DRAFT-const-eval §4; allowed: scalar integer / float / bool)")

// ── DRAFT-static-vars Phase 1–4: 全局变量 / 静态字段 ──────────────────────────
DEF_ERR(3150,
        "#Static field '{}' requires an initializer (DRAFT-static-vars §4.3; v1 static fields must be initialized "
        "at declaration)")
DEF_ERR(3151, "Cannot write to non-#Mut global/static '{}' (DRAFT-static-vars §4/§7; "
              "globals and static fields default to `val`; "
              "use `#Mut let` for globals or `#Mut\\n#Static` for static fields)")
DEF_ERR(3154,
        "Global `let {}` requires an initializer (DRAFT-static-vars §3.3; non-#Cval globals must be initialized at "
        "declaration)")
DEF_ERR(3157, "#Static field '{}' on generic struct '{}' is not allowed (DRAFT-static-vars §4.3; v1 prohibits static "
              "fields on generic structs)")
DEF_ERR(3152, "Cannot access static field '{}' through an instance of '{}'; use `{}::{}` instead "
              "(DRAFT-static-vars §4.4)")
DEF_ERR(3158, "Cannot read uninitialized #Mut global '{}' (DRAFT-static-vars §6; #Mut globals must be initialized "
              "before first read)")
DEF_ERR(3160, "`for-in` iterable must be Array<T> or [T*N], got '{}'")

// ── E315x DRAFT-static-vars Phase 6（跨模块 init 顺序） ──────────────────
DEF_ERR(3153, "Circular module dependency detected involving '{}'; cannot determine global init order "
              "(DRAFT-static-vars §5.5; break the cycle or use lazy init)")
DEF_ERR(3155, "Global/static initializer for '{}' must not contain try-catch; init must be infallible "
              "(DRAFT-static-vars §6)")
DEF_ERR(3156, "Cross-module access to private static '{}' is not allowed (DRAFT-static-vars §4.4; v1 placeholder)")

// ── 构造模型重构: #Static fn / Self / 字段字面量 (DRAFT-static-fn) ────
DEF_ERR(
    3120,
    "`{}::{}` resolves to an instance method, not a `#Static fn`: use `<receiver>.{}(...)` instead (DRAFT-static-fn)")
DEF_ERR(3121,
        "struct `{}` has no static fn `{}` (`Type::name(...)` requires a method annotated `#Static`; DRAFT-static-fn)")
DEF_ERR(3122, "`{}::{}` LHS is neither an enum nor a struct in scope (DRAFT-static-fn)")
DEF_ERR(3123, "`Self` type only allowed inside a `structImpl` body (DRAFT-static-fn)")
DEF_ERR(3124, "`Self {{ ... }}` field literal only allowed inside a `#Static fn` body (DRAFT-static-fn)")
DEF_ERR(3125, "`Self {{ ... }}` for struct `{}` is missing field `.{}` (all fields must be listed; DRAFT-static-fn)")
DEF_ERR(3126, "struct `{}` has no field `.{}` (DRAFT-static-fn)")
DEF_ERR(3127, "duplicate field `.{}` in `Self {{ ... }}` literal (DRAFT-static-fn)")
DEF_ERR(3128, "`$` (current instance) cannot be used inside a `#Static fn` body (DRAFT-static-fn)")
DEF_ERR(3130,
        "Same-name constructor `fn {}(...)` is no longer supported — define a `#Static fn` (e.g. `#Static fn "
        "make(...)` returning `{}` via `Self {{ ... }}`) and call it as `{}::make(...)` (DRAFT-static-fn Phase 6)")
DEF_ERR(3131, "`{}::{}(...)` argument count/type mismatch: expected {} args ({}), got {} args ({}) (DRAFT-static-fn)")

// ── 组合 spec 默认体冲突 (DRAFT-spec-default-body Phase 4) ────────────
DEF_ERR(3132, "Type `{}` inherits conflicting default bodies for method `{}` from specs {}; implementer must provide "
              "an explicit override")

// ── DRAFT-spec-reflect Phase 4+: Field.value / 反射诊断 ───────────────
DEF_ERR(3133, "`Field.value` requires a compile-time-known Field reference (e.g. `Counter::fields.get(0).value`); "
              "runtime Field variables are not supported (DRAFT-spec-reflect §6)")
DEF_ERR(3134, "`Field.value` has no receiver to bind to; use `.value` inside a method body where `$` is available "
              "(DRAFT-spec-reflect §6)")
DEF_ERR(3135, "`variants` is only accessible on enum types; `{}` is not an enum (DRAFT-spec-reflect §2)")
DEF_ERR(3136, "Cannot take `{}::type` by value; use a reference or pointer to the rodata singleton "
              "(DRAFT-spec-reflect §8)")

// ── E4xxx 所有权 / 借用 ───────────────────────────────────────────────
DEF_ERR(4001, "T& borrow initializer must be &expr or an existing T& variable")
DEF_ERR(4002, "T& '{}' borrows root '{}' whose scope does not cover the borrow")
DEF_ERR(4003, "root '{}' cannot be reassigned while borrowed (§3.5)")
DEF_ERR(4004, "Cannot bind T& to non-local: {}")
DEF_ERR(4010, "field '$.{}' {} before initialization (§8.2)")
DEF_ERR(4011, "field '$.{}' must be initialized before {} (§8.2)")
DEF_ERR(4012, "field '$.{}' is not initialized at constructor exit (§8.2)")
DEF_ERR(4013, "cannot return `$` from constructor (§8.3)")
DEF_ERR(4020, "return T& root must be {}, got '{}' (§8.6)")
DEF_ERR(4021, "function returning T& requires exactly one source: `$` (method) or a single T& parameter (free fn)")
DEF_ERR(4022, "lambda value with `T&` capture cannot escape current frame (cannot be returned, stored to "
              "var/field/container/Rc; only consumable inline as call argument; spec §6.3)")
DEF_ERR(4023, "Heap<{}> '{}' escapes its scope: ret position requires NRVO (§8.3a.4.1)")
DEF_ERR(4024, "Heap<{}> '{}' cannot be moved by value; declare as Heap<{}>? for movable slots (§8.3a.3.2)")
DEF_ERR(4025, "{}<Heap<{}>> is forbidden: Heap cannot be nested in Rc / Weak / Array containers (§8.3a.5.1)")
DEF_ERR(4026, "NRVO not applicable for Heap<{}>: multiple ret sources or coexists with outliving borrow (§8.3a.4.1)")
DEF_ERR(4027, "implicit widen Heap<{}> -> Heap<{}>? is forbidden; restructure source signature (§8.3a.4.3)")
DEF_ERR(4028, "extern fn parameter / return must not be Heap<{}>; use Ptr at FFI boundary (§8.3a.5.3)")
DEF_ERR(4029, "Arc<{}> is reserved for v1.x multi-threading; not yet implemented (DRAFT-heap-types §9)")
DEF_WARN(4030, "result of `<-` move-assign is discarded; the old value will be immediately released "
               "— use `a = b` if you don't need the old value (§4.13)")

// ── E403x: #NoCopy / move 语义（Phase B-1）──
DEF_ERR(4031, "Cannot implicitly copy `{}` (marked #NoCopy) in `{}`; use `move:<{}>(x)` to transfer ownership")
DEF_ERR(4032, "Struct `{}` contains `#NoCopy` field `{}` and must itself be annotated `#NoCopy`")
DEF_ERR(4033, "Cannot use `{}` after move; ownership has been transferred")
DEF_ERR(4034, "`move` argument must be T&, got `{}` (spec §3g)")
DEF_ERR(4035, "Type parameter of `move` must not itself be a reference (spec §3g)")
DEF_ERR(4036, "`<-` left-hand side must be a variable, `$`, field, or tuple member "
              "(index not yet supported; spec §4.13.2.2)")
DEF_ERR(4037, "type argument of '{}' cannot be a borrow (`T&`) (§8.6.7.1)")
DEF_ERR(4038, "`Dyn<D&>` cannot appear in owned type position (field, alias, or container element) (§12.9.2.2)")

// ── E5xxx 模块 / 包 ───────────────────────────────────────────────────
DEF_ERR(5001, "yux.toml not found in {}")
DEF_ERR(5002, "yux.toml is missing required field `name`")
DEF_ERR(5003, "yux.toml field `name` must be a string")
DEF_ERR(5004, "yux.toml field `name` must not be empty")
DEF_ERR(5005, "yux.toml `lib` must be a table")
DEF_ERR(5006, "yux.toml `lib.type` must be \"static\" or \"dynamic\"")
DEF_ERR(5007, "yux.toml `lib.type=\"dynamic\"` not yet supported")
DEF_ERR(5008, "yux.toml `[lib]` and `entry` are mutually exclusive")
DEF_ERR(5009, "failed to parse yux.toml: {}")
DEF_ERR(5010, "syntax errors in {}")
DEF_ERR(5011, "circular module import: {}")
DEF_ERR(5012, "module not found: {} (expected file {})")
DEF_ERR(5013, "yux.toml `entry` must be a relative path under `src/`, got absolute path: {}")
DEF_WARN(5014, "yux.toml `entry` resolves outside `src/` (`{}`): convention is that all sources live under `src/`; obj "
               "path layout may also be inconsistent")
// 裸名分层 / 路径单用（限定类型路径）。E2010–E2012 已占用，故用 E5xxx。
DEF_ERR(5015, "ambiguous bare name `{}`: candidates {}")
DEF_ERR(5016, "cannot use module or package `{}` as a value")
DEF_ERR(5017, "cannot use type `{}` as a value")
// pkg 排他 / to（§10.2.4）。站点随 v0.22 接入。
DEF_ERR(5018, "module `{}` is not exported from package `{}`")
DEF_ERR(5019, "pkg `to` target `{}` does not exist")
DEF_ERR(5020, "invalid pkg line in `{}`: {}")

// ── E6xxx 内置 / 调用 ─────────────────────────────────────────────────
DEF_ERR(6001, "module `{}` not found in package `{}`")
DEF_ERR(6002, "function `{}` not found in module `{}`")
DEF_ERR(6003, "Cannot call private function `{}` via package alias")
DEF_ERR(6004, "Cannot call private function `{}` via module alias")
DEF_ERR(6005, "module `{}` (alias `{}`) not loaded")
DEF_ERR(6006, "Cannot call private function '{}'")
DEF_ERR(6007, "Cannot call private method '{}' of struct '{}'")
DEF_ERR(6008, "Cannot use private struct '{}' in constructor")
DEF_ERR(6009, "Generic struct '{}' constructor requires explicit type arguments")
DEF_ERR(6010, "Generic function '{}' expects {} type args, got {}")
DEF_ERR(6011, "Generic struct '{}' expects {} type args, got {}")
DEF_ERR(6012, "Generic function '{}' expects {} params, got {} args")
DEF_ERR(6013, "Cannot infer type parameter '{}' for generic function '{}'")
DEF_ERR(6014, "Ambiguous call to '{}({})': {} overloads match; add type suffix to disambiguate:{}")
DEF_ERR(6015, "Unsupported call expression")
DEF_ERR(6016, "Unknown method '{}' for builtin type '{}'")
DEF_ERR(6017, "Unknown #Builtin function '{}'")
DEF_ERR(6018, "Cannot determine type argument for size_of")
DEF_ERR(6019, "Cannot determine LLVM type for '{}'")

// builtin 调用：参数 / 类型实参数量
// E6020-E6022, E6025 已退役 → E6027
// E6024 已退役 → E6026
DEF_ERR(6023, "Cannot call private function '_ptr_offset' (SDK-only Ptr arithmetic)")
DEF_ERR(6026, "{} expects {} type argument(s)")
DEF_ERR(6027, "{} expects {} argument(s)")
DEF_ERR(6028, "{}:<T&> requires a local var or &expr argument")
DEF_ERR(6029, "{}:<T> requires T to be Rc/Weak/Array/String or U& (got '{}')")
DEF_ERR(6030, "assert_eq:<T> requires T to be a numeric or bool type (got '{}')")
DEF_ERR(6031,
        "assert_eq operand type mismatch: actual is '{}', expected is '{}' (yux 不做隐式整型/浮点转换；整型字面量默认 "
        "i32，需要时加后缀如 `3i64`/`3u8` 或写 `assert_eq:<T>(...)` 锁定类型)")
DEF_ERR(6032, "copy_of:<T> cannot copy types containing Ref fields (offending: '{}') — Ref 借的是别人的可写状态，独立 "
              "owned 副本与借用语义冲突 [DRAFT-const-mut §5.3]")
DEF_ERR(6033, "No matching constructor for '{}({})'; declared overloads:{}")

// Array 内置方法
// E6040/E6043/E6044 已退役 → E6027; E6041 已退役 → E6042; E6045 已退役 → E6027
DEF_ERR(6042, "Array.{}() requires an lvalue array")

// ── E7xxx 错误模型 / panic（DRAFT-错误.md） ─────────────────────────────
// 附录 D §D.3.7 之后段位；E7001-E7014 默认 Error，E7015-E7018 默认 Warning（Phase 10d+ 启用）
// E7012 / E7013 / E7014 由 Phase 10d 启用；E7001 / E7004 / E7006 / E7008 由 Phase 10e 启用；其余诊断码留 10f
DEF_ERR(7001, "`!` used outside of a `T ! E` function and outside of `try` block — wrap call in `try {{ ... }} "
              "catch e E {{ ... }}` or declare the enclosing function with `T ! E`")
DEF_ERR(7004,
        "cannot propagate error of type `{}` through `!`: caller returns `T ! {}`, types differ — wrap the call in "
        "`try {{ ... }} catch e {} {{ ret {}::Variant... }}`, or wrap in a function whose `T ! E` matches `{}`")
DEF_ERR(7006, "call to `T ! E` function `{}` outside `try` block must propagate via `!` (same error type) or "
              "`try-catch` — bare call is forbidden outside `try` (inside `try`, bare call is correct; `!` would be "
              "redundant)")
DEF_ERR(7008, "success type `{}` cannot equal error type `{}` in `T ! E` signature (the compiler cannot disambiguate "
              "`ret` between success and error channels) — split into two enums (one for the success result type, one "
              "for the error type) and rethrow / wrap explicitly")
DEF_ERR(7012, "`#NoReturn` function `{}` cannot declare a return type — remove the return type or remove `#NoReturn`")
DEF_ERR(7013, "`#NoReturn` and `T ! E` are mutually exclusive on the same function — a non-returning function cannot "
              "also propagate errors")
DEF_ERR(7019, "duplicate fallible error type `! {}` in signature — use `T ! E` once")
DEF_ERR(7014, "`#NoReturn` function `{}` may reach end of body — control flow must terminate via `panic`-class call, "
              "another `#NoReturn` call, or unconditional infinite loop")
// E7002 / E7009 / E7010 / E7011 / E7015-E7018 由 Phase 10f 启用（try-catch 块）
DEF_ERR(7002, "non-exhaustive `try` block: error type `{}` thrown by callee `{}` is not handled by any `catch` clause "
              "— add `catch e {} {{ ... }}`")
DEF_ERR(7009, "`try` block must be followed by at least one `catch` clause — bare `try {{ ... }}` is forbidden")
DEF_ERR(7010, "`catch` body must end with `ret`, `panic`-class terminator, or an expression of the same type as the "
              "`try` block (got `{}` vs `{}`)")
DEF_ERR(7011, "`catch e {}` type `{}` must be a declared enum; got `{}`")
DEF_WARN(7015, "redundant `catch` clause: no call in `try` block can throw `{}` declared by `catch e {}` — remove this "
               "`catch` clause")
DEF_WARN(7016, "`!` is redundant inside `try` block: bare call to `T ! E` function `{}` already routes to the matching "
               "`catch e {}` clause — remove `!`")
DEF_WARN(
    7017,
    "redundant `try-catch`: no call in `try` block can throw any error — remove the entire `try` and use a plain block")
DEF_WARN(7018, "`panic` in `catch` arm converts a recoverable error to abort — consider `ret` with an error variant or "
               "`exit(code)` if termination is intended")

#undef DEF_ERR
#undef DEF_WARN
#undef DEF_NOTE
} // namespace ErrorCode

#endif // YUX_LANG_ERROR_CODE_H
