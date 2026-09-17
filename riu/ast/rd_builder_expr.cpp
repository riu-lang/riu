// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "rd_builder.h"

#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "node/type_node.h"

namespace {

string lastSeg(std::string_view dotted) {
    auto pos = dotted.rfind('.');
    if (pos == std::string_view::npos) return string(dotted);
    return string(dotted.substr(pos + 1));
}

bool isTypeKindExpr(rd::NodeKind k) {
    switch (k) {
    case rd::NodeKind::TypePath:
    case rd::NodeKind::TypeGeneric:
    case rd::NodeKind::TypeNullable:
    case rd::NodeKind::TypeFallible:
    case rd::NodeKind::TypeSelf:
    case rd::NodeKind::TypeArray:
    case rd::NodeKind::TypeUnit:
    case rd::NodeKind::TypeTuple:
        return true;
    default:
        return false;
    }
}

void appendUtf8(string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0u | (cp >> 6u));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0u | (cp >> 12u));
        out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xF0u | (cp >> 18u));
        out += static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu));
        out += static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

u32 parseHex(const string& s, size_t pos, size_t n) {
    u32 v = 0;
    for (size_t k = 0; k < n; ++k) {
        char c = s[pos + k];
        v <<= 4u;
        if (c >= '0' && c <= '9')
            v |= static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= static_cast<u32>(c - 'a' + 10);
        else
            v |= static_cast<u32>(c - 'A' + 10);
    }
    return v;
}

int utf8CodepointsBefore(const string& s, size_t byteEnd) {
    int n = 0;
    for (size_t j = 0; j < byteEnd && j < s.size();) {
        auto c = static_cast<unsigned char>(s[j]);
        if ((c & 0xE0u) == 0xC0u)
            j += 2;
        else if ((c & 0xF0u) == 0xE0u)
            j += 3;
        else if ((c & 0xF8u) == 0xF0u)
            j += 4;
        else
            ++j;
        ++n;
    }
    return n;
}

[[noreturn]] void throwBadEscape(size_t line, int startCol, const string& raw, size_t i, const string& seq) {
    throw RiuError(static_cast<int>(line), startCol + utf8CodepointsBefore(raw, i), ErrorCode::E2033, seq);
}

void decodeTplText(const string& raw, string& out, size_t line, int startCol) {
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] != '\\') {
            out += raw[i++];
            continue;
        }
        if (i + 1 >= raw.size()) throwBadEscape(line, startCol, raw, i, "\\");
        char esc = raw[i + 1];
        switch (esc) {
        case 'n':
            out += '\n';
            i += 2;
            break;
        case 'r':
            out += '\r';
            i += 2;
            break;
        case 't':
            out += '\t';
            i += 2;
            break;
        case 'v':
            out += '\v';
            i += 2;
            break;
        case 'b':
            out += '\b';
            i += 2;
            break;
        case '0':
            out += '\0';
            i += 2;
            break;
        case '\\':
            out += '\\';
            i += 2;
            break;
        case '"':
            out += '"';
            i += 2;
            break;
        case '\'':
            out += '\'';
            i += 2;
            break;
        case '$':
            out += '$';
            i += 2;
            break;
        case 'x': {
            if (i + 3 >= raw.size() || !isHexDigit(raw[i + 2]) || !isHexDigit(raw[i + 3])) {
                throwBadEscape(line, startCol, raw, i, raw.substr(i, std::min<size_t>(raw.size() - i, 4)));
            }
            out += static_cast<char>(parseHex(raw, i + 2, 2));
            i += 4;
            break;
        }
        case 'u': {
            if (i + 5 >= raw.size() || !isHexDigit(raw[i + 2]) || !isHexDigit(raw[i + 3]) || !isHexDigit(raw[i + 4]) ||
                !isHexDigit(raw[i + 5])) {
                throwBadEscape(line, startCol, raw, i, raw.substr(i, std::min<size_t>(raw.size() - i, 6)));
            }
            appendUtf8(out, parseHex(raw, i + 2, 4));
            i += 6;
            break;
        }
        default:
            throwBadEscape(line, startCol, raw, i, string{'\\', esc});
        }
    }
}

