// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "ast_builder.h"
#include "node/expr_node.h"
#include "node/statement_node.h"
#include "node/literal_node.h"
#include <algorithm>
#include "types.h"

namespace {

// 已知的构建注解名字白名单；未知注解在 AST 构建期报错
const set<string>& knownAnnos() {
    static const set<string> s = {"CompilerInner"};
    return s;
}

template<typename AnnoVec>
vector<string> collectAnnos(const AnnoVec& annos) {
    vector<string> out;
    for (auto* a : annos) {
        string name = a->name->getText();
        if (!knownAnnos().contains(name)) {
            throw YuxError(
                a->name->getLine(),
                "Unknown build annotation `#{}`", name);
        }
        out.push_back(std::move(name));
    }
    return out;
}

} // namespace

ASTBuilder::ASTBuilder(llvm::LLVMContext& ctx, Yux& yux, const string& moduleName, bool isSdk) :
    context(ctx), irBuilder(ctx), _yux(yux), _moduleName(moduleName), _isSdk(isSdk) {
}

ASTBuilder::~ASTBuilder() {
    if (_isSdk) {
        return;
    }
    for (auto node : _nodes) {
        delete node;
    }
}

p<FileNode> ASTBuilder::build(yux::yuxParser::ProgramContext* ctx) {
    return any_cast_p<FileNode>(visitProgram(ctx));
}

std::any ASTBuilder::visitExternDelc(yux::yuxParser::ExternDelcContext* ctx) {
    DEBUG_LOG("Visit: ExternDelc");
    auto file = any_cast_p<FileNode>(stack.back());

    // extern 本身和内部 fnHeader 的注解当前仅验证名字（预留未来使用）
    (void)collectAnnos(ctx->buildAnnos);

    auto fnHeaders = ctx->fnHeader();
    for (auto header : fnHeaders) {
        (void)collectAnnos(header->buildAnnos);
        auto fnName = header->name->getText();

        vector<TypeInfo> paramTypes;
        if (auto fnParamsCtx = header->fnParams()) {
            for (auto paramCtx : fnParamsCtx->fnParam()) {
                if (auto stdCtx = paramCtx->fnParamStd()) {
                    if (stdCtx->type()) {
                        auto typeNode = any_cast_p<TypeNode>(visit(stdCtx->type()));
                        paramTypes.push_back(typeNode->getType());
                    }
                } else if (auto groupCtx = paramCtx->fnParamGroup()) {
                    if (groupCtx->type()) {
                        auto typeNode = any_cast_p<TypeNode>(visit(groupCtx->type()));
                        for (size_t i = 0; i < groupCtx->names.size(); ++i) {
                            paramTypes.push_back(typeNode->getType());
                        }
                    }
                }
            }
        }
        TypeInfo retType;
        if (header->retType) {
            auto typeNode = any_cast_p<TypeNode>(visit(header->retType));
            retType = typeNode->getType();
        }

        DEBUG_LOG_VAL("  Register external function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = file->moduleName();
        fnSym.isExternal = true;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, file->moduleName(), paramTypes, retType};
        fnFnSym.isExternal = true;
        file->registerFnSymbol(fnName, fnFnSym);
    }

    return nullptr;
}

std::any ASTBuilder::visitGlobalConst(yux::yuxParser::GlobalConstContext* ctx) {
    DEBUG_LOG("Visit: GlobalConst");
    auto file = any_cast_p<FileNode>(stack.back());
    (void)collectAnnos(ctx->buildAnnos);

    auto name = ctx->name;
    auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    
    auto globalConst = createWithLine<GlobalConstNode>(ctx, file, name, typeNode, literal);
    file->addGlobalConst(globalConst);
    
    DEBUG_LOG_VAL("  GlobalConst", name->getText() << " : " << typeNode->getType().name);
    return p<GlobalConstNode>(globalConst);
}

std::any ASTBuilder::visitComment(yux::yuxParser::CommentContext* ctx) {
    DEBUG_LOG("  Visit: Comment");
    return nullptr;
}

std::any ASTBuilder::visitCodeLineEnd(yux::yuxParser::CodeLineEndContext* ctx) {
    DEBUG_LOG("  Visit: CodeLineEnd");
    return nullptr;
}

void ASTBuilder::preloadPackageChildren(FileNode* file, const string& alias, const string& pkgModName,
                                         const string& relPrefix, int errorLine) {
    for (auto& child : _yux.listPackageYuxChildren(pkgModName)) {
        string childMod = pkgModName + "." + child;
        auto childFile = _yux.loadModule(childMod, errorLine);
        string key = relPrefix.empty() ? child : (relPrefix + "." + child);
        file->addPackageChild(alias, key, childFile);
        DEBUG_LOG_VAL("    register package child", alias << "." << key << " -> " << childMod);
    }
    for (auto& sub : _yux.listPackageSubdirs(pkgModName)) {
        string subMod = pkgModName + "." + sub;
        string nextPrefix = relPrefix.empty() ? sub : (relPrefix + "." + sub);
        preloadPackageChildren(file, alias, subMod, nextPrefix, errorLine);
    }
}

