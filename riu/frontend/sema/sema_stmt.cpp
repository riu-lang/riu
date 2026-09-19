// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 语句语义检查：visitStmt 经 accept 分派到 visitX。

#include "builtin_methods.h"
#include "sema/call_resolve.h"
#include "sema/name_resolver.h"
#include "sema/sema_pass.h"
#include "sema/sema_pass_detail.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "analyzer/symbol_suggest.h"
#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/node/type_node.h"
#include "ast/riu.h"
#include "tools/diagnostic.h"
#include "types.h"

using namespace sema::pass;

namespace {
void pushLoopLabel(vector<Token>& stack, const Token& label, int line, int col) {
    if (!label.getText().empty()) {
        for (const auto& existing : stack) {
            if (existing.getText() == label.getText()) {
                throw RiuError(line, col, ErrorCode::E3022, label.getText());
            }
        }
    }
    stack.push_back(label);
}

void checkLoopJump(const vector<Token>& stack, const Token& jumpLabel, int line, int col, const char* kw) {
    if (jumpLabel.getText().empty()) {
        if (stack.empty()) {
            throw RiuError(line, col, ErrorCode::E3094, kw);
        }
        return;
    }
    bool found = false;
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        if (it->getText() == jumpLabel.getText()) {
            found = true;
            break;
        }
    }
    if (!found) {
        throw RiuError(line, col, ErrorCode::E3025, kw, jumpLabel.getText());
    }
}

// 从 Array<T> / [T*N] 抽出元素类型；非迭代类型返回 nullptr。
sp<TypeInfo> forInElementType(const TypeInfo& coll) {
    if (coll.isArrayGeneric()) return coll.arrayGenericElementType();
    if (coll.isArray()) return coll.elementType;
    return nullptr;
}

// for-in item 在包装块与 filled 块上各有一份 SymbolInfo 拷贝；体语句
// nearest scope 走包装块，必须两处都写，否则 Indexed 的 U& 留在空类型上。
void bindForInItemType(StatementBlockNode* blk, const string& name, const TypeInfo& ty) {
    if (!blk || name.empty()) return;
    if (auto* sym = blk->lookupSymbol(name)) {
        sym->type = ty;
    }
    if (auto* p = blk->parentScope()) {
        if (auto* sym = p->lookupSymbol(name)) {
            sym->type = ty;
        }
    }
}

bool isSdkEndType(const TypeInfo& t, const sema::NameResolver& names, FileNode* sdkFile) {
    if (t.name != "End" || !t.genericArgs.empty() || !sdkFile) return false;
    auto* ed = names.lookupEnum(t, nullptr);
    return ed && ed == sdkFile->getEnumDecl("End");
}

string forInCallerFallibleErr(FnNode* fn, LambdaExprNode* lam) {
    if (fn && fn->header()) {
        string e = fn->header()->resolvedFallibleErr();
        if (!e.empty()) return e;
    }
    if (lam && lam->fallibleErrTypeNode()) {
        return fallibleErrKey(lam->fallibleErrTypeNode()->getType());
    }
    if (lam) {
        auto ft = lam->getType();
        if (ft.isFn() && ft.fnReturnType() && !ft.fnReturnType()->fallibleErr.empty()) {
            return ft.fnReturnType()->fallibleErr;
        }
    }
    return {};
}

string forInIdentName(ExprNode* expr) {
    auto* lit = dynamic_cast<ExprLiteralNode*>(expr);
    if (!lit) return {};
    auto* obj = dynamic_cast<LiteralObjNode*>(lit->literal());
    if (!obj) return {};
    return obj->getValue().getText();
}
} // namespace

void SemaPass::visitStmt(StatementNode* stmt) {
    if (!stmt) return;
    stmt->accept(*this);
}

void SemaPass::visitBlock(StatementBlockNode& block) {
    visitBlock(&block, _visitExpected);
}

