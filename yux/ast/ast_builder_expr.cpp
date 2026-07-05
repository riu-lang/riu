// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 表达式 / 字面量族实现：
//   - 调用 / lambda / 算术 / 比较 / 位运算 / 移位
//   - 字面量 (Number / Bool / Null / Obj / String / StringTpl / CodePoint / Num)
//   - 控制流 (IfElse / OneLineIfElse / IfElsePreValue / ElIf / Else / Match / TryCatch)
//   - 集合访问 / 构造 (Get / GetRef / Array / Tuple / TupleMember / ArrayInit / StructLit / FieldInit)
//   - 枚举 / 一元 / NullElse / $（This）
//   - match / catch arm 与 Pattern
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "types.h"
#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include <algorithm>
#include <functional>

std::any ASTBuilder::visitExprParen(yux::yuxParser::ExprParenContext* ctx) {
    DEBUG_LOG("    Expr: Paren");
    auto scope = currentScope();
    auto inner = any_cast_p<ExprNode>(visit(ctx->expr()));
    return static_cast<p<ExprNode>>(createWithLine<ExprParenNode>(ctx, scope, inner));
}

namespace {

// 收集 lambdaParams 上下文为 LambdaParamSlot 列表
// g4 lambdaParam 两 alt：
//   - lambdaParamGroup：names+= ID (',' names+= ID)+ typeWithRef?  → 组糖，N 形参共享同类型
//   - lambdaParamStd：name=ID typeWithRef?                          → 单形参可省类型
// 类型省时 type=nullptr，由调用 / 赋值点的 fn 类型反推（Phase 2b）
vector<LambdaParamSlot>
collectLambdaParams(ASTBuilder* self, yux::yuxParser::LambdaParamsContext* params, p<Node> parent,
                    p<TypeNode> (ASTBuilder::*buildTwr)(yux::yuxParser::TypeWithRefContext*, p<Node>)) {
    vector<LambdaParamSlot> out;
    if (!params) return out;
    for (auto* lp : params->lambdaParam()) {
        if (auto* g = dynamic_cast<yux::yuxParser::LambdaParamGroupContext*>(lp)) {
            // a, b T → 展开为 N 份相同类型；类型可省（→ nullptr）
            p<TypeNode> sharedType = nullptr;
            if (auto* twr = g->typeWithRef()) {
                sharedType = (self->*buildTwr)(twr, parent);
            }
            for (auto* idTok : g->names) {
                out.push_back(LambdaParamSlot{.name = Token(idTok->getText(), static_cast<int>(idTok->getLine())),
                                              .type = sharedType});
            }
        } else if (auto* s = dynamic_cast<yux::yuxParser::LambdaParamStdContext*>(lp)) {
            p<TypeNode> ty = nullptr;
            if (auto* twr = s->typeWithRef()) {
                ty = (self->*buildTwr)(twr, parent);
            }
            out.push_back(
                LambdaParamSlot{.name = Token(s->name->getText(), static_cast<int>(s->name->getLine())), .type = ty});
        }
    }
    return out;
}

// 把 trailingLambda 上下文转为 LambdaExprNode（块形）
// trailingLambdaBlock：'{' lambdaParams '=>' stmts '}'  → Form::Block
// trailingLambdaZeroBlock：'{' stmts '}'                → Form::ZeroBlock
p<LambdaExprNode>
buildTrailingLambda(ASTBuilder* self, yux::yuxParser::TrailingLambdaContext* tl, p<ScopeNode> scope,
                    p<TypeNode> (ASTBuilder::*buildTwr)(yux::yuxParser::TypeWithRefContext*, p<Node>),
                    const std::function<p<LambdaExprNode>(yux::yuxParser::TrailingLambdaContext*, LambdaExprNode::Form,
                                                          vector<LambdaParamSlot>, vector<p<StatementNode>>)>& create) {
    if (auto* b = dynamic_cast<yux::yuxParser::TrailingLambdaBlockContext*>(tl)) {
        auto params = collectLambdaParams(self, b->lambdaParams(), scope, buildTwr);
        vector<p<StatementNode>> stmts;
        for (auto* s : b->statement()) {
            stmts.push_back(any_cast_p<StatementNode>(self->visit(s)));
        }
        return create(b, LambdaExprNode::Form::Block, std::move(params), std::move(stmts));
    } else if (auto* z = dynamic_cast<yux::yuxParser::TrailingLambdaZeroBlockContext*>(tl)) {
        vector<p<StatementNode>> stmts;
        for (auto* s : z->statement()) {
            stmts.push_back(any_cast_p<StatementNode>(self->visit(s)));
        }
        return create(z, LambdaExprNode::Form::ZeroBlock, {}, std::move(stmts));
    }
    return nullptr;
}

} // namespace