std::any ASTBuilder::visitImports(yux::yuxParser::ImportsContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());

    string modName;
    for (size_t i = 0; i < ctx->pkgs.size(); ++i) {
        if (i > 0) modName += ".";
        modName += ctx->pkgs[i]->getText();
    }
    bool wildcard = ctx->useAll != nullptr;
    int line = ctx->getStart() ? (int)ctx->getStart()->getLine() : 0;

    string alias;
    if (!wildcard && !ctx->pkgs.empty()) {
        alias = ctx->pkgs.back()->getText();
    }

    DEBUG_LOG_VAL("  Visit: Import", modName << (wildcard ? ".*" : ""));

    FileNode::UseSpec spec;
    spec.moduleName = modName;
    spec.alias = alias;
    spec.wildcard = wildcard;
    spec.line = line;
    file->addUseSpec(spec);
    file->addImport(modName);

    // `yux.core` 已通过 parent scope 默认注入，跳过。
    if (modName == "yux.core" || modName == file->moduleName()) {
        return nullptr;
    }

    auto pathKind = _yux.modulePathKind(modName);
    if (pathKind == Yux::ModulePathKind::Conflict) {
        throw YuxError(
            "module `" + modName + "` is ambiguous: both `" + modName + ".yux` and `" + modName + "/` exist",
            line);
    }

    if (!wildcard) {
        // 命名空间别名导入：`use a.b.c` 把 `c` 作为指向 a.b.c 的模块/包别名。
        // 冲突检测：仅看当前文件的本地符号（允许覆盖 SDK 在父作用域注册的同名别名）
        if (file->localSymbols().contains(alias)) {
            throw YuxError(
                "module alias `" + alias + "` conflicts with existing symbol",
                line);
        }
        if (pathKind == Yux::ModulePathKind::Package) {
            // 目录作为包别名：`use math` 其中 math/ 是目录。
            // 注册 Package 符号，并递归加载所有子孙 .yux（点分子路径为 key）。
            SymbolInfo aliasSym(SymbolKind::Package, alias, TypeInfo());
            aliasSym.moduleName = modName;
            file->registerSymbol(alias, aliasSym);
            file->addPackageAlias(alias, modName);
            preloadPackageChildren(file, alias, modName, "", line);
            DEBUG_LOG_VAL("    register package alias", alias << " -> " << modName);
            return nullptr;
        }
        auto target = _yux.loadModule(modName, line);
        SymbolInfo aliasSym(SymbolKind::Module, alias, TypeInfo());
        aliasSym.moduleName = modName;
        file->registerSymbol(alias, aliasSym);
        file->addModuleAlias(alias, target);
        DEBUG_LOG_VAL("    register module alias", alias << " -> " << modName);
        return nullptr;
    }

    // 目录（包）通配导入：`use pkg.*` 根据 pkg 文件决定导出内容
    // 如果存在 pkg 文件，则按其内容导出；否则导出所有 .yux 和子目录
    if (pathKind == Yux::ModulePathKind::Package) {
        // 检查是否有 pkg 文件
        if (_yux.hasPkgFile(modName)) {
            // 根据 pkg 文件内容导出
            auto exports = _yux.parsePkgFile(modName);
            for (const auto& exportItem : exports) {
                string childMod = modName + "." + exportItem.name;
                
                if (exportItem.wildcard) {
                    // modName.* 形式：导出模块的所有非私有成员（类似文件模块通配导入）
                    auto pathKind = _yux.modulePathKind(childMod);
                    if (pathKind == Yux::ModulePathKind::File) {
                        // 文件模块通配导入
                        auto target = _yux.loadModule(childMod, line);
                        
                        // 注入函数符号
                        for (auto& [name, overloads] : target->localFnSymbols()) {
                            for (auto& fnInfo : overloads) {
                                if (fnInfo.moduleName != target->moduleName()) continue;
                                if (fnInfo.isPrivate) continue;
                                file->registerFnSymbol(name, fnInfo);
                            }
                        }
                        
                        // 注入值符号
                        for (auto& [name, sym] : target->localSymbols()) {
                            if (sym.moduleName != target->moduleName()) continue;
                            if (sym.isPrivate) continue;
                            if (file->localSymbols().count(name)) continue;
                            file->registerSymbol(name, sym);
                        }
                        
                        // 注入结构体
                        file->addWildcardImport(target);
                        for (auto* decl : target->getStructDecls()) {
                            if (!decl) continue;
                            string sname = decl->name().getText();
                            if (sname.empty() || sname[0] == '_') continue;
                            if (file->lookupSymbol(sname)) continue;
                            file->registerSymbol(sname, {SymbolKind::Struct, sname, TypeInfo(sname)});
                            DEBUG_LOG_VAL("    inject struct (from pkg export)", sname << " from " << childMod);
                        }
                        
                        DEBUG_LOG_VAL("    export module wildcard", exportItem.name << ".* -> " << childMod);
                    } else if (pathKind == Yux::ModulePathKind::Package) {
                        // 子包通配导出：递归导出子包的所有成员
                        for (auto& child : _yux.listPackageYuxChildren(childMod)) {
                            string grandchildMod = childMod + "." + child;
                            bool alreadyWildcard = file->wildcardAliasSources(child) != nullptr;
                            if (!alreadyWildcard && file->hasSymbol(child)) continue;
                            if (!alreadyWildcard) {
                                auto target = _yux.loadModule(grandchildMod, line);
                                SymbolInfo aliasSym(SymbolKind::Module, child, TypeInfo());
                                aliasSym.moduleName = grandchildMod;
                                file->registerSymbol(child, aliasSym);
                                file->addModuleAlias(child, target);
                                DEBUG_LOG_VAL("    register module alias (from pkg.*)", child << " -> " << grandchildMod);
                            }
                            file->addWildcardAliasSource(child, grandchildMod);
                        }
                        for (auto& sub : _yux.listPackageSubdirs(childMod)) {
                            string subsubMod = childMod + "." + sub;
                            bool alreadyWildcard = file->wildcardAliasSources(sub) != nullptr;
                            if (!alreadyWildcard && file->hasSymbol(sub)) continue;
                            if (!alreadyWildcard) {
                                SymbolInfo subSym(SymbolKind::Package, sub, TypeInfo());
                                subSym.moduleName = subsubMod;
                                file->registerSymbol(sub, subSym);
                                file->addPackageAlias(sub, subsubMod);
                                preloadPackageChildren(file, sub, subsubMod, "", line);
                                DEBUG_LOG_VAL("    register package alias (from pkg.*)", sub << " -> " << subsubMod);
                            }
                            file->addWildcardAliasSource(sub, subsubMod);
                        }
                        DEBUG_LOG_VAL("    export package wildcard", exportItem.name << ".* -> " << childMod);
                    }
                } else {
                    // modName 形式：导出为模块别名或包别名
                    auto childPathKind = _yux.modulePathKind(childMod);
                    bool alreadyWildcard = file->wildcardAliasSources(exportItem.name) != nullptr;
                    
                    if (!alreadyWildcard && file->hasSymbol(exportItem.name)) {
                        DEBUG_LOG_VAL("    skip export (symbol exists)", exportItem.name);
                        continue;
                    }
                    
                    if (childPathKind == Yux::ModulePathKind::File) {
                        // 文件模块：注册为模块别名
                        if (!alreadyWildcard) {
                            auto target = _yux.loadModule(childMod, line);
                            SymbolInfo aliasSym(SymbolKind::Module, exportItem.name, TypeInfo());
                            aliasSym.moduleName = childMod;
                            file->registerSymbol(exportItem.name, aliasSym);
                            file->addModuleAlias(exportItem.name, target);
                            DEBUG_LOG_VAL("    export module", exportItem.name << " -> " << childMod);
                        }
                        file->addWildcardAliasSource(exportItem.name, childMod);
                    } else if (childPathKind == Yux::ModulePathKind::Package) {
                        // 包：注册为包别名
                        if (!alreadyWildcard) {
                            SymbolInfo aliasSym(SymbolKind::Package, exportItem.name, TypeInfo());
                            aliasSym.moduleName = childMod;
                            file->registerSymbol(exportItem.name, aliasSym);
                            file->addPackageAlias(exportItem.name, childMod);
                            preloadPackageChildren(file, exportItem.name, childMod, "", line);
                            DEBUG_LOG_VAL("    export package", exportItem.name << " -> " << childMod);
                        }
                        file->addWildcardAliasSource(exportItem.name, childMod);
                    }
                }
            }
        } else {
            // 没有 pkg 文件：导出所有 .yux 和子目录（原有逻辑）
            // 通配注入的别名在 _wildcardAliasSources 中记录来源；已注入过的同名别名不
            // 立即报错，而是追加来源，留到使用点检查歧义（Phase 5）。
            for (auto& child : _yux.listPackageYuxChildren(modName)) {
                string childMod = modName + "." + child;
                bool alreadyWildcard = file->wildcardAliasSources(child) != nullptr;
                // 检查现有符号的类型
                auto existingSym = file->lookupSymbol(child);
                bool canOverride = false;
                // TODO 歧义报错，而不是允许覆盖
                if (existingSym && (existingSym->kind == SymbolKind::Function || existingSym->kind == SymbolKind::Variable)) {
                    // 函数/变量符号可以被模块别名覆盖（使用方式不同，不会产生歧义）
                    canOverride = true;
                }
                // 非通配来源（本地声明 / `use X.Y` 非通配引入）优先，直接跳过。
                // 但函数/值符号可以被模块别名覆盖。
                if (!alreadyWildcard && existingSym && !canOverride) continue;
                if (!alreadyWildcard) {
                    auto target = _yux.loadModule(childMod, line);
                    SymbolInfo aliasSym(SymbolKind::Module, child, TypeInfo());
                    aliasSym.moduleName = childMod;
                    file->registerSymbol(child, aliasSym);
                    file->addModuleAlias(child, target);
                    DEBUG_LOG_VAL("    register module alias (from pkg.*)", child << " -> " << childMod);
                } else {
                    DEBUG_LOG_VAL("    alias collision (pkg.*), mark ambiguous", child << " <- " << childMod);
                }
                file->addWildcardAliasSource(child, childMod);
            }
            for (auto& sub : _yux.listPackageSubdirs(modName)) {
                string subMod = modName + "." + sub;
                bool alreadyWildcard = file->wildcardAliasSources(sub) != nullptr;
                if (!alreadyWildcard && file->hasSymbol(sub)) continue;
                if (!alreadyWildcard) {
                    SymbolInfo subSym(SymbolKind::Package, sub, TypeInfo());
                    subSym.moduleName = subMod;
                    file->registerSymbol(sub, subSym);
                    file->addPackageAlias(sub, subMod);
                    preloadPackageChildren(file, sub, subMod, "", line);
                    DEBUG_LOG_VAL("    register package alias (from pkg.*)", sub << " -> " << subMod);
                } else {
                    DEBUG_LOG_VAL("    alias collision (pkg.*), mark ambiguous", sub << " <- " << subMod);
                }
                file->addWildcardAliasSource(sub, subMod);
            }
        }
        return nullptr;
    }

    // 通配导入（文件模块）：加载目标文件模块并把非私有成员注入当前文件。
    auto imported = _yux.loadModule(modName, line);

    // 注入函数符号（保留源模块名，便于 mangler 生成外部符号）。
    for (auto& [name, overloads] : imported->localFnSymbols()) {
        for (auto& fnInfo : overloads) {
            if (fnInfo.moduleName != imported->moduleName()) continue; // 跳过内置（如 to_i8）
            if (fnInfo.isPrivate) continue;
            file->registerFnSymbol(name, fnInfo);
        }
    }

    // 注入值符号（全局常量、函数占位符等）。
    for (auto& [name, sym] : imported->localSymbols()) {
        if (sym.moduleName != imported->moduleName()) continue;
        if (sym.isPrivate) continue;
        // 已经有同名符号则不覆盖（主要是避免覆盖本模块自己的声明）。
        if (file->localSymbols().count(name)) continue;
        file->registerSymbol(name, sym);
    }

    // Phase 2 — 结构体通配导入：把导入模块的非私有 struct 注册为本文件可见
    // 的 Struct 符号（用于类型解析），并通过 wildcardImports 让
    // FileNode::getStructDecl/getStructImpl 回退检索到原始声明（保留 owner
    // FileNode，使 Compiler 用源模块名 mangle 类型与方法符号）。
    file->addWildcardImport(imported);
    for (auto* decl : imported->getStructDecls()) {
        if (!decl) continue;
        // 注意：decl->name() 按值返回 Token，绑定 .getText() 的引用会悬空，需复制成 string。
        string sname = decl->name().getText();
        if (sname.empty() || sname[0] == '_') continue; // 私有结构体不注入
        if (file->lookupSymbol(sname)) continue;        // 已有同名符号则跳过
        file->registerSymbol(sname, {SymbolKind::Struct, sname, TypeInfo(sname)});
        DEBUG_LOG_VAL("    inject imported struct", sname << " from " << imported->moduleName());
    }

    return nullptr;
}

