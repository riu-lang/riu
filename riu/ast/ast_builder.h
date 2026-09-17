// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_AST_BUILDER_H
#define RIU_LANG_AST_BUILDER_H

#include "node/fn_node.h"
#include "node/global_const_node.h"
#include "node/spec_ref.h"
#include "riu.h"
#include "riu/riuParserBaseVisitor.h"
#include "riu/riuParserVisitor.h"

class LambdaExprNode;

class ASTBuilder : public riu::riuParserBaseVisitor {
    Riu& _riu;
    bool _isTestFile = false;
    string _moduleName;
    string _sourcePath; // 用于 #Test 在非 *.test.ut 文件中的诊断
    // 非空：visitProgram 往这个 FileNode 里填（.ud skeleton 再 parse），不 createFile
    FileNode* _targetFile = nullptr;

    vector<std::any> stack;
    vector<Node*> _nodes;
    vector<ScopeNode*> _scopeStack;

    template <typename T, typename Ctx, typename... Args>
    T* createWithLine(Ctx ctx, Args&&... args) {
        auto node = new T(std::forward<Args>(args)...);
        _nodes.push_back(node);
        if (ctx) {
            if (auto start = ctx->getStart()) {
                // 列号转 1-based（ANTLR 的 charPositionInLine 为 0-based）
                node->setLocation(static_cast<int>(start->getLine()),
                                  static_cast<int>(start->getCharPositionInLine()) + 1);
                if (auto stop = ctx->getStop()) {
                    node->setTokenRange(static_cast<int>(start->getTokenIndex()),
                                        static_cast<int>(stop->getTokenIndex()));
                }
            }
        }
        return node;
    }

    [[nodiscard]] ScopeNode* currentScope() const {
        if (_scopeStack.empty()) return nullptr;
        return _scopeStack.back();
    }

    // 尾部 `&` 包成 Ref<inner>（原 typeWithRef）
    TypeNode* wrapRefIfAnd(antlr4::ParserRuleContext* ctx, Node* parent, TypeNode* inner,
                           antlr4::tree::TerminalNode* andTok);
    // typeGeneric / turbofish 共用：genericDefWithRef 实参列表（可含 T&）
    vector<TypeNode*> typeArgsFromGenericDefWithRef(riu::riuParser::GenericDefWithRefContext* gd, Node* parent);
    LambdaExprNode* makeTrailingLambda(riu::riuParser::TrailingLambdaContext* tl);

    // Function<P..., Ret> → TypeFnNode；末位为返回类型，() 表 unit
    TypeNode* makeFunctionType(antlr4::ParserRuleContext* ctx, Node* parent, vector<TypeNode*> typeArgs, bool nullable);

    // T?：Function 折叠为 TypeFnNode.nullable；其余包 Nullable<T>
    TypeNode* applyNullableSuffix(antlr4::ParserRuleContext* ctx, Node* parent, TypeNode* inner,
                                  antlr4::Token* questTok);

    // Phase 2b 构造模型重构: 从 _scopeStack 找最内层 StructImplNode 的 structName.
    // 用于 TypeSelfNode / ExprStructLitNode 构造时锁定所属结构体; 体外返回空串.
    [[nodiscard]] string findEnclosingStructName() const;

    // `use pkg.*`：有 pkg 只展开公开项（跳过 `to`）；无 pkg 默认别名导出直系孩子。
    // `name.*` 指向子包时递归按该子包清单（或无清单默认规则）展开。
    void expandPackageWildcard(FileNode* file, const string& pkgModName, int line);

public:
    explicit ASTBuilder(Riu& riu, string moduleName = "", bool isTestFile = false, string sourcePath = "");
    ~ASTBuilder() override;

    [[nodiscard]] bool isTestFile() const { return _isTestFile; }
    [[nodiscard]] const string& sourcePath() const { return _sourcePath; }

    // 递归检查表达式树是否包含 try/catch 节点（E3155 校验用）
    static bool exprContainsTryCatch(ExprNode* expr);

    FileNode* build(riu::riuParser::ProgramContext* ctx);

    // .ud skeleton：往已有 FileNode 追加声明（不 createFile）
    void setTargetFile(FileNode* file) { _targetFile = file; }

