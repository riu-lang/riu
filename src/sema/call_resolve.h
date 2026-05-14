// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_CALL_RESOLVE_H
#define YUX_LANG_SEMA_CALL_RESOLVE_H

#include "ast/node/draft_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"

class Yux;
class DraftRegistry;
class DraftImplChecker;

// 调用重载解析（Sema/Codegen 拆分 Phase 3.3 前置）
//
// 从 `src/compiler/compiler_call.cpp` 抠出, 不依赖 LLVM, 用于:
// - 当前: Compiler 在 compile 调用 / 构造点调用, 行为不变
// - 后续: SemaPass 在 visitCall 阶段调用, 提前完成歧义诊断 + 灵活整数推断
//
// 副作用: 唯一匹配时会通过 tryInferIntType 把灵活整数实参类型回填到 AST 节点;
//        因此整条流水线必须保证只调用一次, 否则推断会被重复执行 (幂等但浪费).
namespace sema {

// 解析普通函数 / 模块函数的重载.
// 命中多个候选时抛 E6014 (ambiguous overload).
void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName,
                       const vector<p<ExprNode>>& args, int line);

// 解析构造器重载 (在符号表中以 `S.S` 注册, params[0] 是接收者).
// 命中多个候选时抛 E6014.
void resolveCtorOverload(FileNode* file, const string& structName,
                         const vector<p<ExprNode>>& args, int line);

// ctor 调用形态校验 (Phase 3.3 前置.3b).
//
// 在 callee 已经解析到一个 structDecl 后, 校验调用形态:
//   - E6008: 结构体名以 `_` 开头 (私有), 不允许跨可见性构造
//   - E6009: 泛型结构体调用时未给类型实参
//
// `hasTypeArgs` 为调用点是否携带显式类型实参 (`Foo:<i32>(...)` 形态).
// 非泛型结构体不查 hasTypeArgs (yux 当前不允许给非泛型类型带 typeArgs).
//
// 调用方:
//   - Compiler::compileFunctionCall 在解析 structDecl 后立即调用
//   - SemaPass::visitExpr 在 ExprCallNode 分支识别 ctor 时调用
void validateCtorCallShape(StructDeclNode* structDecl, const string& fnName,
                           bool hasTypeArgs, int line, int col);

// 泛型函数 / 泛型构造器调用点的类型实参 arity 校验 (Phase 3.3 前置.3c).
//
// 当调用点写了显式 `:<T1, T2, ...>` 时, 检查 arity 与声明的 typeParams 是否匹配.
// 不匹配抛 E6010, 带 `.withHint(...)` 提示用户如何补全.
//
// 注意:
//   - 仅在 explicitTypeArgsCount > 0 时调用 (无 typeArgs 走类型推断, 不是 arity 错).
//   - 调用方需自行确认 callee 已识别为泛型 fn / 泛型 struct.
void validateGenericTypeArgsArity(const string& fnName,
                                  size_t expectedCount,
                                  size_t actualCount,
                                  int line, int col);

// 泛型 fn 的 typeArgs 边界校验 (Phase 3.3.3.c, E3032 / E1106).
//
// `<T : D1 + D2>` 形态: 对每个 typeParam[i], 在 fnOwner 的可见性下解析每个 bound
// 名 D (DraftRegistry), 然后用 DraftImplChecker::boundSatisfied 校验 typeArgs[i]
// 是否满足该 draft. 不满足:
//   - bound 名 D 无法解析 → E3032 (draft 名未声明)
//   - 解析成功但 boundSatisfied 返回 false → E1106 (typeArg 不实现 D)
//
// `registry` / `checker` 任一为 nullptr 时 helper 直接 no-op (Yux 未就绪时不强制).
//
// v0.5: 函数声明位 draftBound 暂未携带类型实参 (ast_builder 仅取基名), 这里始终用
// 空 vector 传给 boundSatisfied; 草案 §6.4.4.1 文法允许 `D<T>` 形态留待后续扩展.
//
// 调用方:
//   - Compiler::compileGenericFunctionCall 在 typeArgs 解析就绪后调用
//
// 纯 AST + Registry 查表 + checker 调用, 无 LLVM 依赖.
void validateGenericTypeArgsDraftBound(
    const DraftRegistry* registry,
    const DraftImplChecker* checker,
    FileNode* fnOwner,
    FnHeaderNode* header,
    const vector<TypeInfo>& typeArgs,
    int line, int col);

