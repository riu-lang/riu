// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "const_mut_checker.h"

#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/literal_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "error_code.h"

namespace {

// 拿到表达式所属作用域：优先 expr 自身的 findNearestScope，失败则回退到附近 stmt。
ScopeNode* exprScope(ExprNode* e, ScopeNode* fallback) {
    if (e) {
        if (auto sc = e->findNearestScope()) return sc;
    }
    return fallback;
}

// 判定一个表达式是否符合 §3.3 的"常量表达式"。
// 不通过时抛 E3104，errExpr 指向最里层不合规子表达式。
// scope 用于解析 LiteralObjNode 的符号引用（必须是 cval）。
void requireConstExpr(ExprNode* e, ScopeNode* scope);

void throwNonConst(ExprNode* e, const std::string& what) {
    int line = e->resolveLineNumber();
    int col = e->resolveColumn();
    throw RiuError(line, col, ErrorCode::E3104, what);
}

void requireConstExpr(ExprNode* e, ScopeNode* scope) {
    if (!e) return;

    if (auto paren = dynamic_cast<ExprParenNode*>(e)) {
        requireConstExpr(paren->expr(), scope);
        return;
    }
    if (auto u = dynamic_cast<ExprUnaryNode*>(e)) {
        requireConstExpr(u->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<ExprAddSubNode*>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<ExprMulDivModNode*>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<ExprBinOpNode*>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }
    if (auto b = dynamic_cast<ExprCompareNode*>(e)) {
        requireConstExpr(b->left(), scope);
        requireConstExpr(b->right(), scope);
        return;
    }

    if (auto le = dynamic_cast<ExprLiteralNode*>(e)) {
        auto lit = le->literal();
        if (dynamic_cast<LiteralIntNode*>(lit)) return;
        if (dynamic_cast<LiteralFloatNode*>(lit)) return;
        if (dynamic_cast<LiteralBoolNode*>(lit)) return;
        if (dynamic_cast<LiteralNullNode*>(lit)) return;
        if (dynamic_cast<LiteralStringNode*>(lit)) return;
        if (dynamic_cast<LiteralCodePointNode*>(lit)) return;
        if (auto obj = dynamic_cast<LiteralObjNode*>(lit)) {
            auto name = obj->getValue().getText();
            auto sc = exprScope(e, scope);
            SymbolInfo* sym = sc ? sc->lookupSymbol(name) : nullptr;
            if (sym && sym->isConst) return;
            // Phase 4 (DRAFT-const-eval §4): #Const fn 体内, 参数与默认 (non-cval) 局部
            // 也可作 const 表达式的子项 —— 实际值绑定由 ConstEvaluator 在调用点注入 _env。
            // 这里只放行符号查找; const-evaluability 由后续 eval 真正判定。
            Node* cur = e;
            while (cur) {
                if (auto* fn = dynamic_cast<FnNode*>(cur)) {
                    if (fn->header() && fn->header()->hasAnno("Const")) return;
                    break;
                }
                cur = cur->parent();
            }
            throwNonConst(e, "reference to non-`cval` symbol `" + name + "`");
        }
        if (dynamic_cast<StringTemplateNode*>(lit)) {
            throwNonConst(e, "string template interpolation");
        }
        throwNonConst(e, "unsupported literal");
    }

    if (auto call = dynamic_cast<ExprCallNode*>(e)) {
        // 整数位方法（and/or/xor/shl/shr/inv）走 const-eval，与算术组合一样合法。
        if (auto* dot = dynamic_cast<ExprDotNode*>(call->getCalleeExpr())) {
            const string& m = dot->member();
            if (m == "inv" && call->getArgs().empty()) {
                requireConstExpr(dot->baseExpr(), scope);
                return;
            }
            if ((m == "and" || m == "or" || m == "xor" || m == "shl" || m == "shr") && call->getArgs().size() == 1) {
                requireConstExpr(dot->baseExpr(), scope);
                requireConstExpr(call->getArgs()[0], scope);
                return;
            }
        }
        // Phase 4 (DRAFT-const-eval §4): #Const fn 调用允许进入 const 表达式。
        // 仅识别裸自由函数形态：callee = LiteralObj("name")。其它形态（方法 / 路径 /
        // lambda）当前不接 const-eval，仍按 E3104 拒。
        bool ok = false;
        if (auto cle = dynamic_cast<ExprLiteralNode*>(call->getCalleeExpr())) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(cle->literal())) {
                string fname = obj->getValue().getText();
                // 沿 scope 链向上找 FileNode → 查 #Const fn 符号
                Node* cur = e;
                FileNode* file = nullptr;
                while (cur) {
                    if (auto* fl = dynamic_cast<FileNode*>(cur)) {
                        file = fl;
                        break;
                    }
                    cur = cur->parent();
                }
                if (file) {
                    FnSymbolInfo* fs = file->lookupFnSymbol(fname);
                    if (!fs) {
                        for (auto* imp : file->wildcardImports()) {
                            fs = imp->lookupFnSymbol(fname);
                            if (fs) break;
                        }
                    }
                    if (fs && fs->isConst) ok = true;
                }
            }
        }
        if (!ok) throwNonConst(e, "function / method call");
        for (auto& a : call->getArgs())
            requireConstExpr(a, scope);
        return;
    }
    if (dynamic_cast<ExprDotNode*>(e)) throwNonConst(e, "member access");
    if (dynamic_cast<ExprGetNode*>(e)) throwNonConst(e, "array indexing");
    if (dynamic_cast<ExprGetRefNode*>(e)) throwNonConst(e, "address-of (&) expression");
    if (dynamic_cast<ExprArrayNode*>(e)) throwNonConst(e, "array literal");
    if (dynamic_cast<ExprArrayInitNode*>(e)) throwNonConst(e, "array fill expression");
    if (dynamic_cast<ExprTupleNode*>(e)) throwNonConst(e, "tuple construction");
    if (dynamic_cast<ExprIfElseNode*>(e)) throwNonConst(e, "if-else expression");
    if (dynamic_cast<ExprOneLineIfElseNode*>(e)) throwNonConst(e, "if-else expression");
    if (dynamic_cast<ExprMatchNode*>(e)) throwNonConst(e, "match expression");
    if (dynamic_cast<ExprTryCatchNode*>(e)) throwNonConst(e, "try-catch expression");
    if (dynamic_cast<ExprPathCallNode*>(e)) throwNonConst(e, "enum constructor");
    if (dynamic_cast<ExprDynCtorNode*>(e)) throwNonConst(e, "Dyn<...> construction");
    if (dynamic_cast<ExprNullElseNode*>(e)) throwNonConst(e, "`??` expression");
    if (dynamic_cast<LambdaExprNode*>(e)) throwNonConst(e, "lambda expression");

