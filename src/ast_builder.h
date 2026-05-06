// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_AST_BUILDER_H
#define YUX_LANG_AST_BUILDER_H

#include "yux.h"
#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "yux/yuxParserBaseVisitor.h"
#include "yux/yuxParserVisitor.h"

class ASTBuilder : public yux::yuxParserBaseVisitor {
    Yux& _yux;
    bool _isSdk = false;
    bool _isTestFile = false;
    string _moduleName;
    string _sourcePath; // 用于 #Test 在非 *.test.yux 文件中的诊断

    vector<std::any> stack;
    vector<p<Node>> _nodes;
    vector<p<ScopeNode>> _scopeStack;

    template<typename T, typename Ctx, typename... Args>
    p<T> createWithLine(Ctx ctx, Args&&... args) {
        auto node = new T(std::forward<Args>(args)...);
        _nodes.push_back(node);
        if (ctx) {
            auto start = ctx->getStart();
            if (start) {
                // 列号转 1-based（ANTLR 的 charPositionInLine 为 0-based）
                node->setLocation(static_cast<int>(start->getLine()),
                                  static_cast<int>(start->getCharPositionInLine()) + 1);
            }
        }
        return node;
    }
    
    p<ScopeNode> currentScope() const {
        if (_scopeStack.empty()) return nullptr;
        return _scopeStack.back();
    }

    // Phase 4a: typeWithRef -> TypeNode；若 SymbolAnd 存在，包成 Ref<inner>
    p<TypeNode> buildTypeWithRef(yux::yuxParser::TypeWithRefContext* twr, p<Node> parent);

public:
    explicit ASTBuilder(Yux& yux, const string& moduleName = "", bool isSdk = false,
                        bool isTestFile = false, const string& sourcePath = "");
    ~ASTBuilder() override;

    [[nodiscard]] bool isTestFile() const { return _isTestFile; }
    [[nodiscard]] const string& sourcePath() const { return _sourcePath; }

    p<FileNode> build(yux::yuxParser::ProgramContext* ctx);

    // 递归预加载包 `pkgModName` 下的所有 .yux 后代模块，按点分相对路径（相对于 pkgModName）
    // 注册到 `file` 的 packageChild 表下（键形如 "a.b.inner"）。中间子目录不单独注册。
    void preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName,
                                const string& relPrefix, int errorLine);

    std::any visitComment(yux::yuxParser::CommentContext* ctx) override;
    std::any visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) override;
    std::any visitProgram(yux::yuxParser::ProgramContext* ctx) override;
    std::any visitImports(yux::yuxParser::ImportsContext* ctx) override;
    std::any visitExternDelc(yux::yuxParser::ExternDelcContext* ctx) override;
    std::any visitGlobalConst(yux::yuxParser::GlobalConstContext* ctx) override;
    std::any visitFn(yux::yuxParser::FnContext* ctx) override;
    std::any visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) override;
    std::any visitFnParams(yux::yuxParser::FnParamsContext* ctx) override;
    std::any visitFnParam(yux::yuxParser::FnParamContext* ctx) override;
    std::any visitFnParamStd(yux::yuxParser::FnParamStdContext* ctx) override;
    std::any visitFnParamGroup(yux::yuxParser::FnParamGroupContext* ctx) override;
    std::any visitFnClean(yux::yuxParser::FnCleanContext* ctx) override;

    std::any visitAliasDecl(yux::yuxParser::AliasDeclContext* ctx) override;
    std::any visitStructDecl(yux::yuxParser::StructDeclContext* ctx) override;
    std::any visitStructImpl(yux::yuxParser::StructImplContext* ctx) override;
    std::any visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) override;
    std::any visitDraftDecl(yux::yuxParser::DraftDeclContext* ctx) override;

    std::any visitStatementDeclare(yux::yuxParser::StatementDeclareContext* ctx) override;
    std::any visitStatementDeclareAssign(yux::yuxParser::StatementDeclareAssignContext* ctx) override;
    std::any visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) override;
    std::any visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) override;
    std::any visitStatementRet(yux::yuxParser::StatementRetContext* ctx) override;
    std::any visitStatementRetVoid(yux::yuxParser::StatementRetVoidContext* ctx) override;
    std::any visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) override;
    std::any visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) override;
    std::any visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) override;
    std::any visitStatementSet(yux::yuxParser::StatementSetContext* ctx) override;

    std::any visitExprParen(yux::yuxParser::ExprParenContext* ctx) override;
    std::any visitExprCall(yux::yuxParser::ExprCallContext* ctx) override;
    std::any visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) override;
    std::any visitExprMulDivMod(yux::yuxParser::ExprMulDivModContext* ctx) override;
    std::any visitExprBinOp(yux::yuxParser::ExprBinOpContext* ctx) override;
    std::any visitExprShift(yux::yuxParser::ExprShiftContext* ctx) override;
    std::any visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) override;
    std::any visitExprDot(yux::yuxParser::ExprDotContext* ctx) override;
    std::any visitExprCompare(yux::yuxParser::ExprCompareContext* ctx) override;
    std::any visitExprEq(yux::yuxParser::ExprEqContext* ctx) override;
    std::any visitExprBool(yux::yuxParser::ExprBoolContext* ctx) override;
    std::any visitExprIfElse(yux::yuxParser::ExprIfElseContext* ctx) override;
    std::any visitExprOneLineIfElse(yux::yuxParser::ExprOneLineIfElseContext* ctx) override;
    std::any visitExprIfElsePreValue(yux::yuxParser::ExprIfElsePreValueContext* ctx) override;
    std::any visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) override;
    std::any visitExprElse(yux::yuxParser::ExprElseContext* ctx) override;
    std::any visitExprGet(yux::yuxParser::ExprGetContext* ctx) override;
    std::any visitExprGetRef(yux::yuxParser::ExprGetRefContext* ctx) override;
    std::any visitExprArray(yux::yuxParser::ExprArrayContext* ctx) override;
    std::any visitExprArrayInit(yux::yuxParser::ExprArrayInitContext* ctx) override;
    std::any visitExprUnary(yux::yuxParser::ExprUnaryContext* ctx) override;
    std::any visitExprNullElse(yux::yuxParser::ExprNullElseContext* ctx) override;
    std::any visitExprThis(yux::yuxParser::ExprThisContext* ctx) override;

    std::any visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) override;
    std::any visitTypeGeneric(yux::yuxParser::TypeGenericContext* ctx) override;
    std::any visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) override;
    std::any visitTypeNullable(yux::yuxParser::TypeNullableContext* ctx) override;
    std::any visitTypeTuple(yux::yuxParser::TypeTupleContext* ctx) override;

    std::any visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) override;
    std::any visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) override;
    std::any visitLiteralNull(yux::yuxParser::LiteralNullContext* ctx) override;
    std::any visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) override;
    std::any visitLiteralStringTpl(yux::yuxParser::LiteralStringTplContext* ctx) override;
    std::any visitLiteralStringLineRaw(yux::yuxParser::LiteralStringLineRawContext* ctx) override;
    std::any visitStringTemplate(yux::yuxParser::StringTemplateContext* ctx) override;
    std::any visitLiteralCodePoint(yux::yuxParser::LiteralCodePointContext* ctx) override;
    std::any visitNumInt(yux::yuxParser::NumIntContext* ctx) override;
    std::any visitNumFloat(yux::yuxParser::NumFloatContext* ctx) override;
};

#endif //YUX_LANG_AST_BUILDER_H
