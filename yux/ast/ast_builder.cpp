// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 主入口与共享通路。
//
// 历史：原 ast_builder.cpp 单文件 ~3000 LOC（P1 Phase 2 拆分前）；按 grammar 区域
// 拆为：
//   - ast_builder_decl.cpp   (extern / import / let-global / alias / enum)
//   - ast_builder_struct.cpp (struct / spec / impl / field / destructor)
//   - ast_builder_fn.cpp     (fn / fnHeader / fnParam*)
//   - ast_builder_stmt.cpp   (statement*)
//   - ast_builder_expr.cpp   (expr* / literal* / lambda / match / try-catch)
//   - ast_builder_type.cpp   (type* / buildTypeWithRef / findEnclosingStructName)
// 注解 / 校验 helper 抽到 ast_builder_helpers.h（匿名命名空间 inline）。
//
// 本文件留：ctor / dtor / build / preloadPackageChildren / visitProgram。

#include "ast_builder.h"
#include "types.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include <algorithm>

ASTBuilder::ASTBuilder(Yux& yux, string moduleName, bool isTestFile, string sourcePath)
    : _yux(yux), _isTestFile(isTestFile), _moduleName(std::move(moduleName)), _sourcePath(std::move(sourcePath)) {}

ASTBuilder::~ASTBuilder() {
    for (auto node : _nodes) {
        delete node;
    }
}

p<FileNode> ASTBuilder::build(yux::yuxParser::ProgramContext* ctx) {
    return any_cast_p<FileNode>(visitProgram(ctx));
}

void ASTBuilder::preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName,
                                        const string& relPrefix, int errorLine) {
    for (auto& child : _yux.listPackageYuxChildren(pkgModName)) {
        string childMod = pkgModName;
        childMod += '.';
        childMod += child;
        auto childFile = _yux.loadModule(childMod, errorLine);
        string key;
        if (relPrefix.empty()) {
            key = child;
        } else {
            key = relPrefix;
            key += '.';
            key += child;
        }
        file->addPackageChild(alias, key, childFile);
        DEBUG_LOG_VAL("    register package child", alias << "." << key << " -> " << childMod);
    }
    for (auto& sub : _yux.listPackageSubdirs(pkgModName)) {
        string subMod = pkgModName;
        subMod += '.';
        subMod += sub;
        string nextPrefix;
        if (relPrefix.empty()) {
            nextPrefix = sub;
        } else {
            nextPrefix = relPrefix;
            nextPrefix += '.';
            nextPrefix += sub;
        }
        preloadPackageChildren(file, alias, subMod, nextPrefix, errorLine);
    }
}

std::any ASTBuilder::visitProgram(yux::yuxParser::ProgramContext* ctx) {
    DEBUG_LOG("Visit: Program");
    auto file = _yux.createFile(_moduleName);

    auto moduleName = file->moduleName();

    stack.emplace_back(file);
    _scopeStack.push_back(file);

    auto funs = ctx->fn();
    DEBUG_LOG_VAL("  Functions count", funs.size());
    for (auto fn : funs) {
        auto header = fn->fnHeader();
        auto fnName = header->name->getText();

        vector<TypeInfo> paramTypes;
        if (auto fnParamsCtx = header->fnParams()) {
            for (auto paramCtx : fnParamsCtx->fnParam()) {
                if (auto stdCtx = paramCtx->fnParamStd()) {
                    if (auto twr = stdCtx->typeWithRef(); twr) {
                        auto typeNode = buildTypeWithRef(twr, file);
                        paramTypes.push_back(typeNode->getType());
                    }
                } else if (auto groupCtx = paramCtx->fnParamGroup()) {
                    if (auto twr = groupCtx->typeWithRef(); twr) {
                        auto typeNode = buildTypeWithRef(twr, file);
                        for (size_t i = 0; i < groupCtx->names.size(); ++i) {
                            paramTypes.push_back(typeNode->getType());
                        }
                    }
                }
            }
        }
        TypeInfo retType;
        if (header->retType) {
            auto typeNode = buildTypeWithRef(header->retType, file);
            retType = typeNode->getType();
        }
        DEBUG_LOG_VAL("  Register function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = moduleName;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, moduleName, paramTypes, retType};
        // 预扫 #NoReturn / #Fallible(E)：避免在头部注册阶段重复 collectAnnos 校验
        // Phase 10e：#Fallible(E) 单参 → 错误 enum 类型名（按字符串存）
        for (auto* a : header->buildAnnos) {
            string aname = a->name->getText();
            if (aname == "NoReturn") {
                fnFnSym.isNoReturn = true;
            } else if (aname == "Const") {
                fnFnSym.isConst = true;
            } else if (aname == "Fallible" && a->annoArg()) {
                fnFnSym.fallibleErrType = getBuildAnnoArgText(a);
            }
        }
        // E7008：成功值类型 == #Fallible 错误类型（编译器无法分流 `ret` 通道）
        if (!fnFnSym.fallibleErrType.empty() && retType.name == fnFnSym.fallibleErrType) {
            throw YuxError(static_cast<int>(header->name->getLine()),
                           static_cast<int>(header->name->getCharPositionInLine()) + 1, ErrorCode::E7008, retType.name,
                           fnFnSym.fallibleErrType);
        }
        file->registerFnSymbol(fnName, fnFnSym);
    }

    // spec-unify v1：所有 struct / spec / impl 走统一 visitStructDecl，由
    // visitChildren(ctx) 在源码顺序下触发；addStructDecl / addStructImpl /
    // addSpecDecl 在 visitStructDecl 内部已完成，不要在此处再显式 visit
    // 以免与 visitChildren 重复导致 E1103。

    // DRAFT-let-unify §3 + DRAFT-static-vars Phase 2：全局 let 预登记符号，让早引用合法。
    // 根据注解分三档：#Cval（不可写，编译期常量）/ #Mut（可写）/ 默认 val（不可写，运行期初始化）。
    auto letGlobals = ctx->letGlobal();
    DEBUG_LOG_VAL("  Global lets count", letGlobals.size());
    for (auto letGlobalCtx : letGlobals) {
        if (!letGlobalCtx->type()) continue; // type 缺失走 visitLetGlobal 时由 E3113 拒
        auto name = letGlobalCtx->name->getText();
        auto typeNode = any_cast_p<TypeNode>(visit(letGlobalCtx->type()));
        TypeInfo type = typeNode->getType();

        auto flags = readLetAnnos(letGlobalCtx->letAnnos);
        bool writeable = flags.isMut;
        bool isConst = flags.isCval;

        SymbolInfo sym(SymbolKind::Variable, name, type, writeable);
        sym.moduleName = moduleName;
        sym.isConst = isConst;
        file->registerSymbol(name, sym);

        DEBUG_LOG_VAL("  Register global let",
                      name << " : " << type.name << (flags.isMut ? " #Mut" : "") << (flags.isCval ? " #Cval" : ""));
    }

    visitChildren(ctx);
    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG("Finished: Program");
    return file;
}