class LambdaScopeNode : public ScopeNode {
public:
    explicit LambdaScopeNode(Node* parent) : ScopeNode(parent) {}
};

} // namespace

ExprNode* RdBuilder::wrapLiteral(LiteralNode* lit, rd::NodeId id) {
    return static_cast<ExprNode*>(create<ExprLiteralNode>(id, currentScope(), lit));
}

ExprNode* RdBuilder::identExpr(rd::NodeId id) {
    auto* obj = static_cast<LiteralNode*>(create<LiteralObjNode>(id, currentScope(), makeTok(id)));
    return wrapLiteral(obj, id);
}

LiteralNode* RdBuilder::buildLiteral(rd::NodeId id) {
    const auto& n = at(id);
    switch (n.kind) {
    case rd::NodeKind::IntLit:
        return static_cast<LiteralNode*>(create<LiteralIntNode>(id, makeTok(id)));
    case rd::NodeKind::FloatLit:
        return static_cast<LiteralNode*>(create<LiteralFloatNode>(id, makeTok(id)));
    case rd::NodeKind::BoolLit:
        return static_cast<LiteralNode*>(create<LiteralBoolNode>(id, makeTok(id)));
    case rd::NodeKind::NullLit:
        return static_cast<LiteralNode*>(create<LiteralNullNode>(id, makeTok(id)));
    case rd::NodeKind::CodePoint:
        return static_cast<LiteralNode*>(create<LiteralCodePointNode>(id, makeTok(id)));
    case rd::NodeKind::StringLit: {
        bool raw = !n.value.empty() && n.value.size() >= 2 && n.value[0] == 'r';
        if (!raw) {
            string decoded;
            decodeTplText(string(n.value), decoded, n.pos.line, n.pos.column + 1);
            Token synTok("\"" + decoded + "\"", static_cast<size_t>(n.pos.line));
            return static_cast<LiteralNode*>(create<LiteralStringNode>(id, synTok, true));
        }
        return static_cast<LiteralNode*>(create<LiteralStringNode>(id, makeTok(id), true));
    }
    case rd::NodeKind::UnitLit:
        return nullptr;
    default:
        return nullptr;
    }
}

vector<LambdaParamSlot> RdBuilder::lambdaParamsFromKids(rd::NodeId parent, rd::i32 from, rd::i32 to) {
    vector<LambdaParamSlot> out;
    const auto& n = at(parent);
    if (to < 0) to = n.children_count;
    for (rd::i32 i = from; i < to; ++i) {
        rd::NodeId p = child(parent, i);
        if (at(p).kind != rd::NodeKind::Param) break;
        TypeNode* ty = nullptr;
        for (rd::i32 k = 0; k < at(p).children_count; ++k) {
            if (isTypeKindExpr(at(child(p, k)).kind)) ty = buildType(child(p, k));
        }
        out.push_back(LambdaParamSlot{.name = makeTok(p), .type = ty});
    }
    return out;
}

LambdaExprNode* RdBuilder::buildLambda(rd::NodeId id, bool /*trailing*/) {
    auto* scope = currentScope();
    const auto& n = at(id);
    rd::i32 i = 0;
    vector<LambdaParamSlot> params = lambdaParamsFromKids(id, 0, n.children_count);
    while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Param)
        ++i;
    TypeNode* retType = nullptr;
    TypeNode* retFallibleFromType = nullptr;
    if (i < n.children_count && isTypeKindExpr(at(child(id, i)).kind)) {
        auto parsed = buildType(child(id, i));
        std::tie(retType, retFallibleFromType) = peelFallibleRetType(parsed);
        ++i;
    }
    auto* bodyScope = static_cast<LambdaScopeNode*>(new LambdaScopeNode(scope));
    _nodes.push_back(bodyScope);
    bodyScope->setParentScope(scope);
    for (auto& slot : params) {
        TypeInfo t = slot.type ? slot.type->getType() : TypeInfo();
        bodyScope->registerSymbol(slot.name.getText(), {SymbolKind::Variable, slot.name.getText(), t});
    }
    _scopeStack.push_back(bodyScope);
    LambdaExprNode* node = nullptr;
    if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Block) {
        auto* blk = buildBlock(child(id, i), bodyScope, false);
        node = create<LambdaExprNode>(id, scope, LambdaExprNode::Form::Block, std::move(params), retType, nullptr,
                                      blk->statements());
        if (blk->hasResult()) {
            // 块值已在 statements 里按 ASTBuilder 规则抽出；lambda block 体保留 statements
        }
    } else if (i < n.children_count) {
        auto* bodyExpr = buildExpr(child(id, i));
        node = create<LambdaExprNode>(id, scope, LambdaExprNode::Form::Expr, std::move(params), retType, bodyExpr,
                                      vector<StatementNode*>{});
    } else {
        node = create<LambdaExprNode>(id, scope, LambdaExprNode::Form::Expr, std::move(params), retType, nullptr,
                                      vector<StatementNode*>{});
    }
    _scopeStack.pop_back();
    if (retFallibleFromType) {
        node->setFallibleErrType(retFallibleFromType);
        if (retType) {
            const string err = fallibleErrKey(retFallibleFromType->getType());
            if (retType->getType().getFullName() == err) {
                throw RiuError(node->getLineNumber(), node->getColumn(), ErrorCode::E7008,
                               retType->getType().getFullName(), err);
            }
        }
    }
    node->setBodyScope(bodyScope);
    return node;
}