std::any ASTBuilder::visitProgram(yux::yuxParser::ProgramContext* ctx) {
    DEBUG_LOG("Visit: Program");
    p<FileNode> file;
    if (_isSdk) {
        file = _yux.createSdkFile();
    } else {
        file = _yux.createFile(_moduleName);
    }
    
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
                    if (stdCtx->type()) {
                        auto typeNode = any_cast_p<TypeNode>(visit(stdCtx->type()));
                        paramTypes.push_back(typeNode->getType());
                    }
                } else if (auto groupCtx = paramCtx->fnParamGroup()) {
                    if (groupCtx->type()) {
                        auto typeNode = any_cast_p<TypeNode>(visit(groupCtx->type()));
                        for (size_t i = 0; i < groupCtx->names.size(); ++i) {
                            paramTypes.push_back(typeNode->getType());
                        }
                    }
                }
            }
        }
        TypeInfo retType;
        if (header->retType) {
            auto typeNode = any_cast_p<TypeNode>(visit(header->retType));
            retType = typeNode->getType();
        }
        DEBUG_LOG_VAL("  Register function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = moduleName;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, moduleName, paramTypes, retType};
        file->registerFnSymbol(fnName, fnFnSym);
    }

    auto structDecls = ctx->structDecl();
    DEBUG_LOG_VAL("  Struct declarations count", structDecls.size());
    for (auto structDecl : structDecls) {
        auto decl = any_cast_p<StructDeclNode>(visit(structDecl));
        file->addStructDecl(decl);

        for (auto field : decl->fields()) {
            string methodKey = decl->name().getText() + "." + field->name().getText();
            SymbolInfo fieldSym(SymbolKind::Variable, field->name().getText(), field->getType());
            fieldSym.moduleName = moduleName;
            file->registerSymbol(methodKey, fieldSym);
        }
    }

    auto structImpls = ctx->structImpl();
    DEBUG_LOG_VAL("  Struct implementations count", structImpls.size());
    for (auto structImpl : structImpls) {
        auto impl = any_cast_p<StructImplNode>(visit(structImpl));
        file->addStructImpl(impl);

        string structName = impl->structName();
        for (auto method : impl->methods()) {
            string methodName = method->header()->name().getText();
            string fullName = structName + "." + methodName;

            vector<TypeInfo> paramTypes;
            paramTypes.push_back(TypeInfo(structName));
            for (auto param : method->header()->params()) {
                if (param->type()) {
                    paramTypes.push_back(param->type()->getType());
                }
            }

            TypeInfo retType;
            if (method->header()->retType()) {
                retType = method->header()->retType()->getType();
            }

            DEBUG_LOG_VAL("  Register method", fullName);
            SymbolInfo methodSym(SymbolKind::Function, methodName, retType);
            methodSym.moduleName = moduleName;
            file->registerSymbol(fullName, methodSym);

            FnSymbolInfo methodFnSym{fullName, moduleName, paramTypes, retType};
            file->registerFnSymbol(fullName, methodFnSym);
        }
    }

    auto globalConsts = ctx->globalConst();
    DEBUG_LOG_VAL("  Global constants count", globalConsts.size());
    for (auto globalConstCtx : globalConsts) {
        auto name = globalConstCtx->name->getText();
        auto typeNode = any_cast_p<TypeNode>(visit(globalConstCtx->type()));
        TypeInfo type = typeNode->getType();
        
        SymbolInfo sym(SymbolKind::Variable, name, type, false);
        sym.moduleName = moduleName;
        file->registerSymbol(name, sym);
        
        DEBUG_LOG_VAL("  Register global const", name << " : " << type.name);
    }

    visitChildren(ctx);
    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG("Finished: Program");
    return file;
}