    throwNonConst(e, "unsupported expression");
}

// ==================== §5 frozen helpers ====================
// `e` 是否直接引用了 frozen 符号（不展开 dot / get / call 等表达式）。
// 用于：
//   §5.3 — 拒收 `s.f = ...` / `s[i] = ...`（s 是 frozen）；
//   §5.4 — 拒收"可写槽位"承接 frozen 表达式（顶层 ID 形态）。
// 子表达式（如 `f(frozen)`）暂不深扫，等 callsite 校验上线再补。
bool isFrozenIdRef(ExprNode* e, ScopeNode* scope) {
    if (!e) return false;
    if (auto paren = dynamic_cast<ExprParenNode*>(e)) {
        return isFrozenIdRef(paren->expr(), scope);
    }
    if (auto le = dynamic_cast<ExprLiteralNode*>(e)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
            auto sc = exprScope(e, scope);
            SymbolInfo* sym = sc ? sc->lookupSymbol(obj->getValue().getText()) : nullptr;
            return sym && sym->isFrozen;
        }
    }
    return false;
}

// `e` 是否是 `copy_of:<T>(arg)` 调用（§5.3 唯一脱 const 出口）。
bool isCopyOfCall(ExprNode* e) {
    auto call = dynamic_cast<ExprCallNode*>(e);
    if (!call) return false;
    auto callee = call->getCalleeExpr();
    if (auto le = dynamic_cast<ExprLiteralNode*>(callee)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
            return obj->getValue().getText() == "copy_of";
        }
    }
    return false;
}

// §5.4：RHS 是否携带 frozen，且不是 copy_of 脱出。
// P1-3 简化版：仅识别顶层 frozen ID / paren 包裹的 frozen ID；
// 复杂表达式（含调用、运算、字段访问等）暂按"不携带" —— 留 P1-3-followup。
bool carriesFrozenTopLevel(ExprNode* e, ScopeNode* scope) {
    if (!e) return false;
    if (isCopyOfCall(e)) return false;
    return isFrozenIdRef(e, scope);
}

