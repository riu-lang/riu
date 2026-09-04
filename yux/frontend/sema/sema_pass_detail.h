// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_PASS_DETAIL_H
#define YUX_LANG_SEMA_PASS_DETAIL_H

// SemaPass 跨 TU 共享的内部 helpers（visitStmt / visitExpr / tryValidate 共用）。
// 仅 sema_pass*.cpp / sema_stmt.cpp / sema_expr.cpp / sema_check.cpp 包含，不对外。

#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace sema::pass {

// getType 对方法点 callee 会把「找不到的方法」回落成基类型，再按非函数符号抛 E3095。
// visitExpr 先记下，Dot 分支报完 E1101/E1140 后再决定是否重抛。ID-literal 不走这里。
bool isMethodPointCall(ExprNode* expr);

// 字段 / 变量当 callee 时：Fn / Rc<Fn> / Ref<Fn> / 别名展开后的 Fn 才是合法调用。
bool isFnCalleeType(const TypeInfo& t, FileNode* file, FileNode* sdkFile);

// Phase 3.3.2.f: 与 Compiler::isBuiltinMethod 等价的本地版本.
// 仅查 sdkFile 的 struct impl (内建运算符方法都注册在 SDK 上), 不存在
// 时返回 false. Sema 不依赖 Compiler 成员, 这里复制规则.
bool isBuiltinMethodIn(FileNode* sdkFile, const string& structName, const string& methodName);

// Phase 3.3.2.f: 近似 Compiler 端 compileArrayMethodCall 的 arrayPtr 计算 ——
// arrayPtr 非空当且仅当 baseExpr AST 形态为:
//   * ID-literal (栈/堆局部变量)
//   * ID-literal.<field> ... 的 Dot 链 (struct 字段直接命名)
// 其余形态 (函数调用、字面量、表达式) 在 Compiler 端 arrayPtr 仍为 nullptr,
// 视为 rvalue. SemaPass 没有 _localVarPtrs, 改走 AST 形态判定; 与 Compiler
// 实际语义等价 (Compiler 也只支持这两种 AST 形态查到栈/堆指针).
bool isLvalueArrayBase(ExprNode* baseExpr);

// 索引赋值 lvalue：与 compileArraySetStatement 同款，只认
//   * 简单变量（LiteralObj）
//   * 单层 `obj.field`（base 也是 LiteralObj）
// 比 isLvalueArrayBase 更严（方法调用允许 Dot 链）；多层 `a.b.c[i] =` 与调用结果 /
// 字面量一样走 E3061。
bool isArraySetLvalue(ExprNode* arrayExpr);

// `<-` LHS：与 compileLvalueAddr 同款，只认
//   * 简单变量 / `$`（LiteralObj）
//   * 递归 Dot 链（字段 `a.b` / `$.x`、元组 `.N`）
// 字面量 / 调用 / 索引 / 括号等走 E4036。与 T 无关，模板期也报。
bool isMoveAssignLvalue(ExprNode* expr);

void validateContainerBansAt(const TypeInfo& t, p<TypeNode> tn, int fallbackLine, int fallbackCol);

// Phase B-1: 与 Compiler::isNoCopyType 等价的本地版本（0 LLVM 依赖）。
// 判定类型是否为 #NoCopy：Array<T> 隐含，或 struct decl 显式标注 #NoCopy。
bool isNoCopyTypeIn(const TypeInfo& type, p<FileNode> file, p<FileNode> sdkFile);

// Phase B-1: 与 Compiler::isFreshHandleExpr（compiler_destructor.cpp）等价的本地版本（0 LLVM 依赖）。
// fresh 表达式自带 +1 所有权，隐式复制路径可安全跳过 retain。
// !! 两处须保持同步 — 新增 case 需两边同时添加 !!
bool isFreshHandleExpr(p<ExprNode> expr);

// 空数组字面量 `[]`：getType 为 `[__empty * 0]`，有靶向类型时应接受。
bool isEmptyArrayType(const TypeInfo& t);

// 块值汇合：`[]` 可与 Array<T> / [T*N] 同型（E3005 / E7010 / match 臂）。
bool blockMergeTypesEq(const TypeInfo& a, const TypeInfo& b);

// 数组填充值是 LiteralNode，不是 ExprNode，不能走 tryInferIntType。
void inferFillLiteralInt(p<LiteralNode> lit, const TypeInfo& target);

sp<TypeInfo> arrayElemTarget(const TypeInfo& t);

// 符号定义在 FileNode（本文件 / SDK / wildcard）上 → 全局，不是闭包捕获。
bool isOuterLocalCapture(ScopeNode* from, SymbolInfo* sym, const string& name);

// 与 Compiler `_localVarPtrs.contains` 对齐：当前 codegen 帧内的槽
// （形参含 `$`、本帧局部、lambda 形参与 lambda 体局部）。
// 全局（FileNode）与 lambda 外层局部 / 外层 `$` 为 false。
bool isCodegenFrameLocal(const string& name, p<Node> from, LambdaExprNode* lambda);