std::any ASTBuilder::visitExprCall(yux::yuxParser::ExprCallContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG_VAL("    Expr: Call - ctx->left type", typeid(*ctx->left).name());
    auto callee = any_cast_p<ExprNode>(visit(ctx->left));
    DEBUG_LOG_VAL("    Expr: Call - callee type", typeid(*callee).name());

    auto call = createWithLine<ExprCallNode>(ctx, scope, callee);
    DEBUG_LOG_VAL("    Expr: Call", "args count: " << ctx->args.size());
    for (auto arg : ctx->args) {
        call->addArg(any_cast_p<ExprNode>(visit(arg)));
    }
    if (auto gd = ctx->genericDef()) {
        vector<p<TypeNode>> typeArgs;
        for (auto pCtx : gd->params) {
            // turbofish 不允许 bound（spec §6.4.4.3）
            if (!pCtx->bounds.empty()) {
                auto* tk = pCtx->SymbolColon();
                throw YuxError(tk ? static_cast<int>(tk->getSymbol()->getLine()) : 0,
                               tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                               ErrorCode::E2015);
            }
            typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
        }
        call->setTypeArgs(std::move(typeArgs));
    }
    // 尾随 lambda 糖 §4.5：f(args) { ... } → 等价 f(args, { ... })
    if (auto* tl = ctx->trailing) {
        auto lambda = buildTrailingLambda(this, tl, scope, &ASTBuilder::buildTypeWithRef,
                                          [this](auto* tlCtx, LambdaExprNode::Form form, vector<LambdaParamSlot> params,
                                                 vector<p<StatementNode>> stmts) {
                                              return createWithLine<LambdaExprNode>(tlCtx, currentScope(), form,
                                                                                    std::move(params), nullptr, nullptr,
                                                                                    std::move(stmts));
                                          });
        if (lambda) call->addArg(static_cast<p<ExprNode>>(lambda));
    }
    // Phase 10e：后缀 `!` 错误传播标记（DRAFT-错误.md [#4.B]）
    if (ctx->errPropagate) call->setErrPropagate(true);

    // Dyn<D>(x) 类型构造（DRAFT-dyn-draft / 拟 §12.9）—— 单点拦截 ExprCallNode 重写为 ExprDynCtorNode
    // 命中条件：callee = LiteralObj("Dyn") + 恰好 1 个 typeArg + 恰好 1 个 arg + 无 errPropagate / 无 trailing lambda
    // 注：`Dyn<D&>(x)` 因 g4 `genericDef` 实参不允许内嵌 `&` 而无法解析到这里（Phase 1c 仅 owned）
    if (!call->errPropagate() && call->getArgs().size() == 1 && call->getTypeArgs().size() == 1) {
        if (auto calleeLit = dynamic_cast<ExprLiteralNode*>(call->getCalleeExpr())) {
            if (auto obj = dynamic_cast<LiteralObjNode*>(calleeLit->literal())) {
                if (obj->getValue().getText() == "Dyn") {
                    auto specTypeNode = call->getTypeArgs()[0];
                    auto argExpr = call->getArgs()[0];
                    bool isBorrow = specTypeNode->getType().isRef();
                    auto dyn = createWithLine<ExprDynCtorNode>(ctx, scope, specTypeNode, argExpr, isBorrow);
                    return static_cast<p<ExprNode>>(dyn);
                }
            }
        }
    }
    return static_cast<p<ExprNode>>(call);
}

// 尾随 lambda 唯一实参糖：f { ... } → f({ ... })
std::any ASTBuilder::visitExprCallTrailingOnly(yux::yuxParser::ExprCallTrailingOnlyContext* ctx) {
    auto scope = currentScope();
    auto callee = any_cast_p<ExprNode>(visit(ctx->left));
    auto call = createWithLine<ExprCallNode>(ctx, scope, callee);
    if (auto gd = ctx->genericDef()) {
        vector<p<TypeNode>> typeArgs;
        for (auto pCtx : gd->params) {
            if (!pCtx->bounds.empty()) {
                auto* tk = pCtx->SymbolColon();
                throw YuxError(tk ? static_cast<int>(tk->getSymbol()->getLine()) : 0,
                               tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                               ErrorCode::E2015);
            }
            typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
        }
        call->setTypeArgs(std::move(typeArgs));
    }
    auto lambda = buildTrailingLambda(
        this, ctx->trailing, scope, &ASTBuilder::buildTypeWithRef,
        [this](auto* tlCtx, LambdaExprNode::Form form, vector<LambdaParamSlot> params, vector<p<StatementNode>> stmts) {
            return createWithLine<LambdaExprNode>(tlCtx, currentScope(), form, std::move(params), nullptr, nullptr,
                                                  std::move(stmts));
        });
    if (lambda) call->addArg(static_cast<p<ExprNode>>(lambda));
    // Phase 10e：后缀 `!` 错误传播标记（DRAFT-错误.md [#4.B]）
    if (ctx->errPropagate) call->setErrPropagate(true);
    return static_cast<p<ExprNode>>(call);
}

// 创建 lambda body 的内层作用域，登记形参符号；caller 负责 push/pop _scopeStack。
// 用 LambdaScopeNode 结构区分于普通 ScopeNode，便于未来 sema 区分（如自由变量诊断）。
namespace {
class LambdaScopeNode : public ScopeNode {
public:
    explicit LambdaScopeNode(const p<Node>& parent) : ScopeNode(parent) {}
};

p<ScopeNode> makeLambdaBodyScope(const p<ScopeNode>& parentScope, const vector<LambdaParamSlot>& params) {
    auto scope = static_cast<p<LambdaScopeNode>>(new LambdaScopeNode(parentScope));
    scope->setParentScope(parentScope);
    for (auto& slot : params) {
        TypeInfo t = slot.type ? slot.type->getType() : TypeInfo();
        scope->registerSymbol(slot.name.getText(), {SymbolKind::Variable, slot.name.getText(), t});
    }
    return scope;
}
} // namespace

// Lambda 表达式形：x => expr  （裸单参，类型由上下文反推）
std::any ASTBuilder::visitExprLambdaSingle(yux::yuxParser::ExprLambdaSingleContext* ctx) {
    auto scope = currentScope();
    vector<LambdaParamSlot> params;
    params.push_back(
        LambdaParamSlot{.name = Token(ctx->name->getText(), static_cast<int>(ctx->name->getLine())), .type = nullptr});
    auto bodyScope = makeLambdaBodyScope(scope, params);
    _scopeStack.push_back(bodyScope);
    auto bodyExpr = any_cast_p<ExprNode>(visit(ctx->body->expr()));
    _scopeStack.pop_back();
    DEBUG_LOG("    Expr: LambdaSingle");
    auto node = createWithLine<LambdaExprNode>(ctx, scope, LambdaExprNode::Form::Single, std::move(params), nullptr,
                                               bodyExpr, vector<p<StatementNode>>{});
    node->setBodyScope(bodyScope);
    return static_cast<p<ExprNode>>(node);
}