std::any ASTBuilder::visitFn(yux::yuxParser::FnContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto header = any_cast_p<FnHeaderNode>(visitFnHeader(ctx->fnHeader()));
    auto fn = createWithLine<FnNode>(ctx, file, header);
    fn->setParentScope(file);
    file->addFunction(fn);

    DEBUG_LOG_VAL("Visit: Function", header->name().getText());

    stack.emplace_back(fn);
    _scopeStack.push_back(fn);

    for (auto& tp : header->typeParams()) {
        fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        DEBUG_LOG_VAL("  TypeParam", tp);
    }

    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        fn->registerSymbol(
            param->name().getText(), {SymbolKind::Variable, param->name().getText(), paramType});
        DEBUG_LOG_VAL("  Param", param->name().getText() << " : " << paramType.name);
    }

    if (!ctx->fnBody()) {
        if (!header->hasAnno("CompilerInner")) {
            throw YuxError(
                header->getLineNumber(),
                "Function `{}` has no body; only `#CompilerInner` functions may omit the body",
                header->name().getText());
        }
        DEBUG_LOG("  Body: (compiler-synthesized)");
    } else if (ctx->fnBody()->fnExprkBody()) {
        DEBUG_LOG("  Body: Expression");
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLineNumber(expr->resolveLineNumber());
        fn->addStatement(retStmt);
    } else if (ctx->fnBody()->fnBlockBody()) {
        DEBUG_LOG("  Body: Block");
        auto blockBody = ctx->fnBody()->fnBlockBody();
        auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));

        for (auto stmt : stmtBlockNode->statements()) {
            fn->addStatement(stmt);
        }

        if (stmtBlockNode->hasResult()) {
            DEBUG_LOG("  Block has result, adding return");
            auto resultExpr = stmtBlockNode->resultExpr();
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
            retStmt->setLineNumber(resultExpr->resolveLineNumber());
            fn->addStatement(retStmt);
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();
    DEBUG_LOG_VAL("Finished: Function", header->name().getText());
    return fn;
}

std::any ASTBuilder::visitFnHeader(yux::yuxParser::FnHeaderContext* ctx) {
    DEBUG_LOG_VAL("  Visit: FunctionHeader", ctx->name->getText());
    auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
    if (!file) {
        file = any_cast_p<FileNode>(stack.back());
    }

    p<TypeNode> retType = nullptr;
    if (ctx->retType) {
        retType = any_cast_p<TypeNode>(visit(ctx->retType));
        DEBUG_LOG_VAL("    Return type", retType->getType().name);
    }

    auto header = createWithLine<FnHeaderNode>(ctx, file, ctx->name, retType);
    header->setAnnos(collectAnnos(ctx->buildAnnos));

    if (auto gd = ctx->genericDef()) {
        vector<string> typeParams;
        for (auto tCtx : gd->types) {
            // typeNormal 现为 type 的 labeled alternative，需 dynamic_cast 取出
            if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(tCtx)) {
                typeParams.push_back(tn->ID()->getText());
            }
        }
        header->setTypeParams(typeParams);
        for (auto& tp : header->typeParams()) {
            DEBUG_LOG_VAL("    TypeParam", tp);
        }
    }

    stack.emplace_back(header);

    if (auto fnParamsCtx = ctx->fnParams()) {
        auto params = any_cast_v<vector<p<FnParamNode>>>(visit(fnParamsCtx));
        for (auto param : params) {
            header->addParam(param);
        }
    }

    stack.pop_back();

    return header;
}

std::any ASTBuilder::visitFnParams(yux::yuxParser::FnParamsContext* ctx) {
    vector<p<FnParamNode>> allParams;
    
    for (auto paramCtx : ctx->fnParam()) {
        auto params = any_cast_v<vector<p<FnParamNode>>>(visit(paramCtx));
        allParams.insert(allParams.end(), params.begin(), params.end());
    }
    
    return allParams;
}

std::any ASTBuilder::visitFnParam(yux::yuxParser::FnParamContext* ctx) {
    if (auto stdCtx = ctx->fnParamStd()) {
        return visit(stdCtx);
    }
    if (auto groupCtx = ctx->fnParamGroup()) {
        return visit(groupCtx);
    }
    return vector<p<FnParamNode>>();
}

std::any ASTBuilder::visitFnParamStd(yux::yuxParser::FnParamStdContext* ctx) {
    p<Node> parent = any_cast_p<FnHeaderNode>(stack.back());
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    DEBUG_LOG_VAL("    Param", ctx->name->getText() << " : " << type->getType().name);
    
    vector<p<FnParamNode>> params;
    params.push_back(p<FnParamNode>(createWithLine<FnParamNode>(ctx, parent, ctx->name, type)));
    return params;
}

