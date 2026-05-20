// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 顶层声明族实现：
//   - visitExternDelc / visitLetGlobal / visitImports
//   - visitAliasDecl / visitEnumDecl / visitEnumVariant
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/statement_node.h"
#include "node/literal_node.h"
#include <algorithm>
#include "types.h"

std::any ASTBuilder::visitExternDelc(yux::yuxParser::ExternDelcContext* ctx) {
    DEBUG_LOG("Visit: ExternDelc");
    auto file = any_cast_p<FileNode>(stack.back());

    // extern 本身和内部 fnHeader 的注解当前仅验证名字（预留未来使用）
    // extern 块及其内 fnHeader 不接受 #Test（spec §11.3.1.2）
    (void)collectAnnosNonFn(ctx->buildAnnos);

    auto fnHeaders = ctx->fnHeader();
    for (auto header : fnHeaders) {
        // extern 块内 fnHeader 接受 CompilerInner 与 #NoReturn（spec §11.5.1 / DRAFT-错误.md §8.3）。
        AnnoList headerAnnos = collectAnnosExternFn(header->buildAnnos);
        bool externNoReturn = false;
        for (size_t i = 0; i < headerAnnos.names.size(); ++i) {
            if (headerAnnos.names[i] == "NoReturn") externNoReturn = true;
        }
        // E7012：extern `#NoReturn fn` 不得带 retType（与函数体内 fn 一致）。
        // E7013（与 #Fallible 互斥）暂不触发——`#Fallible` 不在 extern 白名单。
        if (externNoReturn && header->retType) {
            throw YuxError(static_cast<int>(header->name->getLine()),
                           static_cast<int>(header->name->getCharPositionInLine()) + 1, ErrorCode::E7012,
                           header->name->getText());
        }
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

        // Phase 4f / spec §7：extern fn 形参 / 返回值不得含 fn(...) 类型
        // （含捕获 lambda 的 fat-ptr 与 C 函数指针 ABI 不兼容；零捕获静态判定推到 v0.x+1）
        for (auto& pt : paramTypes) {
            if (pt.isFn()) {
                throw YuxError(header->getStart()->getLine(), ErrorCode::E2031, fnName, "parameters");
            }
        }
        if (retType.isFn()) {
            throw YuxError(header->getStart()->getLine(), ErrorCode::E2031, fnName, "return type");
        }

        // DRAFT-heap-types §8b (Phase 3a): Heap<T> 不是 ABI 稳定形态,
        // extern fn 形参 / 返回类型禁出现 Heap; 跨 FFI 走 Ptr.
        for (auto& pt : paramTypes) {
            if (pt.isHeap()) {
                auto inner = pt.heapElementType();
                throw YuxError(header->getStart()->getLine(), ErrorCode::E4028, inner ? inner->name : std::string("?"));
            }
        }
        if (retType.isHeap()) {
            auto inner = retType.heapElementType();
            throw YuxError(header->getStart()->getLine(), ErrorCode::E4028, inner ? inner->name : std::string("?"));
        }

        DEBUG_LOG_VAL("  Register external function", fnName);
        SymbolInfo fnSym(SymbolKind::Function, fnName, retType);
        fnSym.moduleName = file->moduleName();
        fnSym.isExternal = true;
        file->registerSymbol(fnName, fnSym);

        FnSymbolInfo fnFnSym{fnName, file->moduleName(), paramTypes, retType};
        fnFnSym.isExternal = true;
        fnFnSym.isNoReturn = externNoReturn;
        file->registerFnSymbol(fnName, fnFnSym);
    }

    return nullptr;
}

// DRAFT-let-unify §3：全局 `let NAME T = literal`。当前仅支持 #Cval 档（与 globalConst 同义）；
// 其他档位（默认 / #Mut / #Frozen）在全局位由 ast_builder 拒，报 E3116。
std::any ASTBuilder::visitLetGlobal(yux::yuxParser::LetGlobalContext* ctx) {
    DEBUG_LOG("Visit: LetGlobal");
    auto file = any_cast_p<FileNode>(stack.back());
    auto flags = readLetAnnos(ctx->letAnnos);

    auto name = ctx->name;
    if (!flags.isCval) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3116, name->getText());
    }
    if (!ctx->type()) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3113, name->getText());
    }
    if (!ctx->literal()) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3114, name->getText());
    }

    auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));

    auto globalConst = createWithLine<GlobalConstNode>(ctx, file, name, typeNode, literal);
    file->addGlobalConst(globalConst);

    DEBUG_LOG_VAL("  LetGlobal #Cval", name->getText() << " : " << typeNode->getType().name);
    return globalConst;
}

