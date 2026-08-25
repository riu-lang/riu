// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_AST_BUILDER_H
#define YUX_LANG_AST_BUILDER_H

#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "yux.h"
#include "yux/yuxParserBaseVisitor.h"
#include "yux/yuxParserVisitor.h"

class ASTBuilder : public yux::yuxParserBaseVisitor {
    Yux& _yux;
    bool _isTestFile = false;
    string _moduleName;
    string _sourcePath; // 用于 #Test 在非 *.test.yux 文件中的诊断

    vector<std::any> stack;
    vector<p<Node>> _nodes;
    vector<p<ScopeNode>> _scopeStack;

    template <typename T, typename Ctx, typename... Args>
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

    [[nodiscard]] p<ScopeNode> currentScope() const {
        if (_scopeStack.empty()) return nullptr;
        return _scopeStack.back();
    }

    // Phase 4a: typeWithRef -> TypeNode；若 SymbolAnd 存在，包成 Ref<inner>
    p<TypeNode> buildTypeWithRef(yux::yuxParser::TypeWithRefContext* twr, p<Node> parent);

    // Function<P..., Ret> → TypeFnNode；末位为返回类型，() 表 unit
    p<TypeNode> makeFunctionType(antlr4::ParserRuleContext* ctx, p<Node> parent, vector<p<TypeNode>> typeArgs,
                                 bool nullable);

    // T?：Function 折叠为 TypeFnNode.nullable；其余包 Nullable<T>
    p<TypeNode> applyNullableSuffix(antlr4::ParserRuleContext* ctx, p<Node> parent, p<TypeNode> inner,
                                    antlr4::Token* questTok);

    // Phase 2b 构造模型重构: 从 _scopeStack 找最内层 StructImplNode 的 structName.
    // 用于 TypeSelfNode / ExprStructLitNode 构造时锁定所属结构体; 体外返回空串.
    [[nodiscard]] string findEnclosingStructName() const;

public:
    explicit ASTBuilder(Yux& yux, string moduleName = "", bool isTestFile = false, string sourcePath = "");
    ~ASTBuilder() override;

    [[nodiscard]] bool isTestFile() const { return _isTestFile; }
    [[nodiscard]] const string& sourcePath() const { return _sourcePath; }

    // 递归检查表达式树是否包含 try/catch 节点（E3155 校验用）
    static bool exprContainsTryCatch(p<ExprNode> expr);

    p<FileNode> build(yux::yuxParser::ProgramContext* ctx);