std::any ASTBuilder::visitFnParamGroup(yux::yuxParser::FnParamGroupContext* ctx) {
    p<Node> parent = any_cast_p<FnHeaderNode>(stack.back());
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    
    vector<p<FnParamNode>> params;
    for (auto nameToken : ctx->names) {
        DEBUG_LOG_VAL("    Param (group)", nameToken->getText() << " : " << type->getType().name);
        params.push_back(p<FnParamNode>(createWithLine<FnParamNode>(ctx, parent, nameToken, type)));
    }
    return params;
}

std::any ASTBuilder::visitStructDecl(yux::yuxParser::StructDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto structDecl = createWithLine<StructDeclNode>(ctx, file, ctx->name);
    structDecl->setAnnos(collectAnnos(ctx->buildAnnos));

    DEBUG_LOG_VAL("Visit: StructDecl", ctx->name->getText());

    vector<string> typeParams;
    for (auto tCtx : ctx->types) {
        if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(tCtx)) {
            typeParams.push_back(tn->ID()->getText());
        }
    }
    structDecl->setTypeParams(typeParams);

    stack.emplace_back(structDecl);
    _scopeStack.push_back(structDecl);

    for (auto& tp : structDecl->typeParams()) {
        structDecl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        DEBUG_LOG_VAL("    TypeParam", tp);
    }

    for (auto fieldCtx : ctx->filedDecl()) {
        auto field = any_cast_p<StructFieldNode>(visit(fieldCtx));
        structDecl->addField(field);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    return p<StructDeclNode>(structDecl);
}

std::any ASTBuilder::visitStructImpl(yux::yuxParser::StructImplContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto structImpl = createWithLine<StructImplNode>(ctx, file, ctx->name);
    structImpl->setAnnos(collectAnnos(ctx->buildAnnos));

    DEBUG_LOG_VAL("Visit: StructImpl", ctx->name->getText());

    vector<string> typeParams;
    for (auto tCtx : ctx->types) {
        if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(tCtx)) {
            typeParams.push_back(tn->ID()->getText());
        }
    }
    structImpl->setTypeParams(typeParams);

    stack.emplace_back(structImpl);
    _scopeStack.push_back(structImpl);

    for (auto& tp : structImpl->typeParams()) {
        structImpl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        DEBUG_LOG_VAL("    TypeParam", tp);
    }

    string structName = ctx->name->getText();

    if (ctx->fnClean()) {
        auto destructor = any_cast_p<FnNode>(visitFnClean(ctx->fnClean()));
        destructor->setParentScope(file);
        structImpl->setDestructor(destructor);
        
        string destructorName = structName + ".~" + structName;
        DEBUG_LOG_VAL("  Register destructor", destructorName);
        
        vector<TypeInfo> paramTypes;
        paramTypes.push_back(TypeInfo(structName));
        
        SymbolInfo destructorSym(SymbolKind::Function, "~" + structName, TypeInfo());
        destructorSym.moduleName = file->moduleName();
        file->registerSymbol(destructorName, destructorSym);
        
        FnSymbolInfo destructorFnSym{destructorName, file->moduleName(), paramTypes, TypeInfo()};
        file->registerFnSymbol(destructorName, destructorFnSym);
    }

    for (auto fnCtx : ctx->fn()) {
        auto header = any_cast_p<FnHeaderNode>(visitFnHeader(fnCtx->fnHeader()));
        auto fn = createWithLine<FnNode>(ctx, structImpl, header);
        fn->setParentScope(file);

        stack.emplace_back(fn);
        _scopeStack.push_back(fn);

        for (auto& tp : structImpl->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }
        for (auto& tp : header->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }

        fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo(structName)});

        for (auto param : header->params()) {
            TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
            fn->registerSymbol(param->name().getText(), {SymbolKind::Variable, param->name().getText(), paramType});
        }

        if (!fnCtx->fnBody()) {
            if (!header->hasAnno("CompilerInner")) {
                throw YuxError(
                    header->getLineNumber(),
                    "Method `{}.{}` has no body; only `#CompilerInner` methods may omit the body",
                    structName, header->name().getText());
            }
        } else if (fnCtx->fnBody()->fnExprkBody()) {
            auto exprBody = fnCtx->fnBody()->fnExprkBody();
            auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
            retStmt->setLineNumber(expr->resolveLineNumber());
            fn->addStatement(retStmt);
        } else if (fnCtx->fnBody()->fnBlockBody()) {
            auto blockBody = fnCtx->fnBody()->fnBlockBody();
            auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));

            for (auto stmt : stmtBlockNode->statements()) {
                fn->addStatement(stmt);
            }

            if (stmtBlockNode->hasResult()) {
                auto resultExpr = stmtBlockNode->resultExpr();
                auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
                retStmt->setLineNumber(resultExpr->resolveLineNumber());
                fn->addStatement(retStmt);
            }
        }

        _scopeStack.pop_back();
        stack.pop_back();

        structImpl->addMethod(fn);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    return p<StructImplNode>(structImpl);
}

std::any ASTBuilder::visitFnClean(yux::yuxParser::FnCleanContext* ctx) {
    auto parent = currentScope();
    DEBUG_LOG("  Visit: FnClean (destructor)");
    
    auto file = _scopeStack.empty() ? nullptr : dynamic_cast<FileNode*>(_scopeStack[0]);
    if (!file) {
        file = any_cast_p<FileNode>(stack.back());
    }
    
    string structName;
    if (auto structImpl = dynamic_cast<StructImplNode*>(parent)) {
        structName = structImpl->structName();
    }
    
    auto destructorNameToken = ctx->getStart();
    auto header = createWithLine<FnHeaderNode>(ctx, file, destructorNameToken, nullptr);
    
    auto fn = createWithLine<FnNode>(ctx, parent, header);
    
    stack.emplace_back(fn);
    _scopeStack.push_back(fn);
    
    if (!structName.empty()) {
        fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo(structName)});
    }
    
    if (ctx->fnBody()->fnExprkBody()) {
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLineNumber(expr->resolveLineNumber());
        fn->addStatement(retStmt);
    } else if (ctx->fnBody()->fnBlockBody()) {
        auto blockBody = ctx->fnBody()->fnBlockBody();
        auto stmtBlockNode = any_cast_p<StatementBlockNode>(visit(blockBody->statementBlock()));
        
        for (auto stmt : stmtBlockNode->statements()) {
            fn->addStatement(stmt);
        }
        
        if (stmtBlockNode->hasResult()) {
            auto resultExpr = stmtBlockNode->resultExpr();
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, resultExpr);
            retStmt->setLineNumber(resultExpr->resolveLineNumber());
            fn->addStatement(retStmt);
        }
    }
    
    _scopeStack.pop_back();
    stack.pop_back();
    
    DEBUG_LOG_VAL("  Finished: FnClean (destructor)", structName);
    return fn;
}