// Lambda 表达式形：(args) RetT? => expr
std::any ASTBuilder::visitExprLambdaParen(yux::yuxParser::ExprLambdaParenContext* ctx) {
    auto scope = currentScope();
    auto params = collectLambdaParams(this, ctx->lambdaParams(), scope, &ASTBuilder::buildTypeWithRef);
    p<TypeNode> retType = nullptr;
    if (ctx->retType) {
        retType = buildTypeWithRef(ctx->retType, scope);
    }
    auto bodyScope = makeLambdaBodyScope(scope, params);
    _scopeStack.push_back(bodyScope);
    auto bodyExpr = any_cast_p<ExprNode>(visit(ctx->body->expr()));
    _scopeStack.pop_back();
    DEBUG_LOG_VAL("    Expr: LambdaParen", "params=" << params.size());
    auto node = createWithLine<LambdaExprNode>(ctx, scope, LambdaExprNode::Form::Paren, std::move(params), retType,
                                               bodyExpr, vector<p<StatementNode>>{});
    node->setBodyScope(bodyScope);
    return static_cast<p<ExprNode>>(node);
}

// Lambda 块形：{ args => stmts }
std::any ASTBuilder::visitExprLambdaBlock(yux::yuxParser::ExprLambdaBlockContext* ctx) {
    auto scope = currentScope();
    auto params = collectLambdaParams(this, ctx->lambdaParams(), scope, &ASTBuilder::buildTypeWithRef);
    auto bodyScope = makeLambdaBodyScope(scope, params);
    _scopeStack.push_back(bodyScope);
    vector<p<StatementNode>> stmts;
    for (auto* s : ctx->statement()) {
        stmts.push_back(any_cast_p<StatementNode>(visit(s)));
    }
    _scopeStack.pop_back();
    DEBUG_LOG_VAL("    Expr: LambdaBlock", "params=" << params.size() << " stmts=" << stmts.size());
    auto node = createWithLine<LambdaExprNode>(ctx, scope, LambdaExprNode::Form::Block, std::move(params), nullptr,
                                               nullptr, std::move(stmts));
    node->setBodyScope(bodyScope);
    return static_cast<p<ExprNode>>(node);
}

// Lambda 0 参块形：{ stmts }（禁写 =>）
std::any ASTBuilder::visitExprLambdaZeroBlock(yux::yuxParser::ExprLambdaZeroBlockContext* ctx) {
    auto scope = currentScope();
    auto bodyScope = makeLambdaBodyScope(scope, vector<LambdaParamSlot>{});
    _scopeStack.push_back(bodyScope);
    vector<p<StatementNode>> stmts;
    for (auto* s : ctx->statement()) {
        stmts.push_back(any_cast_p<StatementNode>(visit(s)));
    }
    _scopeStack.pop_back();
    DEBUG_LOG_VAL("    Expr: LambdaZeroBlock", "stmts=" << stmts.size());
    auto node = createWithLine<LambdaExprNode>(ctx, scope, LambdaExprNode::Form::ZeroBlock, vector<LambdaParamSlot>{},
                                               static_cast<p<TypeNode>>(nullptr), static_cast<p<ExprNode>>(nullptr),
                                               std::move(stmts));
    node->setBodyScope(bodyScope);
    return static_cast<p<ExprNode>>(node);
}

