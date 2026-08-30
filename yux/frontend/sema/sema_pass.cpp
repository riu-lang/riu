// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 实现 —— 详见 sema_pass.h
//
// Phase 3.2a：visitExpr 在每个表达式节点上写入 `setResolvedType(getType())`,
// 覆盖范围扩到 file 顶层 fn body + struct impl 的方法/析构 body。
// #Builtin 仍跳过。Phase C：泛型模板体也 walk（类型参数不透明）。
//
// Phase B：SemaPass 为 getType 诊断的权威抛出点。默认重抛所有 YuxError。
// Phase C：泛型 fn/impl 体再吞一批依赖 T 具体化的码（见 isMorphologicalGenericCode）。
// 方法点 callee 的 E3095：getType 会把找不到的方法回落成基类型再抛「不是函数」，
// 挡住 E1101/E1140，故先按形态记下，Dot 分支校验 @Spec 后再决定重抛。
// 字段非 Fn 值由 Dot 分支直接报 E3095。ID-literal 的 E3095 仍立即重抛。
// 非 YuxError 在 debug 下 assert，禁止静默吞。

#include "sema/sema_pass.h"
#include "sema/builtin_methods.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>

#include "analyzer/borrow_checker.h"
#include "analyzer/const_mut_checker.h"
#include "analyzer/flow_terminate_checker.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"
#include "ast/yux.h"
#include "tools/diagnostic.h"
#include "types.h"

namespace {
// getType 对方法点 callee 会把「找不到的方法」回落成基类型，再按非函数符号抛 E3095。
// visitExpr 先记下，Dot 分支报完 E1101/E1140 后再决定是否重抛。ID-literal 不走这里。
bool isMethodPointCall(ExprNode* expr) {
    auto* call = dynamic_cast<ExprCallNode*>(expr);
    return call && dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
}

// 字段 / 变量当 callee 时：Fn / Rc<Fn> / Ref<Fn> / 别名展开后的 Fn 才是合法调用。
bool isFnCalleeType(const TypeInfo& t, FileNode* file, FileNode* sdkFile) {
    TypeInfo u = sema::resolveAlias(t, file, sdkFile);
    if (u.isFn()) return true;
    if (u.isRc()) {
        if (auto inner = u.rcElementType()) return sema::resolveAlias(*inner, file, sdkFile).isFn();
    }
    if (u.isRef()) {
        if (auto inner = u.refElementType()) return sema::resolveAlias(*inner, file, sdkFile).isFn();
    }
    return false;
}

// Phase 3.3.2.f: 与 Compiler::isBuiltinMethod 等价的本地版本.
// 仅查 sdkFile 的 struct impl (内建运算符方法都注册在 SDK 上), 不存在
// 时返回 false. Sema 不依赖 Compiler 成员, 这里复制规则.
bool isBuiltinMethodIn(FileNode* sdkFile, const string& structName, const string& methodName) {
    if (!sdkFile) return false;
    auto structImpl = sdkFile->getStructImpl(structName);
    if (!structImpl) return false;
    for (auto& m : structImpl->methods()) {
        if (m->header()->name().getText() == methodName) {
            return m->header()->hasAnno("Builtin");
        }
    }
    return false;
}

// Phase 3.3.2.f: 近似 Compiler 端 compileArrayMethodCall 的 arrayPtr 计算 ——
// arrayPtr 非空当且仅当 baseExpr AST 形态为:
//   * ID-literal (栈/堆局部变量)
//   * ID-literal.<field> ... 的 Dot 链 (struct 字段直接命名)
// 其余形态 (函数调用、字面量、表达式) 在 Compiler 端 arrayPtr 仍为 nullptr,
// 视为 rvalue. SemaPass 没有 _localVarPtrs, 改走 AST 形态判定; 与 Compiler
// 实际语义等价 (Compiler 也只支持这两种 AST 形态查到栈/堆指针).
bool isLvalueArrayBase(ExprNode* baseExpr) {
    while (baseExpr) {
        if (auto lit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            return dynamic_cast<LiteralObjNode*>(lit->literal()) != nullptr;
        }
        if (auto dot = dynamic_cast<ExprDotNode*>(baseExpr)) {
            baseExpr = dot->baseExpr();
            continue;
        }
        return false;
    }
    return false;
}

void validateContainerBansAt(const TypeInfo& t, p<TypeNode> tn, int fallbackLine, int fallbackCol) {
    if (!tn) return;
    int line = tn->getLineNumber();
    int col = tn->getColumn();
    if (line <= 0) line = fallbackLine > 0 ? fallbackLine : 1;
    if (col < 0) col = fallbackCol;
    validateRcContainerBans(t, line, col);
}

// Phase B-1: 与 Compiler::isNoCopyType 等价的本地版本（0 LLVM 依赖）。
// 判定类型是否为 #NoCopy：Array<T> 隐含，或 struct decl 显式标注 #NoCopy。
bool isNoCopyTypeIn(const TypeInfo& type, p<FileNode> file, p<FileNode> sdkFile) {
    if (isBuiltinType(type.name)) return false;
    if (type.isRc() || type.isWeak() || type.isHeap()) return false;
    if (type.isRef() || type.isPtr()) return false;
    if (type.isArrayGeneric()) return true; // Array<T> 隐含 #NoCopy

    auto* decl = sema::NameResolver(file, sdkFile).lookupStruct(type.baseStructName(), true);
    if (decl && decl->hasAnno("NoCopy")) return true;
    return false;
}

// Phase B-1: 与 Compiler::isFreshHandleExpr（compiler_destructor.cpp）等价的本地版本（0 LLVM 依赖）。
// fresh 表达式自带 +1 所有权，隐式复制路径可安全跳过 retain。
// !! 两处须保持同步 — 新增 case 需两边同时添加 !!
bool isFreshHandleExpr(p<ExprNode> expr) {
    if (!expr) return false;
    if (dynamic_cast<p<ExprCallNode>>(expr)) return true;       // 函数调用结果 / builtin intrinsic
    if (dynamic_cast<p<ExprArrayNode>>(expr)) return true;      // 数组字面量
    if (dynamic_cast<p<ExprPathCallNode>>(expr)) return true;   // 枚举构造器 / #Static fn 调用
    if (dynamic_cast<p<ExprMoveAssignNode>>(expr)) return true; // move-assign 结果
    if (dynamic_cast<p<LambdaExprNode>>(expr)) return true;     // lambda 字面量
    if (dynamic_cast<p<ExprStructLitNode>>(expr)) return true;  // struct 字面量 (Self { ... })
    return false;
}

// 空数组字面量 `[]`：getType 为 `[__empty * 0]`，有靶向类型时应接受。
bool isEmptyArrayType(const TypeInfo& t) {
    return t.isArray() && t.elementType && t.elementType->name == "__empty";
}

// 数组填充值是 LiteralNode，不是 ExprNode，不能走 tryInferIntType。
void inferFillLiteralInt(p<LiteralNode> lit, const TypeInfo& target) {
    if (!lit) return;
    if (auto ilit = dynamic_cast<p<LiteralIntNode>>(lit)) {
        if (!ilit->hasSuffix() && isIntTypeName(target.name)) {
            ilit->setType(target);
        }
    }
}

sp<TypeInfo> arrayElemTarget(const TypeInfo& t) {
    if (t.isArray()) return t.elementType;
    if (t.isArrayGeneric()) return t.arrayGenericElementType();
    return nullptr;
}

// Phase C：泛型模板体内仍从 getType 重抛的形态码（不依赖 T 具体化）。
// 其余类型错（E3001 / E3041 / E6016 等）等实例化后再查，此处吞掉。
bool isMorphologicalGenericCode(const char* code) {
    if (!code) return false;
    std::string_view sv(code);
    constexpr std::array<std::string_view, 16> kKeep = {
        "E3030",          // 未定义符号
        "E6010", "E6011", // 泛型 arity
        "E4031", "E4032", // #NoCopy
        "E3103",          // 整数字面量越界
        "E2033",          // 非法转义
        "E4025", "E1132", // 容器禁令
        "E2016", "E2017", // 别名
        "E3130",          // 同名 ctor 定义
        "E3128",          // #Static 体内 $
        "E2030",          // lambda 捕获赋值
        "E4033",          // use-after-move
        "E3095",          // 类型名 / 非函数当 callee（方法点在 Dot 分支延迟重抛）
    };
    for (auto c : kKeep) {
        if (sv == c) return true;
    }
    return false;
}

bool typeParamBoundHasMethod(FnNode* fn, FileNode* file, FileNode* sdk, const string& typeParam, const string& member);

// 实例化后方法返回类型。nullopt = 方法不存在。
std::optional<TypeInfo> instantiatedMethodRet(FnNode* fn, FileNode* file, FileNode* sdk, const TypeInfo& rawRecv,
                                              const TypeInfo& instRecv, const string& member) {
    TypeInfo peeledRaw = rawRecv.peelAutoDeref();
    if (typeParamBoundHasMethod(fn, file, sdk, peeledRaw.name, member) && fn && fn->header()) {
        auto hdr = fn->header();
        const auto& tps = hdr->typeParams();
        const auto& bounds = hdr->typeParamBounds();
        size_t idx = SIZE_MAX;
        for (size_t i = 0; i < tps.size(); ++i) {
            if (tps[i] == peeledRaw.name) {
                idx = i;
                break;
            }
        }
        if (idx != SIZE_MAX && idx < bounds.size()) {
            for (auto& dname : bounds[idx]) {
                SpecDeclNode* draft = file ? file->getSpecDecl(dname) : nullptr;
                if (!draft && sdk && sdk != file) draft = sdk->getSpecDecl(dname);
                if (!draft) continue;
                for (auto& sig : draft->signatures()) {
                    if (!sig || sig->name().getText() != member) continue;
                    if (sig->retType()) return sig->retType()->getType();
                    return TypeInfo();
                }
            }
        }
    }
    TypeInfo t = instRecv.peelAutoDeref();
    if (t.isArray() || t.isArrayGeneric()) {
        if (auto* spec = sema::lookupInstanceBuiltin(t, member)) {
            return sema::builtinMethodReturnType(*spec, t);
        }
        return std::nullopt;
    }
    if (member.starts_with("to_")) {
        string dst = member.substr(3);
        if (isBuiltinType(dst)) return TypeInfo(dst);
    }
    if (t.name.empty()) return std::nullopt;
    auto* fsym = sema::NameResolver(file, sdk).lookupFn(t.name + "." + member);
    if (!fsym) return std::nullopt;
    return fsym->retType;
}

bool receiverHasMethod(const TypeInfo& recv, const string& member, FileNode* file, FileNode* sdk) {
    return instantiatedMethodRet(nullptr, file, sdk, recv, recv, member).has_value();
}

// `<T : D>` 边界上的方法：实例化后仍按边界认，不要求具体类型自己登记同名方法。
bool typeParamBoundHasMethod(FnNode* fn, FileNode* file, FileNode* sdk, const string& typeParam, const string& member) {
    if (!fn || !fn->header() || member.empty()) return false;
    auto hdr = fn->header();
    const auto& tps = hdr->typeParams();
    const auto& bounds = hdr->typeParamBounds();
    size_t idx = SIZE_MAX;
    for (size_t i = 0; i < tps.size(); ++i) {
        if (tps[i] == typeParam) {
            idx = i;
            break;
        }
    }
    if (idx == SIZE_MAX || idx >= bounds.size()) return false;
    for (auto& dname : bounds[idx]) {
        SpecDeclNode* draft = file ? file->getSpecDecl(dname) : nullptr;
        if (!draft && sdk && sdk != file) draft = sdk->getSpecDecl(dname);
        if (!draft) continue;
        for (auto& sig : draft->signatures()) {
            if (sig && sig->name().getText() == member) return true;
        }
    }
    return false;
}

const char* binOpE3001Kind(const string& methodName) {
    if (methodName == "plus" || methodName == "minus") return "arithmetic";
    if (methodName == "mul" || methodName == "div" || methodName == "mod") return "mul/div/mod";
    if (methodName == "and" || methodName == "or" || methodName == "xor" || methodName == "shl" || methodName == "shr")
        return "bitwise";
    if (methodName == "eq" || methodName == "ne" || methodName == "lt" || methodName == "le" || methodName == "gt" ||
        methodName == "ge")
        return "comparison";
    return "arithmetic";
}

void collectOverloadsBoth(FileNode* file, FileNode* sdk, const string& name, vector<FnSymbolInfo*>& out) {
    if (file) file->collectFnOverloads(name, out);
    if (sdk && sdk != file) sdk->collectFnOverloads(name, out);
}

bool sameFnSig(const FnSymbolInfo* a, const FnSymbolInfo* b) {
    if (!a || !b) return a == b;
    if (a->moduleName != b->moduleName) return false;
    if (a->params.size() != b->params.size()) return false;
    for (size_t i = 0; i < a->params.size(); ++i) {
        if (!(a->params[i] == b->params[i])) return false;
    }
    return true;
}

// 同 arity（语义去重后）候选在 skip 之后各位形参的约定类型。
// 仅一位 → 用它的全部形参；多位时只填各位都相同的类型，不一致留空
// （visitExprList 会跳过空位）。没有任何可填位置 → false。
// 不猜重载：各位类型不一致时不下钻，避免绑错候选。
bool agreedArityParamTypes(const vector<FnSymbolInfo*>& cands, size_t wantArity, size_t skip, vector<TypeInfo>& out) {
    vector<FnSymbolInfo*> uniq;
    for (auto* c : cands) {
        if (!c || c->params.size() != wantArity) continue;
        bool dup = false;
        for (auto* u : uniq) {
            if (sameFnSig(u, c)) {
                dup = true;
                break;
            }
        }
        if (!dup) uniq.push_back(c);
    }
    if (uniq.empty() || skip > wantArity) return false;
    const size_t n = wantArity - skip;
    out.assign(n, TypeInfo());
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        const TypeInfo& t0 = uniq[0]->params[skip + i];
        bool all = true;
        for (size_t k = 1; k < uniq.size(); ++k) {
            if (!(uniq[k]->params[skip + i] == t0)) {
                all = false;
                break;
            }
        }
        if (all && !t0.empty()) {
            out[i] = t0;
            any = true;
        }
    }
    return any;
}

// 镜像 Compiler::compileCallExpr / compileDeclareAssignStatement：把 Fn 期望类型
// 写到 lambda，并回填 bodyScope 未标注形参，让随后下钻能做形态检查。
void applyLambdaFnExpected(p<LambdaExprNode> lam, const TypeInfo& fnTy) {
    if (!lam || !fnTy.isFn()) return;
    lam->setInferredFnType(fnTy);
    auto sc = lam->bodyScope();
    if (!sc) return;
    const auto& fps = fnTy.fnParamTypes();
    for (size_t k = 0; k < lam->params().size() && k < fps.size(); ++k) {
        if (lam->params()[k].type) continue;
        if (auto* psym = sc->lookupSymbol(lam->params()[k].name.getText())) {
            if (fps[k]) psym->type = *fps[k];
        }
    }
}

// 显式 retType 优先，否则用上下文反推的 Fn 返回类型（nullptr = void）。
// 两者都没有 → false（尚无期望，不比类型）。
bool lambdaExpectedRetType(p<LambdaExprNode> lam, TypeInfo& out) {
    if (!lam) return false;
    if (lam->retType()) {
        try {
            out = lam->retType()->getType();
            return true;
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return false;
        }
    }
    if (lam->inferredFnType().isFn()) {
        if (auto rt = lam->inferredFnType().fnReturnType())
            out = *rt;
        else
            out = TypeInfo();
        return true;
    }
    return false;
}

struct RetCheck {
    FileNode* file = nullptr;
    FileNode* sdk = nullptr;
    FnNode* fn = nullptr;
    string structName;
    string fallibleErr; // 空 = 非 #Fallible
    const std::set<std::string>* typeParams = nullptr;
    const std::map<std::string, TypeInfo>* subst = nullptr;
};

TypeInfo substSelfType(TypeInfo t, const string& structName) {
    if (structName.empty()) return t;
    if (t.isSelf()) {
        t.name = structName;
        return t;
    }
    if (t.isRef() && t.genericArgs.size() == 1 && t.genericArgs[0] && t.genericArgs[0]->isSelf()) {
        auto inner = *t.genericArgs[0];
        inner.name = structName;
        return TypeInfo("Ref", {make_shared<TypeInfo>(std::move(inner))});
    }
    return t;
}

TypeInfo resolveForRet(const TypeInfo& t, const RetCheck& ctx) {
    return sema::resolveAlias(substSelfType(t, ctx.structName), ctx.file, ctx.sdk);
}

bool tryGetExprType(p<ExprNode> expr, TypeInfo& out) {
    if (!expr) return false;
    if (expr->hasResolvedType()) {
        out = expr->resolvedType();
        return true;
    }
    try {
        out = expr->getType();
        return true;
    } catch (const YuxError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return false;
    }
}

SymbolInfo* lookupRetVar(const string& name, p<Node> n, FnNode* fn) {
    if (n) {
        if (auto sc = n->findNearestScope()) {
            if (auto* s = sc->lookupSymbol(name)) return s;
        }
    }
    return fn ? fn->lookupSymbol(name) : nullptr;
}

// 形态上合法的 T& 返回源：`$` / T& 变量 / `&expr` / 类型本身就是 T&（调用等）。
// 成功时 srcInner 为剥 Ref 后的内层；找不到源 → false。
bool refRetSourceInner(p<ExprNode> expr, FnNode* fn, TypeInfo& srcInner) {
    if (auto litExpr = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        if (auto objLit = dynamic_cast<p<LiteralObjNode>>(litExpr->literal())) {
            auto vname = objLit->getValue().getText();
            auto* sym = lookupRetVar(vname, expr, fn);
            const bool isDollar = (vname == "$");
            const bool isRefVar = sym && sym->type.isRef();
            if (isDollar || isRefVar) {
                if (isDollar) {
                    srcInner = sym ? sym->type : TypeInfo();
                    if (srcInner.isRef()) {
                        if (auto in = srcInner.refElementType()) srcInner = *in;
                    }
                } else if (auto in = sym->type.refElementType()) {
                    srcInner = *in;
                }
                return true;
            }
        }
    }
    if (auto getRef = dynamic_cast<p<ExprGetRefNode>>(expr)) {
        TypeInfo t;
        if (!tryGetExprType(getRef, t)) return false;
        if (t.isRef()) {
            if (auto in = t.refElementType()) srcInner = *in;
        }
        return true;
    }
    TypeInfo t;
    if (!tryGetExprType(expr, t) || !t.isRef()) return false;
    if (auto in = t.refElementType()) srcInner = *in;
    return true;
}

bool isAssignTypeParam(const TypeInfo& t, const std::set<std::string>& typeParams) {
    if (typeParams.empty()) return false;
    TypeInfo peeled = t.peelAutoDeref();
    return peeled.isNormal() && !peeled.name.empty() && typeParams.count(peeled.name) > 0;
}

TypeInfo applySubstMap(const TypeInfo& t, const std::map<std::string, TypeInfo>* subst) {
    if (!subst || subst->empty()) return t;
    return t.substitute(*subst);
}

// 模板体：T / Array<T> 等仍不透明。实例化后 subst 已把 T 换成具体类型，继续比。
bool stillTemplateType(const TypeInfo& t, const std::set<std::string>& typeParams,
                       const std::map<std::string, TypeInfo>* subst) {
    if (isAssignTypeParam(t, typeParams)) return true;
    if (subst && !subst->empty()) return false;
    return !typeParams.empty() && !t.genericArgs.empty();
}

string genericInstKey(const void* p, const std::map<std::string, TypeInfo>& subst) {
    string k = std::format("{}", p);
    k += '{';
    for (auto& [name, ty] : subst) {
        k += name;
        k += '=';
        k += ty.getMangleName();
        k += ',';
    }
    k += '}';
    return k;
}

