// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "anno_call.h"

#include "expr_node.h"
#include "literal_node.h"

#include <string_view>

namespace {

std::string_view compareOpText(ExprCompareNode::Op op) {
    switch (op) {
    case ExprCompareNode::Op::Eq:
        return "==";
    case ExprCompareNode::Op::Ne:
        return "!=";
    case ExprCompareNode::Op::Lt:
        return "<";
    case ExprCompareNode::Op::Le:
        return "<=";
    case ExprCompareNode::Op::Gt:
        return ">";
    case ExprCompareNode::Op::Ge:
        return ">=";
    case ExprCompareNode::Op::AndAnd:
        return "&&";
    case ExprCompareNode::Op::OrOr:
        return "||";
    }
    return {};
}

string unwrapQuotedAnnoText(string t) {
    if (t.size() >= 3 && t.front() == 'r' && t[1] == '"') t = t.substr(1);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

string literalAnnoText(LiteralNode* ln) {
    if (!ln) return {};
    if (auto* s = dynamic_cast<LiteralStringNode*>(ln)) return unwrapQuotedAnnoText(s->getValue().getText());
    if (auto* i = dynamic_cast<LiteralIntNode*>(ln)) return i->getValue().getText();
    if (auto* f = dynamic_cast<LiteralFloatNode*>(ln)) return f->getValue().getText();
    if (auto* b = dynamic_cast<LiteralBoolNode*>(ln)) return b->getValue().getText();
    if (auto* o = dynamic_cast<LiteralObjNode*>(ln)) return o->getValue().getText();
    return ln->getValue().getText();
}

string exprAnnoText(ExprNode* expr) {
    if (!expr) return {};
    if (auto* lit = dynamic_cast<ExprLiteralNode*>(expr)) return literalAnnoText(lit->literal());
    if (auto* cmp = dynamic_cast<ExprCompareNode*>(expr)) {
        return exprAnnoText(cmp->left()) + string(compareOpText(cmp->op())) + exprAnnoText(cmp->right());
    }
    if (auto* arr = dynamic_cast<ExprArrayNode*>(expr)) {
        string out = "[";
        for (size_t i = 0; i < arr->elements().size(); ++i) {
            if (i > 0) out += ", ";
            out += exprAnnoText(arr->elements()[i]);
        }
        out += ']';
        return out;
    }
    if (auto* path = dynamic_cast<ExprPathCallNode*>(expr)) {
        string out = path->lhsPath().dotted() + "::" + path->variantName().getText();
        if (!path->args().empty()) {
            out += '(';
            for (size_t i = 0; i < path->args().size(); ++i) {
                if (i > 0) out += ", ";
                out += exprAnnoText(path->args()[i]);
            }
            out += ')';
        }
        return out;
    }
    return {};
}

} // namespace

string annoArgText(const AnnoArg& arg) {
    if (arg.expr) {
        string fromExpr = exprAnnoText(arg.expr);
        if (!fromExpr.empty()) return fromExpr;
    }
    return unwrapQuotedAnnoText(arg.text);
}

string annoCallFirstArgText(const AnnoCall& call) {
    if (call.args.empty()) return {};
    return annoArgText(call.args[0]);
}