std::any ASTBuilder::visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    auto op = (opText == "+") ? ExprAddSubNode::Op::Add : ExprAddSubNode::Op::Sub;

    DEBUG_LOG_VAL("    Expr: AddSub", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprAddSubNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprMulDivMod(yux::yuxParser::ExprMulDivModContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    ExprMulDivModNode::Op op;
    if (opText == "*") {
        op = ExprMulDivModNode::Op::Mul;
    } else if (opText == "/") {
        op = ExprMulDivModNode::Op::Div;
    } else {
        op = ExprMulDivModNode::Op::Mod;
    }

    DEBUG_LOG_VAL("    Expr: MulDivMod", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprMulDivModNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprBinOp(yux::yuxParser::ExprBinOpContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    ExprBinOpNode::Op op;
    if (opText == "&") {
        op = ExprBinOpNode::Op::And;
    } else if (opText == "|") {
        op = ExprBinOpNode::Op::Or;
    } else {
        op = ExprBinOpNode::Op::Xor;
    }

    DEBUG_LOG_VAL("    Expr: BinOp", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprBinOpNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprShift(yux::yuxParser::ExprShiftContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opShift()->getText();
    ExprBinOpNode::Op op = (opText == "<<") ? ExprBinOpNode::Op::Shl : ExprBinOpNode::Op::Shr;

    DEBUG_LOG_VAL("    Expr: Shift", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprBinOpNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) {
    DEBUG_LOG("    Expr: Literal");
    auto scope = currentScope();
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    return static_cast<p<ExprNode>>(createWithLine<ExprLiteralNode>(ctx, scope, literal));
}

std::any ASTBuilder::visitExprDot(yux::yuxParser::ExprDotContext* ctx) {
    DEBUG_LOG("    Expr: Dot - visitExprDot called");
    auto scope = currentScope();
    auto base = any_cast_p<ExprNode>(visit(ctx->left));
    DEBUG_LOG_VAL("    Expr: Dot - member count", ctx->member.size());
    for (size_t i = 0; i < ctx->member.size(); ++i) {
        DEBUG_LOG_VAL("    Expr: Dot - member[" + to_string(i) + "]", ctx->member[i]->getText());
    }
    DEBUG_LOG_VAL("    Expr: Dot", ctx->member.back()->getText());
    // 检测 ?. 安全访问标志（grammar: SymbolDot SymbolQuest? member）
    bool safe = ctx->SymbolQuest() != nullptr;
    if (safe) {
        DEBUG_LOG("    Expr: Dot - safe access (?.)");
    }
    // §12.10 续节 / DRAFT-spec-disambig-at：a.m@SpecA(...) 的消歧后缀
    string specQual;
    if (ctx->SymbolAt() != nullptr && ctx->specQual != nullptr) {
        specQual = ctx->specQual->getText();
        DEBUG_LOG_VAL("    Expr: Dot - spec qualifier", specQual);
    }
    auto result =
        static_cast<p<ExprNode>>(createWithLine<ExprDotNode>(ctx, scope, base, ctx->member.back(), safe, specQual));
    DEBUG_LOG_VAL("    Expr: Dot - result type", typeid(*result).name());
    return result;
}

// 比较: < <= > >=
std::any ASTBuilder::visitExprCompare(yux::yuxParser::ExprCompareContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opCompare()->getText();
    ExprCompareNode::Op op;
    if (opText == "<") {
        op = ExprCompareNode::Op::Lt;
    } else if (opText == "<=") {
        op = ExprCompareNode::Op::Le;
    } else if (opText == ">") {
        op = ExprCompareNode::Op::Gt;
    } else {
        op = ExprCompareNode::Op::Ge;
    }

    DEBUG_LOG_VAL("    Expr: Compare", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

// 相等: == !=
std::any ASTBuilder::visitExprEq(yux::yuxParser::ExprEqContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opEq()->getText();
    ExprCompareNode::Op op = (opText == "==") ? ExprCompareNode::Op::Eq : ExprCompareNode::Op::Ne;

    DEBUG_LOG_VAL("    Expr: Eq", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

// 短路逻辑: && ||
std::any ASTBuilder::visitExprBool(yux::yuxParser::ExprBoolContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opBool()->getText();
    ExprCompareNode::Op op = (opText == "&&") ? ExprCompareNode::Op::AndAnd : ExprCompareNode::Op::OrOr;

    DEBUG_LOG_VAL("    Expr: Bool", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) {
    DEBUG_LOG("      Literal: Number");
    return (any_cast_p<LiteralNode>(visit(ctx->num)));
}

std::any ASTBuilder::visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) {
    auto token = ctx->True() ? ctx->True()->getSymbol() : ctx->False()->getSymbol();
    DEBUG_LOG_VAL("      Literal: Bool", (ctx->True() ? "true" : "false"));
    return static_cast<p<LiteralNode>>(createWithLine<LiteralBoolNode>(ctx, token));
}

std::any ASTBuilder::visitLiteralNull(yux::yuxParser::LiteralNullContext* ctx) {
    DEBUG_LOG("      Literal: Null");
    return static_cast<p<LiteralNode>>(createWithLine<LiteralNullNode>(ctx, ctx->Null()->getSymbol()));
}

std::any ASTBuilder::visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG_VAL("      Literal: Object", ctx->name->getText());
    return static_cast<p<LiteralNode>>(createWithLine<LiteralObjNode>(ctx, scope, ctx->name));
}

std::any ASTBuilder::visitLiteralStringLineRaw(yux::yuxParser::LiteralStringLineRawContext* ctx) {
    auto token = ctx->STR_LINE_RAW()->getSymbol();
    DEBUG_LOG_VAL("      Literal: StringRaw", token->getText());
    return static_cast<p<LiteralNode>>(createWithLine<LiteralStringNode>(ctx, token, true));
}

std::any ASTBuilder::visitLiteralStringTpl(yux::yuxParser::LiteralStringTplContext* ctx) {
    return visit(ctx->stringTemplate());
}

namespace {

// 把 code point 编码为 UTF-8 追加到 out
void appendUtf8(string& out, u32 cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// 解码 STR_TPL_TEXT 片段中的转义序列：\n \r \t \0 \\ \" \$ \xNN \uNNNN
// 其余 \x 形式按字面追加 x
void decodeTplText(const string& raw, string& out) {
    for (size_t i = 0; i < raw.size();) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
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
            case '$':
                out += '$';
                i += 2;
                break;
            case 'x':
                if (i + 3 < raw.size()) {
                    u8 v = static_cast<u8>(std::stoi(raw.substr(i + 2, 2), nullptr, 16));
                    out += static_cast<char>(v);
                    i += 4;
                } else {
                    out += raw[i];
                    ++i;
                }
                break;
            case 'u':
                if (i + 5 < raw.size()) {
                    u32 cp = static_cast<u32>(std::stoi(raw.substr(i + 2, 4), nullptr, 16));
                    appendUtf8(out, cp);
                    i += 6;
                } else {
                    out += raw[i];
                    ++i;
                }
                break;
            default:
                out += raw[i + 1];
                i += 2;
                break;
            }
        } else {
            out += raw[i++];
        }
    }
}

} // namespace

std::any ASTBuilder::visitStringTemplate(yux::yuxParser::StringTemplateContext* ctx) {
    DEBUG_LOG("      Literal: StringTemplate");
    auto openTok = ctx->STR_TPL_OPEN()->getSymbol();
    auto scope = currentScope();

    vector<string> parts;
    vector<p<ExprNode>> interps;
    string current;

    auto flushText = [&]() {
        parts.push_back(std::move(current));
        current.clear();
    };

    for (auto* part : ctx->templatePart()) {
        if (auto* t = dynamic_cast<yux::yuxParser::TplTextContext*>(part)) {
            decodeTplText(t->STR_TPL_TEXT()->getText(), current);
        } else if (auto* d = dynamic_cast<yux::yuxParser::TplDollarIdContext*>(part)) {
            flushText();
            auto* tok = d->STR_TPL_DOLLAR_ID()->getSymbol();
            // 文本形如 "$ident"，截掉首字符 '$'
            string text = tok->getText();
            if (!text.empty() && text[0] == '$') text = text.substr(1);
            Token synTok(text, tok->getLine());
            auto obj = createWithLine<LiteralObjNode>(part, scope, synTok);
            auto expr = createWithLine<ExprLiteralNode>(part, scope, static_cast<p<LiteralNode>>(obj));
            interps.push_back(static_cast<p<ExprNode>>(expr));
        } else if (auto* in = dynamic_cast<yux::yuxParser::TplInterpContext*>(part)) {
            flushText();
            auto e = any_cast_p<ExprNode>(visit(in->expr()));
            interps.push_back(e);
        }
    }
    flushText();

    // 无插值：降级为 LiteralStringNode（用合成 Token 走 raw 路径，跳过二次转义解析）
    if (interps.empty()) {
        const string& body = parts.empty() ? string() : parts[0];
        Token synTok("\"" + body + "\"", openTok->getLine());
        return static_cast<p<LiteralNode>>(createWithLine<LiteralStringNode>(ctx, synTok, true));
    }

    return static_cast<p<LiteralNode>>(
        createWithLine<StringTemplateNode>(ctx, Token(openTok), std::move(parts), std::move(interps)));
}

std::any ASTBuilder::visitLiteralCodePoint(yux::yuxParser::LiteralCodePointContext* ctx) {
    auto token = ctx->CODE_POINT()->getSymbol();
    DEBUG_LOG_VAL("      Literal: CodePoint", token->getText());
    return static_cast<p<LiteralNode>>(createWithLine<LiteralCodePointNode>(ctx, token));
}

std::any ASTBuilder::visitNumInt(yux::yuxParser::NumIntContext* ctx) {
    DEBUG_LOG_VAL("        Num: Int", ctx->INT()->getSymbol()->getText());
    return static_cast<p<LiteralNode>>(createWithLine<LiteralIntNode>(ctx, Token(ctx->INT()->getSymbol())));
}

std::any ASTBuilder::visitNumFloat(yux::yuxParser::NumFloatContext* ctx) {
    DEBUG_LOG_VAL("        Num: Float", ctx->FLOAT()->getSymbol()->getText());
    return static_cast<p<LiteralNode>>(createWithLine<LiteralFloatNode>(ctx, ctx->FLOAT()->getSymbol()));
}

std::any ASTBuilder::visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) {
    DEBUG_LOG("    Expr: IfElse");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto thenBlock = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));

    vector<p<ExprElIfNode>> elifs;
    DEBUG_LOG_VAL("      Elif count", ctx->elifs.size());
    for (auto elifCtx : ctx->elifs) {
        auto elif = any_cast_p<ExprElIfNode>(visit(elifCtx));
        elifs.push_back(elif);
    }

    p<StatementBlockNode> elseBlock = nullptr;
    if (ctx->exprElse()) {
        DEBUG_LOG("      Has else block");
        elseBlock = any_cast_p<StatementBlockNode>(visit(ctx->exprElse()));
    }

    return static_cast<p<ExprNode>>(createWithLine<ExprIfElseNode>(ctx, scope, condition, thenBlock, elifs, elseBlock));
}

std::any ASTBuilder::visitExprOneLineIfElse(yux::yuxParser::ExprOneLineIfElseContext* ctx) {
    DEBUG_LOG("    Expr: OneLineIfElse");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto trueValue = any_cast_p<ExprNode>(visit(ctx->trueValue));
    auto falseValue = any_cast_p<ExprNode>(visit(ctx->falseValue));

    return static_cast<p<ExprNode>>(
        createWithLine<ExprOneLineIfElseNode>(ctx, scope, condition, trueValue, falseValue));
}

std::any ASTBuilder::visitExprIfElsePreValue(yux::yuxParser::ExprIfElsePreValueContext* ctx) {
    DEBUG_LOG("    Expr: IfElsePreValue (Python-style)");
    auto scope = currentScope();
    auto trueValue = any_cast_p<ExprNode>(visit(ctx->trueValue));
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto falseValue = any_cast_p<ExprNode>(visit(ctx->falseValue));

    return static_cast<p<ExprNode>>(
        createWithLine<ExprIfElsePreValueNode>(ctx, scope, condition, trueValue, falseValue));
}

std::any ASTBuilder::visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) {
    DEBUG_LOG("      Visit: Elif");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    return (createWithLine<ExprElIfNode>(ctx, scope, condition, block));
}

std::any ASTBuilder::visitExprElse(yux::yuxParser::ExprElseContext* ctx) {
    DEBUG_LOG("      Visit: Else");
    return visit(ctx->statementBlock());
}

std::any ASTBuilder::visitExprGet(yux::yuxParser::ExprGetContext* ctx) {
    DEBUG_LOG("visitExprGet called");
    auto scope = currentScope();
    auto exprs = ctx->expr();
    auto arrayExpr = any_cast_p<ExprNode>(visit(exprs[0]));

    vector<p<ExprNode>> indices;
    for (size_t i = 1; i < exprs.size(); ++i) {
        indices.push_back(any_cast_p<ExprNode>(visit(exprs[i])));
    }

    return static_cast<p<ExprNode>>(createWithLine<ExprGetNode>(ctx, scope, arrayExpr, indices));
}

std::any ASTBuilder::visitExprGetRef(yux::yuxParser::ExprGetRefContext* ctx) {
    DEBUG_LOG("    Expr: GetRef");
    auto scope = currentScope();

    auto obj = ctx->obj;
    vector<Token> subs;
    subs.reserve(ctx->subs.size());
    for (auto sub : ctx->subs) {
        subs.emplace_back(sub);
    }

    return static_cast<p<ExprNode>>(createWithLine<ExprGetRefNode>(ctx, scope, obj, subs));
}

std::any ASTBuilder::visitExprArray(yux::yuxParser::ExprArrayContext* ctx) {
    DEBUG_LOG_VAL("    Expr: Array", "elements: " << ctx->velues.size());
    auto scope = currentScope();
    vector<p<ExprNode>> elements;

    elements.reserve(ctx->velues.size());
    for (auto elemCtx : ctx->velues) {
        elements.push_back(any_cast_p<ExprNode>(visit(elemCtx)));
    }

    return static_cast<p<ExprNode>>(createWithLine<ExprArrayNode>(ctx, scope, elements));
}

// 元组构造表达式 (e1, e2, ...)
// 元素至少 2 个（g4 语法保证）；递归 visit 每个 expr 子节点
std::any ASTBuilder::visitExprTuple(yux::yuxParser::ExprTupleContext* ctx) {
    DEBUG_LOG_VAL("    Expr: Tuple", "elements: " << ctx->values.size());
    auto scope = currentScope();
    vector<p<ExprNode>> elements;
    elements.reserve(ctx->values.size());
    for (auto* eCtx : ctx->values) {
        elements.push_back(any_cast_p<ExprNode>(visit(eCtx)));
    }
    return static_cast<p<ExprNode>>(createWithLine<ExprTupleNode>(ctx, scope, std::move(elements)));
}

// unit 值 () —— 0 元素元组构造
std::any ASTBuilder::visitExprUnit(yux::yuxParser::ExprUnitContext* ctx) {
    DEBUG_LOG_VAL("    Expr: Unit", "()");
    auto scope = currentScope();
    return static_cast<p<ExprNode>>(createWithLine<ExprTupleNode>(ctx, scope, vector<p<ExprNode>>{}));
}

// 元组成员访问 a.0
// 复用 ExprDotNode（member token 为 DOT_NUM，文本形如 ".0"），不支持 ?. 安全访问
// 链式 t.0.0 由 parser 左递归后缀重复匹配，每个 DOT_NUM 单独一个 ExprDotNode
std::any ASTBuilder::visitExprTupleMember(yux::yuxParser::ExprTupleMemberContext* ctx) {
    DEBUG_LOG_VAL("    Expr: TupleMember", "member: " << ctx->member->getText());
    auto scope = currentScope();
    auto base = any_cast_p<ExprNode>(visit(ctx->left));
    return static_cast<p<ExprNode>>(createWithLine<ExprDotNode>(ctx, scope, base, ctx->member, false));
}

std::any ASTBuilder::visitExprArrayInit(yux::yuxParser::ExprArrayInitContext* ctx) {
    DEBUG_LOG("    Expr: ArrayInit");
    auto scope = currentScope();

    auto literalCtx = ctx->literal();
    auto literal = any_cast_p<LiteralNode>(visit(literalCtx));

    p<TypeNode> explicitType = nullptr;
    if (ctx->type()) {
        explicitType = any_cast_p<TypeNode>(visit(ctx->type()));
    }

    return static_cast<p<ExprNode>>(createWithLine<ExprArrayInitNode>(ctx, scope, literal, explicitType));
}

// 路径调用表达式：承载两条语义，由 sema 按 LHS 类型分流
//   - 枚举构造：E::V / E::V() / E::V(args)
//   - 静态函数调用：Type::name(args) / Type:<T>::name:<U>(args) / Self::name(args)
// AST 阶段仅做形态过滤；枚举 / 静态符号解析、arity 匹配等留到 sema / codegen
// 别名透传（C::V => E::V）由 ExprPathCallNode::getType 在查询时解析
//
// Phase 1a：仅放行原 enum 形态；Self LHS / turbofish 形态报"未实现"，待 Phase 2 接管
std::any ASTBuilder::visitExprEnumCtor(yux::yuxParser::ExprEnumCtorContext* ctx) {
    // 形态过滤：Self LHS / turbofish 暂不接管
    if (ctx->selfLhs != nullptr) {
        throw YuxError(static_cast<int>(ctx->selfLhs->getLine()),
                       static_cast<int>(ctx->selfLhs->getCharPositionInLine()) + 1, ErrorCode::E0000,
                       "Self::name(...) 静态调用未实现 (Phase 2)");
    }
    DEBUG_LOG_VAL("    Expr: EnumCtor", ctx->enumName->getText() << "::" << ctx->variant->getText());
    auto scope = currentScope();
    auto node = createWithLine<ExprPathCallNode>(ctx, scope, ctx->enumName, ctx->variant);

    // Phase 6E.4: turbofish 形态 `Type:<T>::name:<U>(args)` 解析 LHS / RHS 类型实参.
    // 类型引用位不允许 bounds (与 visitTypeGeneric 同条款), 命中即 E2015.
    auto parseTurbofish = [&](yux::yuxParser::GenericDefContext* g) {
        vector<p<TypeNode>> args;
        for (auto* pCtx : g->params) {
            if (!pCtx->bounds.empty()) {
                auto* tk = pCtx->SymbolColon();
                throw YuxError(tk ? static_cast<int>(tk->getSymbol()->getLine()) : 0,
                               tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                               ErrorCode::E2015);
            }
            args.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
        }
        return args;
    };
    if (ctx->lhsGenerics != nullptr) {
        node->setLhsTypeArgs(parseTurbofish(ctx->lhsGenerics));
    }
    if (ctx->rhsGenerics != nullptr) {
        node->setRhsTypeArgs(parseTurbofish(ctx->rhsGenerics));
    }

    for (auto* aCtx : ctx->args) {
        node->addArg(any_cast_p<ExprNode>(visit(aCtx)));
    }
    return static_cast<p<ExprNode>>(node);
}

// 结构体字段字面量：Self { \n .field = value \n ... }
// Phase 1b：AST 仅承载形态；出现位限制（必须在 #Static fn 体内）由 sema 校验
std::any ASTBuilder::visitExprStructLit(yux::yuxParser::ExprStructLitContext* ctx) {
    DEBUG_LOG("    Expr: StructLit { ... }");
    auto scope = currentScope();
    // DRAFT-const-eval Phase 5: LHS 可为 Self 或通用 ID (typeName)
    bool isSelfForm = ctx->selfLhs != nullptr;
    Token leadTk;
    string structName;
    if (isSelfForm) {
        leadTk = ctx->selfLhs;
        structName = findEnclosingStructName();
    } else {
        leadTk = ctx->typeName;
        structName = ctx->typeName->getText();
    }
    auto node = createWithLine<ExprStructLitNode>(ctx, scope, leadTk, structName, isSelfForm);
    for (auto* fCtx : ctx->fieldInits) {
        node->addField(any_cast_p<FieldInitNode>(visit(fCtx)));
    }
    return static_cast<p<ExprNode>>(node);
}

// 字段初始化项：.name = value
std::any ASTBuilder::visitFieldInit(yux::yuxParser::FieldInitContext* ctx) {
    auto scope = currentScope();
    auto value = any_cast_p<ExprNode>(visit(ctx->value));
    return (createWithLine<FieldInitNode>(ctx, scope, ctx->name, value));
}

// match 模式：E::V / E::V() / E::V(b1, b2, ...)
// 绑定名重复在 ast 阶段不查（v1 留给 codegen 报 E2027）
std::any ASTBuilder::visitPatternEnum(yux::yuxParser::PatternEnumContext* ctx) {
    DEBUG_LOG_VAL("    Pattern: Enum", ctx->enumName->getText() << "::" << ctx->variant->getText());
    auto scope = currentScope();
    vector<Token> binds;
    binds.reserve(ctx->binds.size());
    for (auto* tk : ctx->binds)
        binds.emplace_back(tk);
    return (createWithLine<EnumPatternNode>(ctx, scope, ctx->enumName, ctx->variant, std::move(binds)));
}

// match 模式：else 兜底
std::any ASTBuilder::visitPatternElse(yux::yuxParser::PatternElseContext* ctx) {
    DEBUG_LOG("    Pattern: Else");
    auto scope = currentScope();
    Token elseTok = ctx->Else()->getSymbol();
    return (createWithLine<EnumPatternNode>(ctx, scope, elseTok));
}

// match arm: pattern => body
// 先 visit pattern（不依赖 binding），构造 MatchArmNode 作 ScopeNode，
// 把 pattern 中的 binding 注册到 arm scope（类型暂用占位 enum 名字 — codegen
// 阶段才能拿到 variant payload 的精确类型）。然后 push arm scope 再 visit body，
// 让 body 内对 binding 的 ObjLiteral::getType 能解析到 arm scope。
//
// 占位类型说明：v1 grammar 没把 binding 携带类型注解；spec §5.5 要求绑定类型
// 严格等于 payload 元素类型。binding 类型在 codegen 用 EnumDecl 的
// payloadTypes 拿到；ast 解析期 getType 仅用作"被某表达式引用时的类型推断"，
// 例如 `r * r` 中 r 的类型决定外层 `*` 的判定。占位空 TypeInfo 会导致
// `r * r` 类型推不出来。所以 ast_builder 必须填出真实类型 —— 这里通过 enum
// 声明回查 EnumDecl，找不到则放空 TypeInfo（codegen 仍会报 E2019）。
std::any ASTBuilder::visitMatchArm(yux::yuxParser::MatchArmContext* ctx) {
    DEBUG_LOG("    MatchArm");
    auto outer = currentScope();
    auto pattern = any_cast_p<EnumPatternNode>(visit(ctx->pattern));

    // 先建空 body 的 arm 节点（body 占位 nullptr 不便），但 createWithLine 要参数齐全；
    // 改用先 push 临时 arm，然后 visit body 拿到真实 body 节点
    auto arm = createWithLine<MatchArmNode>(ctx, outer, pattern, static_cast<p<ExprNode>>(nullptr));
    arm->setParentScope(outer);

    // 给 pattern 的 binding 在 arm scope 上注册符号（按 payload 元素类型）
    if (!pattern->isElse() && !pattern->binds().empty()) {
        // 找 enum decl：本文件 -> SDK -> 别名解析后再尝试
        auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
        EnumDeclNode* enumDecl = nullptr;
        string enumName = pattern->enumName().getText();
        auto resolveDecl = [&](const string& nm) -> EnumDeclNode* {
            if (file) {
                if (auto* d = file->getEnumDecl(nm)) return d;
                if (_yux.sdkFile() && _yux.sdkFile() != file) {
                    if (auto* d = _yux.sdkFile()->getEnumDecl(nm)) return d;
                }
                for (auto* imp : file->wildcardImports()) {
                    if (auto* d = imp->getEnumDecl(nm)) return d;
                }
            }
            return nullptr;
        };
        enumDecl = resolveDecl(enumName);
        if (!enumDecl && file) {
            // 别名透传：跟随别名链最多一层（Phase 5 ctor 同处理）
            std::set<std::string> visited;
            string n = enumName;
            while (true) {
                if (visited.count(n)) break;
                visited.insert(n);
                auto* a = file->getAliasDecl(n);
                if (!a || !a->target()) break;
                auto t = a->target()->getType();
                if (t.kind != TypeKind::Normal) break;
                n = t.name;
            }
            enumDecl = resolveDecl(n);
        }

        EnumVariantNode* variant = enumDecl ? enumDecl->variant(pattern->variantName().getText()) : nullptr;
        for (size_t i = 0; i < pattern->binds().size(); ++i) {
            const string& bn = pattern->binds()[i].getText();
            TypeInfo bindType;
            if (variant && i < variant->payloadArity()) {
                bindType = variant->payloadTypes()[i]->getType();
            }
            arm->registerSymbol(bn, {SymbolKind::Variable, bn, bindType, false});
        }
    }

    // 在 arm scope 下 visit body，使其内部 binding 引用走 arm scope -> outer 链
    _scopeStack.push_back(arm);
    auto body = any_cast_p<ExprNode>(visit(ctx->body));
    _scopeStack.pop_back();

    // 修正 arm 的 body
    // MatchArmNode 没暴露 body setter；最简改 ExprNode 字段：直接重建一个新 arm
    auto fullArm = createWithLine<MatchArmNode>(ctx, outer, pattern, body);
    fullArm->setParentScope(outer);
    // 把刚才在 arm scope 注册的 binding 复制过去
    for (auto& [n, sym] : arm->localSymbols()) {
        fullArm->registerSymbol(n, sym);
    }
    return fullArm;
}

// match 表达式：scrutinee + arms
// 仅在此处构造 AST 节点；穷尽性 / 类型一致性 / 绑定 RC / arity 校验留给编译期
std::any ASTBuilder::visitExprMatch(yux::yuxParser::ExprMatchContext* ctx) {
    DEBUG_LOG("    Expr: Match");
    auto scope = currentScope();
    auto scrutinee = any_cast_p<ExprNode>(visit(ctx->expr()));
    vector<p<MatchArmNode>> arms;
    arms.reserve(ctx->arms.size());
    for (auto* armCtx : ctx->arms) {
        arms.push_back(any_cast_p<MatchArmNode>(visit(armCtx)));
    }
    return static_cast<p<ExprNode>>(createWithLine<ExprMatchNode>(ctx, scope, scrutinee, std::move(arms)));
}

// catch arm: `catch <绑定名> <错误 enum 类型> { body }`
// DRAFT-错误.md [#4.H]：构造 CatchArmNode 并在 arm scope 注册绑定符号；
// 类型从 g4 typeRef 取（必须是已声明 enum，校验在 visitExprTryCatch 阶段）
std::any ASTBuilder::visitCatchArm(yux::yuxParser::CatchArmContext* ctx) {
    DEBUG_LOG("    CatchArm");
    auto outer = currentScope();
    auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
    string errType = typeNode->getType().name;
    Token errName = ctx->err;

    // 先建空 body 的 arm（占位 nullptr 不便），与 visitMatchArm 风格一致：先 push scope visit body
    auto arm = createWithLine<CatchArmNode>(ctx, outer, errName, errType, static_cast<p<StatementBlockNode>>(nullptr));
    arm->setParentScope(outer);
    // 在 arm scope 注册绑定符号（错误值类型 = errType；按已声明 enum 处理）
    arm->registerSymbol(errName.getText(), {SymbolKind::Variable, errName.getText(), TypeInfo(errType), false});

    _scopeStack.push_back(arm);
    auto body = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    _scopeStack.pop_back();

    auto fullArm = createWithLine<CatchArmNode>(ctx, outer, errName, errType, body);
    fullArm->setParentScope(outer);
    for (auto& [n, sym] : arm->localSymbols()) {
        fullArm->registerSymbol(n, sym);
    }
    return fullArm;
}

// try { stmts } catch e1 E1 { ... } catch e2 E2 { ... }
// DRAFT-错误.md [#4.H]：仅在此处构造 AST 节点；穷尽性 / 类型校验 / E7011 / E7016 等
// 推到 visitProgram 完成后或编译期统一校验（10f-3 / 10f-4 / 10f-5）
std::any ASTBuilder::visitExprTryCatch(yux::yuxParser::ExprTryCatchContext* ctx) {
    DEBUG_LOG("    Expr: TryCatch");
    auto scope = currentScope();
    auto tryBlock = any_cast_p<StatementBlockNode>(visit(ctx->tryBlock));
    vector<p<CatchArmNode>> catches;
    catches.reserve(ctx->catchs.size());
    for (auto* armCtx : ctx->catchs) {
        catches.push_back(any_cast_p<CatchArmNode>(visit(armCtx)));
    }
    return static_cast<p<ExprNode>>(createWithLine<ExprTryCatchNode>(ctx, scope, tryBlock, std::move(catches)));
}

std::any ASTBuilder::visitExprUnary(yux::yuxParser::ExprUnaryContext* ctx) {
    auto scope = currentScope();
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    ExprUnaryNode::Op op;
    if (opText == "-") {
        op = ExprUnaryNode::Op::Neg;
    } else if (opText == "~") {
        op = ExprUnaryNode::Op::Rev;
    } else {
        op = ExprUnaryNode::Op::Not;
    }

    DEBUG_LOG_VAL("    Expr: Unary", opText);
    return static_cast<p<ExprNode>>(createWithLine<ExprUnaryNode>(ctx, scope, op, right));
}

// a ?? b
std::any ASTBuilder::visitExprNullElse(yux::yuxParser::ExprNullElseContext* ctx) {
    auto scope = currentScope();
    auto exprs = ctx->expr();
    auto left = any_cast_p<ExprNode>(visit(exprs[0]));
    auto right = any_cast_p<ExprNode>(visit(exprs[1]));
    DEBUG_LOG("    Expr: NullElse a??b");
    return static_cast<p<ExprNode>>(createWithLine<ExprNullElseNode>(ctx, scope, left, right));
}

// a <- b：移出旧值、替换新值、返回旧值
// 语义：left 必须为 lvalue（由 sema/codegen 校验），right 为替换值
std::any ASTBuilder::visitExprMoveAssign(yux::yuxParser::ExprMoveAssignContext* ctx) {
    auto scope = currentScope();
    auto exprs = ctx->expr();
    auto left = any_cast_p<ExprNode>(visit(exprs[0]));
    auto right = any_cast_p<ExprNode>(visit(exprs[1]));
    DEBUG_LOG("    Expr: MoveAssign a <- b");
    return static_cast<p<ExprNode>>(createWithLine<ExprMoveAssignNode>(ctx, scope, left, right));
}

// `$` 单独表达式：当前实例引用
// 成员函数体内通过名字 "$" 查到隐式注入的 receiver 变量
std::any ASTBuilder::visitExprThis(yux::yuxParser::ExprThisContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("    Expr: This ($)");
    auto line = ctx->getStart()->getLine();
    auto literal = static_cast<p<LiteralNode>>(createWithLine<LiteralObjNode>(ctx, scope, Token("$", line)));
    return static_cast<p<ExprNode>>(createWithLine<ExprLiteralNode>(ctx, scope, literal));
}