// Phase C：ret 表达式 E3014。Fallible 成功/错误双通道、T& 形态、Nullable wrap、
// 别名 resolveAlias、灵活整数推断。spec 体里未解析的 Self 仍跳过。
void checkRetExpr(p<ExprNode> expr, const TypeInfo& declRet, bool hasDeclRet, int line, const RetCheck& ctx) {
    if (!expr) return;
    TypeInfo decl = applySubstMap(declRet, ctx.subst);
    if (hasDeclRet && decl.isSelf() && ctx.structName.empty()) return;

    if (hasDeclRet) {
        auto resolvedDecl = resolveForRet(decl, ctx);
        if (isIntTypeName(resolvedDecl.name) && isFlexibleIntExpr(expr)) {
            tryInferIntType(expr, resolvedDecl);
        }
        if (resolvedDecl.isNullable()) {
            if (auto inner = resolvedDecl.nullableInnerType()) {
                if (isIntTypeName(inner->name) && isFlexibleIntExpr(expr)) tryInferIntType(expr, *inner);
            }
        }
    }

    TypeInfo retType;
    if (!tryGetExprType(expr, retType)) return;
    retType = applySubstMap(retType, ctx.subst);
    if (ctx.typeParams) {
        if (hasDeclRet && stillTemplateType(decl, *ctx.typeParams, ctx.subst)) return;
        if (stillTemplateType(retType, *ctx.typeParams, ctx.subst)) return;
    }

    if (!ctx.fallibleErr.empty()) {
        auto resolvedRet = resolveForRet(retType, ctx);
        bool isSuccess = hasDeclRet && (resolvedRet == resolveForRet(decl, ctx));
        bool isError = (resolvedRet.name == ctx.fallibleErr);
        if (!isSuccess && !isError) {
            throw YuxError(line, ErrorCode::E3014, hasDeclRet ? decl.getFullName() : string("void"),
                           retType.getFullName());
        }
        return;
    }

    if (hasDeclRet && decl.isRef()) {
        TypeInfo srcInner;
        if (!refRetSourceInner(expr, ctx.fn, srcInner)) {
            throw YuxError(line, ErrorCode::E3014, decl.getFullName(), retType.getFullName())
                .withHint("返回 T& 时，ret 表达式应为 `$` / T& 变量 / `&expr` / 返回 T& 的调用");
        }
        srcInner = applySubstMap(srcInner, ctx.subst);
        auto declInner = substSelfType(decl, ctx.structName).refElementType();
        TypeInfo declInnerResolved = declInner ? resolveForRet(*declInner, ctx) : TypeInfo();
        srcInner = resolveForRet(srcInner, ctx);
        if (declInner && !srcInner.empty() && declInnerResolved != srcInner) {
            throw YuxError(line, ErrorCode::E3014, decl.getFullName(), (srcInner.name + "&"));
        }
        return;
    }

    if (hasDeclRet && decl.isNullable()) {
        auto resolvedDecl = resolveForRet(decl, ctx);
        auto innerType = resolvedDecl.nullableInnerType();
        if (innerType && isFlexibleNullExpr(expr)) return;
        if (innerType) {
            auto resolvedRet = resolveForRet(retType, ctx);
            bool wholeCopy = resolvedRet.isNullable() && resolvedRet == resolvedDecl;
            bool wrap = resolvedRet == resolveForRet(*innerType, ctx);
            if (wholeCopy || wrap) return;
        }
        throw YuxError(line, ErrorCode::E3014, decl.getFullName(),
                       retType.empty() ? string("void") : retType.getFullName());
    }

    if (hasDeclRet) {
        if (retType.empty()) {
            throw YuxError(line, ErrorCode::E3014, decl.getFullName(), "void");
        }
        if (resolveForRet(retType, ctx) != resolveForRet(decl, ctx)) {
            throw YuxError(line, ErrorCode::E3014, decl.getFullName(), retType.getFullName());
        }
    } else if (!retType.empty()) {
        throw YuxError(line, ErrorCode::E3014, "void", retType.getFullName());
    }
}

bool isKnownAssignType(const TypeInfo& t, FileNode* file, FileNode* sdk) {
    if (t.isTuple() || t.isArray() || t.isArrayGeneric() || t.isNullable() || t.isRc() || t.isWeak() || t.isHeap() ||
        t.isFn() || t.isRef() || t.isPtr() || t.isDyn())
        return true;
    if (t.name.empty() || t.isSelf()) return false;
    if (isBuiltinType(t.name)) return true;
    if (file && (file->getStructDecl(t.name) || file->getEnumDecl(t.name))) return true;
    if (sdk && sdk != file && (sdk->getStructDecl(t.name) || sdk->getEnumDecl(t.name))) return true;
    return false;
}

// Phase C：赋值 RHS 相对存储槽类型的 E3014。
// T& 局部 / `$`（Self&）是 store-through：caller 已 peelRef，want 是内层 T。
// Nullable wrap / Rc wrap / 空数组 / 灵活整数与 Compiler 赋值路径对齐。
void checkAssignRhs(p<ExprNode> expr, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                    const std::set<std::string>& typeParams, const std::map<std::string, TypeInfo>* subst = nullptr) {
    if (!expr) return;
    TypeInfo w0 = applySubstMap(want, subst);
    if (w0.empty() || w0.isSelf() || w0.isFn()) return;
    if (stillTemplateType(w0, typeParams, subst)) return;
    if (isFlexibleIntExpr(expr) && isIntTypeName(w0.name)) return;

    TypeInfo got;
    if (!tryGetExprType(expr, got)) return;
    TypeInfo g0 = applySubstMap(got, subst);
    if (g0.empty() || g0.isSelf() || stillTemplateType(g0, typeParams, subst)) return;

    auto g = sema::resolveAlias(g0, file, sdk);
    auto w = sema::resolveAlias(w0, file, sdk);
    if (g == w) return;
    if (isEmptyArrayType(g) && (w.isArray() || w.isArrayGeneric())) return;
    if (w.isNullable()) {
        if (isFlexibleNullExpr(expr)) return;
        if (auto inner = w.nullableInnerType()) {
            auto in = sema::resolveAlias(*inner, file, sdk);
            if (isIntTypeName(in.name) && isFlexibleIntExpr(expr)) {
                tryInferIntType(expr, in);
                if (sema::resolveAlias(expr->getType(), file, sdk) == in) return;
            }
            if (g == in) return;
        }
        throw YuxError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
            .withHint(std::format("赋值目标为 `{}`，表达式为 `{}`；T? 只接受 `null` / 内层 T / 同型 T?",
                                  w0.getFullName(), g0.getFullName()));
    }
    if (w.isRc()) {
        if (auto inner = w.rcElementType()) {
            auto in = sema::resolveAlias(*inner, file, sdk);
            if (isIntTypeName(in.name) && isFlexibleIntExpr(expr)) {
                tryInferIntType(expr, in);
                if (sema::resolveAlias(expr->getType(), file, sdk) == in) return;
            }
            if (g == in) return;
        }
        throw YuxError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
            .withHint(std::format("赋值目标为 `{}`，表达式为 `{}`；Rc<T> 只接受同型句柄或内层 T", w0.getFullName(),
                                  g0.getFullName()));
    }
    if (!isKnownAssignType(w, file, sdk) || !isKnownAssignType(g, file, sdk)) return;
    throw YuxError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
        .withHint(std::format("赋值目标类型为 `{}`，但表达式类型为 `{}`；yux 无隐式类型转换", w0.getFullName(),
                              g0.getFullName()));
}

// Phase C：调用实参相对实例化后形参的 E3014。
// 与赋值的差别：实参不自动解引用（T& 传给 T 要 copy_of）；值传给 T& 允许自动取址。
void checkCallArgAgainst(p<ExprNode> arg, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                         const std::set<std::string>& typeParams,
                         const std::map<std::string, TypeInfo>* subst = nullptr) {
    if (!arg) return;
    TypeInfo w0 = applySubstMap(want, subst);
    if (w0.empty() || w0.isSelf() || w0.isFn()) return;
    if (stillTemplateType(w0, typeParams, subst)) return;

    TypeInfo peeledWant = w0.peelRef();
    if (isFlexibleIntExpr(arg) && isIntTypeName(peeledWant.name)) return;

    TypeInfo got;
    if (!tryGetExprType(arg, got)) return;
    TypeInfo g0 = applySubstMap(got, subst);
    if (g0.empty() || g0.isSelf() || stillTemplateType(g0, typeParams, subst)) return;

    auto g = sema::resolveAlias(g0, file, sdk);
    auto w = sema::resolveAlias(w0, file, sdk);
    if (g == w) return;
    if (w.isRef()) {
        if (auto inner = w.refElementType()) {
            if (g == sema::resolveAlias(*inner, file, sdk)) return;
        }
    }
    if (g.isRef() && !w.isRef()) {
        auto inner = g.refElementType();
        throw YuxError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
            .withHint(std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                  "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                  inner ? inner->name : "?", w0.getFullName(), inner ? inner->name : "?",
                                  inner ? inner->name : "?"));
    }
    if (isEmptyArrayType(g) && (w.isArray() || w.isArrayGeneric())) return;
    if (w.isNullable()) {
        if (isFlexibleNullExpr(arg)) return;
        if (auto inner = w.nullableInnerType()) {
            auto in = sema::resolveAlias(*inner, file, sdk);
            if (isIntTypeName(in.name) && isFlexibleIntExpr(arg)) return;
            if (g == in) return;
        }
    }
    if (!isKnownAssignType(w, file, sdk) || !isKnownAssignType(g, file, sdk)) return;
    int eline = arg->resolveLineNumber();
    int ecol = arg->resolveColumn();
    if (eline <= 0) eline = line;
    if (ecol < 0) ecol = col;
    throw YuxError(eline, ecol, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", g0.getFullName(), w0.getFullName()));
}

bool fillSubstFromTypeNodes(const vector<string>& typeParams, const vector<p<TypeNode>>& typeArgNodes,
                            map<string, TypeInfo>& subst) {
    if (typeParams.size() != typeArgNodes.size()) return false;
    subst.clear();
    for (size_t i = 0; i < typeParams.size(); ++i) {
        try {
            subst[typeParams[i]] = typeArgNodes[i]->getType();
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return false;
        }
    }
    return true;
}

bool fillSubstFromGenericArgs(const vector<string>& typeParams, const vector<sp<TypeInfo>>& genericArgs,
                              map<string, TypeInfo>& subst) {
    if (typeParams.size() != genericArgs.size()) return false;
    subst.clear();
    for (size_t i = 0; i < typeParams.size(); ++i) {
        if (!genericArgs[i] || genericArgs[i]->empty()) return false;
        subst[typeParams[i]] = *genericArgs[i];
    }
    return true;
}

bool substHeaderParams(FnHeaderNode* header, const map<string, TypeInfo>& subst, vector<TypeInfo>& out) {
    if (!header) return false;
    out.clear();
    for (auto p : header->params()) {
        if (!p || !p->type()) return false;
        try {
            out.push_back(p->type()->getType().substitute(subst));
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return false;
        }
    }
    return true;
}

FnNode* uniqueNonBuiltinGenericFn(FileNode* file, const string& name) {
    if (!file) return nullptr;
    vector<pair<FnNode*, FileNode*>> fns;
    file->collectGenericFunctions(name, fns, file);
    FnNode* found = nullptr;
    for (auto& [fn, _] : fns) {
        if (!fn || !fn->header() || fn->header()->hasAnno("Builtin")) continue;
        if (found) return nullptr;
        found = fn;
    }
    return found;
}

StructImplNode* lookupStructImpl(FileNode* file, FileNode* sdk, const string& name) {
    if (file) {
        if (auto* i = file->getStructImpl(name)) return i;
    }
    if (sdk && sdk != file) {
        if (auto* i = sdk->getStructImpl(name)) return i;
    }
    return nullptr;
}

// 同名同 arity 唯一方法。多个重载不猜。
FnHeaderNode* uniqueMethodHeader(StructImplNode* impl, const string& name, size_t arity, bool wantStatic) {
    if (!impl) return nullptr;
    FnHeaderNode* found = nullptr;
    for (auto& m : impl->methods()) {
        auto h = m->header();
        if (!h || h->name().getText() != name) continue;
        if (h->isStatic() != wantStatic) continue;
        if (h->hasAnno("Builtin")) continue;
        if (h->params().size() != arity) continue;
        if (found) return nullptr;
        found = h;
    }
    return found;
}

bool substGenericCallParams(FnHeaderNode* header, const vector<string>& typeParams, const vector<TypeInfo>& typeArgs,
                            vector<TypeInfo>& out) {
    if (!header || typeParams.size() != typeArgs.size()) return false;
    map<string, TypeInfo> subst;
    for (size_t i = 0; i < typeParams.size(); ++i)
        subst[typeParams[i]] = typeArgs[i];
    return substHeaderParams(header, subst, out);
}

// Phase C：match 各臂结果类型须一致（镜像 compileMatchExpr）。流终止臂跳过。
// 模板体里类型参数 / 含 T 的复合类型不下钻，留给实例化期。
void checkMatchArmTypes(const vector<p<MatchArmNode>>& arms, const std::set<std::string>& typeParams,
                        const std::map<std::string, TypeInfo>* subst = nullptr) {
    TypeInfo first;
    bool firstSet = false;
    for (auto& arm : arms) {
        if (!arm || arm->skipsTypeMerge()) continue;
        TypeInfo t;
        try {
            t = applySubstMap(arm->resultType(), subst);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return;
        }
        if (stillTemplateType(t, typeParams, subst)) return;
        if (!firstSet) {
            first = t;
            firstSet = true;
            continue;
        }
        if (t != first) {
            throw YuxError(arm->resultLine(), arm->resultCol(), ErrorCode::E3014, first.getFullName(), t.getFullName())
                .withHint(std::format("match 各臂结果类型须一致：先前臂为 `{}`，此臂为 `{}`", first.getFullName(),
                                      t.getFullName()));
        }
    }
}

// 块体末位无 `;` 的裸表达式语句（隐式尾值），不含 ret / 声明 / 赋值子类。
bool isBareTailExprStmt(p<StatementNode> s) {
    auto se = dynamic_cast<p<StatementExprNode>>(s);
    if (!se || se->hasSemicolon() || !se->expr()) return false;
    if (dynamic_cast<p<StatementRetNode>>(s)) return false;
    if (dynamic_cast<p<StatementDeclareAssignNode>>(s)) return false;
    if (dynamic_cast<p<StatementDeclareAssignTupleNode>>(s)) return false;
    if (dynamic_cast<p<StatementAssignNode>>(s)) return false;
    return true;
}