ExprNode* RdBuilder::buildExpr(rd::NodeId id) {
    if (id == rd::kEmptyNode) return nullptr;
    const auto& n = at(id);
    auto* scope = currentScope();

    switch (n.kind) {
    case rd::NodeKind::Ident:
        return identExpr(id);
    case rd::NodeKind::This: {
        auto* obj = static_cast<LiteralNode*>(create<LiteralObjNode>(id, scope, Token("$", n.pos.line)));
        return wrapLiteral(obj, id);
    }
    case rd::NodeKind::TypeSelf: {
        auto* obj = static_cast<LiteralNode*>(create<LiteralObjNode>(id, scope, makeTok("Self", n.pos)));
        return wrapLiteral(obj, id);
    }
    case rd::NodeKind::IntLit:
    case rd::NodeKind::FloatLit:
    case rd::NodeKind::BoolLit:
    case rd::NodeKind::NullLit:
    case rd::NodeKind::CodePoint:
    case rd::NodeKind::StringLit:
        return wrapLiteral(buildLiteral(id), id);
    case rd::NodeKind::UnitLit:
        return static_cast<ExprNode*>(create<ExprTupleNode>(id, scope, vector<ExprNode*>{}));
    case rd::NodeKind::StringInterp: {
        vector<string> parts;
        vector<ExprNode*> interps;
        string current;
        auto flush = [&]() {
            parts.push_back(std::move(current));
            current.clear();
        };
        for (rd::i32 i = 0; i < n.children_count; ++i) {
            rd::NodeId p = child(id, i);
            const auto& pn = at(p);
            if (pn.kind == rd::NodeKind::TplText) {
                decodeTplText(string(pn.value), current, pn.pos.line, pn.pos.column + 1);
            } else if (pn.kind == rd::NodeKind::TplDollar) {
                flush();
                string text = string(pn.value);
                if (!text.empty() && text[0] == '$') text = text.substr(1);
                Token synTok(text, pn.pos.line);
                auto* obj = static_cast<LiteralNode*>(create<LiteralObjNode>(p, scope, synTok));
                interps.push_back(wrapLiteral(obj, p));
            } else if (pn.kind == rd::NodeKind::TplInterp) {
                flush();
                if (pn.children_count > 0) interps.push_back(buildExpr(child(p, 0)));
            }
        }
        flush();
        if (interps.empty()) {
            const string& body = parts.empty() ? string() : parts[0];
            Token synTok("\"" + body + "\"", static_cast<size_t>(n.pos.line));
            return wrapLiteral(static_cast<LiteralNode*>(create<LiteralStringNode>(id, synTok, true)), id);
        }
        auto* tpl = static_cast<LiteralNode*>(
            create<StringTemplateNode>(id, makeTok("\"", n.pos), std::move(parts), std::move(interps)));
        return wrapLiteral(tpl, id);
    }
    case rd::NodeKind::Paren:
        return static_cast<ExprNode*>(create<ExprParenNode>(id, scope, buildExpr(child(id, 0))));
    case rd::NodeKind::Prefix: {
        auto* right = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto op = n.op == rd::Kind::SymbolSub ? ExprUnaryNode::Op::Neg : ExprUnaryNode::Op::Not;
        return static_cast<ExprNode*>(create<ExprUnaryNode>(id, scope, op, right));
    }
    case rd::NodeKind::Infix: {
        auto* left = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto* right = n.children_count > 1 ? buildExpr(child(id, 1)) : nullptr;
        switch (n.op) {
        case rd::Kind::SymbolAdd:
            return static_cast<ExprNode*>(create<ExprAddSubNode>(id, scope, ExprAddSubNode::Op::Add, left, right));
        case rd::Kind::SymbolSub:
            return static_cast<ExprNode*>(create<ExprAddSubNode>(id, scope, ExprAddSubNode::Op::Sub, left, right));
        case rd::Kind::SymbolMul:
            return static_cast<ExprNode*>(
                create<ExprMulDivModNode>(id, scope, ExprMulDivModNode::Op::Mul, left, right));
        case rd::Kind::SymbolDiv:
            return static_cast<ExprNode*>(
                create<ExprMulDivModNode>(id, scope, ExprMulDivModNode::Op::Div, left, right));
        case rd::Kind::SymbolMod:
            return static_cast<ExprNode*>(
                create<ExprMulDivModNode>(id, scope, ExprMulDivModNode::Op::Mod, left, right));
        case rd::Kind::SymbolEqEq:
            return static_cast<ExprNode*>(create<ExprCompareNode>(id, scope, ExprCompareNode::Op::Eq, left, right));
        case rd::Kind::SymbolExclEq:
            return static_cast<ExprNode*>(create<ExprCompareNode>(id, scope, ExprCompareNode::Op::Ne, left, right));
        case rd::Kind::SymbolLt:
            return static_cast<ExprNode*>(create<ExprCompareNode>(
                id, scope, n.value == "<=" ? ExprCompareNode::Op::Le : ExprCompareNode::Op::Lt, left, right));
        case rd::Kind::SymbolMt:
            return static_cast<ExprNode*>(create<ExprCompareNode>(
                id, scope, n.value == ">=" ? ExprCompareNode::Op::Ge : ExprCompareNode::Op::Gt, left, right));
        case rd::Kind::SymbolAndAnd:
            return static_cast<ExprNode*>(create<ExprCompareNode>(id, scope, ExprCompareNode::Op::AndAnd, left, right));
        case rd::Kind::SymbolOrOr:
            return static_cast<ExprNode*>(create<ExprCompareNode>(id, scope, ExprCompareNode::Op::OrOr, left, right));
        case rd::Kind::SymbolLtSub:
            return static_cast<ExprNode*>(create<ExprMoveAssignNode>(id, scope, left, right));
        default:
            return left;
        }
    }
    case rd::NodeKind::NullElse: {
        auto* left = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto* right = n.children_count > 1 ? buildExpr(child(id, 1)) : nullptr;
        return static_cast<ExprNode*>(create<ExprNullElseNode>(id, scope, left, right));
    }
    case rd::NodeKind::Call: {
        ExprNode* callee = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto* call = create<ExprCallNode>(id, scope, callee);
        rd::i32 i = 1;
        if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
            vector<TypeNode*> targs;
            const auto& g = at(child(id, i));
            targs.reserve(g.children_count);
            for (rd::i32 k = 0; k < g.children_count; ++k)
                targs.push_back(buildType(child(child(id, i), k)));
            call->setTypeArgs(std::move(targs));
            ++i;
        }
        bool trailing = false;
        for (; i < n.children_count; ++i) {
            if (at(child(id, i)).kind == rd::NodeKind::Lambda) {
                call->addArg(static_cast<ExprNode*>(buildLambda(child(id, i), true)));
                trailing = true;
            } else {
                call->addArg(buildExpr(child(id, i)));
            }
        }
        if (trailing) call->setTrailingLambda(true);
        if (n.op == rd::Kind::SymbolExcl) call->setErrPropagate(true);
        if (!call->errPropagate() && call->getArgs().size() == 1 && call->getTypeArgs().size() == 1) {
            if (auto* calleeLit = dynamic_cast<ExprLiteralNode*>(call->getCalleeExpr())) {
                if (auto* obj = dynamic_cast<LiteralObjNode*>(calleeLit->literal())) {
                    if (obj->getValue().getText() == "Dyn") {
                        auto* specTypeNode = call->getTypeArgs()[0];
                        bool isBorrow = specTypeNode->getType().isRef();
                        return static_cast<ExprNode*>(
                            create<ExprDynCtorNode>(id, scope, specTypeNode, call->getArgs()[0], isBorrow));
                    }
                }
            }
        }
        return static_cast<ExprNode*>(call);
    }
    case rd::NodeKind::Dot: {
        auto* base = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        bool safe = n.op == rd::Kind::SymbolQuest;
        string specQual;
        if (n.children_count > 1 && at(child(id, 1)).kind == rd::NodeKind::Ident)
            specQual = string(at(child(id, 1)).value);
        return static_cast<ExprNode*>(create<ExprDotNode>(id, scope, base, makeTok(id), safe, std::move(specQual)));
    }
    case rd::NodeKind::TupleMember: {
        auto* base = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        return static_cast<ExprNode*>(create<ExprDotNode>(id, scope, base, makeTok(id), false));
    }
    case rd::NodeKind::Get: {
        auto* arr = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        vector<ExprNode*> idx;
        for (rd::i32 i = 1; i < n.children_count; ++i)
            idx.push_back(buildExpr(child(id, i)));
        return static_cast<ExprNode*>(create<ExprGetNode>(id, scope, arr, std::move(idx)));
    }
    case rd::NodeKind::GetRef: {
        Token obj;
        vector<Token> subs;
        if (n.children_count > 0) {
            const auto& first = at(child(id, 0));
            obj = first.kind == rd::NodeKind::This ? Token("$", first.pos.line) : makeTok(child(id, 0));
        }
        for (rd::i32 i = 1; i < n.children_count; ++i)
            subs.push_back(makeTok(child(id, i)));
        return static_cast<ExprNode*>(create<ExprGetRefNode>(id, scope, obj, std::move(subs)));
    }
    case rd::NodeKind::Array: {
        vector<ExprNode*> elems;
        elems.reserve(n.children_count);
        for (rd::i32 i = 0; i < n.children_count; ++i)
            elems.push_back(buildExpr(child(id, i)));
        return static_cast<ExprNode*>(create<ExprArrayNode>(id, scope, std::move(elems)));
    }
    case rd::NodeKind::ArrayInit: {
        LiteralNode* lit = nullptr;
        TypeNode* ty = nullptr;
        if (n.children_count > 0) {
            if (at(child(id, 0)).kind == rd::NodeKind::Ident) {
                auto* obj =
                    static_cast<LiteralNode*>(create<LiteralObjNode>(child(id, 0), scope, makeTok(child(id, 0))));
                lit = obj;
            } else {
                lit = buildLiteral(child(id, 0));
            }
        }
        if (n.children_count > 1 && isTypeKindExpr(at(child(id, 1)).kind)) ty = buildType(child(id, 1));
        return static_cast<ExprNode*>(create<ExprArrayInitNode>(id, scope, lit, ty));
    }
    case rd::NodeKind::Tuple: {
        vector<ExprNode*> elems;
        elems.reserve(n.children_count);
        for (rd::i32 i = 0; i < n.children_count; ++i)
            elems.push_back(buildExpr(child(id, i)));
        return static_cast<ExprNode*>(create<ExprTupleNode>(id, scope, std::move(elems)));
    }
    case rd::NodeKind::Lambda:
        return static_cast<ExprNode*>(buildLambda(id, false));
    case rd::NodeKind::IfLine: {
        auto* cond = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        auto* t = n.children_count > 1 ? buildExpr(child(id, 1)) : nullptr;
        auto* f = n.children_count > 2 ? buildExpr(child(id, 2)) : nullptr;
        return static_cast<ExprNode*>(create<ExprOneLineIfElseNode>(id, scope, cond, t, f));
    }
    case rd::NodeKind::If: {
        auto* cond = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        StatementBlockNode* thenBlk = nullptr;
        vector<ExprElIfNode*> elifs;
        StatementBlockNode* elseBlk = nullptr;
        rd::i32 i = 1;
        if (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Block) {
            thenBlk = buildBlock(child(id, i), scope);
            ++i;
        }
        for (; i < n.children_count; ++i) {
            rd::NodeId c = child(id, i);
            if (at(c).kind == rd::NodeKind::Elif) {
                auto* econd = at(c).children_count > 0 ? buildExpr(child(c, 0)) : nullptr;
                auto* ebody = at(c).children_count > 1 ? buildBlock(child(c, 1), scope) : nullptr;
                elifs.push_back(create<ExprElIfNode>(c, scope, econd, ebody));
            } else if (at(c).kind == rd::NodeKind::Block) {
                elseBlk = buildBlock(c, scope);
            }
        }
        return static_cast<ExprNode*>(create<ExprIfElseNode>(id, scope, cond, thenBlk, elifs, elseBlk));
    }
    case rd::NodeKind::Match: {
        auto* scrut = n.children_count > 0 ? buildExpr(child(id, 0)) : nullptr;
        vector<MatchArmNode*> arms;
        for (rd::i32 i = 1; i < n.children_count; ++i) {
            rd::NodeId armId = child(id, i);
            if (at(armId).kind != rd::NodeKind::MatchArm) continue;
            rd::NodeId patId = at(armId).children_count > 0 ? child(armId, 0) : rd::kEmptyNode;
            EnumPatternNode* pattern = nullptr;
            if (patId != rd::kEmptyNode) {
                const auto& pn = at(patId);
                if (pn.value == "else") {
                    pattern = create<EnumPatternNode>(patId, scope, makeTok(patId));
                } else {
                    TypePath path;
                    vector<Token> binds;
                    rd::i32 pi = 0;
                    if (pi < pn.children_count && at(child(patId, pi)).kind == rd::NodeKind::TypePath) {
                        path = pathFromDotted(at(child(patId, pi)).value, at(child(patId, pi)).pos);
                        ++pi;
                    }
                    for (; pi < pn.children_count; ++pi)
                        binds.push_back(makeTok(child(patId, pi)));
                    Token enumTok = path.empty() ? makeTok(patId) : path.last();
                    pattern = create<EnumPatternNode>(patId, scope, enumTok, makeTok(patId), std::move(binds));
                    if (!path.empty()) pattern->setEnumPath(std::move(path));
                }
            }
            auto* arm = create<MatchArmNode>(armId, scope, pattern, static_cast<ExprNode*>(nullptr));
            arm->setParentScope(scope);
            if (pattern && !pattern->isElse()) {
                for (auto& tk : pattern->binds()) {
                    const string& bn = tk.getText();
                    arm->registerSymbol(bn, {SymbolKind::Variable, bn, TypeInfo(), false});
                }
            }
            _scopeStack.push_back(arm);
            if (at(armId).children_count > 1) {
                rd::NodeId body = child(armId, 1);
                if (at(body).kind == rd::NodeKind::Block)
                    arm->setBlock(buildBlock(body, arm));
                else
                    arm->setBody(buildExpr(body));
            }
            _scopeStack.pop_back();
            arms.push_back(arm);
        }
        return static_cast<ExprNode*>(create<ExprMatchNode>(id, scope, scrut, std::move(arms)));
    }
    case rd::NodeKind::TryCatch: {
        StatementBlockNode* tryBlk = nullptr;
        vector<CatchArmNode*> catches;
        if (n.children_count > 0 && at(child(id, 0)).kind == rd::NodeKind::Block)
            tryBlk = buildBlock(child(id, 0), scope);
        for (rd::i32 i = 1; i < n.children_count; ++i) {
            rd::NodeId c = child(id, i);
            if (at(c).kind != rd::NodeKind::Catch) continue;
            TypeNode* ty = at(c).children_count > 0 ? buildType(child(c, 0)) : nullptr;
            TypeInfo errType = ty ? ty->getType() : TypeInfo();
            Token errName = makeTok(c);
            auto* placeholder =
                create<CatchArmNode>(c, scope, errName, errType, static_cast<StatementBlockNode*>(nullptr));
            placeholder->setParentScope(scope);
            placeholder->registerSymbol(errName.getText(), {SymbolKind::Variable, errName.getText(), errType, false});
            _scopeStack.push_back(placeholder);
            StatementBlockNode* body = nullptr;
            if (at(c).children_count > 1) body = buildBlock(child(c, 1), placeholder);
            _scopeStack.pop_back();
            auto* full = create<CatchArmNode>(c, scope, errName, errType, body);
            full->setParentScope(scope);
            for (auto& [nm, sym] : placeholder->localSymbols())
                full->registerSymbol(nm, sym);
            catches.push_back(full);
        }
        return static_cast<ExprNode*>(create<ExprTryCatchNode>(id, scope, tryBlk, std::move(catches)));
    }
    case rd::NodeKind::EnumCtor: {
        rd::i32 i = 0;
        TypePath lhsPath;
        Token lhsTok;
        if (n.children_count > 0) {
            const auto& lhs = at(child(id, 0));
            if (lhs.kind == rd::NodeKind::TypeSelf) {
                lhsTok = makeTok("Self", lhs.pos);
                lhsPath = TypePath(lhsTok);
            } else {
                lhsPath = pathFromDotted(lhs.value, lhs.pos);
                lhsTok = lhsPath.empty() ? makeTok(child(id, 0)) : lhsPath.last();
            }
            ++i;
        }
        auto* node = create<ExprPathCallNode>(id, scope, lhsTok, makeTok(id));
        node->setLhsPath(std::move(lhsPath));
        if (n.op == rd::Kind::SymbolExcl) node->setErrPropagate(true);
        while (i < n.children_count && at(child(id, i)).kind == rd::NodeKind::Generic) {
            vector<TypeNode*> targs;
            const auto& g = at(child(id, i));
            targs.reserve(g.children_count);
            for (rd::i32 k = 0; k < g.children_count; ++k)
                targs.push_back(buildType(child(child(id, i), k)));
            if (g.value == "rhs")
                node->setRhsTypeArgs(std::move(targs));
            else
                node->setLhsTypeArgs(std::move(targs));
            ++i;
        }
        bool hasParens = false;
        for (; i < n.children_count; ++i) {
            hasParens = true;
            node->addArg(buildExpr(child(id, i)));
        }
        // 有括号或零参带 ()：parser 在 ParStart 时才 parseArgList。无孩子 args 且源上有 () 难区分。
        // 有 args 一定有括号；无 args 保持 false（E::V 与 E::V() 语义等价）。
        if (hasParens) node->setHasParens(true);
        return static_cast<ExprNode*>(node);
    }
    case rd::NodeKind::StructLit: {
        bool isSelfForm = n.children_count > 0 && at(child(id, 0)).kind == rd::NodeKind::TypeSelf;
        Token leadTk;
        string structName;
        TypePath path;
        if (n.children_count > 0) {
            if (isSelfForm) {
                leadTk = makeTok("Self", at(child(id, 0)).pos);
                structName = findEnclosingStructName();
            } else {
                path = pathFromDotted(at(child(id, 0)).value, at(child(id, 0)).pos);
                leadTk = path.empty() ? makeTok(child(id, 0)) : path.last();
                structName = path.lastName();
            }
        }
        auto* node = create<ExprStructLitNode>(id, scope, leadTk, structName, isSelfForm);
        if (!isSelfForm) node->setTypePath(std::move(path));
        for (rd::i32 i = 1; i < n.children_count; ++i) {
            rd::NodeId c = child(id, i);
            if (at(c).kind == rd::NodeKind::FieldInit) {
                ExprNode* v = at(c).children_count > 0 ? buildExpr(child(c, 0)) : nullptr;
                node->addField(create<FieldInitNode>(c, scope, makeTok(c), v));
            } else {
                node->setPositional(buildExpr(c));
            }
        }
        return static_cast<ExprNode*>(node);
    }
    case rd::NodeKind::Block:
        // 表达式位的块少见；当语句块值处理时走 buildBlock。
        return nullptr;
    default:
        if (isTypeKindExpr(n.kind)) {
            // TypePath 当表达式：当成 Ident 路径末段
            auto* obj = static_cast<LiteralNode*>(create<LiteralObjNode>(id, scope, makeTok(lastSeg(n.value), n.pos)));
            return wrapLiteral(obj, id);
        }
        return nullptr;
    }
}