// 取 LHS（StatementAssignNode）的 SymbolInfo*；找不到返回 nullptr。
SymbolInfo* lookupLhsSym(StatementAssignNode* a, ScopeNode* scope) {
    auto sc = scope;
    if (!sc) sc = a->findNearestScope();
    return sc ? sc->lookupSymbol(a->obj().getText()) : nullptr;
}

ConstMutWalker::FnContext resolveFnContext(FnNode* fn) {
    ConstMutWalker::FnContext c;
    Node* cur = fn->parent();
    while (cur) {
        if (auto* si = dynamic_cast<StructImplNode*>(cur)) {
            if (!c.impl) c.impl = si;
        } else if (auto* fl = dynamic_cast<FileNode*>(cur)) {
            c.file = fl;
            break;
        }
        cur = cur->parent();
    }
    if (c.impl) {
        c.implStructName = c.impl->structName();
        if (c.impl->hasDestructor() && c.impl->destructor() == fn) {
            c.isDestructor = true;
        }
    }
    return c;
}

// 在 file 与其 wildcardImports 范围内查 typeName 对应的 StructDeclNode。
StructDeclNode* findStructDecl(FileNode* file, const string& typeName) {
    if (!file || typeName.empty()) return nullptr;
    if (auto* owner = file->getStructOwner(typeName)) {
        if (auto* sd = owner->getStructDecl(typeName)) return sd;
    }
    return file->getStructDecl(typeName);
}

} // namespace

void ConstMutWalker::begin(FnNode* fn) {
    _ctx = resolveFnContext(fn);
    _isConstFn = fn->header()->hasAnno("Const");
    _fnName = fn->header()->name().getText();
}

void ConstMutWalker::checkConstFnWrite(StatementAssignNode* as, ScopeNode* scope) {
    if (!_isConstFn) return;
    string name = as->obj().getText();
    if (_localNames.contains(name)) return;
    auto sc = scope ? scope : as->findNearestScope();
    SymbolInfo* sym = sc ? sc->lookupSymbol(name) : nullptr;
    const char* what = nullptr;
    string detail;
    if (name == "$") {
        what = as->subs().empty() ? "rebind `$`" : "write field of `$`";
        detail = as->subs().empty() ? "$" : ("$." + as->subs().front().getText());
    } else if (sym && sym->kind == SymbolKind::Variable && !sym->isConst && !sym->moduleName.empty()) {
        what = as->subs().empty() ? "rebind global variable" : "write field of global variable";
        detail = name + (as->subs().empty() ? "" : ("." + as->subs().front().getText()));
    } else {
        if (as->subs().empty()) return;
        what = "write field of parameter";
        detail = name + "." + as->subs().front().getText();
    }
    throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E3110, _fnName, what, detail);
}

void ConstMutWalker::checkConstFnSet(StatementSetNode* st, ScopeNode* scope) {
    if (!_isConstFn) return;
    ExprNode* ae = st->arrayExpr();
    while (auto paren = dynamic_cast<ExprParenNode*>(ae))
        ae = paren->expr();
    string name;
    if (auto le = dynamic_cast<ExprLiteralNode*>(ae)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
            name = obj->getValue().getText();
        }
    }
    if (!name.empty() && _localNames.contains(name)) return;
    auto sc = scope ? scope : st->findNearestScope();
    SymbolInfo* sym = (sc && !name.empty()) ? sc->lookupSymbol(name) : nullptr;
    const char* what;
    string detail;
    if (name == "$") {
        what = "write element of `$`";
        detail = "$[i]";
    } else if (sym && sym->kind == SymbolKind::Variable && !sym->isConst && !sym->moduleName.empty()) {
        what = "write element of global variable";
        detail = (name.empty() ? "<expr>" : name) + "[i]";
    } else {
        what = "write element of parameter";
        detail = (name.empty() ? "<expr>" : name) + "[i]";
    }
    throw RiuError(st->getLineNumber(), st->getColumn(), ErrorCode::E3110, _fnName, what, detail);
}

FileNode* ConstMutWalker::fileOf(Node* n) const {
    Node* cur = n;
    while (cur) {
        if (auto* fl = dynamic_cast<FileNode*>(cur)) return fl;
        cur = cur->parent();
    }
    return _ctx.file;
}