// Dyn<D> callee 的 draft 名字解析 (Phase 3.3.3.b, E1131).
//
// 入口: 调用 `someDynVal.foo(...)` 时, 取 baseType (`Dyn<D>` / `Dyn<D&>`) 的内层
// D 名, 在 visibleFrom 文件的可见性范围内通过 DraftRegistry 解析 → draft decl + 限定名.
//
// 解析失败 (Phase 2b/2c 理论已拦截, 这里是 codegen 兜底) 抛 E1131, payload 为裸名
// (若 baseType 不是 Dyn 形态导致裸名为空, 用 "?" 占位).
//
// `registry` 允许为 nullptr (SemaPass 早期未持有 Yux*); 此时直接抛 E1131, 由
// Compiler 路径覆盖.
//
// 返回值: 命中 → 填充 decl + qualified; 未命中 → throw, 不返回.
//
// 纯 AST / 字符串查表, 无 LLVM 依赖.
struct DynCalleeResolved {
    DraftDeclNode* decl = nullptr;
    string qualified;
};
DynCalleeResolved resolveDynCalleeDraft(const DraftRegistry* registry,
                                         FileNode* visibleFrom,
                                         const TypeInfo& baseType,
                                         int line, int col);

// Dyn<D> 方法调用的静态形态校验 (Phase 3.3 前置.3f).
//
// 调用方 (Compiler::compileDynMethodCall) 在解析到 draftDecl 后调用.
// 校验:
//   - E6016: draftDecl.signatures 内找不到名为 member 的签名
//   - E6012: 找到签名但 params arity 与 argTypes 不一致
//   - E6015: 形参类型 (按 draft 签名 getFullName 比对) 与实参不一致, 带 hint
//
// 命中 (合法) 时返回找到的 `FnHeaderNode*`, 让 codegen 走 vtable indirect call.
// 纯 AST / TypeInfo, 无 LLVM 依赖.
//
// `draftQualified` 仅用于 E6015 hint 的人类可读名 (e.g. "yux.core.io.Writer").
FnHeaderNode* resolveDynMethodSig(DraftDeclNode* draftDecl,
                                  const string& draftQualified,
                                  const TypeInfo& baseType,
                                  const string& member,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col);

// ctor 重载未匹配的诊断 (Phase 3.3 前置.3e).
//
// 调用方在 ctor 重载解析路径 (compileConstructorCall) 返回 null 后调用:
// 即 callee 已识别为 structDecl, 但既有 ctor 重载均不接受当前 argTypes.
// 函数总是抛 E6033, 附带:
//   - 当前 file + sdkFile 上 `S.S` 的所有重载签名 (跳过 params[0] 接收者), 按
//     Box<T> / Array<T> / [N]T 等用户友好形式渲染
//   - 实参的同款友好渲染列表
//   - `.withHint(...)` 提示如何修正
//
// 纯类型 / 纯 AST, 无 LLVM 依赖.
void diagnoseCtorOverloadMismatch(FileNode* file, FileNode* sdkFile,
                                  const string& fnName,
                                  const vector<TypeInfo>& argTypes,
                                  int line, int col);

// 非-ID callee 路径的 `!` 错误传播 fallback 校验 (Phase 3.3 前置.3d).
//
// 适用场景:
//   - 非 ID-literal callee 写了 `!` (如 nullable 字面量调用 `obj.f!()`)
//   - fn-value callee 写了 `!` (lambda IIFE / fn-typed 变量, 非 ExprDotNode)
//
// 调用方负责在 callee 路径 + `errPropagate() == true` + 不在 try block 内时调用.
// 当 caller 也不是 #Fallible 时抛 E7001.
//
// 与 checkErrPropagateForIdCall 区分: 那一路负责 ID-callee 完整三态 (E7001/E7004/E7006);
// 这里只覆盖 fallback "caller 必须 fallible" 这一个最弱条件.
//
// 当前 Compiler 端是唯一调用方 (compileCallExpr 入口的两处 else 分支);
// SemaPass 暂未跟踪 try block, 不能直接调用.
void checkBangWithoutFallibleCaller(FnNode* currentFnNode, p<ExprCallNode> callNode);