void SemaPass::visitLoop(StatementLoopNode& node) {
    auto* loop = &node;

    const auto& label = loop->label();
    pushLoopLabel(_loopLabelStack, label, loop->getLineNumber(), loop->getColumn());
    if (loop->hasInit()) {
        TypeInfo texp;
        const TypeInfo* tp = nullptr;
        if (loop->initType()) {
            try {
                checkTypeAnn(loop->initType()->getType(), loop->initType(), loop->getLineNumber(), loop->getColumn(),
                             true);
                texp = sema::resolveAlias(applyInstSubst(loop->initType()->getType()), _file, _sdkFile);
                tp = &texp;
            } catch (const RiuError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        visitExpr(loop->initExpr(), tp);
        if (loop->initNames().size() > 1) {
            checkTupleDestructure(loop->initExpr(), loop->initType(), loop->initNames().size(), loop->getLineNumber(),
                                  loop->getColumn(), _file, _sdkFile, _currentTypeParams, currentInstSubst());
        }
    }
    visitBlock(loop->block());
    _loopLabelStack.pop_back();
    return;
}

void SemaPass::visitSet(StatementSetNode& node) {
    auto* set = &node;

    visitExpr(set->arrayExpr());
    for (auto& idx : set->indices())
        visitExpr(idx);
    // 与 compileArraySetStatement 同序：空下标 E3060 → 非 lvalue E3061 → 非数组 E3062。
    // E3060：文法强制 args+=expr，死防御。E3061 与 T 无关，模板期也报。
    if (set->indices().empty()) {
        throw RiuError(set->getLineNumber(), set->getColumn(), ErrorCode::E3060, "assignment");
    }
    if (!isArraySetLvalue(set->arrayExpr())) {
        throw RiuError(set->getLineNumber(), set->getColumn(), ErrorCode::E3061, "assignment");
    }
    TypeInfo elemStorage;
    const TypeInfo* elemExpected = nullptr;
    if (!set->indices().empty()) {
        try {
            TypeInfo at =
                set->arrayExpr()->hasResolvedType() ? set->arrayExpr()->resolvedType() : set->arrayExpr()->getType();
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
                throw RiuError(set->getLineNumber(), set->getColumn(), ErrorCode::E3062, at.name);
            }
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    visitExpr(set->valueExpr(), elemExpected);
    rejectEscapingRefCaptureLambda(set->valueExpr());
    if (elemExpected) {
        checkAssignRhs(set->valueExpr(), *elemExpected, set->getLineNumber(), set->getColumn(), _file, _sdkFile,
                       _currentTypeParams, currentInstSubst());
    }
    // v0.16 闭包捕获: lambda body 内对捕获变量赋值 → E2030。
    // StatementSetNode 覆盖简单变量 `a = 20` / 复合赋值 `a += 1` / 索引赋值 `a[i] = x`。
    // LHS arrayExpr 抽取变量名后按 StatementAssignNode 同款规则判定。
    if (_currentLambda && _currentFn && set->indices().empty()) {
        auto lhsLit = dynamic_cast<ExprLiteralNode*>(set->arrayExpr());
        if (lhsLit) {
            auto lhsObj = dynamic_cast<LiteralObjNode*>(lhsLit->literal());
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
                            throw RiuError(set->getLineNumber(), set->getColumn(), ErrorCode::E2030, objName);
                        }
                    }
                }
            }
        }
    }
    return;
}

void SemaPass::visitBreak(StatementBreakNode& node) {
    auto* br = &node;

    checkLoopJump(_loopLabelStack, br->label(), br->getLineNumber(), br->getColumn(), "break");
    return;
}

void SemaPass::visitContinue(StatementContinueNode& node) {
    auto* cont = &node;

    checkLoopJump(_loopLabelStack, cont->label(), cont->getLineNumber(), cont->getColumn(), "continue");
    return;
}