std::any ASTBuilder::visitFiledDecl(yux::yuxParser::FiledDeclContext* ctx) {
    auto parent = currentScope();
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));
    DEBUG_LOG_VAL("    Field", ctx->name->getText() << " : " << type->getType().name);
    return p<StructFieldNode>(createWithLine<StructFieldNode>(ctx, parent, ctx->name, type));
}

std::any ASTBuilder::visitStatementDeclare(yux::yuxParser::StatementDeclareContext* ctx) {
    auto scope = currentScope();

    auto declKey = ctx->DeclKey()->getText();
    DeclareType declType;
    if (declKey[2] == 'r') {
        declType = DeclareType::Var;
    } else if (declKey[2] == 'l') {
        declType = DeclareType::Val;
    } else {
        declType = DeclareType::CVal;
    }

    auto name = ctx->name;
    auto type = any_cast_p<TypeNode>(visit(ctx->type()));

    TypeInfo varType = type->getType();

    DEBUG_LOG_VAL("  Statement: Declare (no init)", name->getText() << " : " << varType.name << " (" << declKey << ")");

    if (scope) {
        scope->registerSymbol(
            name->getText(), {SymbolKind::Variable, name->getText(), varType, declType == DeclareType::Var});
    }

    return p<StatementNode>(createWithLine<StatementDeclareNode>(ctx, scope, declType, name, type));
}

std::any ASTBuilder::visitStatementDeclareAssign(yux::yuxParser::StatementDeclareAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

    auto declKey = ctx->DeclKey()->getText();
    DeclareType declType;
    if (declKey[2] == 'r') {
        declType = DeclareType::Var;
    } else if (declKey[2] == 'l') {
        declType = DeclareType::Val;
    } else {
        declType = DeclareType::CVal;
    }

    auto name = ctx->name;
    p<TypeNode> type = nullptr;
    if (ctx->type()) {
        type = any_cast_p<TypeNode>(visit(ctx->type()));
    }

    TypeInfo varType;
    if (type) {
        varType = type->getType();
    } else {
        varType = expr->getType();
    }

    DEBUG_LOG_VAL("  Statement: Declare", name->getText() << " : " << varType.name << " (" << declKey << ")");

    if (scope) {
        scope->registerSymbol(
            name->getText(), {SymbolKind::Variable, name->getText(), varType, declType == DeclareType::Var});
    }

    return p<StatementNode>(createWithLine<StatementDeclareAssignNode>(ctx, scope, declType, name, type, expr));
}

std::any ASTBuilder::visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    vector<Token> subs;
    for (auto sub : ctx->subs) {
        subs.push_back(sub);
    }

    AssignOp op = AssignOp::Eq;
    if (ctx->opAssign()) {
        auto opText = ctx->opAssign()->getText();
        if (opText == "+=") op = AssignOp::AddEq;
        else if (opText == "-=") op = AssignOp::SubEq;
        else if (opText == "*=") op = AssignOp::MulEq;
        else if (opText == "/=") op = AssignOp::DivEq;
        else if (opText == "%=") op = AssignOp::ModEq;
        else if (opText == ">>=") op = AssignOp::MtMtEq;
        else if (opText == "<<=") op = AssignOp::LtLtEq;
    }

    Token objToken = (ctx->obj->getType() == yux::yuxParser::SymbolThis)
        ? Token("$", ctx->obj->getLine())
        : Token(ctx->obj);

    DEBUG_LOG_VAL("  Statement: Assign", objToken.getText() << (subs.empty() ? "" : "." + subs[0].getText()));
    return p<StatementNode>(createWithLine<StatementAssignNode>(ctx, scope, objToken, subs, expr, op));
}

std::any ASTBuilder::visitStatementExpr(yux::yuxParser::StatementExprContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    bool hasSemicolon = ctx->SymbolSemicolon() != nullptr;
    DEBUG_LOG_VAL("  Statement: Expression", (hasSemicolon ? "with semicolon" : "without semicolon"));
    return p<StatementNode>(createWithLine<StatementExprNode>(ctx, scope, expr, hasSemicolon));
}

std::any ASTBuilder::visitStatementRet(yux::yuxParser::StatementRetContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    DEBUG_LOG("  Statement: Return");
    auto retStmt = createWithLine<StatementRetNode>(ctx, scope, expr);
    retStmt->setLineNumber(expr->resolveLineNumber());
    return p<StatementNode>(retStmt);
}

std::any ASTBuilder::visitStatementRetVoid(yux::yuxParser::StatementRetVoidContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("  Statement: Return Void");
    return p<StatementNode>(createWithLine<StatementRetVoidNode>(ctx, scope));
}

std::any ASTBuilder::visitStatementLoop(yux::yuxParser::StatementLoopContext* ctx) {
    auto scope = currentScope();
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    DEBUG_LOG("  Statement: Loop");
    return p<StatementNode>(createWithLine<StatementLoopNode>(ctx, scope, block));
}

std::any ASTBuilder::visitStatementBreak(yux::yuxParser::StatementBreakContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("  Statement: Break");
    return p<StatementNode>(createWithLine<StatementBreakNode>(ctx, scope));
}

std::any ASTBuilder::visitStatementSet(yux::yuxParser::StatementSetContext* ctx) {
    DEBUG_LOG("  Statement: ArraySet");
    auto scope = currentScope();

    auto arrayExpr = any_cast_p<ExprNode>(visit(ctx->obj));

    vector<p<ExprNode>> indices;
    for (auto arg : ctx->args) {
        indices.push_back(any_cast_p<ExprNode>(visit(arg)));
    }

    auto valueExpr = any_cast_p<ExprNode>(visit(ctx->value));

    return p<StatementNode>(createWithLine<StatementSetNode>(ctx, scope, arrayExpr, indices, valueExpr));
}