// #Static fn 同名候选：过滤 wantArity，各位约定类型与 agreedArityParamTypes 同款。
// 任一同名泛型静态方法 → 不猜。
bool agreedStaticMethodParams(FileNode* file, FileNode* sdk, const string& lhs, const string& rhs, size_t wantArity,
                              vector<TypeInfo>& out) {
    StructDeclNode* sd = file ? file->getStructDecl(lhs, true) : nullptr;
    if (!sd && sdk && sdk != file) sd = sdk->getStructDecl(lhs, true);
    if (!sd || sd->isGeneric()) return false;
    StructImplNode* impl = file ? file->getStructImpl(lhs) : nullptr;
    if (!impl && sdk && sdk != file) impl = sdk->getStructImpl(lhs);
    if (!impl || impl->isGeneric()) return false;
    vector<vector<TypeInfo>> uniq;
    for (auto& m : impl->methods()) {
        auto header = m->header();
        if (!header || header->name().getText() != rhs || !header->isStatic()) continue;
        if (header->isGeneric()) return false;
        if (header->params().size() != wantArity) continue;
        vector<TypeInfo> ps;
        bool ok = true;
        for (auto p : header->params()) {
            if (!p || !p->type()) {
                ok = false;
                break;
            }
            try {
                ps.push_back(p->type()->getType());
            } catch (...) { // NOLINT(bugprone-empty-catch)
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        bool dup = false;
        for (auto& u : uniq) {
            if (u.size() != ps.size()) continue;
            bool same = true;
            for (size_t i = 0; i < u.size(); ++i) {
                if (u[i] != ps[i]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                dup = true;
                break;
            }
        }
        if (!dup) uniq.push_back(std::move(ps));
    }
    if (uniq.empty()) return false;
    out.assign(wantArity, TypeInfo());
    bool any = false;
    for (size_t i = 0; i < wantArity; ++i) {
        const TypeInfo& t0 = uniq[0][i];
        bool all = true;
        for (size_t k = 1; k < uniq.size(); ++k) {
            if (uniq[k][i] != t0) {
                all = false;
                break;
            }
        }
        if (all && !t0.empty()) {
            out[i] = t0;
            any = true;
        }
    }
    return any;
}

void copyFnParamTypes(const TypeInfo& fnTy, vector<TypeInfo>& out) {
    out.clear();
    if (!fnTy.isFn()) return;
    for (auto& sp : fnTy.fnParamTypes()) {
        out.push_back(sp ? *sp : TypeInfo());
    }
}
} // namespace

SemaPass::SemaPass(p<FileNode> file, Yux* yux)
    : _file(file), _yux(yux), _sdkFile(yux ? yux->sdkFile() : nullptr),
      _sourcePath((yux && file) ? yux->modulePath(file->moduleName()) : ""), _names(_file, _sdkFile) {}

void SemaPass::run() {
    if (!_file) return;
    // 顶层类型别名一次性校验 (E2017 / E2016) + fn 符号表归一化
    sema::validateAliases(_file, _sdkFile);
    // E4025 / E1132：struct 字段上的 Rc/Weak/Array 内嵌 Heap、Rc/Weak 内嵌 Dyn
    for (auto& sd : _file->getStructDecls()) {
        if (!sd) continue;
        auto savedFieldParams = _currentTypeParams;
        for (const auto& tp : sd->typeParams()) {
            _currentTypeParams.insert(tp);
        }
        for (auto& f : sd->fields()) {
            if (!f || !f->type()) continue;
            try {
                auto ft = f->type()->getType();
                validateContainerBansAt(ft, f->type(), f->getLineNumber(), f->getColumn());
                noteConcreteGenericType(ft);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        for (auto& sf : sd->staticFields()) {
            if (!sf.type) continue;
            try {
                noteConcreteGenericType(sf.type->getType());
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        _currentTypeParams = std::move(savedFieldParams);
    }
    for (auto& gc : _file->getGlobalConsts()) {
        if (!gc) continue;
        try {
            noteConcreteGenericType(gc->getType());
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    for (auto& fn : _file->getFunctions()) {
        // #Builtin 无真实体，仍跳过。Phase C：泛型模板体要走 SemaPass
        // （类型参数当不透明 TypeParam，做 #NoCopy / 未定义符号 / arity）。
        if (fn->header()->hasAnno("Builtin")) continue;
        visitFn(fn);
    }
    // struct impl 内的方法 / 析构 body 同样要走 SemaPass —— 它们的 codegen
    // 入口也是 compile<Foo>Expr, 不覆盖会导致后续 3.2b 把 set 改 assert 时
    // 方法体内表达式全部 assert 失败。
    for (auto& impl : _file->getStructImpls()) {
        auto savedTypeParams = _currentTypeParams;
        for (const auto& tp : impl->typeParams()) {
            _currentTypeParams.insert(tp);
        }
        // Phase 3.4.d.2: 进入 impl 时记录 currentStructName, 供 visitExpr 走
        // ExprGetRefNode / ExprDotNode 字段访问时校验 E3042 私有可见性.
        _currentStructName = impl->structName();
        for (auto& m : impl->methods()) {
            if (m->header()->hasAnno("Builtin")) continue;
            // Phase 6A: 砍同名 ctor —— `fn TypeName(...)` 定义形态废除,
            // 构造唯一通道收敛到 `#Static fn`. `#Static fn TypeName(...)` 形态
            // 仍合法 (虽不推荐, 与 `#Static fn make()` 等并行).
            const auto& mname = m->header()->name();
            if (mname.getText() == _currentStructName && !m->header()->isStatic()) {
                throw YuxError(mname.getLine(), static_cast<int>(mname.getCharPositionInLine()), ErrorCode::E3130,
                               _currentStructName, _currentStructName, _currentStructName);
            }
            visitFn(m);
        }
        if (impl->hasDestructor()) {
            visitFn(impl->destructor());
        }
        _currentStructName.clear();
        _currentTypeParams = std::move(savedTypeParams);
    }

    // DRAFT-spec-default-body Phase 2: spec 默认体占位符号校验
    // (sema 期不下钻完整 typecheck; 仅识别 `$.method(...)` 形态)
    visitSpecDefaults();

    // Phase B-1: #NoCopy 字段传播 (E4032) — 含显式 #NoCopy 字段的 struct
    // 自身也必须标注 #NoCopy。泛型模板按字段基名检查（剥 <T>），不整 decl 跳过。
    {
        // 收集所有可见 struct decl（本地 + SDK + wildcard imports）
        vector<StructDeclNode*> allDecls = _file->getStructDecls();
        if (_sdkFile && _sdkFile != _file) {
            for (auto* d : _sdkFile->getStructDecls()) {
                if (std::ranges::find(allDecls, d) == allDecls.end()) {
                    allDecls.push_back(d);
                }
            }
            for (auto* imp : _sdkFile->wildcardImports()) {
                for (auto* d : imp->getStructDecls()) {
                    if (std::ranges::find(allDecls, d) == allDecls.end()) {
                        allDecls.push_back(d);
                    }
                }
            }
        }

        for (auto* decl : allDecls) {
            if (decl->hasAnno("NoCopy")) continue; // 已标注，跳过
            for (auto* field : decl->fields()) {
                auto ft = field->getType();
                if (ft.isRc() || ft.isArrayGeneric() || ft.isWeak() || ft.isHeap()) continue;
                if (ft.isRef() || ft.isPtr()) continue;
                if (isBuiltinType(ft.name)) continue;
                // 字段类型为本模板形参（struct W<T> { item T }）时，实例化前无法判定。
                if (decl->isGeneric() && ft.isNormal() &&
                    std::ranges::find(decl->typeParams(), ft.name) != decl->typeParams().end()) {
                    continue;
                }

                auto* fieldDecl = _names.lookupStruct(ft.baseStructName(), true);
                if (fieldDecl && fieldDecl->hasAnno("NoCopy")) {
                    throw YuxError(decl->getLineNumber(), decl->getColumn(), ErrorCode::E4032, decl->name().getText(),
                                   field->name().getText());
                }
            }
        }
    }
}

namespace {
// 判定 e 是不是字面量 `$`（spec 默认体里 self 句柄, ast_builder 构成
// ExprLiteralNode(LiteralObjNode("$")))。
bool isBareSelf(const p<ExprNode>& e) {
    auto lit = dynamic_cast<p<ExprLiteralNode>>(e);
    if (!lit) return false;
    auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal());
    return obj && obj->getValue().getText() == "$";
}

// DRAFT-spec-default-body Phase 2: 递归扫描 expr 树寻找 `$.method(args)`
// 形态调用; 命中则验证 method 是否在 spec 自身签名集内, 不在则抛 E1140。
// 仅覆盖常见表达式形态; lambda / try-catch / match 等复杂形态在 Phase 2
// 主动 skip (留待 Phase 3 单态化时机的完整 typecheck)。
void walkExprForSpecDefault(const p<ExprNode>& e, SpecDeclNode* spec) {
    if (!e) return;
    if (auto n = dynamic_cast<p<ExprCallNode>>(e)) {
        if (auto dot = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            if (isBareSelf(dot->baseExpr())) {
                const std::string m = dot->member();
                bool found = false;
                for (auto& sig : spec->signatures()) {
                    if (sig->name().getText() == m) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    throw YuxError(dot->resolveLineNumber(), dot->resolveColumn(), ErrorCode::E1140,
                                   spec->name().getText(), m,
                                   " (referenced from default body — must appear in this spec's signatures)");
                }
            }
        }
        walkExprForSpecDefault(n->getCalleeExpr(), spec);
        for (auto& a : n->getArgs())
            walkExprForSpecDefault(a, spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(e)) {
        walkExprForSpecDefault(n->baseExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(e)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(e)) {
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    // 其它形态 (lambda / try-catch / match / struct lit / array / 索引 / 元组 ...)
    // Phase 2 不下钻; Phase 3 克隆 + 真实 typecheck 会兜底。
}

// 递归扫描 stmt 中的所有表达式入口。
void walkStmtForSpecDefault(const p<StatementNode>& s, SpecDeclNode* spec) {
    if (!s) return;
    if (auto n = dynamic_cast<p<StatementRetNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementAssignNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementSetNode>>(s)) {
        walkExprForSpecDefault(n->arrayExpr(), spec);
        for (auto& idx : n->indices())
            walkExprForSpecDefault(idx, spec);
        walkExprForSpecDefault(n->valueExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementDeclareAssignNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementExprNode>>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<p<StatementLoopNode>>(s)) {
        if (n->hasInit()) {
            walkExprForSpecDefault(n->initExpr(), spec);
        }
        return;
    }
    // Block / Declare(无 init) / RetVoid / Break 等 Phase 2 不处理
}
} // namespace

void SemaPass::visitSpecDefaults() {
    if (!_file) return;
    for (auto& spec : _file->getSpecDecls()) {
        if (!spec) continue;
        const auto& bodies = spec->defaultBodies();
        for (auto& body : bodies) {
            if (!body) continue;
            for (auto& stmt : body->body()) {
                walkStmtForSpecDefault(stmt, spec);
            }
        }
    }
}

void SemaPass::visitFn(p<FnNode> fn) {
    if (!fn) return;
    // Phase 3.3 前置.4: 进入 fn 时记 _currentFn, 让 visitExpr 里的
    // checkErrPropagateForIdCall / checkBangWithoutFallibleCaller 能拿到
    // caller 的 #Fallible(E) 注解.
    auto savedFn = _currentFn;
    _currentFn = fn;
    _movedVars.clear(); // Phase B-1: 进入 fn 时清空 move 追踪

    auto savedTypeParams = _currentTypeParams;
    if (auto hdr = fn->header()) {
        for (const auto& tp : hdr->typeParams()) {
            _currentTypeParams.insert(tp);
        }
    }

    // E4025 / E1132：形参 / 返回类型上的容器禁令（getLLVMType 同款，补 yux-check）
    if (auto hdr = fn->header()) {
        for (auto& param : hdr->params()) {
            if (!param || !param->type()) continue;
            try {
                auto pt = param->type()->getType();
                validateContainerBansAt(pt, param->type(), static_cast<int>(param->name().getLine()),
                                        static_cast<int>(param->name().getCharPositionInLine()));
                noteConcreteGenericType(pt);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        if (auto rt = hdr->retType()) {
            try {
                auto rtt = rt->getType();
                validateContainerBansAt(rtt, rt, fn->getLineNumber(), fn->getColumn());
                noteConcreteGenericType(rtt);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
    }

    // Bucket 1 (CURRENT-check.md): 把 0-LLVM analyzer 接入 sema, 让 yux-check
    // 也能覆盖 borrow / const-mut / NoReturn 流终止 检查.
    checkBorrows(fn, _currentStructName);
    checkConstMut(fn);
    checkFlowTerminate(fn);

    for (auto& stmt : fn->body()) {
        visitStmt(stmt);
    }
    _currentTypeParams = std::move(savedTypeParams);
    _currentFn = savedFn;
}

void SemaPass::visitBlock(p<StatementBlockNode> block, const TypeInfo* expected) {
    if (!block) return;
    for (auto& s : block->statements()) {
        visitStmt(s);
    }
    if (block->hasResult()) {
        visitExpr(block->resultExpr(), expected);
    }
}

void SemaPass::visitStmt(p<StatementNode> stmt) {
    if (!stmt) return;
    if (auto loop = dynamic_cast<p<StatementLoopNode>>(stmt)) {
        const auto& label = loop->label();
        // 检测重复 label：同名 label 不可在外层 loop 栈中出现
        if (!label.getText().empty()) {
            for (const auto& existing : _loopLabelStack) {
                if (existing.getText() == label.getText()) {
                    throw YuxError(loop->getLineNumber(), loop->getColumn(), ErrorCode::E3022, label.getText());
                }
            }
        }
        _loopLabelStack.push_back(label); // 空 Token = 无 label
        if (loop->hasInit()) {
            visitExpr(loop->initExpr());
        }
        visitBlock(loop->block());
        _loopLabelStack.pop_back();
        return;
    }
    if (auto set = dynamic_cast<p<StatementSetNode>>(stmt)) {
        visitExpr(set->arrayExpr());
        for (auto& idx : set->indices())
            visitExpr(idx);
        TypeInfo elemStorage;
        const TypeInfo* elemExpected = nullptr;
        if (!set->indices().empty()) {
            try {
                TypeInfo at = set->arrayExpr()->hasResolvedType() ? set->arrayExpr()->resolvedType()
                                                                  : set->arrayExpr()->getType();
                at = applyInstSubst(at).peelRef();
                if (isCurrentTypeParam(at)) {
                    // 未实例化模板体：两边都不查（与未调用泛型 fn 一致）
                } else if (at.isArrayGeneric()) {
                    if (auto e = at.arrayGenericElementType()) {
                        elemStorage = *e;
                        elemExpected = &elemStorage;
                    }
                } else if (at.isArray()) {
                    if (at.elementType) {
                        elemStorage = *at.elementType;
                        elemExpected = &elemStorage;
                    }
                } else if (!at.name.empty()) {
                    throw YuxError(set->getLineNumber(), set->getColumn(), ErrorCode::E3062, at.name);
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        visitExpr(set->valueExpr(), elemExpected);
        if (elemExpected) {
            checkAssignRhs(set->valueExpr(), *elemExpected, set->getLineNumber(), set->getColumn(), _file, _sdkFile,
                           _currentTypeParams, &_instSubst);
        }
        // v0.16 闭包捕获: lambda body 内对捕获变量赋值 → E2030。
        // StatementSetNode 覆盖简单变量 `a = 20` / 复合赋值 `a += 1` / 索引赋值 `a[i] = x`。
        // LHS arrayExpr 抽取变量名后按 StatementAssignNode 同款规则判定。
        if (_currentLambda && _currentFn && set->indices().empty()) {
            auto lhsLit = dynamic_cast<p<ExprLiteralNode>>(set->arrayExpr());
            if (lhsLit) {
                auto lhsObj = dynamic_cast<p<LiteralObjNode>>(lhsLit->literal());
                if (lhsObj) {
                    string objName = lhsObj->getValue().getText();
                    if (objName != "$") {
                        bool isParam = false;
                        for (auto& p : _currentLambda->params()) {
                            if (p.name.getText() == objName) {
                                isParam = true;
                                break;
                            }
                        }
                        bool isLambdaLocal = false;
                        if (auto body = _currentLambda->bodyScope()) {
                            isLambdaLocal = body->localSymbols().contains(objName);
                        }
                        if (!isParam && !isLambdaLocal) {
                            SymbolInfo* sym = nullptr;
                            if (auto sc = set->findNearestScope()) {
                                sym = sc->lookupSymbol(objName);
                            }
                            if (sym && sym->kind == SymbolKind::Variable) {
                                throw YuxError(set->getLineNumber(), set->getColumn(), ErrorCode::E2030, objName);
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    if (auto br = dynamic_cast<p<StatementBreakNode>>(stmt)) {
        const auto& brLabel = br->label();
        if (brLabel.getText().empty()) {
            // 无 label 的 break：检查是否有外层 loop
            if (_loopLabelStack.empty()) {
                throw YuxError(br->getLineNumber(), br->getColumn(), ErrorCode::E3094);
            }
        } else {
            // break@label：从内向外搜索匹配 label
            bool found = false;
            for (auto it = _loopLabelStack.rbegin(); it != _loopLabelStack.rend(); ++it) {
                if (it->getText() == brLabel.getText()) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                throw YuxError(br->getLineNumber(), br->getColumn(), ErrorCode::E3025, brLabel.getText());
            }
        }
        return;
    }
    if (auto rv = dynamic_cast<p<StatementRetVoidNode>>(stmt)) {
        // Phase C：lambda 期望非 void 时 `ret;` → E3014（镜像 compileRetVoidStatement）。
        if (_currentLambda) {
            TypeInfo want;
            if (lambdaExpectedRetType(_currentLambda, want) && !want.empty()) {
                throw YuxError(rv->getLineNumber(), rv->getColumn(), ErrorCode::E3014, want.getFullName(), "void");
            }
        }
        return;
    }
    if (auto d = dynamic_cast<p<StatementDeclareNode>>(stmt)) {
        // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct arity).
        // 无 init 形态 (`let p Pair<i32>`), 仅 varType, 同款检查.
        if (d->varType()) {
            try {
                auto vt = d->varType()->getType();
                noteConcreteGenericType(vt);
                if (!vt.name.empty() && !isBuiltinType(vt.name) && !vt.isRef() && !vt.isFn() && !vt.isTuple()) {
                    if (auto* sd = _names.lookupStruct(vt.name, true)) {
                        size_t want = sd->typeParams().size();
                        size_t got = vt.genericArgs.size();
                        if (want > 0 && want != got) {
                            throw YuxError(d->getLineNumber(), d->getColumn(), ErrorCode::E6011, vt.name, want, got)
                                .withHint(std::format(
                                    "实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", vt.name,
                                    std::string(want == 1 ? "T" : "T1, T2, ..."), want));
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // 留 Compiler 兜底
            }
        }
        return;
    }
    if (auto as = dynamic_cast<p<StatementAssignNode>>(stmt)) {
        // Phase 2e: `$.field = ...` 在 `#Static fn` 体内禁用 (E3128).
        // StatementAssign 的 `obj` (LHS 根) 不会被 visitExpr 递归, 这里单独拦截.
        if (as->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
            throw YuxError(as->obj().getLine(), static_cast<int>(as->obj().getCharPositionInLine()), ErrorCode::E3128);
        }
        // Bucket 2 收口 (CURRENT-check.md): 简单变量赋值 (subs 为空) 的写可见性校验
        // (E3093). 与 compiler_stmt.cpp:952 同款条件: !writeable && !type.isRef().
        // T& 形参 / val 局部 T& 的 writeable=false 不影响"写被引", 由 borrow 检查
        // 在 4d 校验.
        if (as->subs().empty() && _currentFn) {
            string objName = as->obj().getText();
            if (objName != "$") {
                SymbolInfo* sym = nullptr;
                if (auto sc = as->findNearestScope()) {
                    sym = sc->lookupSymbol(objName);
                }
                if (!sym) {
                    sym = _currentFn->lookupSymbol(objName);
                }
                if (sym && !sym->writeable && !sym->type.isRef()) {
                    throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3093, objName);
                }
            }
        }
        // v0.16 闭包捕获: lambda body 内对捕获变量赋值 / 成员链写 → E2030.
        // 覆盖 `=` / `+= -= *= /= %= ^=` / `<<= >>=` 及 `obj.f = ...` / `obj[i] = ...`
        // (obj 为捕获变量)。
        // 判定: objName 不在 lambda 形参、也不在 lambda body 本地 let → 外层变量 → 捕获 → 禁写.
        if (_currentLambda && _currentFn) {
            string objName = as->obj().getText();
            if (objName != "$") {
                bool isParam = false;
                for (auto& p : _currentLambda->params()) {
                    if (p.name.getText() == objName) {
                        isParam = true;
                        break;
                    }
                }
                bool isLambdaLocal = false;
                if (auto body = _currentLambda->bodyScope()) {
                    isLambdaLocal = body->localSymbols().contains(objName);
                }
                if (!isParam && !isLambdaLocal) {
                    SymbolInfo* sym = nullptr;
                    if (auto sc = as->findNearestScope()) {
                        sym = sc->lookupSymbol(objName);
                    }
                    if (sym && sym->kind == SymbolKind::Variable) {
                        throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E2030, objName);
                    }
                }
            }
        }
        // Bucket 5 起步: 成员链赋值的两条简单形态诊断 (与 compiler_stmt.cpp 1188-1207
        // tuple 越界 / 1361 中段拒收 镜像).
        //   * E3100 元组下标越界: actualType.isTuple() + memberText 为纯数字 + idx 越界
        //   * E3046 中段非纯 struct:  walk 到非末段, interType 命中
        //                          Rc/Array/Ref/Nullable/Weak/Ptr/builtin
        // 跳过策略 (留 Compiler 兜底):
        //   * lookupSymbol 失败 (E3031 Compiler 抢先)
        //   * 起点 / 中段是泛型 struct (Compiler applySubst, sema 不替换泛型实参)
        //   * 中段 typeNeedsDestructor (递归 RC 字段扫描, 复杂, 留 Compiler)
        //   * 非纯数字下标命中 tuple 形态.
        // `$` 在方法体登记为 Self&，lookup 即可（T& 赋值链）。
        if (!as->subs().empty() && _currentFn) {
            string objName = as->obj().getText();
            SymbolInfo* sym = nullptr;
            if (auto sc = as->findNearestScope()) {
                sym = sc->lookupSymbol(objName);
            }
            if (!sym) {
                sym = _currentFn->lookupSymbol(objName);
            }
            if (sym) {
                TypeInfo curType = sym->type;
                if (curType.isRef()) {
                    if (auto inner = curType.refElementType()) curType = *inner;
                }
                if (curType.isRc()) {
                    if (auto inner = curType.rcElementType()) curType = *inner;
                }
                const auto& subs = as->subs();
                auto isPureDigits = [](const string& s) {
                    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
                };
                if (curType.isTuple()) {
                    // tuple 链: 仅 OOB (E3100), 中段非 tuple / 非纯数字 留 Compiler
                    bool stop = false;
                    for (size_t i = 0; i < subs.size() && !stop; ++i) {
                        string memberText = subs[i].getText();
                        if (!isPureDigits(memberText)) {
                            stop = true;
                            break;
                        }
                        if (!curType.isTuple()) {
                            stop = true;
                            break;
                        }
                        const auto& elems = curType.tupleElements();
                        auto idx = static_cast<size_t>(std::stoul(memberText));
                        if (idx >= elems.size()) {
                            throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3100, memberText,
                                           curType.getFullName(), std::to_string(elems.size()));
                        }
                        if (i + 1 < subs.size()) curType = *elems[idx];
                    }
                } else if (!curType.name.empty() && !isBuiltinType(curType.name)) {
                    // struct 链: 中段 E3046 (Rc/Array/Ref/Nullable/Weak/Ptr/builtin)
                    StructDeclNode* decl = _file ? _file->getStructDecl(curType.name) : nullptr;
                    if (!decl && _sdkFile) decl = _sdkFile->getStructDecl(curType.name);
                    // 泛型 struct 留 Compiler (applySubst)
                    if (decl && !decl->isGeneric()) {
                        for (size_t i = 0; i + 1 < subs.size(); ++i) {
                            string memberText = subs[i].getText();
                            int fi = decl->fieldIndex(memberText);
                            if (fi < 0) break; // E3040 Compiler 抢先
                            auto interType = decl->fields()[fi]->getType();
                            if (interType.isRc() || interType.isArrayGeneric() || interType.isRef() ||
                                interType.isNullable() || interType.isWeak() || interType.isPtr() ||
                                isBuiltinType(interType.name)) {
                                throw YuxError(as->getLineNumber(), as->getColumn(), ErrorCode::E3046)
                                    .withHint("嵌套成员赋值中间字段需为纯 struct（不含 Rc/Array/Ref/RC "
                                              "等）；可拆方法或在中段先 `var t = $.field` 落地后再写");
                            }
                            StructDeclNode* nextDecl = _file ? _file->getStructDecl(interType.name) : nullptr;
                            if (!nextDecl && _sdkFile) nextDecl = _sdkFile->getStructDecl(interType.name);
                            if (!nextDecl || nextDecl->isGeneric()) break;
                            decl = nextDecl;
                        }
                    }
                }
            }
        }
        TypeInfo assignExpected;
        TypeInfo assignStorage;
        const TypeInfo* assignExpPtr = nullptr;
        bool haveStorage = false;
        if (_currentFn && as->expr()) {
            string objName = as->obj().getText();
            SymbolInfo* sym = nullptr;
            if (auto sc = as->findNearestScope()) {
                sym = sc->lookupSymbol(objName);
            }
            if (!sym) {
                sym = _currentFn->lookupSymbol(objName);
            }
            if (sym) {
                // expected：peelAutoDeref，给嵌套字面量 / 灵活整数（含 Rc<T> 的 T wrap）。
                // storage：只 peelRef（T& store-through）；末字段保持声明类型，供 E3014。
                if (!as->subs().empty()) {
                    vector<string> members;
                    members.reserve(as->subs().size());
                    for (auto& t : as->subs())
                        members.push_back(t.getText());
                    tryValidateFieldChain(sym->type, members, as->getLineNumber(), as->getColumn());
                }
                TypeInfo cur = applyInstSubst(sym->type).peelAutoDeref();
                TypeInfo lastRaw = applyInstSubst(sym->type).peelRef();
                bool ok = true;
                auto isPureDigits = [](const string& s) {
                    return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
                };
                for (size_t i = 0; i < as->subs().size() && ok; ++i) {
                    string mem = as->subs()[i].getText();
                    TypeInfo fieldTy;
                    if (cur.isTuple() && isPureDigits(mem)) {
                        auto idx = static_cast<size_t>(std::stoul(mem));
                        const auto& elems = cur.tupleElements();
                        if (idx >= elems.size() || !elems[idx]) {
                            ok = false;
                            break;
                        }
                        fieldTy = *elems[idx];
                    } else if (!cur.name.empty() && !isBuiltinType(cur.name)) {
                        StructDeclNode* decl = _names.lookupStruct(cur.name);
                        if (!decl) {
                            ok = false;
                            break;
                        }
                        map<string, TypeInfo> fieldSubst = _instSubst;
                        if (decl->isGeneric()) {
                            if (fieldSubst.empty() &&
                                !fillSubstFromGenericArgs(decl->typeParams(), cur.genericArgs, fieldSubst)) {
                                ok = false;
                                break;
                            }
                            if (fieldSubst.empty()) {
                                ok = false;
                                break;
                            }
                        }
                        int fi = decl->fieldIndex(mem);
                        if (fi < 0) {
                            ok = false;
                            break;
                        }
                        fieldTy = decl->fields()[static_cast<size_t>(fi)]->getType();
                        if (!fieldSubst.empty()) fieldTy = fieldTy.substitute(fieldSubst);
                    } else {
                        ok = false;
                        break;
                    }
                    lastRaw = fieldTy;
                    cur = fieldTy.peelAutoDeref();
                }
                if (ok) {
                    assignExpected = cur.peelRef();
                    assignExpPtr = &assignExpected;
                    assignStorage = std::move(lastRaw);
                    haveStorage = true;
                }
            }
        }
        if (as->expr()) visitExpr(as->expr(), assignExpPtr);
        if (haveStorage) {
            checkAssignRhs(as->expr(), assignStorage, as->getLineNumber(), as->getColumn(), _file, _sdkFile,
                           _currentTypeParams, &_instSubst);
        }
        return;
    }
    if (auto tup = dynamic_cast<p<StatementDeclareAssignTupleNode>>(stmt)) {
        // Bucket 2: 元组解构 LHS 数量 vs RHS 元组实际元素数 (E3102).
        // 简化策略 —— 仅在 RHS 直接是 ExprTupleNode 字面量时校验, 因为此时元素
        // 数从 AST 直接可得, 无需走 applySubst. 类型标注路径 (varType) 留 Compiler.
        if (tup->expr()) {
            if (auto tn = dynamic_cast<p<ExprTupleNode>>(tup->expr())) {
                if (tn->elements().size() != tup->names().size()) {
                    throw YuxError(tup->getLineNumber(), tup->getColumn(), ErrorCode::E3102,
                                   std::to_string(tup->names().size()), std::to_string(tn->elements().size()));
                }
            }
            TypeInfo texp;
            const TypeInfo* tp = nullptr;
            if (tup->varType()) {
                try {
                    texp = tup->varType()->getType();
                    tp = &texp;
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
            }
            visitExpr(tup->expr(), tp);
        }
        return;
    }
    if (auto ret = dynamic_cast<p<StatementRetNode>>(stmt)) {
        // Phase C：ret E3014（Fallible 双通道 / T& 形态 / Nullable wrap / 别名 /
        // 灵活整数）。lambda 用自身标注或反推返回类型，不用外层 fn。
        // spec 体未解析 Self、以及 T& 的 borrow 溯源（E4020）仍交 analyzer / Compiler。
        TypeInfo retExpected;
        TypeInfo retExpectedResolved;
        const TypeInfo* retExpPtr = nullptr;
        bool lambdaHasExpected = false;
        RetCheck retCtx{.file = _file,
                        .sdk = _sdkFile,
                        .fn = _currentFn,
                        .structName = _currentStructName,
                        .fallibleErr = {},
                        .typeParams = &_currentTypeParams,
                        .subst = &_instSubst};
        if (_currentLambda) {
            lambdaHasExpected = lambdaExpectedRetType(_currentLambda, retExpected);
            if (lambdaHasExpected) {
                retExpected = applyInstSubst(retExpected);
                retExpectedResolved = resolveForRet(retExpected, retCtx);
                retExpPtr = &retExpectedResolved;
            }
        } else if (_currentFn && ret->expr()) {
            auto header = _currentFn->header();
            if (header && header->retType()) {
                try {
                    retExpected = applyInstSubst(header->retType()->getType());
                    retExpectedResolved = resolveForRet(retExpected, retCtx);
                    retExpPtr = &retExpectedResolved;
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
            }
            if (header) {
                if (auto e = header->getAnnoArg("Fallible")) retCtx.fallibleErr = *e;
            }
        }
        if (ret->expr()) visitExpr(ret->expr(), retExpPtr);
        if (ret->expr() && (_currentLambda ? lambdaHasExpected : _currentFn != nullptr)) {
            int line = ret->getLineNumber();
            if (line < 0) line = ret->expr()->resolveLineNumber();
            if (_currentLambda) {
                checkRetExpr(ret->expr(), retExpected, !retExpected.empty(), line, retCtx);
            } else {
                TypeInfo decl;
                bool hasDecl = false;
                if (_currentFn->header() && _currentFn->header()->retType()) {
                    decl = _currentFn->header()->retType()->getType();
                    hasDecl = true;
                }
                checkRetExpr(ret->expr(), decl, hasDecl, line, retCtx);
            }
        }
        // v0.16 闭包捕获: lambda 字面量直接作 ret expr 且含 T& 捕获 → E4022
        // (spec §8.7.6.5 不可逃逸)。仅拦截直接形 (lambda 字面量), 穿透检测
        // (ret 变量名 / 调用结果含 lambda) 留 codegen 兜底。
        if (ret->expr()) {
            if (auto litLambda = dynamic_cast<p<LambdaExprNode>>(ret->expr())) {
                if (litLambda->hasRefCapture()) {
                    throw YuxError(litLambda->getLineNumber(), litLambda->getColumn(), ErrorCode::E4022);
                }
            }
        }
        return;
    }
    if (auto da = dynamic_cast<p<StatementDeclareAssignNode>>(stmt)) {
        // T& 局部初始化：ID copy-bind E3018、`&expr` 内层 E3014、其余非法形态 E3019.
        // lambda 体 sema 不下钻 — 这里检查 _currentFn 非空再做.
        // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct arity 不匹配).
        // 不依赖 expr / _currentFn, 仅 varType 形态. varType.name 命中已知 struct decl,
        // decl.isGeneric() 且 typeParams.size() != genericArgs.size() → 抛 E6011.
        // 镜像 compiler_types.cpp:241 与 :597 两条路径. Builtin / Ref / Fn / Tuple 跳过.
        if (da->varType()) {
            try {
                auto vt = da->varType()->getType();
                validateContainerBansAt(vt, da->varType(), da->getLineNumber(), da->getColumn());
                if (!vt.name.empty() && !isBuiltinType(vt.name) && !vt.isRef() && !vt.isFn() && !vt.isTuple()) {
                    if (auto* sd = _names.lookupStruct(vt.name, true)) {
                        size_t want = sd->typeParams().size();
                        size_t got = vt.genericArgs.size();
                        if (want > 0 && want != got) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E6011, vt.name, want, got)
                                .withHint(std::format(
                                    "实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", vt.name,
                                    std::string(want == 1 ? "T" : "T1, T2, ..."), want));
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // 留 Compiler 兜底
            }
        }
        TypeInfo daExpected;
        const TypeInfo* daExpPtr = nullptr;
        if (da->varType()) {
            try {
                daExpected = applyInstSubst(da->varType()->getType());
                daExpPtr = &daExpected;
                noteConcreteGenericType(daExpected);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        if (dynamic_cast<p<ExprArrayInitNode>>(da->expr())) {
            if (!da->varType()) {
                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3067, " with size");
            }
            if (daExpPtr && !daExpPtr->isArray()) {
                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3067, "");
            }
        }
        if (da->varType() && _currentFn && da->expr()) {
            auto varType = applyInstSubst(da->varType()->getType());
            // Bucket 6 收口+ (CURRENT-check.md): 目标类型驱动的形态校验.
            // E3012 (fixed-array 大小不匹配) / E3015 (Nullable 内部类型不匹配).
            // 镜像 compiler_stmt.cpp:773 / 740. 复杂路径 (alias / 嵌套数组目标类型)
            // 留 Compiler 兜底. lambda 体 sema 不下钻.
            try {
                if (varType.isArray() && !dynamic_cast<p<ExprArrayNode>>(da->expr()) &&
                    !dynamic_cast<p<ExprArrayInitNode>>(da->expr())) {
                    auto exprType = da->expr()->getType();
                    if (exprType.isArray() && exprType.arraySize > 0 && varType.arraySize != exprType.arraySize) {
                        throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3012, varType.arraySize,
                                       exprType.arraySize);
                    }
                    if (exprType.isArray() && varType.elementType && exprType.elementType &&
                        *varType.elementType != *exprType.elementType) {
                        throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3009,
                                       varType.elementType->getFullName(), exprType.elementType->getFullName());
                    }
                } else if (varType.isNullable()) {
                    auto innerType = varType.nullableInnerType();
                    if (innerType) {
                        // null 字面量直通
                        if (!isFlexibleNullExpr(da->expr())) {
                            if (isIntTypeName(innerType->name) && isFlexibleIntExpr(da->expr())) {
                                tryInferIntType(da->expr(), *innerType);
                            }
                            auto exprType = da->expr()->getType();
                            bool wholeCopy = exprType.isNullable() && exprType == varType;
                            bool wrap = exprType == *innerType;
                            if (!wholeCopy && !wrap) {
                                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, exprType.name,
                                               innerType->name);
                            }
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败: 留 Compiler 兜底
            }
            if (varType.isRef()) {
                auto innerType = varType.refElementType();
                if (innerType) {
                    auto* rhs = da->expr();
                    if (auto getRef = dynamic_cast<p<ExprGetRefNode>>(rhs)) {
                        TypeInfo getTy;
                        if (tryGetExprType(getRef, getTy)) {
                            auto innerOfGetRef = getTy.refElementType();
                            if (!innerOfGetRef || *innerOfGetRef != *innerType) {
                                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, innerType->name,
                                               innerOfGetRef ? innerOfGetRef->name : "?")
                                    .withHint(std::format(
                                        "&expr 的内层类型必须与声明一致；预期 `&<{}>`，源表达式给出 `&<{}>`",
                                        innerType->name, innerOfGetRef ? innerOfGetRef->name : "?"));
                            }
                        }
                    } else if (auto litExpr = dynamic_cast<p<ExprLiteralNode>>(rhs)) {
                        if (auto litObj = dynamic_cast<p<LiteralObjNode>>(litExpr->literal())) {
                            string srcName = litObj->getValue().getText();
                            SymbolInfo* sym = lookupRetVar(srcName, da, _currentFn);
                            if (!sym || !sym->type.isRef() || !sym->type.refElementType() ||
                                *sym->type.refElementType() != *innerType) {
                                throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3018, srcName,
                                               innerType->name)
                                    .withHint(std::format("`{}` 不是 {}& 类型，无法 copy-bind 到此声明；改写为 "
                                                          "`&<expr-of-{}>` 或先声明同类型 T&",
                                                          srcName, innerType->name, innerType->name));
                            }
                        } else {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                                .withHint("T& 局部初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 "
                                          "`val r T& = as_ref(box)`");
                        }
                    } else if (auto callExpr = dynamic_cast<p<ExprCallNode>>(rhs)) {
                        string calleeName;
                        if (auto litCallee = dynamic_cast<p<ExprLiteralNode>>(callExpr->getCalleeExpr())) {
                            if (auto obj = dynamic_cast<p<LiteralObjNode>>(litCallee->literal())) {
                                calleeName = obj->getValue().getText();
                            }
                        }
                        TypeInfo callTy;
                        bool callRetIsRef = tryGetExprType(callExpr, callTy) && callTy.isRef();
                        if (calleeName != "as_ref" && !callRetIsRef) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                                .withHint(
                                    "T& 局部初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量）、`val r "
                                    "T& = as_ref(box)` 或返回 T& 的方法/函数调用");
                        }
                    } else if (auto pathCall = dynamic_cast<p<ExprPathCallNode>>(rhs)) {
                        TypeInfo pathTy;
                        if (!tryGetExprType(pathCall, pathTy) || !pathTy.isRef()) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                                .withHint("静态路径不返回 T& 类型，无法初始化 T& 局部");
                        }
                    } else if (auto getNode = dynamic_cast<p<ExprGetNode>>(rhs)) {
                        TypeInfo getTy;
                        if (!tryGetExprType(getNode, getTy) || !getTy.isRef()) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                                .withHint("数组索引不返回 T& 类型，无法初始化 T& 局部");
                        }
                    } else {
                        throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                            .withHint("T& 局部初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 "
                                      "`val r T& = as_ref(box)`");
                    }
                }
            }

            // 通用类型匹配检查：声明类型与表达式类型必须严格一致
            // 跳过已独立处理的容器类型 (Ref / Array / Nullable / Rc / Weak / ArrayGeneric / Heap)
            // 跳过灵活整数字面量 (类型会在编译器端按目标类型推断)
            // 跳过类型别名声明 (如 `type A = i32` / `type IPair = (i32,i32)`) —
            // 别名解析可能跨 kind（Normal→Tuple/Array），完整解析留 Compiler 端 applySubst 兜底
            auto isAliasName = [&](const string& n) -> bool {
                if (_file && _file->getAliasDecl(n)) return true;
                if (_sdkFile && _sdkFile->getAliasDecl(n)) return true;
                return false;
            };
            // 跳过 ExprPathCallNode（如 `Label::COUNT` / `P::get_x()`）—
            // 这些表达式有独立的类型/语义校验（E3120/E3121 等），不应被通用类型检查遮蔽
            if (!varType.isRef() && !varType.isArray() && !varType.isNullable() && !varType.isRc() &&
                !varType.isWeak() && !varType.isArrayGeneric() && !varType.isHeap() && !varType.isFn() &&
                varType.genericArgs.empty() && !isFlexibleIntExpr(da->expr()) && !isAliasName(varType.name) &&
                !dynamic_cast<p<ExprPathCallNode>>(da->expr())) {
                try {
                    auto exprType = applyInstSubst(da->expr()->getType());
                    // 跳过泛型形参 / 未解析类型（如 T, U 等）：此时尚未实例化，比较无意义
                    auto isKnownType = [&](const TypeInfo& t) -> bool {
                        if (isBuiltinType(t.name)) return true;
                        if (_file && _file->getStructDecl(t.name)) return true;
                        if (_sdkFile && _sdkFile->getStructDecl(t.name)) return true;
                        return false;
                    };
                    if (!varType.name.empty() && !exprType.name.empty() && !varType.isSelf() && !exprType.isSelf() &&
                        !exprType.isRef() && !exprType.isFn() && !isAliasName(exprType.name) && isKnownType(varType) &&
                        isKnownType(exprType)) {
                        if (varType != exprType) {
                            throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014,
                                           varType.getFullName(), exprType.getFullName())
                                .withHint(std::format("声明类型为 `{}`，但表达式类型为 `{}`；yux 无隐式类型转换",
                                                      varType.getFullName(), exprType.getFullName()));
                        }
                    }
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    // getType 失败: 留 Compiler 兜底
                }
            }
        }
        if (da->expr()) visitExpr(da->expr(), daExpPtr);
        // v0.16 闭包捕获: lambda 字面量直接作 var/val 初始化值且含 T& 捕获 → E4022
        // (spec §8.7.6.5 不可逃逸：fn 值不可被存储到寿命外延的变量)。
        // 仅拦截直接形 (lambda 字面量), 穿透检测 (右值 wrapper 调用结果等) 留 codegen 兜底。
        if (da->expr()) {
            if (auto litLambda = dynamic_cast<p<LambdaExprNode>>(da->expr())) {
                if (litLambda->hasRefCapture()) {
                    throw YuxError(litLambda->getLineNumber(), litLambda->getColumn(), ErrorCode::E4022);
                }
            }
        }
        // Phase B-1: #NoCopy 类型不可从现有变量隐式复制（let 绑定）
        if (da->varType() && da->expr()) {
            auto varType = applyInstSubst(da->varType()->getType());
            if (isNoCopyTypeIn(varType, _file, _sdkFile) && !varType.isRef()) {
                if (!isFreshHandleExpr(da->expr())) {
                    throw YuxError(da->getLineNumber(), da->getColumn(), ErrorCode::E4031, varType.name, "let 绑定",
                                   varType.name);
                }
            }
        }
        return;
    }
    if (auto se = dynamic_cast<p<StatementExprNode>>(stmt)) {
        // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
        // E4030: `a <- b` 作为表达式语句时结果被丢弃，建议改用 `a = b`
        if (auto ma = dynamic_cast<p<ExprMoveAssignNode>>(se->expr())) {
            DiagnosticEngine::emit(_sourcePath,
                                   YuxError(ma->resolveLineNumber(), ma->resolveColumn(), ErrorCode::E4030));
        }
        if (se->expr()) visitExpr(se->expr());
        return;
    }
    if (auto sf = dynamic_cast<p<StatementStaticFieldSetNode>>(stmt)) {
        string typeName = sf->typeName().getText();
        string fieldName = sf->fieldName().getText();
        auto* structDecl = _names.lookupStruct(typeName, true);
        if (!structDecl) {
            throw YuxError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3030, typeName);
        }
        const auto* field = structDecl->staticField(fieldName);
        if (!field) {
            throw YuxError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3030, typeName + "::" + fieldName);
        }
        if (!field->isMutable) {
            throw YuxError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3151, typeName + "::" + fieldName);
        }
        TypeInfo ft;
        const TypeInfo* fp = nullptr;
        if (field->type) {
            try {
                ft = field->type->getType();
                fp = &ft;
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        visitExpr(sf->valueExpr(), fp);
        if (fp) {
            checkAssignRhs(sf->valueExpr(), *fp, sf->getLineNumber(), sf->getColumn(), _file, _sdkFile,
                           _currentTypeParams, &_instSubst);
        }
        return;
    }
    // 兜底：未识别的 stmt 直接跳过, 不抛错 —— SemaPass 当前是 no-op, 漏处理
    // 不应阻塞 codegen; 3.2 起开始有实际写入后再改成 assert(false)。
}

void SemaPass::visitExprList(const vector<p<ExprNode>>& args, const vector<TypeInfo>* expected) {
    for (size_t i = 0; i < args.size(); ++i) {
        const TypeInfo* exp = nullptr;
        if (expected && i < expected->size() && !(*expected)[i].empty()) exp = &(*expected)[i];
        visitExpr(args[i], exp);
    }
}

void SemaPass::visitExpr(p<ExprNode> expr, const TypeInfo* expected) {
    if (!expr) return;

    // Phase C：有靶向类型时，数组 / 元组字面量先按 expected 走，避免 getType
    // 用首元素推断造成 E3009 假阳性（嵌套 Array<Array<T>>、灵活整数、空数组）。
    if (expected) {
        TypeInfo want = expected->peelRef();
        if (auto paren = dynamic_cast<p<ExprParenNode>>(expr)) {
            visitExpr(paren->expr(), expected);
            if (paren->expr() && paren->expr()->hasResolvedType()) {
                paren->setResolvedType(paren->expr()->resolvedType());
            }
            return;
        }
        if (auto lam = dynamic_cast<p<LambdaExprNode>>(expr)) {
            if (want.isFn()) applyLambdaFnExpected(lam, want);
        }
        if (isFlexibleIntExpr(expr) && isIntTypeName(want.name)) {
            tryInferIntType(expr, want);
        } else if (isFlexibleIntExpr(expr) && want.isNullable()) {
            if (auto inner = want.nullableInnerType()) {
                if (isIntTypeName(inner->name)) tryInferIntType(expr, *inner);
            }
        } else if (isFlexibleIntExpr(expr) && want.isRc()) {
            if (auto inner = want.rcElementType()) {
                if (isIntTypeName(inner->name)) tryInferIntType(expr, *inner);
            }
        }
        if (isFlexibleNullExpr(expr) && want.isNullable()) {
            tryInferNullType(expr, want);
        }
        if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
            if (want.isArray() || want.isArrayGeneric()) {
                checkArrayLiteral(n, want);
                return;
            }
        }
        if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
            checkArrayInit(n, &want);
            return;
        }
        if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
            if (want.isTuple()) {
                const auto& w = want.tupleElements();
                const auto& src = n->elements();
                for (size_t i = 0; i < src.size(); ++i) {
                    visitExpr(src[i], (i < w.size() && w[i]) ? w[i].get() : nullptr);
                }
                n->setResolvedType(want);
                return;
            }
        }
    }

    // Phase B：getType 诊断默认由 SemaPass 重抛。
    // Phase C：泛型模板体内再吞依赖 T 具体化的码；形态检查仍重抛。
    // 方法点 E3095 先记下，给后面的 Dot 分支报 E1101/E1140；ID-literal 的 E3095 重抛。
    std::optional<YuxError> deferredMethodE3095;
    try {
        expr->setResolvedType(expr->getType());
    } catch (const YuxError& e) {
        const bool methodPointE3095 =
            isMethodPointCall(expr) && e.getCode() && std::string_view(e.getCode()) == "E3095";
        const bool swallow =
            methodPointE3095 || (!_currentTypeParams.empty() && !isMorphologicalGenericCode(e.getCode()));
        if (!swallow) throw;
        if (methodPointE3095) deferredMethodE3095 = e;
    } catch (...) { // NOLINT(bugprone-empty-catch) — release 仍吞非 YuxError；debug 下 assert
#ifndef NDEBUG
        assert(false && "getType threw non-YuxError; SemaPass must not swallow unknown failures");
#endif
    }

    if (auto n = dynamic_cast<p<ExprLiteralNode>>(expr)) {
        // Phase 2e 构造模型重构: `#Static fn` 体内禁用 `$` (E3128).
        // `$` 在 ast_builder 里生成 ExprLiteralNode(LiteralObjNode("$")),
        // `$.field` / `$.method()` 读路径会递归到此, 一处拦截即覆盖.
        if (auto obj = dynamic_cast<p<LiteralObjNode>>(n->literal())) {
            if (obj->getValue().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
            }
            // Phase B-1: use-after-move 检查 (E4033)
            if (_currentFn && obj->getValue().getText() != "$") {
                string varName = obj->getValue().getText();
                if (_movedVars.count(varName)) {
                    // lambda 体内仅当 varName 不是 lambda 形参时才报错
                    if (!_currentLambda) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                    } else {
                        bool isParam = false;
                        for (auto& p : _currentLambda->params()) {
                            if (p.name.getText() == varName) {
                                isParam = true;
                                break;
                            }
                        }
                        if (!isParam) {
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4033, varName);
                        }
                    }
                }
            }
        }
        // 字符串模板含插值表达式; 其余字面量无子表达式
        if (auto tpl = dynamic_cast<p<StringTemplateNode>>(n->literal())) {
            for (auto& e : tpl->interps())
                visitExpr(e);
            // Bucket 6 (CURRENT-check.md): E3026 插值类型必须实现 ToString.
            sema::validateStringTemplateInterps(_file, _sdkFile, tpl);
        }
        // v0.16 闭包捕获: lambda body 内标识符引用检查。
        // - 引用外层 Heap<T> (非空) 变量 → E4024 (Heap 按值捕获禁止, §7.3 / [#18])
        // - 引用外层 T& 变量 → 标记 hasRefCapture (E4022 数据收集)
        // 非 ID-obj / 全局 / template 插值等其它字面量形态不触发捕获, 跳过。
        // $ 在方法体内 lambda 是 Self&, 同样标记 hasRefCapture。
        // 查找起点：lambda body 的 *父* 作用域（块作用域后 let 不在 FnNode 上；
        // 也避免把 lambda 体内 / 嵌套块的本地 Heap let 误判为捕获）。
        if (_currentLambda && _currentFn) {
            auto obj2 = dynamic_cast<p<LiteralObjNode>>(n->literal());
            if (obj2) {
                string varName = obj2->getValue().getText();
                bool isParam = false;
                for (auto& p : _currentLambda->params()) {
                    if (p.name.getText() == varName) {
                        isParam = true;
                        break;
                    }
                }
                if (!isParam) {
                    SymbolInfo* sym = nullptr;
                    if (auto body = _currentLambda->bodyScope()) {
                        if (auto parent = body->parentScope()) {
                            sym = parent->lookupSymbol(varName);
                        }
                    }
                    if (!sym) {
                        sym = _currentFn->lookupSymbol(varName);
                    }
                    if (sym) {
                        const auto& t = sym->type;
                        // 函数名 / 类型名不是变量捕获，跳过 Heap/Ref 捕获检查
                        bool isVarOrParam = sym->kind != SymbolKind::Function && sym->kind != SymbolKind::Struct;
                        if (isVarOrParam && t.isHeap()) {
                            auto elem = t.heapElementType();
                            string elemName = elem ? elem->getFullName() : string("?");
                            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E4024, elemName,
                                           varName, elemName);
                        }
                        if (isVarOrParam && t.isRef()) {
                            _currentLambdaHasRefCapture = true;
                        }
                    }
                }
            }
        }
        // Phase 3.4.f.2: int 字面量越界 (E3103) — getType 仅返回类型不解析值,
        // 这里主动调 sema::parseIntLiteral 触发越界 / 非法格式校验.
        if (auto intLit = dynamic_cast<p<LiteralIntNode>>(n->literal())) {
            (void)sema::parseIntLiteral(intLit->getValue().getText(), n->getLineNumber(), n->getColumn());
        }
        // Phase B：标识符解析挂到 AST，codegen 读 resolvedSymbol。
        if (auto objSym = dynamic_cast<p<LiteralObjNode>>(n->literal())) {
            string varName = objSym->getValue().getText();
            if (varName != "$") {
                SymbolInfo* sym = nullptr;
                if (auto sc = n->findNearestScope()) {
                    sym = sc->lookupSymbol(varName);
                }
                if (!sym && _currentFn) {
                    sym = _currentFn->lookupSymbol(varName);
                }
                if (sym) n->setResolvedVar(sym);
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprAddSubNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 单点: 自定义 struct 二元运算符方法解析 (E3073 + byval hint).
        string m = (n->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprMulDivModNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprMulDivModNode::Op::Mul:
            m = "mul";
            break;
        case ExprMulDivModNode::Op::Div:
            m = "div";
            break;
        case ExprMulDivModNode::Op::Mod:
            m = "mod";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprBinOpNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprBinOpNode::Op::And:
            m = "and";
            break;
        case ExprBinOpNode::Op::Or:
            m = "or";
            break;
        case ExprBinOpNode::Op::Xor:
            m = "xor";
            break;
        case ExprBinOpNode::Op::Shl:
            m = "shl";
            break;
        case ExprBinOpNode::Op::Shr:
            m = "shr";
            break;
        }
        tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCompareNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 (CURRENT-check.md): leftType 形态校验 (E3078 Weak ==/!= /
        // E3073 Ptr ordering). leftType getType 抛错 (lambda 形参等) 跳过.
        try {
            TypeInfo leftType = n->left()->getType();
            sema::validateCompareOpForm(leftType, n->op(), n->getLineNumber(), n->getColumn());
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 内部异常: 留 Compiler 兜底
        }
        // Bucket 6 单点: 自定义 struct 比较运算符方法解析 (E3073 + byval hint).
        // AndAnd / OrOr 是逻辑短路, 无方法名映射, 跳过.
        string m;
        switch (n->op()) {
        case ExprCompareNode::Op::Eq:
            m = "eq";
            break;
        case ExprCompareNode::Op::Ne:
            m = "ne";
            break;
        case ExprCompareNode::Op::Lt:
            m = "lt";
            break;
        case ExprCompareNode::Op::Le:
            m = "le";
            break;
        case ExprCompareNode::Op::Gt:
            m = "gt";
            break;
        case ExprCompareNode::Op::Ge:
            m = "ge";
            break;
        default:
            break;
        }
        if (!m.empty()) {
            tryValidateBinOpMethod(n->left(), n->right(), m, n->getLineNumber(), n->getColumn());
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprParenNode>>(expr)) {
        visitExpr(n->expr());
        return;
    }
    if (auto n = dynamic_cast<p<ExprCallNode>>(expr)) {
        visitExpr(n->getCalleeExpr());

        // Phase C：重载前实参靶向类型。不看实参类型即可确定的形参才下钻：
        // 单 arity 非泛型候选 / Fn 值 callee / 非泛型方法，以及同 arity 重载
        // 各位都相同的类型。不一致的位置留空，不猜候选。
        // 泛型：显式 typeArgs 替换后的形参；接收者已带 typeArgs 的泛型 struct 方法。
        vector<TypeInfo> callArgExpected;
        const vector<TypeInfo>* callArgExpPtr = nullptr;
        const bool hasTypeArgs = !n->getTypeArgs().empty();
        if (hasTypeArgs) {
            if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                    string fnName = obj->getValue().getText();
                    if (auto* genFn = uniqueNonBuiltinGenericFn(_file, fnName)) {
                        map<string, TypeInfo> subst;
                        if (fillSubstFromTypeNodes(genFn->header()->typeParams(), n->getTypeArgs(), subst) &&
                            substHeaderParams(genFn->header(), subst, callArgExpected)) {
                            callArgExpPtr = &callArgExpected;
                        }
                    }
                }
            }
        } else if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                auto* structDecl = _names.lookupStruct(fnName);
                if (structDecl && !structDecl->isGeneric()) {
                    vector<FnSymbolInfo*> cands;
                    collectOverloadsBoth(_file, _sdkFile, fnName + "." + fnName, cands);
                    if (agreedArityParamTypes(cands, n->getArgs().size() + 1, 1, callArgExpected)) {
                        callArgExpPtr = &callArgExpected;
                    }
                } else if (!structDecl && !_file->getGenericFunction(fnName).first) {
                    vector<FnSymbolInfo*> cands;
                    collectOverloadsBoth(_file, _sdkFile, fnName, cands);
                    if (agreedArityParamTypes(cands, n->getArgs().size(), 0, callArgExpected)) {
                        callArgExpPtr = &callArgExpected;
                    }
                }
                if (!callArgExpPtr) {
                    // 函数名 getType 可能只编码某一个重载，不能当靶向类型。
                    // 仅局部 / 形参上的 Fn 值可以。
                    SymbolInfo* sym = nullptr;
                    if (auto sc = lit->findNearestScope()) {
                        sym = sc->lookupSymbol(fnName);
                    }
                    if (!sym && _currentFn) {
                        sym = _currentFn->lookupSymbol(fnName);
                    }
                    if (sym && sym->kind == SymbolKind::Variable && sym->type.isFn()) {
                        copyFnParamTypes(sym->type, callArgExpected);
                        callArgExpPtr = &callArgExpected;
                    }
                }
            }
        } else if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            TypeInfo baseType;
            bool baseOk = true;
            try {
                baseType = dotCallee->baseExpr()->hasResolvedType() ? dotCallee->baseExpr()->resolvedType()
                                                                    : dotCallee->baseExpr()->getType();
                if (dotCallee->isSafe() && baseType.isNullable()) {
                    if (auto inner = baseType.nullableInnerType()) baseType = *inner;
                }
                baseType = baseType.peelAutoDeref();
            } catch (...) { // NOLINT(bugprone-empty-catch)
                baseOk = false;
            }
            if (baseOk && !baseType.name.empty() && !isBuiltinType(baseType.name) && !baseType.isArrayGeneric() &&
                !baseType.isPtr() && !baseType.isDyn()) {
                if (auto* sd = _names.lookupStruct(baseType.name)) {
                    string member = dotCallee->member();
                    if (dotCallee->hasSpecQualifier()) {
                        member = member + "__at__" + dotCallee->specQualifier();
                    }
                    if (!sd->isGeneric()) {
                        vector<FnSymbolInfo*> cands;
                        collectOverloadsBoth(_file, _sdkFile, baseType.name + "." + member, cands);
                        if (agreedArityParamTypes(cands, n->getArgs().size() + 1, 1, callArgExpected)) {
                            callArgExpPtr = &callArgExpected;
                        }
                    } else {
                        map<string, TypeInfo> subst;
                        if (fillSubstFromGenericArgs(sd->typeParams(), baseType.genericArgs, subst)) {
                            auto* impl = lookupStructImpl(_file, _sdkFile, baseType.name);
                            if (auto* hdr = uniqueMethodHeader(impl, dotCallee->member(), n->getArgs().size(),
                                                               /*wantStatic=*/false)) {
                                if (substHeaderParams(hdr, subst, callArgExpected)) {
                                    callArgExpPtr = &callArgExpected;
                                }
                            }
                        }
                    }
                }
            }
        } else {
            TypeInfo calleeType;
            try {
                calleeType = n->getCalleeExpr()->hasResolvedType() ? n->getCalleeExpr()->resolvedType()
                                                                   : n->getCalleeExpr()->getType();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
            if (calleeType.isFn()) {
                copyFnParamTypes(calleeType, callArgExpected);
                callArgExpPtr = &callArgExpected;
            }
        }
        visitExprList(n->getArgs(), callArgExpPtr);

        // E4025 (DRAFT-heap-types §8.3a.5.1): 容器构造 turbofish 内嵌 Heap 拦截.
        // 形态: `Rc:<Heap<T>>(...)` / `Weak:<Heap<T>>(...)` / `Array:<Heap<T>>(...)`
        // 以及任意 call 的 turbofish 内出现 `Rc<Heap<T>>` / `Weak<...>` / `Array<...>` 嵌套.
        // 与 compiler_types.cpp:438/464/484 镜像.
        if (!n->getTypeArgs().empty()) {
            int eline = n->getLineNumber();
            int ecol = n->getColumn();
            string calleeName;
            if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
                if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                    calleeName = obj->getValue().getText();
                }
            }
            try {
                auto t0 = n->getTypeArgs()[0]->getType();
                if ((calleeName == "rc" || calleeName == "Rc" || calleeName == "Weak" || calleeName == "Array") &&
                    t0.isHeap()) {
                    auto inner = t0.heapElementType();
                    throw YuxError(eline, ecol, ErrorCode::E4025, calleeName, inner ? inner->name : std::string("?"));
                }
                if ((calleeName == "rc" || calleeName == "Rc" || calleeName == "Weak") && t0.isDyn()) {
                    throw YuxError(eline, ecol, ErrorCode::E1132, calleeName + "<" + t0.getFullName() + ">");
                }
                for (auto& tn : n->getTypeArgs()) {
                    try {
                        validateRcContainerBans(tn->getType(), eline, ecol);
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch) — sema 非 YuxError 异常留 Compiler 兜底
            }
        }

        // Phase 3.3 前置.4: ID-callee / 非-ID-callee 的错误传播校验
        // (E7001/E7004/E7006/E7016). 协议与 Compiler::compileCallExpr 顶部
        // 完全一致 —— ID-literal 走 checkErrPropagateForIdCall (按 fnName
        // 取第一候选, 10e 视多重载为同质); 非 ID-literal + ! + 不在 try 内
        // 走 checkBangWithoutFallibleCaller. _tryStack 顶端的 vector* 用作
        // tryBlockSeenErrs (callee 是 #Fallible 时把 errType append 进去,
        // 供 E7002 穷尽性使用; SemaPass 暂不读它, 由 Compiler 走 E7002).
        auto calleeExpr = n->getCalleeExpr();
        if (auto litCallee = dynamic_cast<p<ExprLiteralNode>>(calleeExpr)) {
            if (auto objLit = dynamic_cast<p<LiteralObjNode>>(litCallee->literal())) {
                string fnNameProp = objLit->getValue().getText();
                vector<FnSymbolInfo*> cands;
                _file->collectFnOverloads(fnNameProp, cands);
                if (_sdkFile && _sdkFile != _file) {
                    _sdkFile->collectFnOverloads(fnNameProp, cands);
                }
                const FnSymbolInfo* sym = cands.empty() ? nullptr : cands.front();
                vector<string>* seen = _tryStack.empty() ? nullptr : &_tryStack.back();
                sema::checkErrPropagateForIdCall(_currentFn, n, fnNameProp, sym, seen, _sourcePath);
            } else if (n->errPropagate()) {
                if (_tryStack.empty()) {
                    sema::checkBangWithoutFallibleCaller(_currentFn, n);
                }
            }
        } else if (n->errPropagate() && !dynamic_cast<p<ExprDotNode>>(calleeExpr)) {
            if (_tryStack.empty()) {
                sema::checkBangWithoutFallibleCaller(_currentFn, n);
            }
        }

        // Phase 3.3 前置.2：SemaPass 主动驱动重载解析 + 灵活整数推断.
        // 只接管"纯 ID callee + 无显式类型实参 + 非泛型"的情形, 与
        // compiler_call.cpp 中 `else if (structDecl)` / `else` 分支的进入条件保持一致;
        // 其余 (Fn 类型 callee / 泛型 fn 或 ctor / 方法调用) 仍由 codegen 自行处理.
        //
        // 副作用幂等性: tryInferIntType 仅在 isFlexibleIntExpr 为真时改写; SemaPass
        // 跑完后字面量已带类型, Compiler 端再次调用 resolve* 时 isFlexibleIntExpr 返回 false,
        // 不会重复推断 (见 call_resolve.h 的契约说明).
        if (auto lit = dynamic_cast<p<ExprLiteralNode>>(n->getCalleeExpr())) {
            if (auto obj = dynamic_cast<p<LiteralObjNode>>(lit->literal())) {
                string fnName = obj->getValue().getText();
                int line = n->getLineNumber();
                int col = n->getColumn();
                bool hasTypeArgs = !n->getTypeArgs().empty();

                // Phase 3.3.2.f: 自由 intrinsic arity 校验 (E6027).
                // helper 仅对清单内 fnName 实际校验, 其他 fnName 是 no-op,
                // 故无条件调用安全; 与 Compiler 端 compileExternalOrSdkFunctionCall
                sema::validateFreeIntrinsicArity(fnName, n->getArgs().size(), line, col);

                auto* structDecl = _file->getStructDecl(fnName);
                if (!structDecl && _sdkFile) structDecl = _sdkFile->getStructDecl(fnName);

                // Phase 3.3.2.f: Builtin 泛型 intrinsic 的 shape + type-shape 校验.
                // 接管 E6017/E6018/E6026-E6029/E6032 实际抛出点 (与 Compiler::compileGenericFunctionCall
                // 的 #Builtin 分支镜像).
                // 限制:
                //   * 仅在 callee 是 ID-literal 且解析到泛型 fn 且 fn 头部 hasAnno(Builtin) 时接管;
                //   * typeArgs 仅在显式 (`f:<T>(...)`) 时由 SemaPass 取; 无显式 typeArgs 的 Builtin
                //     推断仍跳过（as_ref(Heap<T>) 等特例在 Compiler）. 非 Builtin 的 E6012/E6013
                //     由下方 infer 在调用点 / 实例化后重抛.
                //   * argTypes 经 getType() 计算, 任一 arg 未推断 (lambda 形参) 时跳过.
                if (!structDecl) {
                    auto [genFn, _] = _file->getGenericFunction(fnName);
                    // getGenericFunction 已搜索 wildcardImports，不再需要手动 SDK 回退
                    if (genFn && genFn->header()->hasAnno("Builtin") && hasTypeArgs) {
                        vector<TypeInfo> typeArgs;
                        bool typeArgsOk = true;
                        try {
                            for (auto& tn : n->getTypeArgs())
                                typeArgs.push_back(tn->getType());
                        } catch (...) {
                            typeArgsOk = false;
                        }

                        vector<TypeInfo> argTypes;
                        bool argTypesOk = true;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(a->getType());
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }

                        if (typeArgsOk) {
                            sema::validateBuiltinIntrinsicShape(fnName, typeArgs.size(), n->getArgs().size(), line,
                                                                col);
                            if (argTypesOk) {
                                sema::validateBuiltinIntrinsicTypeShape(fnName, typeArgs, argTypes, n->getArgs(), _file,
                                                                        _sdkFile, line, col);

                                // Phase B-1: copy_of 拒绝 #NoCopy 类型（含 Array<T>，深拷贝统一用 .clone()）
                                if (fnName == "copy_of" && !typeArgs.empty()) {
                                    const auto& T = typeArgs[0];
                                    if (isNoCopyTypeIn(T, _file, _sdkFile)) {
                                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031, T.name,
                                                       "copy_of", T.name);
                                    }
                                }
                            }
                        }
                    }
                }

                // Phase 6D: 同名 ctor 已被 sema E3130 拦截在定义点; 调用点 `Foo(args)`
                // 不再分派 ctor, 形态 / arity 校验全部失效, 整段块移除.

                // Phase B-1: move:<T>(var) — 标记源变量为 moved (E4033 判定依据)
                if (_currentFn && fnName == "move" && !n->getArgs().empty()) {
                    if (auto argLit = dynamic_cast<p<ExprLiteralNode>>(n->getArgs()[0])) {
                        if (auto argObj = dynamic_cast<p<LiteralObjNode>>(argLit->literal())) {
                            _movedVars.insert(argObj->getValue().getText());
                        }
                    }
                }

                // 泛型 fn + 显式 typeArgs 的 arity 校验 (E6010, 与 compileCallExpr 入口一致)
                if (!structDecl && hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn2, _] = _file->getFunctionWithOwner(fnName);
                    if (genFn2 && genFn2->header()->isGeneric()) {
                        sema::validateGenericTypeArgsArity(fnName, genFn2->header()->typeParams().size(),
                                                           n->getTypeArgs().size(), line, col);
                    }
                }

                // Bucket 4 收口 (CURRENT-check.md): 泛型 fn typeArgs 的 spec bound
                // 校验 (E1106, E3032 由 helper 内部抛 draft 名未声明). 显式 typeArgs
                // 直接收取; 隐式 typeArgs 走 sema::inferGenericFnTypeArgs.
                // E6012/E6013：调用点与实例化复查重抛；未实例化的模板体内吞掉
                // （与未调用泛型 fn 一致，yux-check 是 yux build 的子集）.
                if (_yux && !structDecl) {
                    // 收集所有同名泛型重载（如 print<T>(x T) + print<T>(x T&)），
                    // 用 resolveBestGenericOverload 选最佳匹配后再 infer + spec-bound 校验。
                    // collectGenericFunctions 已搜索本地 + wildcardImports，不再需要手动 SDK 回退。
                    vector<pair<FnNode*, FileNode*>> genericFns;
                    _file->collectGenericFunctions(fnName, genericFns, _file);
                    FnNode* genericFn = nullptr;
                    p<FileNode> fnOwner = _file;
                    if (!genericFns.empty()) {
                        // 多泛型重载消歧：用实参类型驱动，选 Ref/Ref 匹配最佳者
                        if (genericFns.size() > 1) {
                            vector<TypeInfo> disambigArgTypes;
                            bool disambigOk = true;
                            for (auto& a : n->getArgs()) {
                                try {
                                    disambigArgTypes.push_back(a->getType());
                                } catch (...) {
                                    disambigOk = false;
                                    break;
                                }
                            }
                            if (disambigOk && !disambigArgTypes.empty()) {
                                auto [best, bestOwner] =
                                    sema::resolveBestGenericOverload(genericFns, n, fnName, disambigArgTypes);
                                if (best) {
                                    genericFn = best;
                                    fnOwner = bestOwner;
                                }
                            }
                        }
                        if (!genericFn) {
                            genericFn = genericFns[0].first;
                            fnOwner = genericFns[0].second;
                        }
                    }
                    if (genericFn && genericFn->header()->isGeneric() && !genericFn->header()->hasAnno("Builtin")) {
                        vector<TypeInfo> typeArgs;
                        bool argTypesOk = true;
                        vector<TypeInfo> argTypes;
                        for (auto& a : n->getArgs()) {
                            try {
                                argTypes.push_back(applyInstSubst(a->getType()));
                            } catch (...) {
                                argTypesOk = false;
                                break;
                            }
                        }
                        // BUG5: 非泛型重载优先 (call_fn.cpp Phase 4b)；
                        // 同名存在严格匹配的非泛型时，spec-bound / infer 不应越过重载消歧。
                        // 泛型自身的符号表项（形参与声明一致，含 `wrap<U>(x i32)` 这种
                        // T 不出现在形参里的）不当成非泛型。
                        if (argTypesOk && !hasTypeArgs) {
                            auto* nonGen = _file->lookupFnSymbolWithParams(fnName, argTypes);
                            if (!nonGen && _sdkFile && _sdkFile != _file) {
                                nonGen = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                            }
                            if (nonGen) {
                                bool isGenericOwnSym = false;
                                for (auto& [gFn, _] : genericFns) {
                                    auto gp = gFn->header()->params();
                                    if (nonGen->params.size() != gp.size()) continue;
                                    bool paramsMatch = true;
                                    for (size_t i = 0; i < gp.size(); ++i) {
                                        if (!gp[i]->type()) continue;
                                        if (nonGen->params[i] != gp[i]->type()->getType()) {
                                            paramsMatch = false;
                                            break;
                                        }
                                    }
                                    if (paramsMatch) {
                                        isGenericOwnSym = true;
                                        break;
                                    }
                                }
                                if (!isGenericOwnSym) {
                                    argTypesOk = false; // 真非泛型命中，跳过泛型 infer
                                }
                            }
                        }
                        bool typeArgsOk = true;
                        if (hasTypeArgs) {
                            try {
                                for (auto& tn : n->getTypeArgs()) {
                                    typeArgs.push_back(applyInstSubst(tn->getType()));
                                }
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else if (argTypesOk) {
                            try {
                                sema::inferGenericFnTypeArgs(n, genericFn, fnName, argTypes, typeArgs);
                                for (auto& t : typeArgs)
                                    t = applyInstSubst(t);
                            } catch (const YuxError&) {
                                // 调用点 / 实例化后：E6012 arity、E6013 无法反推。
                                // 未实例化模板体内仍吞（Compiler 同样不编未调用泛型体）.
                                if (_currentTypeParams.empty() || !_instSubst.empty()) throw;
                                typeArgsOk = false;
                            } catch (...) {
                                typeArgsOk = false;
                            }
                        } else {
                            typeArgsOk = false;
                        }
                        if (typeArgsOk && typeArgs.size() == genericFn->header()->typeParams().size()) {
                            sema::validateGenericTypeArgsSpecBound(&_yux->specRegistry(), &_yux->specImplChecker(),
                                                                   fnOwner, genericFn->header(), typeArgs, line, col);
                            // Phase C：typeArgs 已知后按替换后的形参检查实参（E3014）。
                            vector<TypeInfo> instParams;
                            if (substGenericCallParams(genericFn->header(), genericFn->header()->typeParams(), typeArgs,
                                                       instParams)) {
                                for (size_t i = 0; i < n->getArgs().size() && i < instParams.size(); ++i) {
                                    checkCallArgAgainst(n->getArgs()[i], instParams[i], line, col, _file, _sdkFile,
                                                        _currentTypeParams, &_instSubst);
                                }
                            }
                            checkGenericFnInst(genericFn, typeArgs);
                        }
                    }
                }

                // 仅在无显式 typeArgs + 非泛型路径上才驱动重载解析:
                // 泛型 fn 的 typeArgs 替换后实参检查在上方；非泛型走 resolveFnOverload。
                if (!hasTypeArgs) {
                    // getFunctionWithOwner 已搜索 wildcardImports
                    auto [genFn3, _] = _file->getFunctionWithOwner(fnName);
                    if (!genFn3 || !genFn3->header()->isGeneric()) {
                        if (structDecl && !structDecl->isGeneric()) {
                            sema::resolveCtorOverload(_file, fnName, n->getArgs(), line);
                            if (_sdkFile && _sdkFile != _file) {
                                sema::resolveCtorOverload(_sdkFile, fnName, n->getArgs(), line);
                            }
                        } else if (!structDecl) {
                            sema::resolveFnOverload(_file, _sdkFile, fnName, n->getArgs(), line);
                            // Phase 3.3.1.c: 非泛型 ID-callee 解析到 fnSymbol 后做可见性校验 (E6006).
                            // argTypes 经 getType() 计算; 若任一实参未推断 (lambda 形参等),
                            // 跳过并交给 Compiler 兜底.
                            vector<TypeInfo> argTypes;
                            bool ok = true;
                            for (auto& a : n->getArgs()) {
                                try {
                                    argTypes.push_back(a->getType());
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            if (ok) {
                                auto* fnSym = _file->lookupFnSymbolWithParams(fnName, argTypes);
                                if (!fnSym && _sdkFile && _sdkFile != _file) {
                                    fnSym = _sdkFile->lookupFnSymbolWithParams(fnName, argTypes);
                                }
                                sema::validateFnSymbolVisibility(fnSym, _file->moduleName(), fnName, line, col);
                                if (fnSym) n->setResolvedFn(fnSym);
                                // Phase B-1: #NoCopy 类型不可按值传参
                                if (fnSym) {
                                    for (size_t i = 0; i < n->getArgs().size() && i < fnSym->params.size(); ++i) {
                                        const auto& pt = fnSym->params[i];
                                        if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                            if (!isFreshHandleExpr(n->getArgs()[i])) {
                                                throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031,
                                                               pt.name, "按值传参", pt.name);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Phase 3.3.1.a: Dot-callee 包/模块别名调用 (E6001-E6005).
        // 与 compileMethodCall line 195-254 同款条件; argTypes 经 getType()
        // 计算, 任一参数未推断时跳过, 交给 Compiler 兜底.
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            // DRAFT-spec-disambig-at §3.3: `$.m@SpecA()` / `obj.m@SpecA()` 显式消歧.
            // 校验三件: (1) baseType T 必须在 #Impl 列表里出现 SpecA; (2) SpecA 必须
            // 含名为 m 的签名; (3) SpecA.m 必须带默认体 (纯抽象签名无法 disambiguate).
            if (dotCallee->hasSpecQualifier()) {
                TypeInfo baseType;
                bool baseOk = true;
                try {
                    baseType = dotCallee->baseExpr()->getType();
                } catch (...) {
                    baseOk = false;
                }
                if (baseOk && !baseType.name.empty()) {
                    const string& specName = dotCallee->specQualifier();
                    const string memberName = dotCallee->member();
                    int dline = dotCallee->resolveLineNumber();
                    int dcol = dotCallee->resolveColumn();
                    // Dyn<D> / Dyn<D&> 上 `d.m@SpecA()`: SpecA 必须等于 D (vtable 只
                    // 携带 D 的槽位, 无法 dispatch 到其它 spec). 等于 D 时直接走常规
                    // Dyn dispatch (codegen 不重写 member).
                    if (baseType.isDyn()) {
                        if (_yux) {
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolvedDyn = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            if (resolvedDyn.decl && resolvedDyn.decl->name().getText() != specName) {
                                throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                            }
                        }
                        // OK: 等价于 d.m(); 走 Dyn dispatch
                    } else {
                        auto* implNode = _file->getStructImpl(baseType.name);
                        if (!implNode && _sdkFile) {
                            implNode = _sdkFile->getStructImpl(baseType.name);
                        }
                        bool inImplList = false;
                        if (implNode) {
                            for (const auto& ref : implNode->specRefs()) {
                                if (ref.name == specName) {
                                    inImplList = true;
                                    break;
                                }
                            }
                        }
                        if (!inImplList) {
                            throw YuxError(dline, dcol, ErrorCode::E1101, baseType.name, specName, memberName);
                        }
                        SpecDeclNode* specDecl = nullptr;
                        if (_yux) {
                            auto resolved = _yux->specRegistry().resolve(specName, _file);
                            if (resolved) specDecl = resolved->decl;
                        }
                        if (specDecl) {
                            constexpr size_t kNoIdx = ~size_t{0};
                            auto sigIdx = kNoIdx;
                            for (size_t i = 0; i < specDecl->signatures().size(); ++i) {
                                if (specDecl->signatures()[i]->name().getText() == memberName) {
                                    sigIdx = i;
                                    break;
                                }
                            }
                            if (sigIdx == kNoIdx) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " (referenced via `@" + specName + "` — method missing in spec)");
                            }
                            if (!specDecl->hasDefaultBody(sigIdx)) {
                                throw YuxError(dline, dcol, ErrorCode::E1140, specName, memberName,
                                               " with a default body (`@" + specName +
                                                   "` disambiguation requires a default-body method)");
                            }
                        }
                    } // end else (non-Dyn)
                }
            }
            // Phase C：字段当 callee 且类型不是 Fn 值 → E3095。
            // @Spec 已在上面报完 E1101/E1140；未知方法的 getType 假阳性不走这里。
            if (!dotCallee->hasSpecQualifier()) {
                bool isField = false;
                try {
                    isField = dotCallee->isFieldAccess();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
                if (isField) {
                    TypeInfo fieldTy;
                    bool tyOk = true;
                    try {
                        fieldTy = dotCallee->hasResolvedType() ? dotCallee->resolvedType() : dotCallee->getType();
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                        tyOk = false;
                    }
                    if (tyOk && !fieldTy.empty() && !isFnCalleeType(fieldTy, _file, _sdkFile)) {
                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3095, fieldTy.getFullName());
                    }
                }
            }
            vector<TypeInfo> argTypes;
            bool ok = true;
            for (auto& a : n->getArgs()) {
                try {
                    argTypes.push_back(a->getType());
                } catch (...) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                auto modCall = sema::resolveModuleFnCall(_file, nullptr, n, dotCallee, argTypes);
                if (!modCall.matched) {
                    // Phase 3.3.2.f: 镜像 Compiler::compileMethodCall 的 baseType 派发,
                    // 主动调用 3.3.2.a / 3.3.2.e 抠出的 helper.
                    //   * baseType.isArrayGeneric() → validateArrayMethodCall (E3055/E6042/E6027)
                    //   * isBuiltinType + isBuiltinMethodIn → validateOperatorMethodCall (E6027/E3070)
                    // baseType 经 getType() 计算; 任一异常 (lambda 形参等) → 跳过, 交 Compiler 兜底.
                    // SemaPass 走非泛型 fn / 非泛型 impl 路径, 不需要 applySubst (替换栈为空).
                    TypeInfo baseType;
                    bool baseOk = true;
                    try {
                        baseType = dotCallee->baseExpr()->getType();
                        baseType = applyInstSubst(baseType);
                    } catch (...) {
                        baseOk = false;
                    }
                    if (baseOk) {
                        const string& member = dotCallee->member();
                        size_t argsCount = n->getArgs().size();
                        int dline = n->getLineNumber();
                        int dcol = n->getColumn();
                        if (baseType.isArrayGeneric()) {
                            bool baseIsLvalue = isLvalueArrayBase(dotCallee->baseExpr());
                            sema::validateArrayMethodCall(baseType, member, argsCount, baseIsLvalue, dline, dcol);
                        } else if (isBuiltinType(baseType.name) && isBuiltinMethodIn(_sdkFile, baseType.name, member)) {
                            sema::validateOperatorMethodCall(member, baseType, argsCount, dline, dcol);
                        } else if (baseType.isDyn() && _yux) {
                            // Bucket 4 收口 (CURRENT-check.md): Dyn<D> 方法调用 (E1131/E6016/
                            // E6012/E6015). 镜像 Compiler::compileDynMethodCall 顶部 — 通过
                            // resolveDynCalleeSpec 拿 specDecl, 再 resolveDynMethodSig 校验
                            // member 存在 + arity + 形参类型.
                            const SpecRegistry* reg = &_yux->specRegistry();
                            auto resolved = sema::resolveDynCalleeSpec(reg, _file, baseType, dline, dcol);
                            sema::resolveDynMethodSig(resolved.decl, resolved.qualified, baseType, member, argTypes,
                                                      dline, dcol);
                        }
                    }
                }
            }
        }

        // Fn-typed callee 实参类型校验：当 callee 静态类型为 Fn(...)R 时，
        // 检查每个实参与形参类型是否匹配（Ref<T> 不能隐式转为 T 等）。
        // 非 ID-literal callee（如 lambda 变量 f(xs[i])）走 compiler_lambda.cpp
        // 的 compileFnValueCall，此处提前检测避免 "bad signature" LLVM 断言。
        // 排除函数名字面量：它们虽然现在返回准确的 Fn TypeInfo，但实参类型
        // 校验（含 extern Ptr 自动转换）已在 codegen 的 matchFnParams 中完成。
        {
            TypeInfo calleeType;
            try {
                calleeType = n->getCalleeExpr()->getType();
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }

            if (calleeType.isFn()) {
                // 函数名字面量（如 `strLen(a)`）的参数类型匹配（含 extern Ptr
                // 自动转换）由 codegen matchFnParams 负责，不在此处重复校验
                bool isFnNameLiteral = false;
                if (auto* lit = dynamic_cast<ExprLiteralNode*>(n->getCalleeExpr())) {
                    if (auto* objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                        auto scope = objLit->findNearestScope();
                        if (scope) {
                            auto* sym = scope->lookupSymbol(objLit->getValue().getText());
                            if (sym && sym->kind == SymbolKind::Function) {
                                isFnNameLiteral = true;
                            }
                        }
                    }
                }
                if (!isFnNameLiteral) {
                    const auto& expectedParams = calleeType.fnParamTypes();
                    for (size_t idx = 0; idx < n->getArgs().size() && idx < expectedParams.size(); ++idx) {
                        try {
                            auto argType = n->getArgs()[idx]->getType();
                            if (expectedParams[idx] && !argType.name.empty() && !argType.isSelf() &&
                                !expectedParams[idx]->isSelf()) {
                                // Ref<T> 实参传给值类型形参 T
                                if (argType.isRef() && !expectedParams[idx]->isRef()) {
                                    auto inner = argType.refElementType();
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                                   expectedParams[idx]->getFullName(), argType.getFullName())
                                        .withHint(
                                            std::format("实参类型为 `{}&`（借用），形参期望 `{}`；"
                                                        "若需取值请用 `copy_of:<{}>(...)` 或先 `let tmp {} = expr`",
                                                        inner ? inner->name : "?", expectedParams[idx]->getFullName(),
                                                        inner ? inner->name : "?", inner ? inner->name : "?"));
                                }
                                // 值类型不匹配
                                if (!argType.isRef() && !expectedParams[idx]->isRef() &&
                                    argType != *expectedParams[idx]) {
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3014,
                                                   expectedParams[idx]->getFullName(), argType.getFullName())
                                        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配",
                                                              argType.getFullName(),
                                                              expectedParams[idx]->getFullName()));
                                }
                            }
                        } catch (const YuxError&) {
                            throw;
                        } catch (...) { // NOLINT(bugprone-empty-catch)
                            // getType 失败: 留 Compiler 兜底
                        }
                    }
                } // if (!isFnNameLiteral)
            }
        }
        // struct 方法私有可见性检查（E6007）——因 yux-check 不跑 LLVM
        // codegen，必须在 sema 阶段独立校验。与 codegen compileStructMethodCall
        // 中的 validateStructMethodVisibility 同义，构成双重保障。
        if (auto dotCallee = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr())) {
            try {
                TypeInfo rawBase = dotCallee->baseExpr()->getType();
                if (rawBase.isRef()) {
                    if (auto inner = rawBase.refElementType()) rawBase = *inner;
                }
                if (rawBase.isRc()) {
                    if (auto inner = rawBase.rcElementType()) rawBase = *inner;
                }
                TypeInfo baseType = applyInstSubst(rawBase);
                // Phase C：实例化后 TypeParam 已换成具体类型，查方法是否存在。
                // 模板期 raw 仍是 T → 跳过。`<T : D>` 边界方法按边界认。
                if (!isCurrentTypeParam(baseType) && !_instSubst.empty() && isCurrentTypeParam(rawBase) &&
                    !baseType.isDyn() && !baseType.isPtr() && !baseType.name.empty()) {
                    string member = dotCallee->member();
                    auto rt = instantiatedMethodRet(_currentFn, _file, _sdkFile, rawBase, baseType, member);
                    if (!rt) {
                        throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E3095, baseType.getFullName());
                    }
                    n->setResolvedType(*rt);
                }
                if (isCurrentTypeParam(baseType)) {
                    // Phase C：不透明 TypeParam，方法存在性等实例化后再查
                } else if (!baseType.name.empty() && !baseType.isDyn() && !isBuiltinType(baseType.name) &&
                           !baseType.isArrayGeneric() && !baseType.isPtr()) {
                    string member = dotCallee->member();
                    if (dotCallee->hasSpecQualifier()) {
                        member = member + "__at__" + dotCallee->specQualifier();
                    }
                    // Phase B：与 compileCallExpr 对齐，方法重载 + 灵活整数推断进 SemaPass。
                    sema::resolveMethodOverload(_file, _sdkFile, baseType.name, member, n->getArgs(),
                                                n->getLineNumber());
                    string methodFullName = baseType.name + "." + member;
                    vector<TypeInfo> methodParamTypes;
                    methodParamTypes.push_back(baseType);
                    for (auto& a : n->getArgs()) {
                        try {
                            methodParamTypes.emplace_back(a->getType());
                        } catch (...) {
                            methodParamTypes.emplace_back();
                        }
                    }
                    auto* methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    if (!methodSymbol && _sdkFile && _sdkFile != _file) {
                        methodSymbol = _sdkFile->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
                    }
                    if (methodSymbol) n->setResolvedFn(methodSymbol);
                    sema::validateStructMethodVisibility(methodSymbol, _currentStructName, baseType.name,
                                                         dotCallee->member(), n->getLineNumber(), n->getColumn());
                    // Phase C：泛型 struct 实例方法，用接收者 typeArgs 替换形参后检查实参
                    if (auto* sd = _names.lookupStruct(baseType.name)) {
                        if (sd->isGeneric()) {
                            map<string, TypeInfo> subst;
                            if (fillSubstFromGenericArgs(sd->typeParams(), baseType.genericArgs, subst)) {
                                auto* impl = lookupStructImpl(_file, _sdkFile, baseType.name);
                                if (auto* hdr = uniqueMethodHeader(impl, dotCallee->member(), n->getArgs().size(),
                                                                   /*wantStatic=*/false)) {
                                    vector<TypeInfo> instParams;
                                    if (substHeaderParams(hdr, subst, instParams)) {
                                        for (size_t i = 0; i < n->getArgs().size() && i < instParams.size(); ++i) {
                                            checkCallArgAgainst(n->getArgs()[i], instParams[i], n->getLineNumber(),
                                                                n->getColumn(), _file, _sdkFile, _currentTypeParams,
                                                                &_instSubst);
                                        }
                                    }
                                }
                                checkGenericImplInst(impl, subst);
                            }
                        }
                    }
                    // Phase B-1: 方法调用的 #NoCopy 按值传参检查
                    if (methodSymbol) {
                        for (size_t i = 0; i < n->getArgs().size() && i < methodSymbol->params.size(); ++i) {
                            const auto& pt = methodSymbol->params[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->getArgs()[i])) {
                                    throw YuxError(n->getLineNumber(), n->getColumn(), ErrorCode::E4031, pt.name,
                                                   "按值传参", pt.name);
                                }
                            }
                        }
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败或符号查找失败——留 Compiler 兜底
            }
        }
        // 未知方法等：getType 回落成基类型抛的 E3095，@Spec / TypeParam 已排除。
        if (deferredMethodE3095) {
            auto* dot = dynamic_cast<p<ExprDotNode>>(n->getCalleeExpr());
            if (dot && !dot->hasSpecQualifier()) {
                TypeInfo baseType;
                bool baseOk = true;
                try {
                    baseType = dot->baseExpr()->hasResolvedType() ? dot->baseExpr()->resolvedType()
                                                                  : dot->baseExpr()->getType();
                    baseType = applyInstSubst(baseType.peelAutoDeref());
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    baseOk = false;
                }
                if (!(baseOk && isCurrentTypeParam(baseType))) {
                    bool isField = false;
                    try {
                        isField = dot->isFieldAccess();
                    } catch (...) { // NOLINT(bugprone-empty-catch)
                    }
                    // 实例化后接收者已有该方法：getType 仍按 T 抛的 E3095 不重抛。
                    if (!isField && !(baseOk && receiverHasMethod(baseType, dot->member(), _file, _sdkFile))) {
                        throw *deferredMethodE3095;
                    }
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDotNode>>(expr)) {
        visitExpr(n->baseExpr());

        // DRAFT-spec-reflect Phase 4: 实例形访问 `c.type` / `c.fields` / `c.methods` /
        // `c.variants` / `$.type` / `$.fields` 拦截 (草案 §5 / [#1.AB]);
        // 提示用 `<Type>::field` / `Self::field`.
        // 仅当 base 是已知 struct 且不含同名 instance 字段时触发 (用户若自己声明
        // `type` 字段, 走常规字段访问).
        {
            string mem = n->member();
            if (mem == "type" || mem == "fields" || mem == "methods" || mem == "variants") {
                TypeInfo bt;
                try {
                    bt = n->baseExpr()->getType();
                } catch (...) {
                    bt = TypeInfo();
                }
                if (bt.isRef()) {
                    if (auto inner = bt.refElementType()) bt = *inner;
                }
                if (bt.isRc()) {
                    if (auto inner = bt.rcElementType()) bt = *inner;
                }
                if (bt.kind == TypeKind::Normal && !bt.name.empty()) {
                    auto* sd = _file ? _file->getStructDecl(bt.name) : nullptr;
                    if (!sd && _sdkFile && _sdkFile != _file) sd = _sdkFile->getStructDecl(bt.name);
                    if (sd && sd->fieldIndex(mem) < 0) {
                        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E1138, mem, bt.name,
                                       bt.name, mem);
                    }
                }
            }
        }

        // Phase 3.4.d.2: 字段私有可见性 (E3042). safe `?.` 路径在 helper 内
        // 自跳过 (走 getType, SemaPass 默认重抛). 异常静默吞掉, 留 Compiler.
        try {
            sema::validateDotFieldPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprIfElseNode>>(expr)) {
        visitExpr(n->condition());
        // Phase B-1: 分支 _movedVars 汇合 — 各分支分别从 saved 出发，最后取并集
        // Phase C：块末尾值带靶向类型（嵌套数组 E3009）。
        auto savedMoved = _movedVars;
        visitBlock(n->thenBlock(), expected);
        auto afterThenMoved = std::move(_movedVars);
        _movedVars = savedMoved;

        for (auto& el : n->elifs()) {
            visitExpr(el->condition());
            auto savedElif = _movedVars;
            visitBlock(el->block(), expected);
            for (auto& v : _movedVars)
                afterThenMoved.insert(v);
            _movedVars = savedElif;
        }

        if (n->elseBlock()) {
            visitBlock(n->elseBlock(), expected);
            for (auto& v : afterThenMoved)
                _movedVars.insert(v);
        } else {
            _movedVars = std::move(afterThenMoved);
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprOneLineIfElseNode>>(expr)) {
        visitExpr(n->condition());
        visitExpr(n->trueValue(), expected);
        visitExpr(n->falseValue(), expected);
        return;
    }
    if (auto n = dynamic_cast<p<ExprGetNode>>(expr)) {
        visitExpr(n->arrayExpr());
        for (auto& i : n->indices())
            visitExpr(i);
        tryValidateIndexBase(n->arrayExpr(), n->resolveLineNumber(), n->resolveColumn());
        return;
    }
    if (auto n = dynamic_cast<p<ExprArrayNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprTupleNode>>(expr)) {
        for (auto& e : n->elements())
            visitExpr(e);
        return;
    }
    if (auto n = dynamic_cast<p<ExprUnaryNode>>(expr)) {
        visitExpr(n->right());
        string m;
        switch (n->op()) {
        case ExprUnaryNode::Op::Neg:
            m = "neg";
            break;
        case ExprUnaryNode::Op::Rev:
            m = "inv";
            break;
        case ExprUnaryNode::Op::Not:
            m = "not";
            break;
        }
        tryValidateUnaryOpMethod(n->right(), m, n->getLineNumber(), n->getColumn());
        return;
    }
    if (auto n = dynamic_cast<p<LambdaExprNode>>(expr)) {
        // v0.16 闭包捕获: sema 下钻 lambda body (策略 2b 宽松模式)。
        // - 形参类型可能缺 (由调用点反推), 不依赖形参类型的检查 deferred 给 codegen。
        // - 不依赖形参类型的检查在此完成: E2030 (捕获写禁) / E4024 (Heap 非空捕获禁) /
        //   E4022 数据收集 (hasRefCapture)。
        // - 下钻前保存外层 lambda 状态, 支持嵌套闭包。
        auto savedLambda = _currentLambda;
        auto savedHasRef = _currentLambdaHasRefCapture;
        _currentLambda = n;
        _currentLambdaHasRefCapture = false;

        const TypeInfo* bodyExp = nullptr;
        TypeInfo bodyRetStorage;
        TypeInfo bodyRetResolved;
        if (lambdaExpectedRetType(n, bodyRetStorage)) {
            bodyRetResolved = resolveForRet(
                bodyRetStorage,
                RetCheck{.file = _file, .sdk = _sdkFile, .fn = _currentFn, .structName = _currentStructName});
            bodyExp = &bodyRetResolved;
        }
        if (n->bodyExpr()) {
            visitExpr(n->bodyExpr(), bodyExp);
            if (bodyExp) {
                int line = n->bodyExpr()->resolveLineNumber();
                checkRetExpr(
                    n->bodyExpr(), bodyRetStorage, !bodyRetStorage.empty(), line,
                    RetCheck{.file = _file, .sdk = _sdkFile, .fn = _currentFn, .structName = _currentStructName});
            }
        } else {
            const auto& stmts = n->bodyStmts();
            for (size_t i = 0; i < stmts.size(); ++i) {
                const bool lastBare = (i + 1 == stmts.size()) && isBareTailExprStmt(stmts[i]);
                if (lastBare && bodyExp) {
                    auto se = dynamic_cast<p<StatementExprNode>>(stmts[i]);
                    if (!se || !se->expr()) {
                        visitStmt(stmts[i]);
                        continue;
                    }
                    if (auto ma = dynamic_cast<p<ExprMoveAssignNode>>(se->expr())) {
                        DiagnosticEngine::emit(
                            _sourcePath, YuxError(ma->resolveLineNumber(), ma->resolveColumn(), ErrorCode::E4030));
                    }
                    visitExpr(se->expr(), bodyExp);
                    int line = se->expr()->resolveLineNumber();
                    checkRetExpr(
                        se->expr(), bodyRetStorage, !bodyRetStorage.empty(), line,
                        RetCheck{.file = _file, .sdk = _sdkFile, .fn = _currentFn, .structName = _currentStructName});
                } else {
                    visitStmt(stmts[i]);
                }
            }
        }

        // 将 hasRefCapture 写回 LambdaExprNode, 供 E4022 检查 (StatementRetNode /
        // StatementDeclareAssignNode) 读取。
        if (_currentLambdaHasRefCapture) {
            n->setHasRefCapture(true);
        }

        _currentLambda = savedLambda;
        _currentLambdaHasRefCapture = savedHasRef;
        return;
    }
    if (auto n = dynamic_cast<p<ExprStructLitNode>>(expr)) {
        // Phase 2d 构造模型重构: `Self { ... }` 字段字面量校验.
        //   * 出现位: 仅 `#Static fn` 体内 (E3124, 仅 Self 形态).
        //   * 完整性: 必须列全所属结构体所有字段 (E3125).
        //   * 已知字段: `.name` 必须是所属结构体的字段 (E3126).
        //   * 唯一: 同名 `.field` 出现两次报 (E3127).
        // DRAFT-const-eval Phase 5: TypeName{...} 形态放行至任意 expr 位.
        // codegen 仍走 E0000 占位 (Phase 3 接管).
        int line = n->resolveLineNumber();
        int col = n->resolveColumn();
        string structName;
        if (n->isSelfForm()) {
            if (!_currentFn || !_currentFn->header()->isStatic() || _currentStructName.empty()) {
                throw YuxError(line, col, ErrorCode::E3124);
            }
            structName = _currentStructName;
        } else {
            structName = n->structName();
        }
        StructDeclNode* decl = _file ? _file->getStructDecl(structName) : nullptr;
        if (!decl && _sdkFile && _sdkFile != _file) {
            decl = _sdkFile->getStructDecl(structName);
        }
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E3124);
        }
        std::set<string> seen;
        for (auto& fi : n->fields()) {
            string fname = fi->name().getText();
            size_t fline = fi->name().getLine();
            int fcol = static_cast<int>(fi->name().getCharPositionInLine());
            if (decl->fieldIndex(fname) < 0) {
                throw YuxError(fline, fcol, ErrorCode::E3126, structName, fname);
            }
            if (!seen.insert(fname).second) {
                throw YuxError(fline, fcol, ErrorCode::E3127, fname);
            }
            TypeInfo fieldExpected;
            const TypeInfo* fieldExpPtr = nullptr;
            if (!decl->isGeneric()) {
                int fieldIdx = decl->fieldIndex(fname);
                if (fieldIdx >= 0) {
                    fieldExpected = decl->fields()[static_cast<size_t>(fieldIdx)]->getType();
                    fieldExpPtr = &fieldExpected;
                }
            }
            visitExpr(fi->value(), fieldExpPtr);
            // Phase B-1: #NoCopy 字段不可从现有变量隐式复制
            if (decl) {
                int fieldIdx = decl->fieldIndex(fname);
                if (fieldIdx >= 0) {
                    auto* fieldDecl = decl->fields()[fieldIdx];
                    auto fieldType = fieldDecl->getType();
                    if (isNoCopyTypeIn(fieldType, _file, _sdkFile)) {
                        if (!isFreshHandleExpr(fi->value())) {
                            throw YuxError(fline, fcol, ErrorCode::E4031, fieldType.name, "struct 字面量字段初始化",
                                           fieldType.name);
                        }
                    }
                }
            }
        }
        if (seen.size() != decl->fields().size()) {
            for (auto& f : decl->fields()) {
                if (!seen.count(f->name().getText())) {
                    throw YuxError(line, col, ErrorCode::E3125, structName, f->name().getText());
                }
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprPathCallNode>>(expr)) {
        vector<TypeInfo> pathArgExpected;
        const vector<TypeInfo>* pathArgExpPtr = nullptr;
        if (n->lhsTypeArgs().empty() &&
            agreedStaticMethodParams(_file, _sdkFile, n->enumName().getText(), n->variantName().getText(),
                                     n->args().size(), pathArgExpected)) {
            pathArgExpPtr = &pathArgExpected;
        } else if (!n->lhsTypeArgs().empty()) {
            // Phase C：泛型 struct #Static fn + turbofish，替换后的形参作靶向类型
            string lhsName = n->enumName().getText();
            auto* sd = _names.lookupStruct(lhsName);
            if (sd && sd->isGeneric()) {
                map<string, TypeInfo> subst;
                if (fillSubstFromTypeNodes(sd->typeParams(), n->lhsTypeArgs(), subst)) {
                    for (auto& [_, t] : subst)
                        t = applyInstSubst(t);
                    auto* impl = lookupStructImpl(_file, _sdkFile, lhsName);
                    if (auto* hdr = uniqueMethodHeader(impl, n->variantName().getText(), n->args().size(),
                                                       /*wantStatic=*/true)) {
                        if (substHeaderParams(hdr, subst, pathArgExpected)) {
                            pathArgExpPtr = &pathArgExpected;
                        }
                    }
                }
            }
        }
        visitExprList(n->args(), pathArgExpPtr);

        // Phase B：Array:<T>::with_capacity 形态校验（E6011 / E3131）。
        // 泛型 struct 的 #Static fn 路径 skipTypeCheck，必须在此单独接管。
        if (n->enumName().getText() == "Array" && n->variantName().getText() == "with_capacity") {
            sema::validateArrayWithCapacity(n);
        }

        // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` /
        // `<Struct>::methods` / `<Struct>::variants` reflect 静态访问.
        // 优先于 impl-method / enum-ctor 分流 (struct 无需 impl 也能取反射元数据).
        {
            string lhsName = n->enumName().getText();
            string rhsName = n->variantName().getText();
            if (n->args().empty() &&
                (rhsName == "type" || rhsName == "fields" || rhsName == "methods" || rhsName == "variants")) {
                auto* sd = _file ? _file->getStructDecl(lhsName) : nullptr;
                if (!sd && _sdkFile && _sdkFile != _file) sd = _sdkFile->getStructDecl(lhsName);
                if (sd) return;
            }
        }

        // Phase 2c 构造模型重构: `Type::name(...)` 按 LHS 分流.
        //   * LHS 是 struct -> 必须是 #Static 方法 (E3120/E3121); codegen Phase 3 落地.
        //   * LHS 是 enum   -> 走原 validateEnumCtorShape 路径 (E2019/E2020/E2021/E2032).
        // struct/enum 重名在 yux 里非法 (E2017), 此处直接按 lhsName 查 struct 优先.
        {
            string lhsName = n->enumName().getText();

            // DRAFT-static-vars Phase 4: 零参且 LHS 是 struct 且 RHS 是静态字段 → 放行
            // includeBuiltin=true：允许 #Builtin struct（如 i8）上的静态字段访问（如 i8::MAX）
            if (n->args().empty()) {
                auto* structDecl = _file ? _file->getStructDecl(lhsName, /*includeBuiltin=*/true) : nullptr;
                if (!structDecl && _sdkFile && _sdkFile != _file) {
                    structDecl = _sdkFile->getStructDecl(lhsName, /*includeBuiltin=*/true);
                }
                if (structDecl) {
                    if (auto* sf = structDecl->staticField(n->variantName().getText())) {
                        // 设置正确类型（字段类型而非 struct 类型）
                        n->setResolvedType(sf->type->getType());
                        return;
                    }
                }
            }

            auto* structImpl = _file ? _file->getStructImpl(lhsName) : nullptr;
            if (!structImpl && _sdkFile && _sdkFile != _file) {
                structImpl = _sdkFile->getStructImpl(lhsName);
            }
            if (structImpl) {
                string rhsName = n->variantName().getText();

                // DRAFT-static-vars Phase 4: 若零参且 RHS 是静态字段名 → 放行
                if (n->args().empty()) {
                    auto* structDecl = _file ? _file->getStructDecl(lhsName, /*includeBuiltin=*/true) : nullptr;
                    if (!structDecl && _sdkFile && _sdkFile != _file) {
                        structDecl = _sdkFile->getStructDecl(lhsName, /*includeBuiltin=*/true);
                    }
                    if (structDecl && structDecl->staticField(rhsName)) {
                        return; // 静态字段读，放行
                    }
                }

                p<FnHeaderNode> methodHeader = nullptr;
                for (auto& m : structImpl->methods()) {
                    if (m->header()->name().getText() == rhsName) {
                        methodHeader = m->header();
                        break;
                    }
                }
                int line = n->resolveLineNumber();
                int col = n->resolveColumn();
                if (!methodHeader) {
                    throw YuxError(line, col, ErrorCode::E3121, lhsName, rhsName);
                }
                if (!methodHeader->isStatic()) {
                    throw YuxError(line, col, ErrorCode::E3120, lhsName, rhsName, rhsName);
                }
                // Bucket 4 收口 (CURRENT-check.md): #Static fn 调用站点的 arity +
                // 类型校验 (E3131). 镜像 compiler_expr.cpp::compileEnumCtorExpr 的
                // #Static fn 分派 (2343-2377). 泛型 struct 在 turbofish 齐时
                // TypeInfo::substitute 替换形参，不再 skip。
                bool skipTypeCheck = false;
                map<string, TypeInfo> staticSubst;
                auto* structDecl = _file ? _file->getStructDecl(lhsName) : nullptr;
                if (!structDecl && _sdkFile && _sdkFile != _file) {
                    structDecl = _sdkFile->getStructDecl(lhsName);
                }
                if (structDecl && structDecl->isGeneric()) {
                    if (!fillSubstFromTypeNodes(structDecl->typeParams(), n->lhsTypeArgs(), staticSubst)) {
                        skipTypeCheck = true;
                    } else {
                        for (auto& [_, t] : staticSubst)
                            t = applyInstSubst(t);
                    }
                }
                if (!skipTypeCheck) {
                    vector<TypeInfo> paramTypes;
                    bool paramTypesOk = true;
                    for (auto p : methodHeader->params()) {
                        if (p->type()) {
                            try {
                                TypeInfo pt = p->type()->getType();
                                if (!staticSubst.empty()) pt = pt.substitute(staticSubst);
                                paramTypes.push_back(std::move(pt));
                            } catch (...) {
                                paramTypesOk = false;
                                break;
                            }
                        } else {
                            paramTypesOk = false;
                            break;
                        }
                    }
                    if (paramTypesOk) {
                        // 灵活整数实参按形参类型回填 (与 Compiler 端 2340 一致)
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            tryInferIntType(n->args()[i], paramTypes[i]);
                        }
                        auto renderTypes = [](const vector<TypeInfo>& ts) {
                            string s;
                            for (size_t i = 0; i < ts.size(); ++i) {
                                if (i) s += ", ";
                                s += ts[i].getFullName();
                            }
                            return s;
                        };
                        // arity 校验
                        if (n->args().size() != paramTypes.size()) {
                            string expected = renderTypes(paramTypes);
                            vector<TypeInfo> argTypesRaw;
                            bool ok = true;
                            for (auto& a : n->args()) {
                                try {
                                    argTypesRaw.push_back(a->getType());
                                } catch (...) {
                                    ok = false;
                                    break;
                                }
                            }
                            string got = ok ? renderTypes(argTypesRaw) : string("<unresolved>");
                            throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(), expected,
                                           n->args().size(), got);
                        }
                        // 类型逐位比对
                        vector<TypeInfo> argTypes;
                        bool argOk = true;
                        for (auto& a : n->args()) {
                            try {
                                argTypes.push_back(a->getType());
                            } catch (...) {
                                argOk = false;
                                break;
                            }
                        }
                        if (argOk) {
                            for (size_t i = 0; i < argTypes.size(); ++i) {
                                if (argTypes[i].empty()) continue;
                                if (!(argTypes[i] == paramTypes[i])) {
                                    // Nullable<T> 形参接受 T 值实参（自动包装）
                                    bool nullableMatch = false;
                                    if (paramTypes[i].isNullable()) {
                                        auto inner = paramTypes[i].nullableInnerType();
                                        if (inner && *inner == argTypes[i]) nullableMatch = true;
                                    }
                                    if (!nullableMatch) {
                                        throw YuxError(line, col, ErrorCode::E3131, lhsName, rhsName, paramTypes.size(),
                                                       renderTypes(paramTypes), argTypes.size(), renderTypes(argTypes));
                                    }
                                }
                            }
                        }
                        // Phase B-1: #NoCopy 类型不可按值传参（#Static fn 调用）
                        for (size_t i = 0; i < n->args().size() && i < paramTypes.size(); ++i) {
                            const auto& pt = paramTypes[i];
                            if (isNoCopyTypeIn(pt, _file, _sdkFile)) {
                                if (!isFreshHandleExpr(n->args()[i])) {
                                    throw YuxError(line, col, ErrorCode::E4031, pt.name, "按值传参", pt.name);
                                }
                            }
                        }
                    }
                }
                if (!staticSubst.empty()) {
                    checkGenericImplInst(structImpl, staticSubst);
                }
                return;
            }
        }

        // Phase 3.4.a: SemaPass 接管 E2019/E2020/E2021/E2032.
        // node->setResolvedType 已在 visitExpr 顶部写好 (getType 抛错时已在白名单
        // 重抛, 否则吞掉; 这里能跑到说明 getType 至少没抛已迁移码).
        // 任一异常被 helper 内部 try/catch (E2032 路径) 吞掉; E2019/E2020/E2021
        // 由 helper 主动抛出, SemaPass 实际接管.
        try {
            sema::validateEnumCtorShape(_file, _sdkFile, n);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御: helper 内部异常 (理论不应出现) 跳过, 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMatchNode>>(expr)) {
        visitExpr(n->scrutinee());
        for (auto& arm : n->arms()) {
            if (arm->hasBlock())
                visitBlock(arm->block(), expected);
            else
                visitExpr(arm->body(), expected);
        }
        checkMatchArmTypes(n->arms(), _currentTypeParams, &_instSubst);

        // Phase C：scrut 别名 / Rc<E> / Heap<E> / E& 与 compileMatchExpr 对齐。
        // 内层是 enum 才剥 wrapper；临时 Rc/Heap 直接 match → E2022。
        try {
            TypeInfo checkType = sema::resolveAlias(n->scrutinee()->getType(), _file, _sdkFile);
            int line = n->getLineNumber();
            int col = n->getColumn();
            auto peelEnumWrapper = [&](bool isRc) {
                auto inner = isRc ? checkType.rcElementType() : checkType.heapElementType();
                if (!inner) return;
                TypeInfo in = sema::resolveAlias(*inner, _file, _sdkFile);
                if (!_names.lookupEnum(in.name)) return;
                if (isFreshHandleExpr(n->scrutinee())) {
                    throw YuxError(line, col, ErrorCode::E2022, checkType.name)
                        .withHint(isRc ? "不支持对临时 Rc<E> 直接 match；先 `let b Rc<E> = ...` 落地再 match b"
                                       : "不支持对临时 Heap<E> 直接 match；先 `let h Heap<E> = ...` 落地再 match h");
                }
                checkType = std::move(in);
            };
            if (checkType.isRc()) {
                peelEnumWrapper(true);
            } else if (checkType.isHeap()) {
                peelEnumWrapper(false);
            } else if (checkType.isRef()) {
                if (auto inner = checkType.refElementType()) {
                    TypeInfo in = sema::resolveAlias(*inner, _file, _sdkFile);
                    if (_names.lookupEnum(in.name)) checkType = std::move(in);
                }
            }
            auto* enumDecl = _names.lookupEnum(checkType.name);
            if (enumDecl) {
                sema::validateMatchArms(enumDecl, checkType.name, n, _file);
            } else if (isBuiltinType(checkType.name) || checkType.isString()) {
                throw YuxError(line, col, ErrorCode::E2022, checkType.name);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 等内部异常: 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprTryCatchNode>>(expr)) {
        // Phase 3.3 前置.5: SemaPass 接管 E7011 (catch 类型必须是已声明 enum)
        // 与 E7002 (try block 内 callee 错误类型未被任一 catch 覆盖).
        //
        // 顺序:
        //   1) 逐 arm 校验 errType 为已声明 enum (E7011), 同时收集 catchTypes;
        //   2) push 新的 seenErrTypes 层, visitBlock(tryBlock) —— 内部 ID-callee
        //      检查把 #Fallible callee 的错误类型 append 进栈顶;
        //   3) pop 取出 seenErrTypes, 与 catchTypes 比对穷尽性 (E7002);
        //   4) 再访问每个 catch arm body (catches 在外层 try 视野之外).
        vector<string> catchTypes;
        catchTypes.reserve(n->catches().size());
        int line = n->getLineNumber();
        int col = n->getColumn();
        for (auto& arm : n->catches()) {
            const string& errType = arm->errType();
            auto* enumDecl = _names.lookupEnum(errType);
            if (!enumDecl) {
                int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                throw YuxError(aline, acol, ErrorCode::E7011, arm->errName().getText(), errType, errType);
            }
            catchTypes.push_back(errType);
        }

        _tryStack.emplace_back();
        visitBlock(n->tryBlock(), expected);
        vector<string> seenErrTypes = std::move(_tryStack.back());
        _tryStack.pop_back();

        for (auto& seen : seenErrTypes) {
            bool covered = false;
            for (auto& ct : catchTypes) {
                if (ct == seen) {
                    covered = true;
                    break;
                }
            }
            if (!covered) {
                throw YuxError(line, col, ErrorCode::E7002, seen, string("<unknown>"), seen);
            }
        }

        for (auto& c : n->catches())
            visitBlock(c->body(), expected);

        // Bucket 6 (CURRENT-check.md): SemaPass 接管 E7010 (catch arm body 末
        // 表达式类型必须与 try block 末表达式类型一致).
        //
        // 仅在 try block hasResult 且 result expr getType 成功时启用; 任一 arm
        // 的 getType 抛错 (lambda 形参等) 跳过该 arm, 留 Compiler 兜底. 流终止
        // arm 自然 hasResult=false, 此处略过. 与 Compiler 端 (compiler_expr.cpp
        // E7010 throw) 同语义按 .name 比对.
        if (n->tryBlock()->hasResult() && n->tryBlock()->resultExpr()) {
            try {
                auto resultType = n->tryBlock()->resultExpr()->getType();
                for (auto& arm : n->catches()) {
                    if (!arm->body()->hasResult() || !arm->body()->resultExpr()) continue;
                    try {
                        auto armT = arm->body()->resultExpr()->getType();
                        if (armT.name != resultType.name) {
                            int aline = arm->getLineNumber() > 0 ? arm->getLineNumber() : line;
                            int acol = arm->getColumn() > 0 ? arm->getColumn() : col;
                            throw YuxError(aline, acol, ErrorCode::E7010, armT.name, resultType.name);
                        }
                    } catch (const YuxError&) {
                        throw;
                    } catch (...) { // NOLINT(bugprone-empty-catch) — arm getType 失败: 留 Compiler 兜底
                    }
                }
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch) — try result getType 失败: 留 Compiler 兜底
            }
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprDynCtorNode>>(expr)) {
        visitExpr(n->arg());
        // Bucket 4 收口 (CURRENT-check.md): Dyn<D>(x) 构造的 E1131/E1132/E1134/E1133
        // 接管. 镜像 compiler_expr.cpp::compileDynCtorExpr 顶部 (line 2497-2576).
        // 仅在 _yux 就绪时校验 (spec 注册表 + impl 检查器都从 Yux 取); SDK 自构建
        // 等无 Yux 场景 skip, 留 Compiler 兜底.
        if (!_yux) return;
        try {
            auto resultType = n->getType();
            int line = n->getLineNumber();
            int col = n->getColumn();
            auto specInner = resultType.dynSpecType();
            string specBareName = specInner ? specInner->name : string();

            auto& reg = _yux->specRegistry();
            SpecDeclNode* specDecl = nullptr;
            string specQualified;
            if (!specBareName.empty()) {
                if (auto resolved = reg.resolve(specBareName, _file)) {
                    specDecl = resolved->decl;
                    specQualified = resolved->qualifiedName;
                }
            }
            if (!specDecl) {
                throw YuxError(line, col, ErrorCode::E1131, specBareName.empty() ? string("?") : specBareName);
            }
            if (specInner && specInner->isDyn()) {
                throw YuxError(line, col, ErrorCode::E1132, resultType.getFullName());
            }
            auto& checker = _yux->specImplChecker();
            if (!checker.specIsObjectSafe(specDecl)) {
                throw YuxError(line, col, ErrorCode::E1134, specQualified, specQualified, specQualified);
            }

            auto argType = n->arg()->getType();
            bool isBorrow = n->isBorrow();
            string concreteBare;
            if (isBorrow) {
                if (argType.isRef()) {
                    if (auto inner = argType.refElementType()) concreteBare = inner->name;
                } else if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            } else {
                if (argType.isRc()) {
                    if (auto inner = argType.rcElementType()) concreteBare = inner->name;
                }
            }
            if (concreteBare.empty()) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
            TypeInfo concreteTI(concreteBare);
            vector<TypeInfo> specTypeArgs;
            if (!checker.boundSatisfied(concreteTI, specDecl, specQualified, specTypeArgs)) {
                throw YuxError(line, col, ErrorCode::E1133, specQualified, argType.getFullName(), specQualified);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 等内部异常 (lambda 形参未推断等): 留 Compiler 兜底
        }
        return;
    }
    // heap:<T>(v) / rc:<T>(v) 由 #Builtin generic 路径在 call_fn.cpp 处理，
    // 类型校验由 sema::validateBuiltinIntrinsicShape/TypeShape 覆盖，不在此处重复。
    if (auto n = dynamic_cast<p<ExprNullElseNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // Bucket 6 收口+ (CURRENT-check.md): E3024 (左侧非 Nullable) + E3023 (右侧
        // 类型不匹配). 镜像 compiler_expr.cpp:1955-2000. 复杂路径 (alias / Self) 由
        // getType 抛错时跳过, 留 Compiler 兜底.
        try {
            auto leftType = n->left()->getType();
            // Array<Nullable<T>> 下标返回 Ref<Nullable<T>>（T?&），剥 Ref 后校验
            if (leftType.isRef()) {
                if (auto refInner = leftType.refElementType(); refInner && refInner->isNullable()) {
                    leftType = *refInner;
                }
            }
            if (!leftType.isNullable()) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3024, leftType.name);
            }
            auto innerType = leftType.nullableInnerType();
            if (!innerType) return;
            if (isIntTypeName(innerType->name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), *innerType);
            }
            tryInferNullType(n->right(), *innerType);
            auto rightType = n->right()->getType();
            if (!(rightType == *innerType)) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3014, innerType->name,
                               rightType.name);
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 抛 std::runtime_error 等: 留 Compiler 兜底
        }
        return;
    }
    if (auto n = dynamic_cast<p<ExprMoveAssignNode>>(expr)) {
        visitExpr(n->left());
        visitExpr(n->right());
        // 校验 left 为合法 lvalue + 类型兼容。
        // left 必须是变量引用 ($ / a.b / a[ ... ]) 等可赋值表达式。
        // right 类型必须能与 left 类型兼容（相同或灵活整数字面量）。
        try {
            auto leftType = n->left()->getType();
            if (isIntTypeName(leftType.name) && isFlexibleIntExpr(n->right())) {
                tryInferIntType(n->right(), leftType);
            }
            auto rightType = n->right()->getType();
            if (leftType != rightType) {
                // 非内置类型允许跨类型形参 (§7.2.3.3)
                if (isBuiltinType(leftType.name)) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3001, "arithmetic",
                                   leftType.name, rightType.name);
                }
            }
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 异常, 留 codegen 兜底
        }
        return;
    }
    // Phase 3.4.d.1: ExprGetRefNode —— 无子表达式可递, 顶部
    // setResolvedType(getType()) 已经触发 ExprGetRefNode::getType 抛
    // E3040/E3041 (SemaPass 默认重抛), 由此 Compiler 端
    // compileGetRefExpr 的 1656/1661 内联 throw 在正常 codepath 下不可达。
    // Phase 3.4.d.2: 补 E3042 链式私有字段可见性校验.
    if (auto n = dynamic_cast<p<ExprGetRefNode>>(expr)) {
        // Phase 2e: `&$.x` 在 `#Static fn` 体内禁用 (E3128).
        if (n->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3128);
        }
        // #Inline #Cval 检查：内联常量无存储地址，不可取址。
        // 先查本文件，再查 SDK 文件的全局常量列表。
        auto checkInlineConst = [&](p<FileNode> f) {
            if (!f) return;
            for (const auto& gc : f->getGlobalConsts()) {
                if (gc->name().getText() == n->obj().getText() && gc->isInline()) {
                    throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3118, n->obj().getText());
                }
            }
        };
        checkInlineConst(_file);
        checkInlineConst(_sdkFile);
        try {
            sema::validateGetRefPrivacy(_file, _sdkFile, n, _currentStructName);
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 防御性
        }
        // Phase C：实例化后字段链 E3040 / E3041（getType 在模板体吞掉）。
        if (!n->subs().empty() && _currentFn) {
            string objName = n->obj().getText();
            SymbolInfo* sym = nullptr;
            if (auto sc = n->findNearestScope()) {
                sym = sc->lookupSymbol(objName);
            }
            if (!sym) {
                sym = _currentFn->lookupSymbol(objName);
            }
            if (sym) {
                vector<string> members;
                members.reserve(n->subs().size());
                for (auto& t : n->subs())
                    members.push_back(t.getText());
                tryValidateFieldChain(sym->type, members, n->resolveLineNumber(), n->resolveColumn());
            }
        }
        return;
    }
    // Phase C：ExprArrayInitNode 无靶向类型时仍校验 explicitType vs fill（E3009）。
    if (auto n = dynamic_cast<p<ExprArrayInitNode>>(expr)) {
        checkArrayInit(n, nullptr);
        return;
    }
    // 其余未识别节点 3.2 起补 assert。
}

void SemaPass::tryValidateBinOpMethod(p<ExprNode> leftExpr, p<ExprNode> rightExpr, const string& methodName, int line,
                                      int col) {
    // gate 与 Compiler::compileAddSubExpr / MulDivMod / BinOp / Compare 内
    // `!isBuiltinType(leftType.name) → compileCustomTypeBinaryOp` 一致, 但
    // 进一步把容器类排除 (容器走专属 codegen / sema 路径, 不该走到 method 解析):
    //   * Ref / Rc / Array / Heap / Weak / Nullable / Ptr / Tuple
    // 泛型 struct 实例仍跳过（方法解析要完整 subst）；模板形参 T 等实例化后再查。
    // leftType / rightType getType 抛错 (lambda 形参等) 跳过, 留 Compiler 兜底.
    if (methodName.empty()) return;
    try {
        TypeInfo leftType = applyInstSubst(leftExpr->getType()).peelAutoDeref();
        TypeInfo rightType = applyInstSubst(rightExpr->getType()).peelAutoDeref();
        if (isCurrentTypeParam(leftType) || isCurrentTypeParam(rightType)) return;
        // String + 任意：getType 整链结果即 String，不走 E3001。
        if (methodName == "plus" && (leftType.isString() || rightType.isString())) return;
        // 实例化后内置类型：镜像 ExprAddSubNode::getType 的 E3001。
        if (isBuiltinType(leftType.name)) {
            if (leftType != rightType) {
                if (isIntTypeName(leftType.name) && isFlexibleIntExpr(rightExpr)) {
                    tryInferIntType(rightExpr, leftType);
                    return;
                }
                if (isIntTypeName(rightType.name) && isFlexibleIntExpr(leftExpr)) {
                    tryInferIntType(leftExpr, rightType);
                    return;
                }
                throw YuxError(line, col, ErrorCode::E3001, binOpE3001Kind(methodName), leftType.name, rightType.name);
            }
            return;
        }
        if (leftType.name.empty()) return;
        // String 走 StringBuilder 特殊 lowering / 其它 builtin-handled 路径,
        // 没有用户可见的 plus/eq/... 方法签名, 不能走 customBinaryOp 解析.
        if (leftType.isString()) return;
        if (leftType.isRef() || leftType.isArrayGeneric() || leftType.isWeak() || leftType.isNullable() ||
            leftType.isPtr() || leftType.isTuple())
            return;
        // Heap<T> → T / Rc<T> → T：运算符穿透 wrapper，在内部类型上验证方法
        TypeInfo resolvedLeftType = leftType;
        if (leftType.isHeap()) {
            auto heapInner = leftType.heapElementType();
            if (!heapInner) return;
            resolvedLeftType = *heapInner;
        }
        if (leftType.isRc()) {
            auto rcInner = leftType.rcElementType();
            if (!rcInner) return;
            resolvedLeftType = *rcInner;
        }
        StructDeclNode* decl = _names.lookupStruct(resolvedLeftType.name);
        if (!decl || decl->isGeneric()) return;
        TypeInfo effRightType = rightType.peelAutoDeref();
        sema::validateBinOpMethodResolution(_file, _sdkFile, resolvedLeftType, effRightType, methodName, line, col);
    } catch (const YuxError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateUnaryOpMethod(p<ExprNode> rightExpr, const string& methodName, int line, int col) {
    // gate 与 Compiler::compileUnaryExpr 内 `!isBuiltinType → compileCustomTypeUnaryOp` 一致,
    // 并排除容器（与 tryValidateBinOpMethod 同款）。模板形参等实例化后再查。
    if (methodName.empty()) return;
    try {
        TypeInfo rightType = applyInstSubst(rightExpr->getType()).peelAutoDeref();
        if (isCurrentTypeParam(rightType)) return;
        if (isBuiltinType(rightType.name)) {
            if (methodName == "inv" && rightType.isFloat()) {
                throw YuxError(line, col, ErrorCode::E3070, rightType.name);
            }
            if (methodName == "not" && rightType.name != "bool") {
                throw YuxError(line, col, ErrorCode::E3071, rightType.name);
            }
            return;
        }
        if (rightType.name.empty()) return;
        if (rightType.isString()) return;
        if (rightType.isRef() || rightType.isArrayGeneric() || rightType.isWeak() || rightType.isNullable() ||
            rightType.isPtr() || rightType.isTuple())
            return;
        TypeInfo resolved = rightType;
        if (rightType.isHeap()) {
            auto heapInner = rightType.heapElementType();
            if (!heapInner) return;
            resolved = *heapInner;
        }
        if (rightType.isRc()) {
            auto rcInner = rightType.rcElementType();
            if (!rcInner) return;
            resolved = *rcInner;
        }
        StructDeclNode* decl = _names.lookupStruct(resolved.name);
        if (!decl || decl->isGeneric()) return;
        sema::validateUnaryOpMethodResolution(_file, _sdkFile, resolved, methodName, line, col);
    } catch (const YuxError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateIndexBase(p<ExprNode> arrayExpr, int line, int col) {
    // 与 ExprGetNode::getType / compileArraySetStatement 同款：只剥 Ref。
    // 模板形参等实例化后再查。
    if (!arrayExpr) return;
    try {
        TypeInfo at = arrayExpr->hasResolvedType() ? arrayExpr->resolvedType() : arrayExpr->getType();
        at = applyInstSubst(at).peelRef();
        if (isCurrentTypeParam(at)) return;
        if (at.isArrayGeneric() || at.isArray()) return;
        if (at.name.empty()) return;
        throw YuxError(line, col, ErrorCode::E3062, at.name);
    } catch (const YuxError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        // getType 内部异常: 留 Compiler 兜底
    }
}

void SemaPass::tryValidateFieldChain(const TypeInfo& start, const vector<string>& members, int line, int col) {
    // 与 compileAssignStatement 成员赋值 / ExprGetRefNode::getType 同款。
    // 模板形参等实例化后再查；已知 struct 的缺字段不依赖 T，模板期也报。
    if (members.empty()) return;
    auto peel = [this](TypeInfo t) {
        t = substSelfType(applyInstSubst(t), _currentStructName).peelAutoDeref();
        try {
            t = sema::resolveAlias(t, _file, _sdkFile).peelAutoDeref();
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        return t;
    };
    TypeInfo cur = peel(start);
    auto isPureDigits = [](const string& s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
    };
    for (const auto& mem : members) {
        if (isCurrentTypeParam(cur)) return;
        // DRAFT-spec-reflect §6: Field.value 写/取由 codegen 改写，不按 struct 字段查。
        if (cur.name == "Field" && mem == "value") return;
        if (cur.isTuple()) {
            if (!isPureDigits(mem)) {
                throw YuxError(line, col, ErrorCode::E3040, cur.getFullName(), mem);
            }
            auto idx = static_cast<size_t>(std::stoul(mem));
            const auto& elems = cur.tupleElements();
            if (idx >= elems.size() || !elems[idx]) return; // E3100 已报
            cur = peel(*elems[idx]);
            continue;
        }
        if (cur.name.empty()) return;
        if (isBuiltinType(cur.name)) {
            throw YuxError(line, col, ErrorCode::E3041, cur.name);
        }
        StructDeclNode* decl = _names.lookupStruct(cur.name);
        if (!decl) {
            throw YuxError(line, col, ErrorCode::E3041, cur.name);
        }
        if (decl->staticField(mem)) {
            throw YuxError(line, col, ErrorCode::E3152, mem, cur.name, cur.name, mem);
        }
        int fi = decl->fieldIndex(mem);
        if (fi < 0) {
            throw YuxError(line, col, ErrorCode::E3040, cur.name, mem);
        }
        TypeInfo fieldTy = decl->fields()[static_cast<size_t>(fi)]->getType();
        map<string, TypeInfo> fieldSubst = _instSubst;
        if (decl->isGeneric() && fieldSubst.empty()) {
            fillSubstFromGenericArgs(decl->typeParams(), cur.genericArgs, fieldSubst);
        }
        if (!fieldSubst.empty()) fieldTy = fieldTy.substitute(fieldSubst);
        cur = peel(fieldTy);
    }
}

bool SemaPass::isCurrentTypeParam(const TypeInfo& t) const {
    TypeInfo peeled = t.peelAutoDeref();
    if (!peeled.isNormal() || peeled.name.empty()) return false;
    return _currentTypeParams.count(peeled.name) > 0;
}

TypeInfo SemaPass::applyInstSubst(const TypeInfo& t) const {
    return _instSubst.empty() ? t : t.substitute(_instSubst);
}

bool SemaPass::typeStillTemplate(const TypeInfo& t) const {
    TypeInfo t0 = applyInstSubst(t);
    if (isCurrentTypeParam(t0)) return true;
    for (auto& a : t0.genericArgs) {
        if (a && typeStillTemplate(*a)) return true;
    }
    if (t0.elementType && typeStillTemplate(*t0.elementType)) return true;
    return false;
}

void SemaPass::noteConcreteGenericType(const TypeInfo& t) {
    TypeInfo t0 = applyInstSubst(t);
    if (t0.empty() || typeStillTemplate(t0)) return;

    auto noteInner = [this](const sp<TypeInfo>& inner) {
        if (inner) noteConcreteGenericType(*inner);
    };

    if (t0.isRef()) {
        noteInner(t0.refElementType());
        return;
    }
    if (t0.isPtr()) {
        noteInner(t0.ptrElementType());
        return;
    }
    if (t0.isNullable()) {
        noteInner(t0.nullableInnerType());
        return;
    }
    if (t0.isArray()) {
        noteInner(t0.elementType);
        return;
    }
    if (t0.isTuple()) {
        for (auto& e : t0.tupleElements())
            noteInner(e);
        return;
    }
    if (t0.isFn()) {
        for (auto& p : t0.fnParamTypes())
            noteInner(p);
        noteInner(t0.fnReturnType());
        return;
    }
    if (t0.isRc()) noteInner(t0.rcElementType());
    if (t0.isHeap()) noteInner(t0.heapElementType());
    if (t0.isWeak()) noteInner(t0.weakElementType());
    if (t0.isArrayGeneric()) noteInner(t0.arrayGenericElementType());
    for (auto& a : t0.genericArgs)
        noteInner(a);

    auto* sd = _names.lookupStruct(t0.name);
    if (!sd || !sd->isGeneric()) return;
    map<string, TypeInfo> subst;
    if (!fillSubstFromGenericArgs(sd->typeParams(), t0.genericArgs, subst)) return;
    checkGenericImplInst(lookupStructImpl(_file, _sdkFile, t0.name), subst);
}

void SemaPass::checkGenericFnInst(p<FnNode> fn, const vector<TypeInfo>& typeArgs) {
    if (!fn || !fn->header() || fn->header()->hasAnno("Builtin")) return;
    const auto& tps = fn->header()->typeParams();
    if (tps.empty() || tps.size() != typeArgs.size()) return;
    map<string, TypeInfo> subst;
    for (size_t i = 0; i < tps.size(); ++i) {
        TypeInfo a = applyInstSubst(typeArgs[i]);
        if (isCurrentTypeParam(a)) return;
        subst[tps[i]] = std::move(a);
    }
    checkGenericBodyInst(fn, subst, "");
}

void SemaPass::checkGenericImplInst(StructImplNode* impl, const map<string, TypeInfo>& subst) {
    if (!impl || subst.empty()) return;
    for (auto& [_, t] : subst) {
        if (isCurrentTypeParam(t)) return;
    }
    string key = genericInstKey(impl, subst);
    if (!_checkedGenericInst.insert(key).second) return;
    for (auto& m : impl->methods()) {
        if (!m || !m->header() || m->header()->hasAnno("Builtin")) continue;
        checkGenericBodyInst(m, subst, impl->structName());
    }
    if (impl->hasDestructor()) checkGenericBodyInst(impl->destructor(), subst, impl->structName());
}

void SemaPass::checkGenericBodyInst(p<FnNode> fn, const map<string, TypeInfo>& subst, const string& structName) {
    if (!fn || subst.empty()) return;
    string key = genericInstKey(fn, subst);
    if (!_checkedGenericInst.insert(key).second) return;

    auto savedFn = _currentFn;
    auto savedStruct = _currentStructName;
    auto savedParams = _currentTypeParams;
    auto savedSubst = _instSubst;
    auto savedMoved = _movedVars;

    _currentFn = fn;
    _currentStructName = structName;
    _instSubst = subst;
    _currentTypeParams.clear();
    for (auto& [k, _] : subst)
        _currentTypeParams.insert(k);
    _movedVars.clear();

    if (auto hdr = fn->header()) {
        for (auto& param : hdr->params()) {
            if (param && param->type()) noteConcreteGenericType(param->type()->getType());
        }
        if (auto rt = hdr->retType()) noteConcreteGenericType(rt->getType());
    }
    for (auto& stmt : fn->body())
        visitStmt(stmt);

    _movedVars = std::move(savedMoved);
    _instSubst = std::move(savedSubst);
    _currentTypeParams = std::move(savedParams);
    _currentStructName = std::move(savedStruct);
    _currentFn = savedFn;
}

void SemaPass::checkArrayElemAgainst(p<ExprNode> elem, const TypeInfo& want, int line, int col) {
    if (!elem) return;
    TypeInfo w0 = applyInstSubst(want);
    if (isCurrentTypeParam(w0)) return;
    if (isFlexibleIntExpr(elem) && isIntTypeName(w0.name)) return;
    if (w0.isNullable() && isFlexibleNullExpr(elem)) return;

    TypeInfo got;
    if (elem->hasResolvedType()) {
        got = elem->resolvedType();
    } else {
        try {
            got = elem->getType();
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return;
        }
    }
    got = applyInstSubst(got);
    if (isEmptyArrayType(got)) return;
    if (got == w0) return;
    if (got.peelRef() == w0) return;
    if (w0.isNullable()) {
        if (auto inner = w0.nullableInnerType()) {
            if (got == *inner || got.peelRef() == *inner) return;
        }
    }
    throw YuxError(line, col, ErrorCode::E3009, w0.getFullName(), got.getFullName());
}

void SemaPass::checkArrayLiteral(p<ExprArrayNode> n, const TypeInfo& expected) {
    if (!n) return;
    TypeInfo want = expected.peelRef();
    if (want.isArray() && want.arraySize != n->elements().size()) {
        throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3012, want.arraySize,
                       n->elements().size());
    }
    auto elemWant = arrayElemTarget(want);
    for (auto& e : n->elements()) {
        visitExpr(e, elemWant ? elemWant.get() : nullptr);
        if (!elemWant) continue;
        int eline = e->resolveLineNumber();
        int ecol = e->resolveColumn();
        if (eline <= 0) eline = n->resolveLineNumber();
        if (ecol < 0) ecol = n->resolveColumn();
        checkArrayElemAgainst(e, *elemWant, eline, ecol);
    }
    n->setResolvedType(want);
}

void SemaPass::checkArrayInit(p<ExprArrayInitNode> n, const TypeInfo* expected) {
    if (!n) return;
    TypeInfo elemType;
    if (n->explicitType()) {
        elemType = n->explicitType()->getType();
        inferFillLiteralInt(n->value(), elemType);
        auto fillType = n->value()->getType();
        if (fillType != elemType) {
            throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3009, elemType.getFullName(),
                           fillType.getFullName());
        }
    } else {
        elemType = n->value()->getType();
        if (expected) {
            if (auto wantElem = arrayElemTarget(expected->peelRef())) {
                inferFillLiteralInt(n->value(), *wantElem);
                elemType = n->value()->getType();
            }
        }
    }

    if (expected) {
        TypeInfo want = expected->peelRef();
        if (auto wantElem = arrayElemTarget(want)) {
            if (!isCurrentTypeParam(*wantElem) && elemType != *wantElem) {
                throw YuxError(n->resolveLineNumber(), n->resolveColumn(), ErrorCode::E3009, wantElem->getFullName(),
                               elemType.getFullName());
            }
            n->setResolvedType(want);
            return;
        }
    }
    n->setResolvedType(TypeInfo(make_shared<TypeInfo>(elemType), 0));
}