void SemaPass::visitForIn(StatementForInNode& node) {
    auto* forin = &node;

    const auto& label = forin->label();
    pushLoopLabel(_loopLabelStack, label, forin->getLineNumber(), forin->getColumn());
    try {
        visitExpr(forin->expr());
        TypeInfo at;
        try {
            at = forin->expr()->hasResolvedType() ? forin->expr()->resolvedType() : forin->expr()->getType();
            at = applyInstSubst(at).peelRef();
            at = sema::resolveAlias(at, _file, _sdkFile);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        if (isCurrentTypeParam(at)) {
            // 模板形参：等实例化后再查 E3160
        } else if (auto elem = forInElementType(at)) {
            TypeInfo itemTy("Ref", {std::make_shared<TypeInfo>(*elem)});
            bindForInItemType(forin->block(), forin->item().getText(), itemTy);
        } else if (_riu) {
            // 第二档：`#Impl(Indexed<U>)`，item = U&
            auto indexed = _riu->specImplChecker().findSpecImplArgs(at, "Indexed", _file);
            if (indexed && indexed->size() == 1) {
                TypeInfo itemTy("Ref", {std::make_shared<TypeInfo>((*indexed)[0])});
                bindForInItemType(forin->block(), forin->item().getText(), itemTy);
            } else {
                // 第三档：`#Impl(Iter<U, E>)`，item = U 值；E = SDK End 时不可失败
                auto iter = _riu->specImplChecker().findSpecImplArgs(at, "Iter", _file);
                if (iter && iter->size() == 2) {
                    TypeInfo itemTy = (*iter)[0];
                    TypeInfo errTy = (*iter)[1];
                    bindForInItemType(forin->block(), forin->item().getText(), itemTy);
                    if (isNoCopyTypeIn(at, _file, _sdkFile) && !at.isRef()) {
                        string ident = forInIdentName(forin->expr());
                        if (!ident.empty()) _movedVars.insert(ident);
                    }
                    if (!isSdkEndType(errTy, _names, _sdkFile)) {
                        string eKey = fallibleErrKey(errTy);
                        if (!_tryStack.empty()) {
                            _tryStack.back().push_back(eKey);
                        } else {
                            string callerErr = forInCallerFallibleErr(_currentFn, _currentLambda);
                            if (callerErr.empty()) {
                                throw RiuError(forin->getLineNumber(), forin->getColumn(), ErrorCode::E7006, "for-in");
                            }
                            if (callerErr != eKey) {
                                throw RiuError(forin->getLineNumber(), forin->getColumn(), ErrorCode::E7004, eKey,
                                               callerErr, eKey, callerErr, eKey);
                            }
                        }
                    }
                } else {
                    throw RiuError(forin->getLineNumber(), forin->getColumn(), ErrorCode::E3160,
                                   at.getFullName().empty() ? "<unknown>" : at.getFullName());
                }
            }
        } else {
            throw RiuError(forin->getLineNumber(), forin->getColumn(), ErrorCode::E3160,
                           at.getFullName().empty() ? "<unknown>" : at.getFullName());
        }
        visitBlock(forin->block());
    } catch (...) {
        _loopLabelStack.pop_back();
        throw;
    }
    _loopLabelStack.pop_back();
    return;
}

void SemaPass::visitRetVoid(StatementRetVoidNode& node) {
    auto* rv = &node;

    // Phase C：lambda 期望非 void 时 `ret;` → E3014（镜像 compileRetVoidStatement）。
    if (_currentLambda) {
        TypeInfo want;
        if (lambdaExpectedRetType(_currentLambda, want) && !want.empty()) {
            throw RiuError(rv->getLineNumber(), rv->getColumn(), ErrorCode::E3014, want.getFullName(), "void");
        }
    }
    return;
}

void SemaPass::visitDeclare(StatementDeclareNode& node) {
    auto* d = &node;

    // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct / enum arity).
    // 无 init 形态 (`let p Pair<i32>` / `let x Box`), 仅 varType, 同款检查.
    if (d->varType()) {
        try {
            auto vt = d->varType()->getType();
            sema::validateGenericNamedTypeArity(vt, _names, d->getLineNumber(), d->getColumn(), _currentStructName);
            if (!typeStillTemplate(vt) && !sema::typeHasLlvmLayout(vt, _file, _sdkFile, _currentTypeParams)) {
                auto* sd = _names.lookupStruct(vt);
                auto* ed = _names.lookupEnum(vt);
                if (!(sd && sd->isGeneric()) && !(ed && ed->isGeneric())) {
                    throw RiuError(d->getLineNumber(), d->getColumn(), ErrorCode::E3096, vt.getFullName());
                }
            }
            noteConcreteGenericType(vt);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // 留 Compiler 兜底
        }
    }
    return;
}

void SemaPass::visitAssign(StatementAssignNode& node) {
    auto* as = &node;

    // Phase 2e: `$.field = ...` 在 `#Static fn` 体内禁用 (E3128).
    // StatementAssign 的 `obj` (LHS 根) 不会被 visitExpr 递归, 这里单独拦截.
    if (as->obj().getText() == "$" && _currentFn && _currentFn->header()->isStatic()) {
        throw RiuError(as->obj().getLine(), static_cast<int>(as->obj().getCharPositionInLine()), ErrorCode::E3128);
    }
    // Phase C：LHS 根不是表达式，visitExpr 走不到；未定义 → E3030（与 getType 读路径对齐）。
    if (_currentFn) {
        string assignObj = as->obj().getText();
        SymbolInfo* assignSym = nullptr;
        if (auto sc = as->findNearestScope()) {
            assignSym = sc->lookupSymbol(assignObj);
        }
        if (!assignSym) {
            assignSym = _currentFn->lookupSymbol(assignObj);
        }
        if (!assignSym) {
            ScopeNode* scope = as->findNearestScope();
            if (!scope) scope = _currentFn;
            SymbolSuggest::throwSymbolNotFound(scope, as->getLineNumber(), as->getColumn(), ErrorCode::E3030,
                                               assignObj);
        }
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
                throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E3093, objName);
            }
        }
    }
    // v0.16 闭包捕获: lambda body 内对捕获变量赋值 / 成员链写 → E2030.
    // 覆盖 `=` / `+= -= *= /= %=` 及 `obj.f = ...` / `obj[i] = ...`
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
                    throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E2030, objName);
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
                        throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E3100, memberText,
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
                            throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E3046)
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
                tryValidateReflectFieldValueWrite(objName, sym->type, members, as->getLineNumber(), as->getColumn());
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
                    map<string, TypeInfo> fieldSubst;
                    if (const auto* s = currentInstSubst()) fieldSubst = *s;
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
    if (as->expr()) rejectEscapingRefCaptureLambda(as->expr());
    if (haveStorage) {
        checkAssignRhs(as->expr(), assignStorage, as->getLineNumber(), as->getColumn(), _file, _sdkFile,
                       _currentTypeParams, currentInstSubst());
    }
    return;
}