FnSymbolInfo* ConstMutWalker::lookupFnSymbolCrossFile(FileNode* file, const std::string& fnName) const {
    if (!file) return nullptr;
    if (auto* fs = file->lookupFnSymbol(fnName)) return fs;
    for (auto* imp : file->wildcardImports()) {
        if (auto* fs = imp->lookupFnSymbol(fnName)) return fs;
    }
    return nullptr;
}

void ConstMutWalker::checkConstFnCall(ExprCallNode* call) {
    if (!_isConstFn) return;
    auto callee = call->getCalleeExpr();
    auto scope = call->findNearestScope();
    FileNode* file = fileOf(call);

    if (auto le = dynamic_cast<ExprLiteralNode*>(callee)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
            string fname = obj->getValue().getText();
            if (_localNames.contains(fname)) return;
            if (scope) {
                SymbolInfo* sym = scope->lookupSymbol(fname);
                if (sym && sym->kind == SymbolKind::Variable) return;
            }
            FnSymbolInfo* fs = lookupFnSymbolCrossFile(file, fname);
            if (!fs) return;
            if (fs->isConst) return;
            throw RiuError(call->resolveLineNumber(), call->resolveColumn(), ErrorCode::E3111, _fnName, fname);
        }
    }
    if (auto dot = dynamic_cast<ExprDotNode*>(callee)) {
        auto base = dot->baseExpr();
        string recvName;
        if (auto le = dynamic_cast<ExprLiteralNode*>(base)) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
                recvName = obj->getValue().getText();
            }
        }
        if (recvName.empty() || !scope) return;
        SymbolInfo* sym = scope->lookupSymbol(recvName);
        if (!sym) return;
        string typeName =
            sym->type.isRef() && sym->type.refElementType() ? sym->type.refElementType()->name : sym->type.name;
        if (typeName.empty()) return;
        string fullName = typeName + "." + dot->member();
        FnSymbolInfo* fs = lookupFnSymbolCrossFile(file, fullName);
        if (!fs) return;
        if (fs->isConst) return;
        throw RiuError(call->resolveLineNumber(), call->resolveColumn(), ErrorCode::E3111, _fnName, fullName);
    }
}

void ConstMutWalker::checkFieldWrite(StatementAssignNode* as, ScopeNode* scope) {
    if (_ctx.isDestructor) return;
    if (as->subs().empty()) return;
    auto sc = scope ? scope : as->findNearestScope();
    if (!sc) return;
    SymbolInfo* sym = sc->lookupSymbol(as->obj().getText());
    if (!sym) return;
    string typeName =
        sym->type.isRef() && sym->type.refElementType() ? sym->type.refElementType()->name : sym->type.name;
    StructDeclNode* sd = findStructDecl(_ctx.file, typeName);
    if (!sd) return;
    const string& fieldName = as->subs().front().getText();
    const StructFieldNode* fd = sd->field(fieldName);
    if (!fd) return;
    bool deep = as->subs().size() > 1;
    bool reject = deep ? fd->isFrozen() : (fd->isVal() || fd->isFrozen());
    if (!reject) return;
    const char* tag = fd->isFrozen() ? "Frozen" : "Val";
    throw RiuError(as->getLineNumber(), as->getColumn(), ErrorCode::E3109, fieldName, tag, sd->name().getText());
}

void ConstMutWalker::rejectIfDisallowedInConstFn(StatementNode* s) {
    if (!_isConstFn) return;
    auto throwE = [&](const char* what) {
        throw RiuError(s->getLineNumber(), s->getColumn(), ErrorCode::E3141, _fnName, what);
    };
    if (dynamic_cast<StatementLoopNode*>(s)) throwE("loop");
    if (dynamic_cast<StatementForInNode*>(s)) throwE("for-in");
    if (dynamic_cast<StatementBreakNode*>(s)) throwE("break");
    if (dynamic_cast<StatementContinueNode*>(s)) throwE("continue");
    if (dynamic_cast<StatementAssignNode*>(s)) return;
    if (dynamic_cast<StatementSetNode*>(s)) return;
    if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
        if (!da->isConst()) throwE("non-`#Cval` local `let`");
        return;
    }
    if (dynamic_cast<StatementDeclareAssignTupleNode*>(s)) throwE("tuple destructure `let`");
    if (auto dn = dynamic_cast<StatementDeclareNode*>(s)) {
        if (!dn->isConst()) throwE("uninitialized local `let`");
        return;
    }
    if (auto se = dynamic_cast<StatementExprNode*>(s)) {
        if (dynamic_cast<StatementRetNode*>(s)) return;
        ExprNode* e = se->expr();
        if (dynamic_cast<ExprMatchNode*>(e)) throwE("match expression");
        if (dynamic_cast<ExprTryCatchNode*>(e)) throwE("try-catch expression");
        throwE("expression statement");
    }
    if (dynamic_cast<StatementBlockNode*>(s)) throwE("nested block statement");
}