// ID-callee 错误传播语义校验 (Phase 10e/10f).
//
// 抛错码:
//   - E7001: caller 非 #Fallible 且不在 try block 内, 但调用点写了 `!`
//   - E7004: caller/callee 错误类型不一致, 同 `!` 不可跨类型透传
//   - E7006: callee 是 #Fallible 但调用点未加 `!`, 且不在 try block 内
//   - E7016: try block 内 `!` 冗余 (TODO: 接 warning 通道; 当前 silent)
//
// 副作用: 当 tryBlockSeenErrs != nullptr 且 callee 的 fallibleErrType 非空时,
// 把 calleeErr 追加进 vector —— 用于 try block 的 E7002/E7015 穷尽性判定.
//
// 当前 Compiler 端是唯一调用方 (在 compileCallExpr 入口); SemaPass 暂未跟踪
// try block 栈, 不能直接调用 (传 nullptr 会对 try 内 `foo()!` 误报 E7001).
// 等 SemaPass 落 try block visit 后再接入.
void checkErrPropagateForIdCall(FnNode* currentFnNode,
                                p<ExprCallNode> callNode,
                                const string& fnName,
                                const FnSymbolInfo* calleeSym,
                                vector<string>* tryBlockSeenErrs);

// 包/模块别名调用解析 (Phase 3.3.1.a).
//
// 识别两种调用形态:
//   1. 包别名: `pkg.mod.fn(args)` —— ExprDotNode::parseChain 拿到 (aliasName, segs),
//      segs.size() >= 2, aliasName 在 file 符号表登记为 Package
//   2. 模块别名: `mod.fn(args)` —— baseExpr 是 ID literal, aliasName 登记为 Module
//
// 命中其中一种时返回 `matched = true`, 填充 fnName + fnSym, 调用方据此走
// compileKnownFunctionCall 路径; 未命中时返回 `matched = false`, 调用方继续后续
// 内置类型 / 结构体方法分派.
//
// 抛错:
//   - E6001 (包下找不到子模块) / E6002 (模块下找不到 fn) / E6003 (包下 fn 私有)
//   - E6005 (模块文件 Yux::module 加载失败) / E6004 (模块下 fn 私有)
//   - 多重别名命中 (Package + Module 同名) 直接走 throwAmbiguousAlias
//
// `yux` 参数允许为 nullptr (SemaPass 早期可能拿不到; Compiler 路径恒非空), 此时
// 模块别名分支整段跳过 (返回 matched=false), 由 Compiler 路径兜底; 不会因 yux 缺
// 失而误报 E6005.
//
// 纯 AST / 纯符号查 + 字符串拼接, 无 LLVM 依赖.
struct ModuleFnCallResult {
    bool matched = false;
    string fnName;
    FnSymbolInfo* fnSym = nullptr;
};

ModuleFnCallResult resolveModuleFnCall(FileNode* file, Yux* yux,
                                       p<ExprCallNode> callNode,
                                       p<ExprDotNode> dotNode,
                                       const vector<TypeInfo>& argTypes);

// 泛型函数调用的 typeArgs 推断 (Phase 3.3.1.b).
//
// 调用前提: callNode->getTypeArgs() 为空 (无显式 typeArgs 路径), genericFn->header()->isGeneric()
// 已被调用方确认.
//
// 抛错:
//   - E6012: genericFn 的 params arity 与 argTypes.size() 不一致
//   - E6013: 某 type param 无法从形参 / 实参的 unify 反推 (未出现在裸类型位置或嵌套泛型对位)
//
// 输出: outTypeArgs 按 typeParams 顺序追加推断结果 (调用方应保证传入为空 vector).
// 纯 TypeInfo unify + map 查表, 无 LLVM 依赖.
void inferGenericFnTypeArgs(p<ExprCallNode> callNode, p<FnNode> genericFn,
                            const string& fnName,
                            const vector<TypeInfo>& argTypes,
                            vector<TypeInfo>& outTypeArgs);