void SemaPass::visitDeclareAssignTuple(StatementDeclareAssignTupleNode& node) {
    auto* tup = &node;

    // Phase C：元组解构 E3101 / E3102（标注或 RHS；别名 resolveAlias；泛型体 subst）。
    if (tup->expr()) {
        TypeInfo texp;
        const TypeInfo* tp = nullptr;
        if (tup->varType()) {
            try {
                texp = sema::resolveAlias(applyInstSubst(tup->varType()->getType()), _file, _sdkFile);
                tp = &texp;
            } catch (const RiuError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        visitExpr(tup->expr(), tp);
        checkTupleDestructure(tup->expr(), tup->varType(), tup->names().size(), tup->getLineNumber(), tup->getColumn(),
                              _file, _sdkFile, _currentTypeParams, currentInstSubst());
    }
    return;
}

void SemaPass::visitRet(StatementRetNode& node) {
    auto* ret = &node;

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
                    .subst = currentInstSubst()};
    if (_currentLambda) {
        lambdaHasExpected = lambdaExpectedRetType(_currentLambda, retExpected);
        if (lambdaHasExpected) {
            retExpected = applyInstSubst(retExpected);
            retExpectedResolved = resolveForRet(retExpected, retCtx);
            retExpPtr = &retExpectedResolved;
        }
        if (_currentLambda->fallibleErrTypeNode()) {
            retCtx.fallibleErr = fallibleErrKey(applyInstSubst(_currentLambda->fallibleErrTypeNode()->getType()));
        } else if (retExpected.isFallible()) {
            retCtx.fallibleErr = retExpected.fallibleErr;
        }
    } else if (_currentFn && ret->expr()) {
        auto header = _currentFn->header();
        if (header && header->retType()) {
            try {
                retExpected = applyInstSubst(header->retType()->getType());
                retExpectedResolved = resolveForRet(retExpected, retCtx);
                retExpPtr = &retExpectedResolved;
            } catch (const RiuError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
            }
        }
        if (header && header->fallibleErrTypeNode()) {
            retCtx.fallibleErr = fallibleErrKey(applyInstSubst(header->fallibleErrTypeNode()->getType()));
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
        rejectEscapingRefCaptureLambda(ret->expr());
    }
    return;
}

void SemaPass::visitDeclareAssign(StatementDeclareAssignNode& node) {
    auto* da = &node;

    // T& 局部初始化：ID copy-bind E3018、`&expr` 内层 E3014、其余非法形态 E3019.
    // lambda 体 sema 不下钻 — 这里检查 _currentFn 非空再做.
    // Bucket 4 起步 (CURRENT-check.md): E6011 (泛型 struct / enum arity 不匹配).
    // 不依赖 expr / _currentFn, 仅 varType 形态.
    if (da->varType()) {
        try {
            auto vt = da->varType()->getType();
            checkTypeAnn(vt, da->varType(), da->getLineNumber(), da->getColumn(), true);
        } catch (const RiuError&) {
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
            if (!typeStillTemplate(daExpected) &&
                !sema::typeHasLlvmLayout(daExpected, _file, _sdkFile, _currentTypeParams)) {
                auto* sd = _names.lookupStruct(daExpected);
                auto* ed = _names.lookupEnum(daExpected);
                if (!(sd && sd->isGeneric()) && !(ed && ed->isGeneric())) {
                    throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3096, daExpected.getFullName());
                }
            }
            daExpPtr = &daExpected;
            noteConcreteGenericType(daExpected);
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    if (dynamic_cast<ExprArrayInitNode*>(da->expr())) {
        if (!da->varType()) {
            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3067, " with size");
        }
        if (daExpPtr && !daExpPtr->isArray()) {
            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3067, "");
        }
    }
    if (da->varType() && _currentFn && da->expr()) {
        auto varType = applyInstSubst(da->varType()->getType());
        // Bucket 6 收口+ (CURRENT-check.md): 目标类型驱动的形态校验.
        // E3012 (fixed-array 大小不匹配) / E3015 (Nullable 内部类型不匹配).
        // 镜像 compiler_stmt.cpp:773 / 740. 复杂路径 (alias / 嵌套数组目标类型)
        // 留 Compiler 兜底. lambda 体 sema 不下钻.
        try {
            if (varType.isArray() && !dynamic_cast<ExprArrayNode*>(da->expr()) &&
                !dynamic_cast<ExprArrayInitNode*>(da->expr())) {
                auto exprType = da->expr()->getType();
                if (exprType.isArrayGeneric()) {
                    throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, varType.getFullName(),
                                   exprType.getFullName())
                        .withHint("[T * N] and Array<T> are distinct types and are not interchangeable");
                }
                if (exprType.isArray() && exprType.arraySize > 0 && varType.arraySize != exprType.arraySize) {
                    throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3012, varType.arraySize,
                                   exprType.arraySize);
                }
                if (exprType.isArray() && varType.elementType && exprType.elementType &&
                    *varType.elementType != *exprType.elementType) {
                    throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3009,
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
                            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, exprType.name,
                                           innerType->name);
                        }
                    }
                }
            }
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
            // getType 失败: 留 Compiler 兜底
        }
        if (varType.isRef()) {
            auto innerType = varType.refElementType();
            if (innerType) {
                auto* rhs = da->expr();
                if (auto getRef = dynamic_cast<ExprGetRefNode*>(rhs)) {
                    TypeInfo getTy;
                    if (tryGetExprType(getRef, getTy)) {
                        auto innerOfGetRef = getTy.refElementType();
                        if (!innerOfGetRef || *innerOfGetRef != *innerType) {
                            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, innerType->name,
                                           innerOfGetRef ? innerOfGetRef->name : "?")
                                .withHint(
                                    std::format("&expr 的内层类型必须与声明一致；预期 `&<{}>`，源表达式给出 `&<{}>`",
                                                innerType->name, innerOfGetRef ? innerOfGetRef->name : "?"));
                        }
                    }
                } else if (auto litExpr = dynamic_cast<ExprLiteralNode*>(rhs)) {
                    if (auto litObj = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                        string srcName = litObj->getValue().getText();
                        SymbolInfo* sym = lookupRetVar(srcName, da, _currentFn);
                        if (!sym || !sym->type.isRef() || !sym->type.refElementType() ||
                            *sym->type.refElementType() != *innerType) {
                            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3018, srcName,
                                           innerType->name)
                                .withHint(std::format("`{}` 不是 {}& 类型，无法 copy-bind 到此声明；改写为 "
                                                      "`&<expr-of-{}>` 或先声明同类型 T&",
                                                      srcName, innerType->name, innerType->name));
                        }
                        // 与 compileDeclareAssignStatement `_localVarPtrs` 对齐：
                        // 形参 / 本帧局部可拷绑；全局与 lambda 外层 T& → E4004。
                        if (!isCodegenFrameLocal(srcName, da, _currentLambda)) {
                            throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E4004, srcName)
                                .withHint("T& 只能绑定到当前函数内的局部变量；不可绑参数、全局符号或外层闭包变量");
                        }
                    } else {
                        throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                            .withHint("T& 局部初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量），或 "
                                      "`val r T& = as_ref(box)`");
                    }
                } else if (auto callExpr = dynamic_cast<ExprCallNode*>(rhs)) {
                    string calleeName;
                    if (auto litCallee = dynamic_cast<ExprLiteralNode*>(callExpr->getCalleeExpr())) {
                        if (auto obj = dynamic_cast<LiteralObjNode*>(litCallee->literal())) {
                            calleeName = obj->getValue().getText();
                        }
                    }
                    TypeInfo callTy;
                    bool callRetIsRef = tryGetExprType(callExpr, callTy) && callTy.isRef();
                    if (calleeName != "as_ref" && !callRetIsRef) {
                        throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                            .withHint("T& 局部初始化形如 `val r T& = &x`、`val r2 T& = r1`（拷绑已有 T& 变量）、`val r "
                                      "T& = as_ref(box)` 或返回 T& 的方法/函数调用");
                    }
                } else if (auto pathCall = dynamic_cast<ExprPathCallNode*>(rhs)) {
                    TypeInfo pathTy;
                    if (!tryGetExprType(pathCall, pathTy) || !pathTy.isRef()) {
                        throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                            .withHint("静态路径不返回 T& 类型，无法初始化 T& 局部");
                    }
                } else if (auto getNode = dynamic_cast<ExprGetNode*>(rhs)) {
                    TypeInfo getTy;
                    if (!tryGetExprType(getNode, getTy) || !getTy.isRef()) {
                        throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
                            .withHint("数组索引不返回 T& 类型，无法初始化 T& 局部");
                    }
                } else {
                    throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3019)
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
            if (sema::lookupScopedAlias(da, n)) return true;
            return false;
        };
        // 跳过 ExprPathCallNode（如 `Label::COUNT` / `P::get_x()`）—
        // 这些表达式有独立的类型/语义校验（E3120/E3121 等），不应被通用类型检查遮蔽
        if (!varType.isRef() && !varType.isArray() && !varType.isNullable() && !varType.isRc() && !varType.isWeak() &&
            !varType.isArrayGeneric() && !varType.isHeap() && !varType.isFn() && varType.genericArgs.empty() &&
            !isFlexibleIntExpr(da->expr()) && !isAliasName(varType.name) &&
            !dynamic_cast<ExprPathCallNode*>(da->expr())) {
            try {
                auto exprType = applyInstSubst(da->expr()->getType());
                // 跳过泛型形参 / 未解析类型（如 T, U 等）：此时尚未实例化，比较无意义
                auto isKnownType = [&](const TypeInfo& t) -> bool {
                    if (isBuiltinType(t.name)) return true;
                    if (_file && (_file->getStructDecl(t.name) || _file->getEnumDecl(t.name))) return true;
                    if (_sdkFile && (_sdkFile->getStructDecl(t.name) || _sdkFile->getEnumDecl(t.name))) return true;
                    return false;
                };
                if (!varType.name.empty() && !exprType.name.empty() && !varType.isSelf() && !exprType.isSelf() &&
                    !exprType.isRef() && !exprType.isFn() && !isAliasName(exprType.name) && isKnownType(varType) &&
                    isKnownType(exprType)) {
                    if (varType != exprType) {
                        throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E3014, varType.getFullName(),
                                       exprType.getFullName())
                            .withHint(std::format("声明类型为 `{}`，但表达式类型为 `{}`；riu 无隐式类型转换",
                                                  varType.getFullName(), exprType.getFullName()));
                    }
                }
            } catch (const RiuError&) {
                throw;
            } catch (...) { // NOLINT(bugprone-empty-catch)
                // getType 失败: 留 Compiler 兜底
            }
        }
    }
    if (da->expr()) visitExpr(da->expr(), daExpPtr);
    refreshInferredLetType(da);
    // Phase C：句柄声明形态必须在 visitExpr 带靶向类型之后——
    // if/match 块末尾数组字面量先走 E3009，再查非 Array 的 E3064。
    if (da->varType() && _currentFn && da->expr()) {
        auto handleTy = applyInstSubst(da->varType()->getType());
        if (handleTy.isRc() || handleTy.isWeak() || handleTy.isArrayGeneric()) {
            checkDeclareHandleRhs(da->expr(), handleTy, da->getLineNumber(), da->getColumn(), _file, _sdkFile,
                                  _currentTypeParams, currentInstSubst());
        } else if (_names.lookupEnum(handleTy)) {
            checkAssignRhs(da->expr(), handleTy, da->getLineNumber(), da->getColumn(), _file, _sdkFile,
                           _currentTypeParams, currentInstSubst());
        }
    }
    // v0.16 闭包捕获: lambda 字面量直接作 var/val 初始化值且含 T& 捕获 → E4022
    // (spec §8.7.6.5 不可逃逸：fn 值不可被存储到寿命外延的变量)。
    // 仅拦截直接形 (lambda 字面量), 穿透检测 (右值 wrapper 调用结果等) 留 codegen 兜底。
    if (da->expr()) {
        rejectEscapingRefCaptureLambda(da->expr());
    }
    // Phase B-1: #NoCopy 类型不可从现有变量隐式复制（let 绑定）
    if (da->varType() && da->expr()) {
        auto varType = applyInstSubst(da->varType()->getType());
        if (isNoCopyTypeIn(varType, _file, _sdkFile) && !varType.isRef()) {
            if (!isFreshHandleExpr(da->expr())) {
                throw RiuError(da->getLineNumber(), da->getColumn(), ErrorCode::E4031, varType.name, "let 绑定",
                               varType.name);
            }
        }
    }
    return;
}