// 与 compileLiteralExpr 捕获门控对齐：标量 / 堆句柄 / T& / Heap? 可捕，其余 E2029。
enum class LambdaCapKind : std::uint8_t { Skip, Scalar, Handle, Ref, HeapNullable, Unsupported };

LambdaCapKind classifyLambdaCapture(const TypeInfo& t);

// Phase C：泛型模板体内仍从 getType 重抛的形态码（不依赖 T 具体化）。
// 其余类型错（E3001 / E3041 / E6016 等）等实例化后再查，此处吞掉。
bool isMorphologicalGenericCode(const char* code);

// 实例化后方法返回类型。nullopt = 方法不存在。
std::optional<TypeInfo> instantiatedMethodRet(FnNode* fn, FileNode* file, FileNode* sdk, const TypeInfo& rawRecv,
                                              const TypeInfo& instRecv, const string& member);

bool receiverHasMethod(const TypeInfo& recv, const string& member, FileNode* file, FileNode* sdk);

// `?.` / `??`：剥 Ref<Nullable<T>>（下标返回 T?&）。
TypeInfo peelRefIfNullable(TypeInfo t);

// `?.` 接收者：已确认 Nullable 后，取内层并 Rc 自动 deref（与 getType / compileSafeDotExpr 同款）。
TypeInfo peelSafeDotInner(TypeInfo t);

// `<T : D>` 边界上的方法：实例化后仍按边界认，不要求具体类型自己登记同名方法。
bool typeParamBoundHasMethod(FnNode* fn, FileNode* file, FileNode* sdk, const string& typeParam, const string& member);

const char* binOpE3001Kind(const string& methodName);

void collectOverloadsBoth(FileNode* file, FileNode* sdk, const string& name, vector<FnSymbolInfo*>& out);

bool sameFnSig(const FnSymbolInfo* a, const FnSymbolInfo* b);

// 同 arity（语义去重后）候选在 skip 之后各位形参的约定类型。
// 仅一位 → 用它的全部形参；多位时只填各位都相同的类型，不一致留空
// （visitExprList 会跳过空位）。没有任何可填位置 → false。
// 不猜重载：各位类型不一致时不下钻，避免绑错候选。
bool agreedArityParamTypes(const vector<FnSymbolInfo*>& cands, size_t wantArity, size_t skip, vector<TypeInfo>& out);

// 镜像 Compiler::compileCallExpr / compileDeclareAssignStatement：把 Fn 期望类型
// 写到 lambda，并回填 bodyScope 未标注形参，让随后下钻能做形态检查。
void applyLambdaFnExpected(p<LambdaExprNode> lam, const TypeInfo& fnTy);

// 显式 retType 优先，否则用上下文反推的 Fn 返回类型（nullptr = void）。
// 两者都没有 → false（尚无期望，不比类型）。
bool lambdaExpectedRetType(p<LambdaExprNode> lam, TypeInfo& out);

struct RetCheck {
    FileNode* file = nullptr;
    FileNode* sdk = nullptr;
    FnNode* fn = nullptr;
    string structName;
    string fallibleErr; // 空 = 非 #Fallible
    const std::set<std::string>* typeParams = nullptr;
    const std::map<std::string, TypeInfo>* subst = nullptr;
};

TypeInfo substSelfType(TypeInfo t, const string& structName);

TypeInfo resolveForRet(const TypeInfo& t, const RetCheck& ctx);

bool tryGetExprType(p<ExprNode> expr, TypeInfo& out);

SymbolInfo* lookupRetVar(const string& name, p<Node> n, FnNode* fn);

// 形态上合法的 T& 返回源：`$` / T& 变量 / `&expr` / 类型本身就是 T&（调用等）。
// 成功时 srcInner 为剥 Ref 后的内层；找不到源 → false。
bool refRetSourceInner(p<ExprNode> expr, FnNode* fn, TypeInfo& srcInner);

bool isAssignTypeParam(const TypeInfo& t, const std::set<std::string>& typeParams);

TypeInfo applySubstMap(const TypeInfo& t, const std::map<std::string, TypeInfo>* subst);

// 模板体：T / Array<T> 等仍不透明。实例化后 subst 已把 T 换成具体类型，继续比。
bool stillTemplateType(const TypeInfo& t, const std::set<std::string>& typeParams,
                       const std::map<std::string, TypeInfo>* subst);

// Phase C：元组解构 E3101 / E3102。
// 与 compileDeclareAssignTupleStatement / loop init 对齐：
//   标注类型优先，否则 RHS 推断；subst + resolveAlias 后再判 isTuple。
//   非元组 → E3101；元素数 ≠ 名字数 → E3102。
// 模板形参 T（含 T& / Rc<T> 剥后仍是 T）等实例化后再查；Array<T> 永远不是元组，模板期也报。
void checkTupleDestructure(p<ExprNode> expr, p<TypeNode> annotated, size_t nameCount, int line, int col, FileNode* file,
                           FileNode* sdk, const std::set<std::string>& typeParams,
                           const std::map<std::string, TypeInfo>* subst);