// CompilerInner 泛型 intrinsic 的 typeArgs / args arity 校验 (Phase 3.3.2.c).
//
// 覆盖 compileGenericFunctionCall 的 #CompilerInner 分支的纯计数校验:
//   - E6017: 未知 intrinsic (兜底, 不在清单内的 fnName)
//   - E6018: size_of typeArgs 为空
//   - E6024: upgrade typeArgs != 1
//   - E6025: upgrade args != 1
//   - E6026: assert_eq / same_ref / ptr_of / as_ref / copy_of / weak typeArgs != 1
//   - E6027: same_ref args != 2; ptr_of / as_ref / copy_of / weak args != 1
//
// 内建清单 (按当前 SDK assert.yux / mem.yux / ref.yux / weak.yux):
//   assert_eq, size_of, upgrade, same_ref, ptr_of, as_ref, copy_of, weak
//
// E6019 (size_of typeArg getLLVMType 失败) 依赖 LLVM, 留在 codegen.
// E6028 / E6029 / E6032 是类型形态校验, 留给 3.3.2.d.
//
// 纯字符串 + size 比较, 无 LLVM 依赖.
void validateCompilerInnerIntrinsicShape(const string& fnName,
                                          size_t typeArgsCount,
                                          size_t argsCount,
                                          int line, int col);

// CompilerInner 泛型 intrinsic 的类型形态校验 (Phase 3.3.2.d).
//
// 在 validateCompilerInnerIntrinsicShape 通过 arity 校验之后调用.
// 覆盖与类型相关 (typeArg / argType / AST 形态) 的检查:
//   - same_ref / ptr_of:
//       * E6029: T 必须是堆句柄 (Box / Weak / Array 泛型 / String Normal) 或 T&
//       * E6028: T = Ref 时, 每个 arg 的 AST 必须是 ID-literal 或 ExprGetRefNode
//                (取裸源指针只支持这两种 AST 形态; 其他形态无静态出口)
//   - as_ref:
//       * E6029: argType[0] 必须是 Box 且不能是 Nullable
//   - weak:
//       * E6029: argType[0] 必须是 Box 或 Box?, 若 Nullable 其 inner 必须是 Box
//   - copy_of:
//       * E6032: T 深度含有 Ref<U> 字段, 拒绝 (DRAFT-const-mut §5.3 决议 #2)
//
// 不覆盖:
//   - E6019 (size_of llvm getLLVMType 失败) — 依赖 LLVM, 留 codegen
//   - extractRawPtr 内 _localVarPtrs 查不到的 E6028 fallback — 依赖 Compiler 局部
//     变量符号表, 不在 AST 上, 留 inline
//
// `file` / `sdkFile` 用于 copy_of 的 struct 字段深度递归; 为 nullptr 时按"找不到声明 → 保守放过"处理.
void validateCompilerInnerIntrinsicTypeShape(const string& fnName,
                                              const vector<TypeInfo>& typeArgs,
                                              const vector<TypeInfo>& argTypes,
                                              const vector<p<ExprNode>>& argNodes,
                                              FileNode* file, FileNode* sdkFile,
                                              int line, int col);

// CompilerInner 操作符方法的 arity / 类型域校验 (Phase 3.3.2.e).
//
// 覆盖 `compileBuiltinTypeMethodCall` 内 `isCompilerInnerMethod` 分支的纯校验:
//   - E6045: 二元 op (plus/minus/mul/div/mod/eq/ne/lt/le/gt/ge/and/or/xor/shl/shr) args.size() != 1
//   - E3070: 一元 `inv` 不接受 float 类型
//
// 不命中 op 清单时 no-op (调用方继续 fall-through 到 IR emit).
// 纯字符串 + 计数 + baseType.startsWith('f'), 无 LLVM 依赖.
void validateOperatorMethodCall(const string& member, const TypeInfo& baseType,
                                size_t argsCount, int line, int col);