std::any ASTBuilder::visitImports(yux::yuxParser::ImportsContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());

    string modName;
    for (size_t i = 0; i < ctx->pkgs.size(); ++i) {
        if (i > 0) modName += ".";
        modName += ctx->pkgs[i]->getText();
    }
    bool wildcard = ctx->useAll != nullptr;
    int line = ctx->getStart() ? static_cast<int>(ctx->getStart()->getLine()) : 0;

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
        throw YuxError(line, ErrorCode::E2003, modName, modName, modName);
    }

    if (!wildcard) {
        // 命名空间别名导入：`use a.b.c` 把 `c` 作为指向 a.b.c 的模块/包别名。
        // 冲突检测：仅看当前文件的本地符号（允许覆盖 SDK 在父作用域注册的同名别名）
        if (file->localSymbols().contains(alias)) {
            throw YuxError(line, ErrorCode::E2004, alias);
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
                                DEBUG_LOG_VAL("    register module alias (from pkg.*)",
                                              child << " -> " << grandchildMod);
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
                if (existingSym &&
                    (existingSym->kind == SymbolKind::Function || existingSym->kind == SymbolKind::Variable)) {
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

// 顶层透明类型别名 `A = T` / `Pair<T> = (T, T)`
// 注册到 FileNode，目标类型节点保留原貌；透明替换在 Phase 2b 解析层接入
std::any ASTBuilder::visitAliasDecl(yux::yuxParser::AliasDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto nameTok = ctx->ID()->getSymbol();

    vector<string> typeParams;
    if (auto gd = ctx->genericDef()) {
        for (auto pCtx : gd->params) {
            if (!pCtx->bounds.empty()) {
                auto* tk = pCtx->SymbolColon();
                throw YuxError(tk ? static_cast<int>(tk->getSymbol()->getLine()) : 0,
                               tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                               ErrorCode::E2015);
            }
            if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(pCtx->type(0))) {
                typeParams.push_back(tn->ID()->getText());
            }
        }
    }

    auto aliasDecl = createWithLine<AliasDeclNode>(ctx, file, nameTok, p<TypeNode>(nullptr));
    aliasDecl->setTypeParams(typeParams);

    // 类型形参纳入别名作用域，使 `Pair<T> = (T, T)` 的目标类型解析能识别 T
    stack.emplace_back(aliasDecl);
    _scopeStack.push_back(aliasDecl);
    for (auto& tp : typeParams) {
        aliasDecl->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
    }
    auto target = any_cast_p<TypeNode>(visit(ctx->type()));
    _scopeStack.pop_back();
    stack.pop_back();

    aliasDecl->setTarget(target);
    DEBUG_LOG_VAL("Visit: AliasDecl", nameTok->getText() << " -> " << (target ? target->getType().name : string("?")));
    file->addAliasDecl(aliasDecl);
    return aliasDecl;
}

// 顶层 enum 声明：构造 EnumDeclNode，逐个添加 variant，登记到当前 FileNode
// variant 名重复触发 E2018；零参 variant 的 payloadTypes 为空向量
std::any ASTBuilder::visitEnumDecl(yux::yuxParser::EnumDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto enumDecl = createWithLine<EnumDeclNode>(ctx, file, ctx->name);
    enumDecl->setParentScope(file);

    DEBUG_LOG_VAL("Visit: EnumDecl", ctx->name->getText());

    stack.emplace_back(enumDecl);
    _scopeStack.push_back(enumDecl);

    for (auto* vCtx : ctx->variants) {
        auto variant = any_cast_p<EnumVariantNode>(visit(vCtx));
        if (!enumDecl->addVariant(variant)) {
            _scopeStack.pop_back();
            stack.pop_back();
            throw YuxError(static_cast<int>(vCtx->name->getLine()),
                           static_cast<int>(vCtx->name->getCharPositionInLine()) + 1, ErrorCode::E2018,
                           variant->name().getText(), ctx->name->getText());
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();

    file->addEnumDecl(enumDecl);
    return enumDecl;
}

// 单个 enum variant：短名 + 可选 tuple-style payload 类型列表
std::any ASTBuilder::visitEnumVariant(yux::yuxParser::EnumVariantContext* ctx) {
    auto parent = currentScope();
    auto variant = createWithLine<EnumVariantNode>(ctx, parent, ctx->name);
    for (auto* tCtx : ctx->payloads) {
        auto typeNode = any_cast_p<TypeNode>(visit(tCtx));
        variant->addPayloadType(typeNode);
    }
    DEBUG_LOG_VAL("  Variant", ctx->name->getText() << " arity=" << variant->payloadArity());
    return variant;
}
