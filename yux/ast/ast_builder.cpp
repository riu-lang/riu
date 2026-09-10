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
//   - ast_builder_type.cpp   (type* / wrapRefIfAnd / findEnclosingStructName)
// 注解 / 校验 helper 抽到 ast_builder_helpers.h（匿名命名空间 inline）。
//
// 本文件留：ctor / dtor / build / preloadPackageChildren / visitProgram。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "types.h"
#include <algorithm>

ASTBuilder::ASTBuilder(Yux& yux, string moduleName, bool isTestFile, string sourcePath)
    : _yux(yux), _isTestFile(isTestFile), _moduleName(std::move(moduleName)), _sourcePath(std::move(sourcePath)) {}

ASTBuilder::~ASTBuilder() {
    for (auto node : _nodes) {
        delete node;
    }
}

FileNode* ASTBuilder::build(yux::yuxParser::ProgramContext* ctx) {
    return any_cast_p<FileNode>(visitProgram(ctx));
}

void ASTBuilder::preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName,
                                        const string& relPrefix, int errorLine) {
    auto childKey = [&](const string& name) -> string {
        if (relPrefix.empty()) return name;
        return relPrefix + "." + name;
    };

    if (_yux.hasPkgFile(pkgModName)) {
        for (const auto& item : _yux.visiblePkgItems(file, pkgModName)) {
            string childMod = pkgModName + "." + item.name;
            string key = childKey(pkgExportName(item));
            auto kind = _yux.modulePathKind(childMod);
            if (kind == Yux::ModulePathKind::File) {
                auto childFile = childMod == file->moduleName() ? file : _yux.loadModule(childMod, errorLine);
                file->addPackageChild(alias, key, childFile);
                DEBUG_LOG_VAL("    register package child", alias << "." << key << " -> " << childMod);
            } else if (kind == Yux::ModulePathKind::Package) {
                preloadPackageChildren(file, alias, childMod, key, errorLine);
            }
        }
        return;
    }

    for (auto& child : _yux.listPackageYuxChildren(pkgModName)) {
        string childMod = pkgModName;
        childMod += '.';
        childMod += child;
        auto childFile = childMod == file->moduleName() ? file : _yux.loadModule(childMod, errorLine);
        string key = childKey(child);
        file->addPackageChild(alias, key, childFile);
        DEBUG_LOG_VAL("    register package child", alias << "." << key << " -> " << childMod);
    }
    for (auto& sub : _yux.listPackageSubdirs(pkgModName)) {
        string subMod = pkgModName;
        subMod += '.';
        subMod += sub;
        preloadPackageChildren(file, alias, subMod, childKey(sub), errorLine);
    }
}

std::any ASTBuilder::visitProgram(yux::yuxParser::ProgramContext* ctx) {
    DEBUG_LOG("Visit: Program");
    auto file = _targetFile ? _targetFile : _yux.createFile(_moduleName);
    if (!_sourcePath.empty()) file->setSourcePath(_sourcePath);

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
                    if (auto t = stdCtx->type(); t) {
                        auto typeNode = any_cast_p<TypeNode>(visit(t));
                        paramTypes.push_back(typeNode->getType());
                    }
                } else if (auto groupCtx = paramCtx->fnParamGroup()) {
                    if (auto t = groupCtx->type(); t) {
                        auto typeNode = any_cast_p<TypeNode>(visit(t));
                        for (size_t i = 0; i < groupCtx->names.size(); ++i) {
                            paramTypes.push_back(typeNode->getType());
                        }
                    }
                }
            }
        }
        TypeInfo retType;
        string suffixFallibleErr;
        if (header->retType) {
            auto typeNode = any_cast_p<TypeNode>(visit(header->retType));
            if (auto* fallible = dynamic_cast<TypeFallibleNode*>(typeNode)) {
                suffixFallibleErr = fallible->errType()->getType().name;
                retType = fallible->baseType()->getType();
            } else {
                retType = typeNode->getType();
            }
        }
        DEBUG_LOG_VAL("  Register function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = moduleName;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, moduleName, paramTypes, retType};
        for (auto* a : header->buildAnnos) {
            string aname = a->name->getText();
            if (aname == "NoReturn") {
                fnFnSym.isNoReturn = true;
            } else if (aname == "Const") {
                fnFnSym.isConst = true;
            }
        }
        if (header->errType) {
            auto errNode = any_cast_p<TypeNode>(visit(header->errType));
            suffixFallibleErr = errNode->getType().name;
        }
        fnFnSym.fallibleErrType = suffixFallibleErr;
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
    file->syncFnSymbolsFromAst();
    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG("Finished: Program");
    return file;
}