void SemaPass::visitExprStmt(StatementExprNode& node) {
    auto* se = &node;

    // 覆盖 StatementExprNode / Ret / DeclareAssign / DeclareAssignTuple / Assign
    // E4030: `a <- b` 作为表达式语句时结果被丢弃，建议改用 `a = b`
    if (auto ma = dynamic_cast<ExprMoveAssignNode*>(se->expr())) {
        DiagnosticEngine::emit(_sourcePath, RiuError(ma->resolveLineNumber(), ma->resolveColumn(), ErrorCode::E4030));
    }
    if (se->expr()) visitExpr(se->expr());
    return;
}

void SemaPass::visitStaticFieldSet(StatementStaticFieldSetNode& node) {
    auto* sf = &node;

    auto r = sema::resolveExprTypeLhs(_file, _riu, sf->typePath(), sf->getLineNumber(), sf->getColumn());
    string typeName = r.type.name;
    string fieldName = sf->fieldName().getText();
    auto* structDecl = r.structDecl ? r.structDecl : _names.lookupStruct(r.type, true);
    if (!structDecl) {
        throw RiuError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3030, typeName);
    }
    const auto* field = structDecl->staticField(fieldName);
    if (!field) {
        throw RiuError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3030, typeName + "::" + fieldName);
    }
    if (!field->isMutable) {
        throw RiuError(sf->getLineNumber(), sf->getColumn(), ErrorCode::E3151, typeName + "::" + fieldName);
    }
    TypeInfo ft;
    const TypeInfo* fp = nullptr;
    if (field->type) {
        try {
            ft = field->type->getType();
            fp = &ft;
        } catch (const RiuError&) {
            throw;
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
    }
    visitExpr(sf->valueExpr(), fp);
    if (fp) {
        checkAssignRhs(sf->valueExpr(), *fp, sf->getLineNumber(), sf->getColumn(), _file, _sdkFile, _currentTypeParams,
                       currentInstSubst());
    }
    return;
}

void SemaPass::visitAlias(AliasDeclNode& n) {
    if (!n.target()) return;
    int line = n.getLineNumber();
    int col = n.getColumn();
    TypeInfo t = n.target()->getType();
    validateTypeArgRefPolicy(t, line, col, false);
    checkTypeAnn(t, n.target(), line, col, false);
}