string genericInstKey(const void* p, const std::map<std::string, TypeInfo>& subst);

// Phase C：ret 表达式 E3014。Fallible 成功/错误双通道、T& 形态、Nullable wrap、
// 别名 resolveAlias、灵活整数推断。spec 体里未解析的 Self 仍跳过。
void checkRetExpr(p<ExprNode> expr, const TypeInfo& declRet, bool hasDeclRet, int line, const RetCheck& ctx);

bool isKnownAssignType(const TypeInfo& t, FileNode* file, FileNode* sdk);

// Phase C：赋值 RHS 相对存储槽类型的 E3014。
// T& 局部 / `$`（Self&）是 store-through：caller 已 peelRef，want 是内层 T。
// Nullable wrap / Rc wrap / 空数组 / 灵活整数与 Compiler 赋值路径对齐。
void checkAssignRhs(p<ExprNode> expr, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                    const std::set<std::string>& typeParams, const std::map<std::string, TypeInfo>* subst = nullptr);

// Phase C：声明处 Rc / Weak / Array 构造形态。
// 与 compileDeclareAssignStatement 对齐：
//   Rc：同型句柄或内层 T 包装 → E3014（走 checkAssignRhs）
//   Weak：仅 Rc<T> / Weak<T> → E3016
//   Array<T>：仅 Array 表达式或数组字面量 → E3064（不查元素类型，与 Compiler 一致）
// Heap 声明非 `heap:<T>(...)` 由 borrow checker E4024 先报，不在这里重复。
// 模板形参等实例化后再查。须在 visitExpr 带靶向类型之后调用。
void checkDeclareHandleRhs(p<ExprNode> expr, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                           const std::set<std::string>& typeParams,
                           const std::map<std::string, TypeInfo>* subst = nullptr);

// Phase C：调用实参相对实例化后形参的 E3014。
// 与赋值的差别：实参不自动解引用（T& 传给 T 要 copy_of）；值传给 T& 允许自动取址。
void checkCallArgAgainst(p<ExprNode> arg, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                         const std::set<std::string>& typeParams,
                         const std::map<std::string, TypeInfo>* subst = nullptr);

bool fillSubstFromTypeNodes(const vector<string>& typeParams, const vector<p<TypeNode>>& typeArgNodes,
                            map<string, TypeInfo>& subst);

bool fillSubstFromGenericArgs(const vector<string>& typeParams, const vector<sp<TypeInfo>>& genericArgs,
                              map<string, TypeInfo>& subst);

bool substHeaderParams(FnHeaderNode* header, const map<string, TypeInfo>& subst, vector<TypeInfo>& out);

FnNode* uniqueNonBuiltinGenericFn(FileNode* file, const string& name);

StructImplNode* lookupStructImpl(FileNode* file, FileNode* sdk, const string& name);

StructImplNode* lookupStructImpl(FileNode* file, FileNode* sdk, const TypeInfo& t);

// 同名同 arity 唯一方法。多个重载不猜。
FnHeaderNode* uniqueMethodHeader(StructImplNode* impl, const string& name, size_t arity, bool wantStatic);

bool substGenericCallParams(FnHeaderNode* header, const vector<string>& typeParams, const vector<TypeInfo>& typeArgs,
                            vector<TypeInfo>& out);

// Phase C：match 各臂结果类型须一致（镜像 compileMatchExpr）。流终止臂跳过。
// 模板体里类型参数 / 含 T 的复合类型不下钻，留给实例化期。
void checkMatchArmTypes(const vector<p<MatchArmNode>>& arms, const std::set<std::string>& typeParams,
                        const std::map<std::string, TypeInfo>* subst = nullptr);

// 块体末位无 `;` 的裸表达式语句（隐式尾值），不含 ret / 声明 / 赋值子类。
bool isBareTailExprStmt(p<StatementNode> s);

// #Static fn 同名候选：过滤 wantArity，各位约定类型与 agreedArityParamTypes 同款。
// 任一同名泛型静态方法 → 不猜。
bool agreedStaticMethodParams(FileNode* file, FileNode* sdk, const TypeInfo& lhs, const string& rhs, size_t wantArity,
                              vector<TypeInfo>& out);

void copyFnParamTypes(const TypeInfo& fnTy, vector<TypeInfo>& out);

// DRAFT-spec-reflect §6：把 Field 表达式追溯到 `{structDecl, fieldIndex}`。
// 认 `Type::fields[N]` / `Type::fields.get(N)`（N 字面量）以及 fn 体内 let 绑定。
// 对不上 → `{nullptr, -1}`（运行期 Field → E3133）。
std::pair<const StructDeclNode*, int> tryResolveReflectField(ExprNode* expr);

} // namespace sema::pass

#endif // YUX_LANG_SEMA_PASS_DETAIL_H