// 自由内建 intrinsic 的 arity 校验 (Phase 3.3.2.b).
//
// 覆盖 compileExternalOrSdkFunctionCall 顶部的纯 arity 分派:
//   - E6020: `ptr_from_addr(addr u64)` arity != 1
//   - E6021: `rc_leak_count()` arity != 0
//   - E6022: `_ptr_offset(p, off)` arity != 2
//
// 命中的 fnName 才会校验, 其他 fnName 是 no-op (调用方继续 fall-through 到
// fnSymbol / generic / external 路径). E6023 (_ptr_offset 跨模块私有) 与 E6006
// 语义重叠但错误码不同, 暂留 inline; 等 SemaPass 接管后再统一.
//
// 纯字符串 + size 比较, 无 LLVM 依赖.
void validateFreeIntrinsicArity(const string& fnName, size_t argsCount,
                                int line, int col);

// Array<T> 方法调用的静态形态校验 (Phase 3.3.2.a).
//
// 覆盖 Array<T> 内置方法 (len/cap/is_empty/at/first/last/pop/push/set_len/clear)
// 的 arity + elemType + lvalue 校验:
//   - E3055: baseType.arrayGenericElementType() 缺 (除 len/cap 之外都需要)
//   - E6040: at arity != 1
//   - E6041: pop 在 rvalue 上 (无法原位修改 len)
//   - E6042: push/set_len/clear 在 rvalue 上
//   - E6043: set_len arity != 1
//   - E6044: push arity != 1
//
// `baseIsLvalue` 由调用方算: Compiler 看 arrayPtr != nullptr (即 baseExpr 是
// 局部变量或 struct 字段链能解析到栈/堆指针); SemaPass 后续 (3.3.2.f) 走镜像逻辑.
//
// 未知方法名时不抛错, 调用方 (Compiler::compileArrayMethodCall) 返回 nullptr
// 让上层 fall-through 到 builtin-type method / sdk method 路径.
//
// 纯 TypeInfo / 字符串比较, 无 LLVM 依赖.
void validateArrayMethodCall(const TypeInfo& baseType, const string& member,
                             size_t argsCount, bool baseIsLvalue,
                             int line, int col);

// `_ptr_offset` 跨模块私有访问检查 (Phase 3.3.3.a, E6023).
//
// 私有 `_`-prefixed 内建 fn 仅允许在声明所在模块内调用. 语义上与 E6006
// (validateFnSymbolVisibility) 重叠, 但 yux 历史给 `_ptr_offset` 这条单独分配了
// 错误码 E6023 以便诊断更精确; 这里保留分支以保持错误码不变.
//
// 调用方:
//   - Compiler::compileExternalOrSdkFunctionCall 在 `_ptr_offset` 分支顶部调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validatePtrOffsetVisibility(const FnSymbolInfo* fnSymbol,
                                  const string& currentModuleName,
                                  int line, int col);

// struct method 跨可见性调用检查 (Phase 3.3.3.a, E6007).
//
// 私有方法 (符号表 `isPrivate=true`, 即 `_` 前缀) 只允许 `self` struct 自身调用.
// `currentStructName` 是调用方所在 impl 的 struct 名 (可能带 `$<泛型实例后缀>`,
// 由 helper 内部剥掉); 不在任何 struct 内时传空串, 此时任何私有方法调用都被拒.
//
// 调用方:
//   - Compiler::compileStructMethodCall 在 methodSymbol 命中且 isPrivate 时调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validateStructMethodVisibility(const FnSymbolInfo* methodSymbol,
                                     const string& currentStructName,
                                     const string& actualTypeName,
                                     const string& member,
                                     int line, int col);

