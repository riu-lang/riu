// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 内部 helpers。实现原位于 sema_pass.cpp 匿名命名空间。

#include "sema/sema_pass_detail.h"
#include "builtin_methods.h"
#include "generic/generic.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

#include "analyzer/spec_registry.h"
#include "ast/node/spec_node.h"
#include "tools/diagnostic.h"
#include "types.h"

namespace sema::pass {

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

// 索引赋值 lvalue：与 compileArraySetStatement 同款，只认
//   * 简单变量（LiteralObj）
//   * 单层 `obj.field`（base 也是 LiteralObj）
// 比 isLvalueArrayBase 更严（方法调用允许 Dot 链）；多层 `a.b.c[i] =` 与调用结果 /
// 字面量一样走 E3061。
bool isArraySetLvalue(ExprNode* arrayExpr) {
    if (!arrayExpr) return false;
    if (auto lit = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        return dynamic_cast<LiteralObjNode*>(lit->literal()) != nullptr;
    }
    if (auto dot = dynamic_cast<ExprDotNode*>(arrayExpr)) {
        if (auto baseLit = dynamic_cast<ExprLiteralNode*>(dot->baseExpr())) {
            return dynamic_cast<LiteralObjNode*>(baseLit->literal()) != nullptr;
        }
    }
    return false;
}

// `<-` LHS：与 compileLvalueAddr 同款。
bool isMoveAssignLvalue(ExprNode* expr) {
    while (expr) {
        if (auto lit = dynamic_cast<ExprLiteralNode*>(expr)) {
            return dynamic_cast<LiteralObjNode*>(lit->literal()) != nullptr;
        }
        if (auto dot = dynamic_cast<ExprDotNode*>(expr)) {
            expr = dot->baseExpr();
            continue;
        }
        return false;
    }
    return false;
}

void validateContainerBansAt(const TypeInfo& t, TypeNode* tn, int fallbackLine, int fallbackCol, bool allowDynBorrow) {
    if (!tn) return;
    int line = tn->getLineNumber();
    int col = tn->getColumn();
    if (line <= 0) line = fallbackLine > 0 ? fallbackLine : 1;
    if (col < 0) col = fallbackCol;
    validateRcContainerBans(t, line, col);
    validateTypeArgRefPolicy(t, line, col, allowDynBorrow);
}

// Phase B-1: 与 Compiler::isNoCopyType 等价的本地版本（0 LLVM 依赖）。
// 判定类型是否为 #NoCopy：Array<T> 隐含，或 struct decl 显式标注 #NoCopy。
bool isNoCopyTypeIn(const TypeInfo& type, FileNode* file, FileNode* sdkFile) {
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
bool isFreshHandleExpr(ExprNode* expr) {
    if (!expr) return false;
    if (dynamic_cast<ExprCallNode*>(expr)) return true;       // 函数调用结果 / builtin intrinsic
    if (dynamic_cast<ExprArrayNode*>(expr)) return true;      // 数组字面量
    if (dynamic_cast<ExprPathCallNode*>(expr)) return true;   // 枚举构造器 / #Static fn 调用
    if (dynamic_cast<ExprMoveAssignNode*>(expr)) return true; // move-assign 结果
    if (dynamic_cast<LambdaExprNode*>(expr)) return true;     // lambda 字面量
    if (dynamic_cast<ExprStructLitNode*>(expr)) return true;  // struct 字面量 (Self { ... })
    // if / match / try：与 Compiler::isFreshHandleExpr 同步 — 各值产生分支均 fresh。
    auto blockFresh = [](StatementBlockNode* block) -> bool {
        if (!block || blockTerminatesFlow(block, block)) return true;
        if (!block->hasResult() || !block->resultExpr()) return true;
        return isFreshHandleExpr(block->resultExpr());
    };
    if (auto* ifn = dynamic_cast<ExprIfElseNode*>(expr)) {
        if (!blockFresh(ifn->thenBlock())) return false;
        for (auto& el : ifn->elifs()) {
            if (el && !blockFresh(el->block())) return false;
        }
        if (ifn->elseBlock() && !blockFresh(ifn->elseBlock())) return false;
        return true;
    }
    if (auto* ol = dynamic_cast<ExprOneLineIfElseNode*>(expr)) {
        ScopeNode* sc = ol->findNearestScope();
        bool tTerm = exprTerminatesFlow(sc, ol->trueValue());
        bool fTerm = exprTerminatesFlow(sc, ol->falseValue());
        if (tTerm && fTerm) return true;
        if (tTerm) return isFreshHandleExpr(ol->falseValue());
        if (fTerm) return isFreshHandleExpr(ol->trueValue());
        return isFreshHandleExpr(ol->trueValue()) && isFreshHandleExpr(ol->falseValue());
    }
    if (auto* mn = dynamic_cast<ExprMatchNode*>(expr)) {
        for (auto& arm : mn->arms()) {
            if (!arm || arm->skipsTypeMerge()) continue;
            if (arm->hasBlock()) {
                if (!blockFresh(arm->block())) return false;
            } else if (!isFreshHandleExpr(arm->body())) {
                return false;
            }
        }
        return true;
    }
    if (auto* tn = dynamic_cast<ExprTryCatchNode*>(expr)) {
        if (!blockFresh(tn->tryBlock())) return false;
        for (auto& arm : tn->catches()) {
            if (arm && !blockFresh(arm->body())) return false;
        }
        return true;
    }
    // `??`：两侧都 fresh 时整体 fresh（`give() ?? []`）；变量左侧仍是复制，E4031。
    if (auto* ne = dynamic_cast<ExprNullElseNode*>(expr)) {
        return isFreshHandleExpr(ne->left()) && isFreshHandleExpr(ne->right());
    }
    return false;
}

// 空数组字面量 `[]`：getType 为 `[__empty * 0]`，有靶向类型时应接受。
bool isEmptyArrayType(const TypeInfo& t) {
    return t.isArray() && t.elementType && t.elementType->name == "__empty";
}

bool blockMergeTypesEq(const TypeInfo& a, const TypeInfo& b) {
    if (a == b) return true;
    if (isEmptyArrayType(a) && (b.isArrayGeneric() || b.isArray())) return true;
    if (isEmptyArrayType(b) && (a.isArrayGeneric() || a.isArray())) return true;
    return false;
}

// 数组填充值是 LiteralNode，不是 ExprNode，不能走 tryInferIntType。
void inferFillLiteralInt(LiteralNode* lit, const TypeInfo& target) {
    if (!lit) return;
    if (auto ilit = dynamic_cast<LiteralIntNode*>(lit)) {
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

// 符号定义在 FileNode（本文件 / SDK / wildcard）上 → 全局，不是闭包捕获。
bool isOuterLocalCapture(ScopeNode* from, SymbolInfo* sym, const string& name) {
    if (!from || !sym) return false;
    for (auto* sc = from; sc; sc = sc->parentScope()) {
        const auto& locs = sc->localSymbols();
        auto it = locs.find(name);
        if (it != locs.end() && &it->second == sym) {
            return dynamic_cast<FileNode*>(sc) == nullptr;
        }
    }
    return false;
}

bool isCodegenFrameLocal(const string& name, Node* from, LambdaExprNode* lambda) {
    if (!from || name.empty()) return false;
    ScopeNode* start = from->findNearestScope();
    ScopeNode* stopParent = nullptr;
    if (lambda && lambda->bodyScope()) {
        stopParent = lambda->bodyScope()->parentScope();
    }
    for (auto* sc = start; sc && sc != stopParent; sc = sc->parentScope()) {
        if (sc->localSymbols().contains(name)) {
            return dynamic_cast<FileNode*>(sc) == nullptr;
        }
    }
    return false;
}

LambdaCapKind classifyLambdaCapture(const TypeInfo& t) {
    if (t.isNormal() && t.name.empty()) return LambdaCapKind::Skip;
    if (t.isNormal() && isBuiltinType(t.name)) return LambdaCapKind::Scalar;
    if (t.isRcHandle()) return LambdaCapKind::Handle;
    if (t.isRef()) return LambdaCapKind::Ref;
    if (t.isNullable()) {
        auto inner = t.nullableInnerType();
        if (inner && inner->isHeap()) return LambdaCapKind::HeapNullable;
    }
    return LambdaCapKind::Unsupported;
}

// Phase C：泛型模板体内仍从 getType 重抛的形态码（不依赖 T 具体化）。
// 其余类型错（E3001 / E3041 / E6016 等）等实例化后再查，此处吞掉。
bool isMorphologicalGenericCode(const char* code) {
    if (!code) return false;
    std::string_view sv(code);
    constexpr std::array<std::string_view, 25> kKeep = {
        "E3030",                                              // 未定义符号
        "E6010", "E6011",                                     // 泛型 arity
        "E2037",                                              // enum 头边界
        "E4031", "E4032",                                     // #NoCopy
        "E3103",                                              // 整数字面量越界
        "E2033",                                              // 非法转义
        "E4025", "E1132", "E4037", "E4038", "E4039", "E4040", // 容器禁令 / T& 位置
        "E2016", "E2017",                                     // 别名
        "E3130",                                              // 同名 ctor 定义
        "E3120", "E3121", "E3123",                            // Self:: / Type:: 静态调用形态
        "E3128",                                              // #Static 体内 $
        "E2030",                                              // lambda 捕获赋值
        "E4033",                                              // use-after-move
        "E3095",                                              // 类型名 / 非函数当 callee（方法点在 Dot 分支延迟重抛）
        "E5018",                                              // 包边界不依赖泛型实参。
    };
    for (auto c : kKeep) {
        if (sv == c) return true;
    }
    return false;
}

// 实例化后方法返回类型。nullopt = 方法不存在。
std::optional<TypeInfo> instantiatedMethodRet(FnNode* fn, FileNode* file, FileNode* sdk, const TypeInfo& rawRecv,
                                              const TypeInfo& instRecv, const string& member,
                                              const std::map<string, TypeInfo>* instSubst) {
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
            for (auto& bound : bounds[idx]) {
                SpecDeclNode* draft = file ? file->getSpecDecl(bound.name) : nullptr;
                if (!draft && sdk && sdk != file) draft = sdk->getSpecDecl(bound.name);
                if (!draft) continue;
                for (auto& sig : draft->signatures()) {
                    if (!sig || sig->name().getText() != member) continue;
                    if (sig->retType()) {
                        std::map<string, TypeInfo> subst;
                        const auto& dParams = draft->typeParams();
                        for (size_t pi = 0; pi < dParams.size() && pi < bound.typeArgs.size(); ++pi) {
                            TypeInfo a = bound.typeArgs[pi];
                            if (instSubst) a = a.substitute(*instSubst);
                            subst[dParams[pi]] = std::move(a);
                        }
                        subst["Self"] = instRecv.peelAutoDeref();
                        return sig->retType()->getType().substitute(subst);
                    }
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

// `?.` / `??`：剥 Ref<Nullable<T>>（下标返回 T?&）。
TypeInfo peelRefIfNullable(TypeInfo t) {
    if (t.isRef()) {
        if (auto inner = t.refElementType(); inner && inner->isNullable()) return *inner;
    }
    return t;
}

// `?.` 接收者：已确认 Nullable 后，取内层并 Rc 自动 deref（与 getType / compileSafeDotExpr 同款）。
TypeInfo peelSafeDotInner(TypeInfo t) {
    t = peelRefIfNullable(t);
    if (!t.isNullable()) return t;
    auto inner = t.nullableInnerType();
    if (!inner) return t;
    t = *inner;
    if (t.isRc()) {
        if (auto rc = t.rcElementType()) t = *rc;
    }
    return t;
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
    for (auto& bound : bounds[idx]) {
        SpecDeclNode* draft = file ? file->getSpecDecl(bound.name) : nullptr;
        if (!draft && sdk && sdk != file) draft = sdk->getSpecDecl(bound.name);
        if (!draft) continue;
        for (auto& sig : draft->signatures()) {
            if (sig && sig->name().getText() == member) return true;
        }
    }
    return false;
}

std::optional<TypeInfo> typeParamBoundStaticFieldType(FnNode* fn, FileNode* file, FileNode* sdk,
                                                      const string& typeParam, const string& field) {
    if (!fn || !fn->header() || field.empty()) return std::nullopt;
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
    if (idx == SIZE_MAX || idx >= bounds.size()) return std::nullopt;
    for (auto& bound : bounds[idx]) {
        SpecDeclNode* spec = file ? file->getSpecDecl(bound.name) : nullptr;
        if (!spec && sdk && sdk != file) spec = sdk->getSpecDecl(bound.name);
        if (!spec) continue;
        for (auto& sf : spec->staticFields()) {
            if (!sf || sf->name().getText() != field) continue;
            std::map<string, TypeInfo> subst;
            const auto& dParams = spec->typeParams();
            for (size_t pi = 0; pi < dParams.size() && pi < bound.typeArgs.size(); ++pi) {
                subst[dParams[pi]] = bound.typeArgs[pi];
            }
            subst["Self"] = TypeInfo(typeParam);
            return sf->getType().substitute(subst);
        }
    }
    return std::nullopt;
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
void applyLambdaFnExpected(LambdaExprNode* lam, const TypeInfo& fnTy) {
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
bool lambdaExpectedRetType(LambdaExprNode* lam, TypeInfo& out) {
    if (!lam) return false;
    if (lam->retType()) {
        try {
            out = lam->retType()->getType();
            if (lam->fallibleErrTypeNode()) {
                out.attachFallibleErr(fallibleErrKey(lam->fallibleErrTypeNode()->getType()));
            }
            return true;
        } catch (const RiuError&) {
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

bool tryGetExprType(ExprNode* expr, TypeInfo& out) {
    if (!expr) return false;
    if (expr->hasResolvedType()) {
        out = expr->resolvedType();
        return true;
    }
    try {
        out = expr->getType();
        return true;
    } catch (const RiuError&) {
        throw;
    } catch (...) { // NOLINT(bugprone-empty-catch)
        return false;
    }
}

SymbolInfo* lookupRetVar(const string& name, Node* n, FnNode* fn) {
    if (n) {
        if (auto sc = n->findNearestScope()) {
            if (auto* s = sc->lookupSymbol(name)) return s;
        }
    }
    return fn ? fn->lookupSymbol(name) : nullptr;
}

// 形态上合法的 T& 返回源：`$` / T& 变量 / `&expr` / 类型本身就是 T&（调用等）。
// 成功时 srcInner 为剥 Ref 后的内层；找不到源 → false。
bool refRetSourceInner(ExprNode* expr, FnNode* fn, TypeInfo& srcInner) {
    if (auto litExpr = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
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
    if (auto getRef = dynamic_cast<ExprGetRefNode*>(expr)) {
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
    return generic::applySubstMap(t, subst);
}

// 模板体：T / Array<T> 等仍不透明。实例化后 subst 已把 T 换成具体类型，继续比。
bool stillTemplateType(const TypeInfo& t, const std::set<std::string>& typeParams,
                       const std::map<std::string, TypeInfo>* subst) {
    if (isAssignTypeParam(t, typeParams)) return true;
    if (subst && !subst->empty()) return false;
    return !typeParams.empty() && !t.genericArgs.empty();
}

// Phase C：元组解构 E3101 / E3102。
// 与 compileDeclareAssignTupleStatement / loop init 对齐：
//   标注类型优先，否则 RHS 推断；subst + resolveAlias 后再判 isTuple。
//   非元组 → E3101；元素数 ≠ 名字数 → E3102。
// 模板形参 T（含 T& / Rc<T> 剥后仍是 T）等实例化后再查；Array<T> 永远不是元组，模板期也报。
void checkTupleDestructure(ExprNode* expr, TypeNode* annotated, size_t nameCount, int line, int col, FileNode* file,
                           FileNode* sdk, const std::set<std::string>& typeParams,
                           const std::map<std::string, TypeInfo>* subst) {
    TypeInfo raw;
    if (annotated) {
        try {
            raw = annotated->getType();
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            return;
        }
    } else if (!tryGetExprType(expr, raw)) {
        return;
    }
    raw = applySubstMap(raw, subst);
    if (raw.empty()) return;
    auto resolved = sema::resolveAlias(raw, file, sdk);
    if (resolved.empty()) return;
    if (isAssignTypeParam(resolved, typeParams)) return;
    if (!resolved.isTuple()) {
        string shown = raw.getFullName();
        if (shown.empty()) shown = raw.name;
        throw RiuError(line, col, ErrorCode::E3101, shown);
    }
    const auto& elems = resolved.tupleElements();
    if (elems.size() != nameCount) {
        throw RiuError(line, col, ErrorCode::E3102, std::to_string(nameCount), std::to_string(elems.size()));
    }
}

// Phase C：ret 表达式 E3014。Fallible 成功/错误双通道、T& 形态、Nullable wrap、
// 别名 resolveAlias、灵活整数推断。spec 体里未解析的 Self 仍跳过。
void checkRetExpr(ExprNode* expr, const TypeInfo& declRet, bool hasDeclRet, int line, const RetCheck& ctx) {
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
        TypeInfo declOk = hasDeclRet ? decl.withoutFallible() : TypeInfo();
        auto resolvedOk = resolveForRet(declOk, ctx);
        bool isSuccess = hasDeclRet && (resolvedRet.withoutFallible() == resolvedOk);
        if (!isSuccess && hasDeclRet && isEmptyArrayType(resolvedRet) && resolvedOk.isArrayGeneric()) {
            isSuccess = true;
        }
        bool isError = (resolvedRet.getFullName() == ctx.fallibleErr);
        if (!isSuccess && !isError) {
            // fallible void 成功通道：`ret <void-expr>` 与 `ret;` 同义（先求副作用）。
            // 典型：`fn f() ! E { if c { ret E::V } }` 块末 if 无 else，被包成隐式 ret。
            if (!hasDeclRet && resolvedRet.empty()) return;
            throw RiuError(line, ErrorCode::E3014, hasDeclRet ? decl.getFullName() : string("void"),
                           retType.getFullName());
        }
        return;
    }

    if (hasDeclRet && decl.isRef()) {
        TypeInfo srcInner;
        if (!refRetSourceInner(expr, ctx.fn, srcInner)) {
            throw RiuError(line, ErrorCode::E3014, decl.getFullName(), retType.getFullName())
                .withHint("返回 T& 时，ret 表达式应为 `$` / T& 变量 / `&expr` / 返回 T& 的调用");
        }
        srcInner = applySubstMap(srcInner, ctx.subst);
        auto declInner = substSelfType(decl, ctx.structName).refElementType();
        TypeInfo declInnerResolved = declInner ? resolveForRet(*declInner, ctx) : TypeInfo();
        srcInner = resolveForRet(srcInner, ctx);
        if (declInner && !srcInner.empty() && declInnerResolved != srcInner) {
            throw RiuError(line, ErrorCode::E3014, decl.getFullName(), (srcInner.name + "&"));
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
        throw RiuError(line, ErrorCode::E3014, decl.getFullName(),
                       retType.empty() ? string("void") : retType.getFullName());
    }

    if (hasDeclRet) {
        if (retType.empty()) {
            throw RiuError(line, ErrorCode::E3014, decl.getFullName(), "void");
        }
        if (resolveForRet(retType, ctx) != resolveForRet(decl, ctx)) {
            if (isEmptyArrayType(retType) && resolveForRet(decl, ctx).isArrayGeneric()) return;
            throw RiuError(line, ErrorCode::E3014, decl.getFullName(), retType.getFullName());
        }
    } else if (!retType.empty()) {
        throw RiuError(line, ErrorCode::E3014, "void", retType.getFullName());
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
void checkAssignRhs(ExprNode* expr, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                    const std::set<std::string>& typeParams, const std::map<std::string, TypeInfo>* subst) {
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
        throw RiuError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
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
        throw RiuError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
            .withHint(std::format("赋值目标为 `{}`，表达式为 `{}`；Rc<T> 只接受同型句柄或内层 T", w0.getFullName(),
                                  g0.getFullName()));
    }
    if (!isKnownAssignType(w, file, sdk) || !isKnownAssignType(g, file, sdk)) return;
    throw RiuError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
        .withHint(std::format("赋值目标类型为 `{}`，但表达式类型为 `{}`；riu 无隐式类型转换", w0.getFullName(),
                              g0.getFullName()));
}

// Phase C：声明处 Rc / Weak / Array 构造形态。
// 与 compileDeclareAssignStatement 对齐：
//   Rc：同型句柄或内层 T 包装 → E3014（走 checkAssignRhs）
//   Weak：仅 Rc<T> / Weak<T> → E3016
//   Array<T>：仅 Array 表达式或数组字面量 → E3064（不查元素类型，与 Compiler 一致）
// Heap 声明非 `heap:<T>(...)` 由 borrow checker E4024 先报，不在这里重复。
// 模板形参等实例化后再查。须在 visitExpr 带靶向类型之后调用。
void checkDeclareHandleRhs(ExprNode* expr, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                           const std::set<std::string>& typeParams, const std::map<std::string, TypeInfo>* subst) {
    if (!expr) return;
    TypeInfo w0 = applySubstMap(want, subst);
    if (w0.empty()) return;
    if (stillTemplateType(w0, typeParams, subst)) return;

    if (w0.isRc()) {
        checkAssignRhs(expr, want, line, col, file, sdk, typeParams, subst);
        return;
    }
    if (w0.isWeak()) {
        auto elem = w0.weakElementType();
        if (!elem) return;
        if (stillTemplateType(*elem, typeParams, subst)) return;
        TypeInfo got;
        if (!tryGetExprType(expr, got)) return;
        TypeInfo g0 = applySubstMap(got, subst);
        if (g0.empty() || stillTemplateType(g0, typeParams, subst)) return;
        auto g = sema::resolveAlias(g0, file, sdk);
        auto in = sema::resolveAlias(*elem, file, sdk);
        const bool fromRc = g.isRc() && g.rcElementType() && sema::resolveAlias(*g.rcElementType(), file, sdk) == in;
        const bool fromWeak =
            g.isWeak() && g.weakElementType() && sema::resolveAlias(*g.weakElementType(), file, sdk) == in;
        if (!fromRc && !fromWeak) {
            throw RiuError(line, col, ErrorCode::E3016, in.name, in.name, in.name);
        }
        return;
    }
    if (w0.isArrayGeneric()) {
        if (dynamic_cast<ExprArrayNode*>(expr)) return;
        TypeInfo got;
        if (!tryGetExprType(expr, got)) return;
        TypeInfo g0 = applySubstMap(got, subst);
        if (g0.empty() || stillTemplateType(g0, typeParams, subst)) return;
        auto g = sema::resolveAlias(g0, file, sdk).withoutFallible();
        if (g.isArrayGeneric() || g.name == "Array") return;
        throw RiuError(line, col, ErrorCode::E3064).withHint(std::format("表达式类型为 `{}`", g.getFullName()));
    }
}

// Phase C：调用实参相对实例化后形参的 E3014。
// 与赋值的差别：实参不自动解引用（T& 传给 T 要 copy_of）；值传给 T& 允许自动取址。
void checkCallArgAgainst(ExprNode* arg, const TypeInfo& want, int line, int col, FileNode* file, FileNode* sdk,
                         const std::set<std::string>& typeParams, const std::map<std::string, TypeInfo>* subst) {
    if (!arg) return;
    TypeInfo w0 = applySubstMap(want, subst);
    if (w0.empty() || w0.isSelf() || w0.isFn()) return;
    if (stillTemplateType(w0, typeParams, subst)) return;

    TypeInfo peeledWant = w0.peelRef();
    if (isFlexibleIntExpr(arg) && isIntTypeName(peeledWant.name)) {
        tryInferIntType(arg, peeledWant);
        return;
    }

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
        throw RiuError(line, col, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
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
            if (isIntTypeName(in.name) && isFlexibleIntExpr(arg)) {
                tryInferIntType(arg, in);
                return;
            }
            if (g == in) return;
        }
    }
    if (!isKnownAssignType(w, file, sdk) || !isKnownAssignType(g, file, sdk)) return;
    int eline = arg->resolveLineNumber();
    int ecol = arg->resolveColumn();
    if (eline <= 0) eline = line;
    if (ecol < 0) ecol = col;
    throw RiuError(eline, ecol, ErrorCode::E3014, w0.getFullName(), g0.getFullName())
        .withHint(std::format("实参类型 `{}` 与形参类型 `{}` 不匹配", g0.getFullName(), w0.getFullName()));
}

// T& 标识符在值上下文 getType 自解为 T。#Static 形参 T& 仍匹配该借用（不把值自动取址）。
bool argIsRefIdentMatching(ExprNode* arg, const TypeInfo& wantRef) {
    if (!arg || !wantRef.isRef()) return false;
    auto inner = wantRef.refElementType();
    if (!inner) return false;
    auto* lit = dynamic_cast<ExprLiteralNode*>(arg);
    if (!lit) return false;
    auto* obj = dynamic_cast<LiteralObjNode*>(lit->literal());
    if (!obj) return false;
    auto* sc = arg->findNearestScope();
    if (!sc) return false;
    auto* sym = sc->lookupSymbol(obj->getValue().getText());
    if (!sym || !sym->type.isRef()) return false;
    auto se = sym->type.refElementType();
    return se && *se == *inner;
}

bool fillSubstFromTypeNodes(const vector<string>& typeParams, const vector<TypeNode*>& typeArgNodes,
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
    return sema::NameResolver(file, sdk).lookupStructImpl(name);
}

StructImplNode* lookupStructImpl(FileNode* file, FileNode* sdk, const TypeInfo& t) {
    return sema::NameResolver(file, sdk).lookupStructImpl(t);
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
void checkMatchArmTypes(const vector<MatchArmNode*>& arms, const std::set<std::string>& typeParams,
                        const std::map<std::string, TypeInfo>* subst) {
    TypeInfo first;
    bool firstSet = false;
    for (auto& arm : arms) {
        if (!arm || arm->skipsTypeMerge()) continue;
        TypeInfo t;
        try {
            t = applySubstMap(arm->resultType(), subst);
        } catch (const RiuError&) {
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
        if (blockMergeTypesEq(t, first)) {
            if (isEmptyArrayType(first) && !isEmptyArrayType(t)) first = t;
            continue;
        }
        throw RiuError(arm->resultLine(), arm->resultCol(), ErrorCode::E3014, first.getFullName(), t.getFullName())
            .withHint(std::format("match 各臂结果类型须一致：先前臂为 `{}`，此臂为 `{}`", first.getFullName(),
                                  t.getFullName()));
    }
}

// 块体末位无 `;` 的裸表达式语句（隐式尾值），不含 ret / 声明 / 赋值子类。
bool isBareTailExprStmt(StatementNode* s) {
    auto se = dynamic_cast<StatementExprNode*>(s);
    if (!se || se->hasSemicolon() || !se->expr()) return false;
    if (dynamic_cast<StatementRetNode*>(s)) return false;
    if (dynamic_cast<StatementDeclareAssignNode*>(s)) return false;
    if (dynamic_cast<StatementDeclareAssignTupleNode*>(s)) return false;
    if (dynamic_cast<StatementAssignNode*>(s)) return false;
    return true;
}

// #Static fn 同名候选：过滤 wantArity，各位约定类型与 agreedArityParamTypes 同款。
// 任一同名泛型静态方法 → 不猜。
bool agreedStaticMethodParams(FileNode* file, FileNode* sdk, const TypeInfo& lhs, const string& rhs, size_t wantArity,
                              vector<TypeInfo>& out) {
    sema::NameResolver nr(file, sdk);
    StructDeclNode* sd = nr.lookupStruct(lhs, true);
    if (!sd || sd->isGeneric()) return false;
    StructImplNode* impl = nr.lookupStructImpl(lhs);
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

std::pair<const StructDeclNode*, int> tryResolveReflectField(ExprNode* expr) {
    if (!expr) return {nullptr, -1};

    auto fileOf = [](Node* n) -> FileNode* {
        auto* s = n->findNearestScope();
        while (s) {
            if (auto* f = dynamic_cast<FileNode*>(s)) return f;
            s = s->parentScope();
        }
        return nullptr;
    };
    auto lookupStruct = [](FileNode* file, const string& sn) -> StructDeclNode* {
        if (!file) return nullptr;
        if (auto* sd = file->getStructDecl(sn)) return sd;
        auto* p = dynamic_cast<ScopeNode*>(file);
        while (p) {
            p = dynamic_cast<ScopeNode*>(p->parentScope());
            if (auto* pf = dynamic_cast<FileNode*>(p)) {
                if (auto* sd = pf->getStructDecl(sn)) return sd;
            }
        }
        return nullptr;
    };
    auto fieldAtIndex = [](StructDeclNode* sd, i64 idx) -> int {
        int n = 0;
        for (auto& f : sd->fields()) {
            if (f->isStatic()) continue;
            if (n == idx) return sd->fieldIndex(f->name().getText());
            ++n;
        }
        return -1;
    };
    auto literalIndex = [](ExprNode* idxExpr) -> std::optional<i64> {
        auto* idxLit = dynamic_cast<ExprLiteralNode*>(idxExpr);
        if (!idxLit) return std::nullopt;
        auto* intLit = dynamic_cast<LiteralIntNode*>(idxLit->literal());
        if (!intLit) return std::nullopt;
        Token tok = intLit->getValue();
        i64 idx = sema::parseIntLiteral(tok.getText(), static_cast<int>(tok.getLine()),
                                        static_cast<int>(tok.getCharPositionInLine()) + 1);
        if (idx < 0) return std::nullopt;
        return idx;
    };

    // `Type::fields.get(N)`
    if (auto* call = dynamic_cast<ExprCallNode*>(expr)) {
        if (call->getArgs().size() == 1) {
            auto* dot = dynamic_cast<ExprDotNode*>(call->getCalleeExpr());
            if (dot && dot->member() == "get") {
                auto* path = dynamic_cast<ExprPathCallNode*>(dot->baseExpr());
                if (path && path->variantName().getText() == "fields") {
                    string sn = path->resolvedLhsName();
                    if (sn != "Self") {
                        if (auto idx = literalIndex(call->getArgs()[0])) {
                            if (auto* sd = lookupStruct(fileOf(expr), sn)) {
                                int fi = fieldAtIndex(sd, *idx);
                                if (fi >= 0) return {sd, fi};
                            }
                        }
                    }
                }
            }
        }
    }

    // `Type::fields[N]`
    if (auto* get = dynamic_cast<ExprGetNode*>(expr)) {
        if (get->indices().size() == 1) {
            auto* path = dynamic_cast<ExprPathCallNode*>(get->arrayExpr());
            if (path && path->variantName().getText() == "fields") {
                string sn = path->resolvedLhsName();
                if (sn != "Self") {
                    if (auto idx = literalIndex(get->indices()[0])) {
                        if (auto* sd = lookupStruct(fileOf(expr), sn)) {
                            int fi = fieldAtIndex(sd, *idx);
                            if (fi >= 0) return {sd, fi};
                        }
                    }
                }
            }
        }
    }

    // let 绑定：与 getType / compileAssignStatement 一样只搜所在 fn 体。
    if (auto* exLit = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto* objLit = dynamic_cast<LiteralObjNode*>(exLit->literal())) {
            string varName = objLit->getValue().getText();
            auto* s = expr->findNearestScope();
            while (s) {
                if (auto* fn = dynamic_cast<FnNode*>(s)) {
                    for (auto& stmt : fn->body()) {
                        if (auto* letStmt = dynamic_cast<StatementDeclareAssignNode*>(stmt)) {
                            if (letStmt->name().getText() == varName) {
                                return tryResolveReflectField(letStmt->expr());
                            }
                        }
                    }
                    break;
                }
                s = s->parentScope();
            }
        }
    }
    return {nullptr, -1};
}

} // namespace sema::pass