std::any ASTBuilder::visitStatementBlock(yux::yuxParser::StatementBlockContext* ctx) {
    DEBUG_LOG("  Visit: StatementBlock");
    auto parentScope = currentScope();
    vector<p<StatementNode>> statements;
    for (auto stmtCtx : ctx->statement()) {
        auto stmt = any_cast_p<StatementNode>(visit(stmtCtx));
        statements.push_back(stmt);
    }

    p<ExprNode> resultExpr = nullptr;
    bool hasResult = false;

    if (!statements.empty()) {
        auto lastStmt = statements.back();
        if (auto exprStmt = dynamic_cast<StatementExprNode*>(lastStmt)) {
            if (!exprStmt->hasSemicolon()) {
                resultExpr = exprStmt->expr();
                hasResult = true;
                statements.pop_back();
                DEBUG_LOG("    Block has result expression");
            }
        }
    }

    DEBUG_LOG_VAL("    Statements count", statements.size());
    auto block = createWithLine<StatementBlockNode>(ctx, parentScope, statements, resultExpr, hasResult);
    block->setParentScope(parentScope);
    return p<StatementBlockNode>(block);
}

std::any ASTBuilder::visitExprParen(yux::yuxParser::ExprParenContext* ctx) {
    DEBUG_LOG("    Expr: Paren");
    auto scope = currentScope();
    auto inner = any_cast_p<ExprNode>(visit(ctx->expr()));
    return p<ExprNode>(createWithLine<ExprParenNode>(ctx, scope, inner));
}

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
        for (auto tCtx : gd->types) {
            typeArgs.push_back(any_cast_p<TypeNode>(visit(tCtx)));
        }
        call->setTypeArgs(std::move(typeArgs));
    }
    return p<ExprNode>(call);
}