void ConstMutWalker::onStmt(StatementNode* s) {
    if (!s) return;

    rejectIfDisallowedInConstFn(s);

    if (auto dn = dynamic_cast<StatementDeclareNode*>(s)) {
        _localNames.insert(dn->name().getText());
        return;
    }
    if (auto da = dynamic_cast<StatementDeclareAssignNode*>(s)) {
        _localNames.insert(da->name().getText());
        auto scope = s->findNearestScope();
        if (da->isConst() && da->expr()) {
            requireConstExpr(da->expr(), scope);
        } else if (da->expr() && carriesFrozenTopLevel(da->expr(), scope)) {
            int line = s->getLineNumber();
            int col = s->getColumn();
            ExprNode* e = da->expr();
            while (auto paren = dynamic_cast<ExprParenNode*>(e))
                e = paren->expr();
            string srcName = "<frozen>";
            if (auto le = dynamic_cast<ExprLiteralNode*>(e)) {
                if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
                    srcName = obj->getValue().getText();
                }
            }
            throw RiuError(line, col, ErrorCode::E3107, srcName, da->name().getText());
        }
        return;
    }
    if (auto as = dynamic_cast<StatementAssignNode*>(s)) {
        auto scope = s->findNearestScope();
        if (!as->subs().empty()) {
            SymbolInfo* sym = lookupLhsSym(as, scope);
            if (sym && sym->isFrozen) {
                int line = s->getLineNumber();
                int col = s->getColumn();
                throw RiuError(line, col, ErrorCode::E3106, as->subs().front().getText(), as->obj().getText());
            }
            checkFieldWrite(as, scope);
            checkConstFnWrite(as, scope);
        } else {
            checkConstFnWrite(as, scope);
            SymbolInfo* sym = lookupLhsSym(as, scope);
            bool lhsFrozen = sym && sym->isFrozen;
            if (!lhsFrozen && as->expr() && carriesFrozenTopLevel(as->expr(), scope)) {
                int line = s->getLineNumber();
                int col = s->getColumn();
                ExprNode* e = as->expr();
                while (auto paren = dynamic_cast<ExprParenNode*>(e))
                    e = paren->expr();
                string srcName = "<frozen>";
                if (auto le = dynamic_cast<ExprLiteralNode*>(e)) {
                    if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
                        srcName = obj->getValue().getText();
                    }
                }
                throw RiuError(line, col, ErrorCode::E3107, srcName, as->obj().getText());
            }
        }
        return;
    }
    if (auto sfs = dynamic_cast<StatementStaticFieldSetNode*>(s)) {
        if (_isConstFn) {
            throw RiuError(s->getLineNumber(), s->getColumn(), ErrorCode::E3110, _fnName, "write static field",
                           sfs->typeName().getText() + "::" + sfs->fieldName().getText());
        }
        return;
    }
    if (auto st = dynamic_cast<StatementSetNode*>(s)) {
        auto scope = s->findNearestScope();
        if (isFrozenIdRef(st->arrayExpr(), scope)) {
            int line = s->getLineNumber();
            int col = s->getColumn();
            string objName = "<expr>";
            ExprNode* ae = st->arrayExpr();
            while (auto paren = dynamic_cast<ExprParenNode*>(ae))
                ae = paren->expr();
            if (auto le = dynamic_cast<ExprLiteralNode*>(ae)) {
                if (auto obj = dynamic_cast<LiteralObjNode*>(le->literal())) {
                    objName = obj->getValue().getText();
                }
            }
            throw RiuError(line, col, ErrorCode::E3106, "[i]", objName);
        }
        checkConstFnSet(st, scope);
        return;
    }
}

void ConstMutWalker::onExpr(ExprNode* e) {
    if (auto call = dynamic_cast<ExprCallNode*>(e)) {
        checkConstFnCall(call);
    }
}
