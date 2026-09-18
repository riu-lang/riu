// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// FlatAst → FileNode。Sema / codegen 继续吃现有节点；rd 只负责词法+语法+建树。

#ifndef RIU_LANG_RD_BUILDER_H
#define RIU_LANG_RD_BUILDER_H

#include "ast/rd/flat.h"
#include "ast/rd/parser.h"
#include "ast/rd/token.h"
#include "node/expr_node.h"
#include "node/file_node.h"
#include "riu.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

class LiteralNode;
class StatementNode;
class StatementBlockNode;
class FnHeaderNode;
class FnNode;
class LambdaExprNode;
class StructImplNode;

struct RdAnnoList {
    vector<string> names;
    vector<string> args;
};

struct RdLetFlags {
    bool isMut = false;
    bool isFrozen = false;
    bool isCval = false;
    bool isInline = false;
};

class RdBuilder {
    Riu& _riu;
    bool _isTestFile = false;
    string _moduleName;
    string _sourcePath;
    string _src;
    rd::FlatAst _ast;
    vector<rd::ParseError> _errors;
    vector<rd::Token> _defaultToks;
    vector<Node*> _nodes;
    vector<ScopeNode*> _scopeStack;
    FileNode* _targetFile = nullptr;
    bool _expandImports = true;

    template <typename T, typename... Args>
    T* create(const rd::Pos& pos, Args&&... args) {
        auto* node = new T(std::forward<Args>(args)...);
        _nodes.push_back(node);
        node->setLocation(pos.line, pos.column + 1);
        auto [ts, te] = tokenRange(pos);
        if (ts >= 0) node->setTokenRange(ts, te);
        return node;
    }

    template <typename T, typename... Args>
    T* create(rd::NodeId id, Args&&... args) {
        return create<T>(at(id).pos, std::forward<Args>(args)...);
    }

    [[nodiscard]] ScopeNode* currentScope() const {
        if (_scopeStack.empty()) return nullptr;
        return _scopeStack.back();
    }
    [[nodiscard]] string findEnclosingStructName() const;

    [[nodiscard]] const rd::Node& at(rd::NodeId id) const { return _ast.at(id); }
    [[nodiscard]] rd::NodeId child(rd::NodeId parent, rd::i32 index) const { return _ast.child(parent, index); }
    [[nodiscard]] std::pair<int, int> tokenRange(const rd::Pos& pos) const;
    [[nodiscard]] size_t tokenIndexAt(rd::i32 offset) const;
    [[nodiscard]] Token makeTok(std::string_view text, const rd::Pos& pos) const;
    [[nodiscard]] Token makeTok(rd::NodeId id) const;
    [[nodiscard]] string srcSlice(const rd::Pos& pos) const;
    [[nodiscard]] TypePath pathFromDotted(std::string_view dotted, const rd::Pos& pos) const;

    void indexDefaultTokens();
    void preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName, const string& relPrefix,
                                int errorLine);
    void expandPackageWildcard(FileNode* file, const string& pkgModName, int line);

    [[nodiscard]] RdAnnoList collectAnnos(rd::NodeId parent, rd::i32 from, rd::i32 to, bool nonFn, bool externFn);
    [[nodiscard]] RdLetFlags readLetAnnos(const vector<rd::NodeId>& annos);
    [[nodiscard]] string annoArgText(rd::NodeId anno) const;
    SpecRef specRefFromType(rd::NodeId id);
    SpecRef specRefFromAnno(rd::NodeId anno);

    TypeNode* wrapRefIf(TypeNode* inner, bool isAnd, const rd::Pos& pos);
    TypeNode* applyNullableSuffix(TypeNode* inner, const rd::Pos& questPos);
    TypeNode* makeFunctionType(const rd::Pos& pos, vector<TypeNode*> typeArgs, bool nullable);
    TypeNode* buildType(rd::NodeId id);
    void parseTypeParams(rd::NodeId generic, vector<string>& names, vector<vector<SpecRef>>& bounds);
    string requireBareTypeParamName(rd::NodeId id);

    ExprNode* buildExpr(rd::NodeId id);
    ExprNode* wrapLiteral(LiteralNode* lit, rd::NodeId id);
    LiteralNode* buildLiteral(rd::NodeId id);
    ExprNode* identExpr(rd::NodeId id);
    LambdaExprNode* buildLambda(rd::NodeId id, bool trailing);
    vector<LambdaParamSlot> lambdaParamsFromKids(rd::NodeId parent, rd::i32 from, rd::i32 to);
    StatementBlockNode* buildBlock(rd::NodeId id, ScopeNode* parentScope, bool extractResult = true);
    void fillFnBody(FnNode* fn, rd::NodeId body);

    StatementNode* buildStmt(rd::NodeId id);
    StatementNode* buildLet(rd::NodeId id, bool global);
    void addUse(rd::NodeId id);
    void addFn(rd::NodeId id);
    void addExtern(rd::NodeId id);
    void addStruct(rd::NodeId id);
    void addEnum(rd::NodeId id);
    StatementNode* addAlias(rd::NodeId id);
    FnHeaderNode* buildFnHeader(rd::NodeId id, FileNode* file, const vector<rd::NodeId>& annos, rd::NodeId generic,
                                const vector<rd::NodeId>& params, rd::NodeId ret);
    FnNode* buildMethod(rd::NodeId id, StructImplNode* impl, FileNode* file);
    FnNode* buildFnClean(rd::NodeId id, ScopeNode* parent, FileNode* file);
    void preregisterFnsAndLets(rd::NodeId program);
    void addItem(rd::NodeId id);

public:
    RdBuilder(Riu& riu, string src, string moduleName = "", bool isTestFile = false, string sourcePath = "");
    ~RdBuilder();

    void setTargetFile(FileNode* file) { _targetFile = file; }
    // 格式化只记 UseSpec，不 loadModule（单文件、无工程根）。
    void setExpandImports(bool v) { _expandImports = v; }
    [[nodiscard]] const vector<rd::ParseError>& errors() const { return _errors; }

    FileNode* build();
};

#endif // RIU_LANG_RD_BUILDER_H