std::any ASTBuilder::visitExprAddSub(yux::yuxParser::ExprAddSubContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->op->getText();
    auto op = (opText == "+") ? ExprAddSubNode::Op::Add : ExprAddSubNode::Op::Sub;

    DEBUG_LOG_VAL("    Expr: AddSub", opText);
    return p<ExprNode>(createWithLine<ExprAddSubNode>(ctx, scope, op, left, right));
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
    return p<ExprNode>(createWithLine<ExprMulDivModNode>(ctx, scope, op, left, right));
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
    return p<ExprNode>(createWithLine<ExprBinOpNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprShift(yux::yuxParser::ExprShiftContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opShift()->getText();
    ExprBinOpNode::Op op = (opText == "<<") ? ExprBinOpNode::Op::Shl : ExprBinOpNode::Op::Shr;

    DEBUG_LOG_VAL("    Expr: Shift", opText);
    return p<ExprNode>(createWithLine<ExprBinOpNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitExprLiteral(yux::yuxParser::ExprLiteralContext* ctx) {
    DEBUG_LOG("    Expr: Literal");
    auto scope = currentScope();
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    return p<ExprNode>(createWithLine<ExprLiteralNode>(ctx, scope, literal));
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
    auto result = p<ExprNode>(createWithLine<ExprDotNode>(ctx, scope, base, ctx->member.back(), safe));
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
    return p<ExprNode>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

// 相等: == !=
std::any ASTBuilder::visitExprEq(yux::yuxParser::ExprEqContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opEq()->getText();
    ExprCompareNode::Op op = (opText == "==") ? ExprCompareNode::Op::Eq : ExprCompareNode::Op::Ne;

    DEBUG_LOG_VAL("    Expr: Eq", opText);
    return p<ExprNode>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

// 短路逻辑: && ||
std::any ASTBuilder::visitExprBool(yux::yuxParser::ExprBoolContext* ctx) {
    auto scope = currentScope();
    auto left = any_cast_p<ExprNode>(visit(ctx->left));
    auto right = any_cast_p<ExprNode>(visit(ctx->right));

    auto opText = ctx->opBool()->getText();
    ExprCompareNode::Op op = (opText == "&&") ? ExprCompareNode::Op::AndAnd : ExprCompareNode::Op::OrOr;

    DEBUG_LOG_VAL("    Expr: Bool", opText);
    return p<ExprNode>(createWithLine<ExprCompareNode>(ctx, scope, op, left, right));
}

std::any ASTBuilder::visitLiteralNumber(yux::yuxParser::LiteralNumberContext* ctx) {
    DEBUG_LOG("      Literal: Number");
    return p<LiteralNode>(any_cast_p<LiteralNode>(visit(ctx->num)));
}

std::any ASTBuilder::visitLiteralBool(yux::yuxParser::LiteralBoolContext* ctx) {
    auto token = ctx->True() ? ctx->True()->getSymbol() : ctx->False()->getSymbol();
    DEBUG_LOG_VAL("      Literal: Bool", (ctx->True() ? "true" : "false"));
    return p<LiteralNode>(createWithLine<LiteralBoolNode>(ctx, token));
}

std::any ASTBuilder::visitLiteralNull(yux::yuxParser::LiteralNullContext* ctx) {
    DEBUG_LOG("      Literal: Null");
    return p<LiteralNode>(createWithLine<LiteralNullNode>(ctx, ctx->Null()->getSymbol()));
}

std::any ASTBuilder::visitLiteralObj(yux::yuxParser::LiteralObjContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG_VAL("      Literal: Object", ctx->name->getText());
    return p<LiteralNode>(createWithLine<LiteralObjNode>(ctx, scope, ctx->name));
}

std::any ASTBuilder::visitLiteralStringLine(yux::yuxParser::LiteralStringLineContext* ctx) {
    auto token = ctx->STR_LINE()->getSymbol();
    DEBUG_LOG_VAL("      Literal: String", token->getText());
    return p<LiteralNode>(createWithLine<LiteralStringNode>(ctx, token));
}

std::any ASTBuilder::visitLiteralStringLineRaw(yux::yuxParser::LiteralStringLineRawContext* ctx) {
    auto token = ctx->STR_LINE_RAW()->getSymbol();
    DEBUG_LOG_VAL("      Literal: StringRaw", token->getText());
    return p<LiteralNode>(createWithLine<LiteralStringNode>(ctx, token, true));
}

std::any ASTBuilder::visitLiteralCodePoint(yux::yuxParser::LiteralCodePointContext* ctx) {
    auto token = ctx->CODE_POINT()->getSymbol();
    DEBUG_LOG_VAL("      Literal: CodePoint", token->getText());
    return p<LiteralNode>(createWithLine<LiteralCodePointNode>(ctx, token));
}

std::any ASTBuilder::visitNumInt(yux::yuxParser::NumIntContext* ctx) {
    DEBUG_LOG_VAL("        Num: Int", ctx->INT()->getSymbol()->getText());
    return p<LiteralNode>(createWithLine<LiteralIntNode>(ctx, Token(ctx->INT()->getSymbol())));
}

std::any ASTBuilder::visitNumFloat(yux::yuxParser::NumFloatContext* ctx) {
    DEBUG_LOG_VAL("        Num: Float", ctx->FLOAT()->getSymbol()->getText());
    return p<LiteralNode>(createWithLine<LiteralFloatNode>(ctx, ctx->FLOAT()->getSymbol()));
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

    return p<ExprNode>(createWithLine<ExprIfElseNode>(ctx, scope, condition, thenBlock, elifs, elseBlock));
}

std::any ASTBuilder::visitExprOneLineIfElse(yux::yuxParser::ExprOneLineIfElseContext* ctx) {
    DEBUG_LOG("    Expr: OneLineIfElse");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto trueValue = any_cast_p<ExprNode>(visit(ctx->trueValue));
    auto falseValue = any_cast_p<ExprNode>(visit(ctx->falseValue));

    return p<ExprNode>(createWithLine<ExprOneLineIfElseNode>(ctx, scope, condition, trueValue, falseValue));
}

std::any ASTBuilder::visitExprIfElsePreValue(yux::yuxParser::ExprIfElsePreValueContext* ctx) {
    DEBUG_LOG("    Expr: IfElsePreValue (Python-style)");
    auto scope = currentScope();
    auto trueValue = any_cast_p<ExprNode>(visit(ctx->trueValue));
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto falseValue = any_cast_p<ExprNode>(visit(ctx->falseValue));

    return p<ExprNode>(createWithLine<ExprIfElsePreValueNode>(ctx, scope, condition, trueValue, falseValue));
}

std::any ASTBuilder::visitExprElIf(yux::yuxParser::ExprElIfContext* ctx) {
    DEBUG_LOG("      Visit: Elif");
    auto scope = currentScope();
    auto condition = any_cast_p<ExprNode>(visit(ctx->condition));
    auto block = any_cast_p<StatementBlockNode>(visit(ctx->statementBlock()));
    return p<ExprElIfNode>(createWithLine<ExprElIfNode>(ctx, scope, condition, block));
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

    return p<ExprNode>(createWithLine<ExprGetNode>(ctx, scope, arrayExpr, indices));
}

std::any ASTBuilder::visitExprGetRef(yux::yuxParser::ExprGetRefContext* ctx) {
    DEBUG_LOG("    Expr: GetRef");
    auto scope = currentScope();
    
    auto obj = ctx->obj;
    vector<Token> subs;
    for (auto sub : ctx->subs) {
        subs.push_back(sub);
    }
    
    return p<ExprNode>(createWithLine<ExprGetRefNode>(ctx, scope, obj, subs));
}

std::any ASTBuilder::visitExprArray(yux::yuxParser::ExprArrayContext* ctx) {
    DEBUG_LOG_VAL("    Expr: Array", "elements: " << ctx->velues.size());
    auto scope = currentScope();
    vector<p<ExprNode>> elements;

    for (auto elemCtx : ctx->velues) {
        elements.push_back(any_cast_p<ExprNode>(visit(elemCtx)));
    }

    return p<ExprNode>(createWithLine<ExprArrayNode>(ctx, scope, elements));
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

    return p<ExprNode>(createWithLine<ExprArrayInitNode>(ctx, scope, literal, explicitType));
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
    return p<ExprNode>(createWithLine<ExprUnaryNode>(ctx, scope, op, right));
}

std::any ASTBuilder::visitTypeNormal(yux::yuxParser::TypeNormalContext* ctx) {
    p<Node> parent = currentScope();
    DEBUG_LOG_VAL("    Type: Normal", ctx->ID()->getSymbol()->getText());
    return p<TypeNode>(createWithLine<TypeNormalNode>(ctx, parent, ctx->ID()->getSymbol()));
}

std::any ASTBuilder::visitTypeGeneric(yux::yuxParser::TypeGenericContext* ctx) {
    p<Node> parent = currentScope();
    auto baseName = ctx->ID()->getSymbol();

    vector<p<TypeNode>> typeArgs;
    for (auto typeCtx : ctx->genericDef()->types) {
        typeArgs.push_back(any_cast_p<TypeNode>(visit(typeCtx)));
    }
    
    string argsStr;
    for (size_t i = 0; i < typeArgs.size(); ++i) {
        if (i > 0) argsStr += ", ";
        argsStr += typeArgs[i]->getType().name;
    }
    DEBUG_LOG_VAL("    Type: Generic", baseName->getText() << "<" << argsStr << ">");
    
    return p<TypeNode>(createWithLine<TypeGenericNode>(ctx, parent, baseName, typeArgs));
}

std::any ASTBuilder::visitTypeArray(yux::yuxParser::TypeArrayContext* ctx) {
    p<Node> parent = currentScope();
    auto elementType = any_cast_p<TypeNode>(visit(ctx->type()));
    auto count = ctx->INT()->getSymbol();
    DEBUG_LOG_VAL("    Type: Array", "[" << count->getText() << "]");
    return p<TypeNode>(createWithLine<TypeArrayNode>(ctx, parent, elementType, count));
}

// T? 解糖为 Nullable<T>
// 直接构造 TypeGenericNode("Nullable", [T])，复用现有泛型实例化通路
// "Nullable" 名字 token 用合成构造，line 取自 SymbolQuest
std::any ASTBuilder::visitTypeNullable(yux::yuxParser::TypeNullableContext* ctx) {
    p<Node> parent = currentScope();
    auto inner = any_cast_p<TypeNode>(visit(ctx->type()));

    auto questTok = ctx->SymbolQuest()->getSymbol();
    Token nullableName(string("Nullable"), questTok ? questTok->getLine() : 0);

    vector<p<TypeNode>> args;
    args.push_back(inner);

    DEBUG_LOG_VAL("    Type: Nullable", inner->getType().name << "?");
    return p<TypeNode>(createWithLine<TypeGenericNode>(ctx, parent, nullableName, args));
}

// a ?? b
std::any ASTBuilder::visitExprNullElse(yux::yuxParser::ExprNullElseContext* ctx) {
    auto scope = currentScope();
    auto exprs = ctx->expr();
    auto left = any_cast_p<ExprNode>(visit(exprs[0]));
    auto right = any_cast_p<ExprNode>(visit(exprs[1]));
    DEBUG_LOG("    Expr: NullElse a??b");
    return p<ExprNode>(createWithLine<ExprNullElseNode>(ctx, scope, left, right));
}

// `$` 单独表达式：当前实例引用
// 成员函数体内通过名字 "$" 查到隐式注入的 receiver 变量
std::any ASTBuilder::visitExprThis(yux::yuxParser::ExprThisContext* ctx) {
    auto scope = currentScope();
    DEBUG_LOG("    Expr: This ($)");
    auto line = ctx->getStart()->getLine();
    auto literal = p<LiteralNode>(createWithLine<LiteralObjNode>(ctx, scope, Token("$", line)));
    return p<ExprNode>(createWithLine<ExprLiteralNode>(ctx, scope, literal));
}