// 函数符号可见性检查 (Phase 3.3.1.c, E6006).
//
// 当 `fnSymbol` 已解析到一个跨模块的私有函数 (`_`-prefixed) 时, 拒绝调用.
// 私有函数: `fnSymbol->isPrivate == true` 且 `moduleName` 非空且与当前 file 模块不同.
// `currentModuleName` 是调用点所在 file 的 moduleName (`file->moduleName()`).
//
// 调用方:
//   - Compiler::compileFunctionCall 在 fnSymbol 命中重载后立即调用
//   - SemaPass.visitExpr 在 ID-callee 路径解析到 fnSymbol 时调用
//
// 纯字符串比较, 无 LLVM 依赖.
void validateFnSymbolVisibility(const FnSymbolInfo* fnSymbol,
                                const string& currentModuleName,
                                const string& fnName,
                                int line, int col);

// 枚举构造表达式形态校验 (Phase 3.4.a, E2019/E2020/E2021/E2032).
//
// 入口: `E::V` / `E::V(args)` 构造点. 调用前 node->setResolvedType(getType()) 已写好
// (Compiler::compileEnumCtorExpr 顶部 / SemaPass.visitExpr 顶部),
// 因此 helper 直接读 node->getType().name 得到经别名解析后的真实 enum 名.
//
// 抛错:
//   - E2019: enum 名找不到 (用 node->enumName().getText() 即用户写法填 payload)
//   - E2020: variant 名不在 enum 内
//   - E2021: arity 不匹配 (零参 / tuple-payload variant 严格相等)
//   - E2032: tuple-payload variant 第 i 个实参类型 != 声明 payload 类型,
//            payload 用 Box<T> / Array<T> / [N]T 等用户友好形式渲染
//
// 实参类型经 argExpr->getType() 计算; 任一参数 getType 抛错时跳过该参数的 E2032 校验
// (典型: lambda 形参未推断 → E3001), 留给 codegen 路径继续报.
//
// 纯 AST / TypeInfo, 无 LLVM 依赖. 调用方:
//   - Compiler::compileEnumCtorExpr 在 setResolvedType 后立即调用
//   - SemaPass.visitExpr ExprEnumCtorNode 分支调用
void validateEnumCtorShape(FileNode* file, FileNode* sdkFile,
                           p<ExprEnumCtorNode> node);

// match 表达式 arm 静态校验 (Phase 3.4.b).
//
// 在调用方已解析 scrutinee 的 enum 名 (含 box-deref / alias) 并 lookup 到 enumDecl
// 之后调用. helper 一次性覆盖以下错误码:
//   - E2023: arms 空 (语法上 +, 防御性) / 不带 else 时穷尽性失败 (列缺失 variant)
//   - E2025: else arm 不在末位
//   - E2019: pattern 的 enum 名既不等于 enumName, 也不能经 file 上一步别名解析到 enumName
//   - E2020: variant 名不在 enumDecl 内
//   - E2024: 同一 variant 在多个 arm 中重复
//   - E2026: arm 绑定 arity 与 variant payload 声明 arity 不一致 (零参允许 0 binds)
//   - E2027: 同一 arm 内绑定名重复
//
// 不覆盖:
//   - E2022 (scrutinee 不是 enum / Box<E> 仅借用语义) —— 调用方 (Compiler) 自身在
//     lookupEnumDecl 失败时抛, 涉及 box-deref / alias / isFreshHandleExpr; SemaPass 暂跳过
//   - E3027 (arm body 结果类型不一致) —— 跨 arm body getType 计算, 可能因 lambda
//     形参未推断而误判, 留 Compiler
//   - E3091/E3096 —— codegen 兜底
//
// 调用方:
//   - Compiler::compileMatchExpr 在 enumDecl 取到后立即调用
//   - SemaPass.visitExpr ExprMatchNode 分支主动调用 (scrutType 直接是 enum 名,
//     非 Box/非 alias 时才接入; 否则跳过, 由 Compiler 兜底)
//
// 纯 AST / 字符串, 无 LLVM 依赖.
void validateMatchArms(EnumDeclNode* enumDecl, const string& enumName,
                       p<ExprMatchNode> node, FileNode* file);

} // namespace sema

#endif //YUX_LANG_SEMA_CALL_RESOLVE_H