    // 递归预加载包 `pkgModName` 下的所有 .yux 后代模块，按点分相对路径（相对于 pkgModName）
    // 注册到 `file` 的 packageChild 表下（键形如 "a.b.inner"）。中间子目录不单独注册。
    void preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName, const string& relPrefix,
                                int errorLine);

    std::any visitProgram(yux::yuxParser::ProgramContext* ctx) override;
    std::any visitImports(yux::yuxParser::ImportsContext* ctx) override;
    std::any visitExternDelc(yux::yuxParser::ExternDelcContext* ctx) override;
    std::any visitLetGlobal(yux::yuxParser::LetGlobalContext* ctx) override;
    std::any visitFn(yux::yuxParser::FnContext* ctx) override;
    std::any visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) override;
    std::any visitFnParams(yux::yuxParser::FnParamsContext* ctx) override;
    std::any visitFnParam(yux::yuxParser::FnParamContext* ctx) override;
    std::any visitFnParamStd(yux::yuxParser::FnParamStdContext* ctx) override;
    std::any visitFnParamGroup(yux::yuxParser::FnParamGroupContext* ctx) override;
    std::any visitFnClean(yux::yuxParser::FnCleanContext* ctx) override;

    std::any visitAliasDecl(yux::yuxParser::AliasDeclContext* ctx) override;
    // spec-unify v1：声明合一 — 字段段 + fnClean? + fn 段（含 #Spec / #Impl 注解分支）
    std::any visitStructDecl(yux::yuxParser::StructDeclContext* ctx) override;
    std::any visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) override;
    std::any visitEnumDecl(yux::yuxParser::EnumDeclContext* ctx) override;
    std::any visitEnumVariant(yux::yuxParser::EnumVariantContext* ctx) override;

    std::any visitStatementLet(yux::yuxParser::StatementLetContext* ctx) override;
    std::any visitStatementLetTuple(yux::yuxParser::StatementLetTupleContext* ctx) override;
    std::any visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) override;
    std::any visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) override;
    std::any visitStatementRet(yux::yuxParser::StatementRetContext* ctx) override;
    std::any visitStatementRetVoid(yux::yuxParser::StatementRetVoidContext* ctx) override;
    std::any visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) override;
    std::any visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) override;
    std::any visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) override;
    std::any visitStatementSet(yux::yuxParser::StatementSetContext* ctx) override;
    std::any visitStatementStaticFieldSet(yux::yuxParser::StatementStaticFieldSetContext* ctx) override;

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
    std::any visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) override;
    std::any visitExprElse(yux::yuxParser::ExprElseContext* ctx) override;
    std::any visitExprGet(yux::yuxParser::ExprGetContext* ctx) override;
    std::any visitExprGetRef(yux::yuxParser::ExprGetRefContext* ctx) override;
    std::any visitExprArray(yux::yuxParser::ExprArrayContext* ctx) override;
    std::any visitExprArrayInit(yux::yuxParser::ExprArrayInitContext* ctx) override;
    std::any visitExprTuple(yux::yuxParser::ExprTupleContext* ctx) override;
    std::any visitExprUnit(yux::yuxParser::ExprUnitContext* ctx) override;
    std::any visitExprTupleMember(yux::yuxParser::ExprTupleMemberContext* ctx) override;
    std::any visitExprUnary(yux::yuxParser::ExprUnaryContext* ctx) override;
    std::any visitExprNullElse(yux::yuxParser::ExprNullElseContext* ctx) override;
    std::any visitExprMoveAssign(yux::yuxParser::ExprMoveAssignContext* ctx) override;
    std::any visitExprEnumCtor(yux::yuxParser::ExprEnumCtorContext* ctx) override;
    std::any visitExprStructLit(yux::yuxParser::ExprStructLitContext* ctx) override;
    std::any visitFieldInit(yux::yuxParser::FieldInitContext* ctx) override;
    std::any visitExprMatch(yux::yuxParser::ExprMatchContext* ctx) override;
    std::any visitExprTryCatch(yux::yuxParser::ExprTryCatchContext* ctx) override;
    std::any visitCatchArm(yux::yuxParser::CatchArmContext* ctx) override;
    std::any visitExprLambdaSingle(yux::yuxParser::ExprLambdaSingleContext* ctx) override;
    std::any visitExprLambdaParen(yux::yuxParser::ExprLambdaParenContext* ctx) override;
    std::any visitExprLambdaBlock(yux::yuxParser::ExprLambdaBlockContext* ctx) override;
    std::any visitExprLambdaZeroBlock(yux::yuxParser::ExprLambdaZeroBlockContext* ctx) override;
    std::any visitExprCallTrailingOnly(yux::yuxParser::ExprCallTrailingOnlyContext* ctx) override;
    std::any visitMatchArm(yux::yuxParser::MatchArmContext* ctx) override;
    std::any visitPatternEnum(yux::yuxParser::PatternEnumContext* ctx) override;
    std::any visitPatternElse(yux::yuxParser::PatternElseContext* ctx) override;
    std::any visitExprThis(yux::yuxParser::ExprThisContext* ctx) override;

    std::any visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) override;
    std::any visitTypeSelf(yux::yuxParser::TypeSelfContext* ctx) override;
    std::any visitTypeGeneric(yux::yuxParser::TypeGenericContext* ctx) override;
    std::any visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) override;
    std::any visitTypeNullable(yux::yuxParser::TypeNullableContext* ctx) override;
    std::any visitTypeTuple(yux::yuxParser::TypeTupleContext* ctx) override;
    std::any visitTypeUnit(yux::yuxParser::TypeUnitContext* ctx) override;

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

#endif // YUX_LANG_AST_BUILDER_H
