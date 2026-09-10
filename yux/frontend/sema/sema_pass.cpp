// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// SemaPass 入口：run / visitFn / visitBlock / spec 默认体 / 泛型实例化复查。
// visitStmt → sema_stmt.cpp；visitExpr → sema_expr.cpp；
// tryValidate* / 数组字面量 → sema_check.cpp；共享 helpers → sema_pass_detail.cpp。

#include "sema/sema_pass.h"
#include "sema/builtin_methods.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"
#include "sema/sema_pass_detail.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <format>
#include <functional>
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

using namespace sema::pass;

namespace {
// 判定 e 是不是字面量 `$`（spec 默认体里 self 句柄, ast_builder 构成
// ExprLiteralNode(LiteralObjNode("$")))。
bool isBareSelf(ExprNode* e) {
    auto lit = dynamic_cast<ExprLiteralNode*>(e);
    if (!lit) return false;
    auto obj = dynamic_cast<LiteralObjNode*>(lit->literal());
    return obj && obj->getValue().getText() == "$";
}

// DRAFT-spec-default-body Phase 2: 递归扫描 expr 树寻找 `$.method(args)`
// 形态调用; 命中则验证 method 是否在 spec 自身签名集内, 不在则抛 E1140。
// 仅覆盖常见表达式形态; lambda / try-catch / match 等复杂形态在 Phase 2
// 主动 skip (留待 Phase 3 单态化时机的完整 typecheck)。
void walkExprForSpecDefault(ExprNode* e, SpecDeclNode* spec) {
    if (!e) return;
    if (auto n = dynamic_cast<ExprCallNode*>(e)) {
        if (auto dot = dynamic_cast<ExprDotNode*>(n->getCalleeExpr())) {
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
    if (auto n = dynamic_cast<ExprDotNode*>(e)) {
        walkExprForSpecDefault(n->baseExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprAddSubNode*>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprMulDivModNode*>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprBinOpNode*>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprCompareNode*>(e)) {
        walkExprForSpecDefault(n->left(), spec);
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprParenNode*>(e)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<ExprUnaryNode*>(e)) {
        walkExprForSpecDefault(n->right(), spec);
        return;
    }
    // 其它形态 (lambda / try-catch / match / struct lit / array / 索引 / 元组 ...)
    // Phase 2 不下钻; Phase 3 克隆 + 真实 typecheck 会兜底。
}

// 递归扫描 stmt 中的所有表达式入口。
void walkStmtForSpecDefault(StatementNode* s, SpecDeclNode* spec) {
    if (!s) return;
    if (auto n = dynamic_cast<StatementRetNode*>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<StatementAssignNode*>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<StatementSetNode*>(s)) {
        walkExprForSpecDefault(n->arrayExpr(), spec);
        for (auto& idx : n->indices())
            walkExprForSpecDefault(idx, spec);
        walkExprForSpecDefault(n->valueExpr(), spec);
        return;
    }
    if (auto n = dynamic_cast<StatementDeclareAssignNode*>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<StatementExprNode*>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        return;
    }
    if (auto n = dynamic_cast<StatementLoopNode*>(s)) {
        if (n->hasInit()) {
            walkExprForSpecDefault(n->initExpr(), spec);
        }
        return;
    }
    if (auto n = dynamic_cast<StatementForInNode*>(s)) {
        walkExprForSpecDefault(n->expr(), spec);
        if (auto blk = n->block()) {
            for (auto& st : blk->statements())
                walkStmtForSpecDefault(st, spec);
            if (blk->hasResult()) walkExprForSpecDefault(blk->resultExpr(), spec);
        }
        return;
    }
    // Block / Declare(无 init) / RetVoid / Break 等 Phase 2 不处理
}
} // namespace

SemaPass::SemaPass(FileNode* file, Yux* yux)
    : _file(file), _yux(yux), _sdkFile(yux ? yux->sdkFile() : nullptr),
      _sourcePath((yux && file) ? yux->modulePath(file->moduleName()) : ""), _names(_file, _sdkFile) {}

void SemaPass::run() {
    if (!_file) return;
    // 顶层类型别名一次性校验 (E2017 / E2016) + fn 符号表归一化
    sema::validateAliases(_file, _sdkFile);
    validateExternFns();
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
                validateContainerBansAt(ft, f->type(), f->getLineNumber(), f->getColumn(), false);
                if (!typeStillTemplate(ft) && !sema::typeHasLlvmLayout(ft, _file, _sdkFile, _currentTypeParams)) {
                    auto* fieldSd = _names.lookupStruct(ft);
                    if (!(fieldSd && fieldSd->isGeneric())) {
                        if (sd->isGeneric()) {
                            throw YuxError(static_cast<int>(f->name().getLine()), ErrorCode::E3098, ft.getFullName(),
                                           f->name().getText(), sd->name().getText());
                        }
                        throw YuxError(static_cast<int>(f->name().getLine()), ErrorCode::E3096, ft.getFullName());
                    }
                }
                noteConcreteGenericType(ft);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        for (auto& sf : sd->staticFields()) {
            if (!sf.type) continue;
            try {
                auto sft = sf.type->getType();
                validateContainerBansAt(sft, sf.type, sf.type->getLineNumber(), sf.type->getColumn(), false);
                noteConcreteGenericType(sft);
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
            if (gc->typeNode()) {
                validateContainerBansAt(gc->getType(), gc->typeNode(), gc->getLineNumber(), gc->getColumn(), false);
            }
            noteConcreteGenericType(gc->getType());
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    for (auto& gv : _file->getGlobalVars()) {
        if (!gv) continue;
        try {
            if (gv->typeNode()) {
                validateContainerBansAt(gv->getType(), gv->typeNode(), gv->getLineNumber(), gv->getColumn(), false);
            }
            noteConcreteGenericType(gv->getType());
        } catch (const YuxError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    for (auto& ed : _file->getEnumDecls()) {
        if (!ed) continue;
        for (auto& v : ed->variants()) {
            if (!v) continue;
            for (auto* pt : v->payloadTypes()) {
                if (!pt) continue;
                try {
                    validateContainerBansAt(pt->getType(), pt, v->getLineNumber(), v->getColumn(), false);
                } catch (const YuxError&) {
                    throw;
                } catch (...) { // NOLINT(bugprone-empty-catch)
                }
            }
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

void SemaPass::visitFn(FnNode* fn) {
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
    // 形参 / 返回不报 E3096：同 arity 重载用未声明名（如 `str`）作标签，不建布局。
    if (auto hdr = fn->header()) {
        for (auto& param : hdr->params()) {
            if (!param || !param->type()) continue;
            try {
                auto pt = param->type()->getType();
                validateContainerBansAt(pt, param->type(), static_cast<int>(param->name().getLine()),
                                        static_cast<int>(param->name().getCharPositionInLine()), true);
                noteConcreteGenericType(pt);
            } catch (const YuxError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        if (auto rt = hdr->retType()) {
            try {
                auto rtt = rt->getType();
                validateContainerBansAt(rtt, rt, fn->getLineNumber(), fn->getColumn(), true);
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

void SemaPass::visitBlock(StatementBlockNode* block, const TypeInfo* expected) {
    if (!block) return;
    for (auto& s : block->statements()) {
        visitStmt(s);
    }
    if (block->hasResult()) {
        visitExpr(block->resultExpr(), expected);
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
    sema::validateGenericStructFieldLayouts(sd, subst, _file, _sdkFile, _currentTypeParams);
    checkGenericImplInst(lookupStructImpl(_file, _sdkFile, t0.name), subst);
}

void SemaPass::checkGenericFnInst(FnNode* fn, const vector<TypeInfo>& typeArgs) {
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

void SemaPass::checkGenericBodyInst(FnNode* fn, const map<string, TypeInfo>& subst, const string& structName) {
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
    // `$` 的 Self 类型在 .decl / 旧 AST 上可能没有 ownerModule；实例化时补上声明模块，
    // 避免调用方文件里的同名 struct 抢走字段查找。
    if (auto* owner = fn->enclosingFile()) {
        if (auto* dollar = fn->lookupSymbol("$")) {
            if (dollar->type.isRef()) {
                if (auto inner = dollar->type.refElementType()) {
                    if (inner->ownerModule.empty()) inner->ownerModule = owner->moduleName();
                }
            } else if (dollar->type.ownerModule.empty()) {
                dollar->type.ownerModule = owner->moduleName();
            }
        }
    }
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

void SemaPass::validateExternFns() {
    if (!_file) return;
    const string& mod = _file->moduleName();
    for (auto& [name, overloads] : _file->localFnSymbols()) {
        for (auto& fn : overloads) {
            if (!fn.isExternal) continue;
            if (!fn.moduleName.empty() && fn.moduleName != mod) continue;
            const int line = fn.declLine > 0 ? fn.declLine : 1;
            for (auto& p : fn.params) {
                checkExternCLayoutType(p, fn.name, "parameters", line);
            }
            if (!fn.retType.empty()) {
                checkExternCLayoutType(fn.retType, fn.name, "return type", line);
            }
        }
    }

    // 同一 C 链接名须同一签名（§6.6.1）：先登记 SDK / 通配已见声明，再查本模块。
    map<string, const FnSymbolInfo*> byLink;
    auto consider = [&](const FnSymbolInfo& fn) {
        if (!fn.isExternal) return;
        string link = fn.externLinkName();
        if (link.empty()) return;
        auto it = byLink.find(link);
        if (it == byLink.end()) {
            byLink[link] = &fn;
            return;
        }
        if (!it->second->sameExternCSig(fn)) {
            const int line = fn.declLine > 0 ? fn.declLine : 1;
            throw YuxError(line, ErrorCode::E2036, link);
        }
    };
    auto walkFile = [&](FileNode* f) {
        if (!f) return;
        for (auto& [_, overloads] : f->localFnSymbols()) {
            for (auto& fn : overloads)
                consider(fn);
        }
    };
    for (ScopeNode* p = _file->parentScope(); p; p = p->parentScope()) {
        if (auto* pf = dynamic_cast<FileNode*>(p)) {
            walkFile(pf);
            for (auto* imp : pf->wildcardImports())
                walkFile(imp);
        }
    }
    for (auto* imp : _file->wildcardImports())
        walkFile(imp);
    for (auto& [_, overloads] : _file->localFnSymbols()) {
        for (auto& fn : overloads) {
            if (!fn.isExternal) continue;
            if (!fn.moduleName.empty() && fn.moduleName != mod) continue;
            consider(fn);
        }
    }
}

void SemaPass::checkExternCLayoutType(const TypeInfo& raw, const string& fnName, const char* where, int line) {
    TypeInfo t = sema::resolveAlias(raw, _file, _sdkFile);
    if (t.empty() || t.isPtr()) return;
    if (t.isFallible()) {
        throw YuxError(line, ErrorCode::E2034, fnName, t.getFullName(), where);
    }
    if (t.isNormal() && isBuiltinType(t.name)) return;
    if (t.isHeap() || t.isFn() || t.isDyn() || t.isRc() || t.isWeak() || t.isNullable() || t.isArrayGeneric() ||
        t.isRef() || t.isTuple() || t.isString() || t.isStringBuilder() || t.isArray()) {
        if (t.isHeap()) {
            auto inner = t.heapElementType();
            throw YuxError(line, ErrorCode::E4028, inner ? inner->name : std::string("?"));
        }
        if (t.isFn()) throw YuxError(line, ErrorCode::E2031, fnName, where);
        if (t.isDyn()) {
            auto spec = t.dynSpecType();
            throw YuxError(line, ErrorCode::E1136, spec ? spec->getFullName() : t.getFullName());
        }
        throw YuxError(line, ErrorCode::E2034, fnName, t.getFullName(), where);
    }
    if (_names.lookupEnum(t)) {
        throw YuxError(line, ErrorCode::E2034, fnName, t.getFullName(), where);
    }
    auto* sd = _names.lookupStruct(t);
    if (!sd) {
        throw YuxError(line, ErrorCode::E2034, fnName, t.getFullName(), where);
    }
    std::set<string> visiting;
    checkCLayoutFields(t, sd, line, visiting);
}

void SemaPass::checkCLayoutFields(const TypeInfo& structTy, StructDeclNode* sd, int line, std::set<string>& visiting) {
    const string key = structTy.identityKey();
    if (visiting.contains(key)) {
        throw YuxError(line, ErrorCode::E2035, structTy.getFullName(), "?", structTy.getFullName());
    }
    visiting.insert(key);

    std::map<string, TypeInfo> subst;
    if (sd->isGeneric()) {
        const auto& tps = sd->typeParams();
        if (structTy.genericArgs.size() != tps.size()) {
            throw YuxError(line, ErrorCode::E2035, structTy.getFullName(), "?", structTy.getFullName());
        }
        for (size_t i = 0; i < tps.size(); ++i) {
            if (structTy.genericArgs[i]) subst[tps[i]] = *structTy.genericArgs[i];
        }
    }

    std::function<void(const TypeInfo&, const string&)> checkFieldTy;
    checkFieldTy = [&](const TypeInfo& rawFt, const string& fieldName) {
        TypeInfo ft = sema::resolveAlias(rawFt.substitute(subst), _file, _sdkFile);
        if (ft.isPtr()) return;
        if (ft.isNormal() && isBuiltinType(ft.name) && ft.name != "bool") return;
        if (ft.isArray()) {
            if (ft.elementType) checkFieldTy(*ft.elementType, fieldName);
            return;
        }
        if (ft.isHeap() || ft.isFn() || ft.isDyn() || ft.isRc() || ft.isWeak() || ft.isNullable() ||
            ft.isArrayGeneric() || ft.isRef() || ft.isTuple() || ft.isString() || ft.isStringBuilder() ||
            (ft.isNormal() && ft.name == "bool") || ft.isFallible()) {
            throw YuxError(line, ErrorCode::E2035, structTy.getFullName(), fieldName, ft.getFullName());
        }
        if (_names.lookupEnum(ft)) {
            throw YuxError(line, ErrorCode::E2035, structTy.getFullName(), fieldName, ft.getFullName());
        }
        auto* nested = _names.lookupStruct(ft);
        if (!nested) {
            throw YuxError(line, ErrorCode::E2035, structTy.getFullName(), fieldName, ft.getFullName());
        }
        checkCLayoutFields(ft, nested, line, visiting);
    };

    for (auto& f : sd->fields()) {
        if (!f) continue;
        checkFieldTy(f->getType(), f->name().getText());
    }
    visiting.erase(key);
}