    // 预加载包 `pkgModName` 的孩子到 `file` 的 packageChild 表（键相对 pkgModName，形如 "a.b.inner"）。
    // 有 pkg：只挂调用方可见项，键用导出名（`as` 别名）；子包按各自清单递归。
    // 无 pkg：递归加载全部子孙 .ut。中间子目录不单独注册。
    void preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName, const string& relPrefix,
                                int errorLine);

    std::any visitProgram(riu::riuParser::ProgramContext* ctx) override;
    std::any visitImports(riu::riuParser::ImportsContext* ctx) override;
    std::any visitExternDelc(riu::riuParser::ExternDelcContext* ctx) override;
    std::any visitLetGlobal(riu::riuParser::LetGlobalContext* ctx) override;
    std::any visitFn(riu::riuParser::FnContext* ctx) override;
    std::any visitFnHeader(riu::riuParser::FnHeaderContext* ctx) override;
    std::any visitFnParams(riu::riuParser::FnParamsContext* ctx) override;
    std::any visitFnParam(riu::riuParser::FnParamContext* ctx) override;
    std::any visitFnParamStd(riu::riuParser::FnParamStdContext* ctx) override;
    std::any visitFnParamGroup(riu::riuParser::FnParamGroupContext* ctx) override;
    std::any visitFnClean(riu::riuParser::FnCleanContext* ctx) override;

    std::any visitAliasDecl(riu::riuParser::AliasDeclContext* ctx) override;
    // spec-unify v1：声明合一 — 字段段 + fnClean? + fn 段（含 #Spec / #Impl 注解分支）
    std::any visitStructDecl(riu::riuParser::StructDeclContext* ctx) override;
    std::any visitFiledDecl(riu::riuParser::FiledDeclContext* ctx) override;
    std::any visitEnumDecl(riu::riuParser::EnumDeclContext* ctx) override;
    std::any visitEnumVariant(riu::riuParser::EnumVariantContext* ctx) override;

    std::any visitStatementLet(riu::riuParser::StatementLetContext* ctx) override;
    std::any visitStatementLetTuple(riu::riuParser::StatementLetTupleContext* ctx) override;
    std::any visitStatementAssign(riu::riuParser::StatementAssignContext* ctx) override;
    std::any visitStatementExpr(riu::riuParser::StatementExprContext* ctx) override;
    std::any visitStatementRet(riu::riuParser::StatementRetContext* ctx) override;
    std::any visitStatementRetVoid(riu::riuParser::StatementRetVoidContext* ctx) override;
    std::any visitStatementBlock(riu::riuParser::StatementBlockContext* ctx) override;
    std::any visitStatementLoop(riu::riuParser::StatementLoopContext* ctx) override;
    std::any visitStatementForIn(riu::riuParser::StatementForInContext* ctx) override;
    std::any visitStatementBreak(riu::riuParser::StatementBreakContext* ctx) override;
    std::any visitStatementContinue(riu::riuParser::StatementContinueContext* ctx) override;
    std::any visitStatementSet(riu::riuParser::StatementSetContext* ctx) override;
    std::any visitStatementStaticFieldSet(riu::riuParser::StatementStaticFieldSetContext* ctx) override;

    std::any visitExprParen(riu::riuParser::ExprParenContext* ctx) override;
    std::any visitExprCall(riu::riuParser::ExprCallContext* ctx) override;
    std::any visitExprAddSub(riu::riuParser::ExprAddSubContext* ctx) override;
    std::any visitExprMulDivMod(riu::riuParser::ExprMulDivModContext* ctx) override;
    std::any visitExprLiteral(riu::riuParser::ExprLiteralContext* ctx) override;
    std::any visitExprDot(riu::riuParser::ExprDotContext* ctx) override;
    std::any visitExprCompare(riu::riuParser::ExprCompareContext* ctx) override;
    std::any visitExprEq(riu::riuParser::ExprEqContext* ctx) override;
    std::any visitExprBool(riu::riuParser::ExprBoolContext* ctx) override;
    std::any visitExprIfElse(riu::riuParser::ExprIfElseContext* ctx) override;
    std::any visitExprOneLineIfElse(riu::riuParser::ExprOneLineIfElseContext* ctx) override;
    std::any visitExprElIf(riu::riuParser::ExprElIfContext* ctx) override;
    std::any visitExprElse(riu::riuParser::ExprElseContext* ctx) override;
    std::any visitExprGet(riu::riuParser::ExprGetContext* ctx) override;
    std::any visitExprGetRef(riu::riuParser::ExprGetRefContext* ctx) override;
    std::any visitExprArray(riu::riuParser::ExprArrayContext* ctx) override;
    std::any visitExprArrayInit(riu::riuParser::ExprArrayInitContext* ctx) override;
    std::any visitExprTuple(riu::riuParser::ExprTupleContext* ctx) override;
    std::any visitExprUnit(riu::riuParser::ExprUnitContext* ctx) override;
    std::any visitExprTupleMember(riu::riuParser::ExprTupleMemberContext* ctx) override;
    std::any visitExprUnary(riu::riuParser::ExprUnaryContext* ctx) override;
    std::any visitExprNullElse(riu::riuParser::ExprNullElseContext* ctx) override;
    std::any visitExprMoveAssign(riu::riuParser::ExprMoveAssignContext* ctx) override;
    std::any visitExprEnumCtor(riu::riuParser::ExprEnumCtorContext* ctx) override;
    std::any visitExprStructLit(riu::riuParser::ExprStructLitContext* ctx) override;
    std::any visitFieldInit(riu::riuParser::FieldInitContext* ctx) override;
    std::any visitExprMatch(riu::riuParser::ExprMatchContext* ctx) override;
    std::any visitExprTryCatch(riu::riuParser::ExprTryCatchContext* ctx) override;
    std::any visitCatchArm(riu::riuParser::CatchArmContext* ctx) override;
    std::any visitExprLambdaParen(riu::riuParser::ExprLambdaParenContext* ctx) override;
    std::any visitExprCallTrailingOnly(riu::riuParser::ExprCallTrailingOnlyContext* ctx) override;
    std::any visitMatchArm(riu::riuParser::MatchArmContext* ctx) override;
    std::any visitPatternEnum(riu::riuParser::PatternEnumContext* ctx) override;
    std::any visitPatternElse(riu::riuParser::PatternElseContext* ctx) override;
    std::any visitExprThis(riu::riuParser::ExprThisContext* ctx) override;

    std::any visitTypeNormal(riu::riuParser::TypeNormalContext* ctx) override;
    std::any visitTypeSelf(riu::riuParser::TypeSelfContext* ctx) override;
    std::any visitTypeGeneric(riu::riuParser::TypeGenericContext* ctx) override;
    std::any visitTypeArray(riu::riuParser::TypeArrayContext* ctx) override;
    std::any visitTypeNullable(riu::riuParser::TypeNullableContext* ctx) override;
    std::any visitTypeFallible(riu::riuParser::TypeFallibleContext* ctx) override;
    std::any visitTypeTuple(riu::riuParser::TypeTupleContext* ctx) override;
    std::any visitTypeUnit(riu::riuParser::TypeUnitContext* ctx) override;

    std::any visitLiteralNumber(riu::riuParser::LiteralNumberContext* ctx) override;
    std::any visitLiteralBool(riu::riuParser::LiteralBoolContext* ctx) override;
    std::any visitLiteralNull(riu::riuParser::LiteralNullContext* ctx) override;
    std::any visitLiteralObj(riu::riuParser::LiteralObjContext* ctx) override;
    std::any visitLiteralStringTpl(riu::riuParser::LiteralStringTplContext* ctx) override;
    std::any visitLiteralStringLineRaw(riu::riuParser::LiteralStringLineRawContext* ctx) override;
    std::any visitStringTemplate(riu::riuParser::StringTemplateContext* ctx) override;
    std::any visitLiteralCodePoint(riu::riuParser::LiteralCodePointContext* ctx) override;
    std::any visitNumInt(riu::riuParser::NumIntContext* ctx) override;
    std::any visitNumFloat(riu::riuParser::NumFloatContext* ctx) override;

    // `#Impl(D<A>)` 实参 / `<T : D<A>>` 边界：把 type 槽收成 TypeInfo / SpecRef。
    TypeInfo typeArgFromTypeCtx(riu::riuParser::TypeContext* ctx);
    SpecRef specBoundFromTypeCtx(riu::riuParser::TypeContext* ctx);
};

#endif // RIU_LANG_AST_BUILDER_H
