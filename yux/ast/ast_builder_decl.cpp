// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// ast_builder 顶层声明族实现：
//   - visitExternDelc / visitLetGlobal / visitImports
//   - visitAliasDecl / visitEnumDecl / visitEnumVariant
// 拆自原 ast_builder.cpp（P1 Phase 2），方法体一字不动。

#include "ast_builder.h"
#include "ast_builder_helpers.h"
#include "node/expr_node.h"
#include "node/global_var_node.h"
#include "node/literal_node.h"
#include "node/statement_node.h"
#include "sema/const_eval.h"
#include "types.h"
#include <algorithm>

std::any ASTBuilder::visitExternDelc(yux::yuxParser::ExternDelcContext* ctx) {
    DEBUG_LOG("Visit: ExternDelc");
    auto file = any_cast_p<FileNode>(stack.back());

    // extern 块的注解当前仅验证名字（预留未来使用）
    (void)collectAnnosNonFn(ctx->buildAnnos);

    auto fnHeaders = ctx->fnHeader();
    for (auto header : fnHeaders) {
        // extern 块内 fnHeader 接受 Builtin 与 #NoReturn（spec §11.5.1 / DRAFT-错误.md §8.3）。
        AnnoList headerAnnos = collectAnnosExternFn(header->buildAnnos);
        bool externNoReturn = false;
        for (const auto& name : headerAnnos.names) {
            if (name == "NoReturn") externNoReturn = true;
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

        // Phase 4f / spec §7：extern fn 形参 / 返回值不得含 Function<...> 类型
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
        // #CName("link_symbol")：extern fn 的链接时符号名（§6.6.1.2）
        for (size_t i = 0; i < headerAnnos.names.size(); ++i) {
            if (headerAnnos.names[i] == "CName") {
                fnFnSym.cName = headerAnnos.args[i];
                break;
            }
        }
        file->registerFnSymbol(fnName, fnFnSym);
    }

    return nullptr;
}

// DRAFT-let-unify §3 + DRAFT-static-vars Phase 1：全局 `let NAME T = expr`。
// 三档：无注解（val，运行期 init） / #Cval（编译期常量） / #Mut（可变，Phase 2）。
// #Frozen 在全局位拒（E3116 既有语义——全局 #Frozen 无意义，用 #Cval 替代）。
std::any ASTBuilder::visitLetGlobal(yux::yuxParser::LetGlobalContext* ctx) {
    DEBUG_LOG("Visit: LetGlobal");
    auto file = any_cast_p<FileNode>(stack.back());
    auto flags = readLetAnnos(ctx->letAnnos);

    auto name = ctx->name;

    // #Frozen 在全局位拒（和 #Mut 一样先报 E3116）
    if (!flags.isCval && !flags.isMut && flags.isFrozen) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3116, name->getText());
    }

    // Phase 2: #Mut 全局 —— 运行期初始化，可变
    if (flags.isMut) {
        if (!ctx->type()) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3113, name->getText());
        }
        // 决议 [#1.A]: #Mut 全局无 init → E3154（v1 禁）
        if (!ctx->expr()) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3154, name->getText());
        }

        auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
        auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

        // E3155: 全局 init 内禁 try/catch (DRAFT-static-vars §6)
        if (exprContainsTryCatch(expr)) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3155, name->getText());
        }

        auto globalVar = createWithLine<GlobalVarNode>(ctx, file, name, typeNode, expr, /*isMutable=*/true);

        // Phase 3: const-eval 优先分流 —— #Mut 初始化器也试 const-eval
        // 成功 → ConstantInitializer（零运行期开销）；失败 → 降级 runtime init
        ConstEvaluator ev;
        ev.setFile(file);
        for (const auto& prior : file->getGlobalConsts()) {
            auto v = ev.eval(prior->value());
            if (v) ev.setNamedConst(prior->name().getText(), *v);
        }
        if (auto cv = ev.eval(expr)) {
            globalVar->setConstValue(std::move(*cv));
            DEBUG_LOG_VAL("  LetGlobal #Mut (const-eval)", name->getText());
        }

        file->addGlobalVar(globalVar);

        DEBUG_LOG_VAL("  LetGlobal #Mut", name->getText() << " : " << typeNode->getType().name);
        return globalVar;
    }

    // #Cval 档：走既有 const-eval 通路
    if (flags.isCval) {
        if (!ctx->type()) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3113, name->getText());
        }
        if (!ctx->expr()) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3114, name->getText());
        }

        auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
        auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

        // DRAFT-const-eval Phase 2: RHS 必须 const-evaluable。失败抛 E3140；溢出 / 除 0 抛 E3143。
        ConstEvaluator ev;
        ev.setFile(file);
        for (const auto& prior : file->getGlobalConsts()) {
            auto v = ev.eval(prior->value());
            if (v) ev.setNamedConst(prior->name().getText(), *v);
        }
        auto value = ev.eval(expr);
        if (!value) {
            throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                           ErrorCode::E3140, name->getText());
        }

        auto globalConst = createWithLine<GlobalConstNode>(ctx, file, name, typeNode, expr, flags.isInline);
        file->addGlobalConst(globalConst);

        DEBUG_LOG_VAL("  LetGlobal #Cval",
                      name->getText() << " : " << typeNode->getType().name << (flags.isInline ? " [inline]" : ""));
        return globalConst;
    }

    // #Inline 必须与 #Cval 组合使用，单独出现报错
    if (flags.isInline) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3117);
    }

    // DRAFT-static-vars Phase 1：默认 val 档 —— 运行期初始化全局变量
    if (!ctx->type()) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3113, name->getText());
    }
    // E3154: 全局 val 必须有 init
    if (!ctx->expr()) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3154, name->getText());
    }

    auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));

    // E3155: 全局 init 内禁 try/catch (DRAFT-static-vars §6)
    if (exprContainsTryCatch(expr)) {
        throw YuxError(static_cast<int>(name->getLine()), static_cast<int>(name->getCharPositionInLine()) + 1,
                       ErrorCode::E3155, name->getText());
    }

    auto globalVar = createWithLine<GlobalVarNode>(ctx, file, name, typeNode, expr);

    // Phase 3: const-eval 优先分流 —— 先试 ConstEvaluator
    // 成功 → ConstantInitializer（零运行期开销，不进 _yux_global_init）；
    // 失败 → 降级 runtime init（Phase 1 路径）
    ConstEvaluator ev;
    ev.setFile(file);
    for (const auto& prior : file->getGlobalConsts()) {
        auto v = ev.eval(prior->value());
        if (v) ev.setNamedConst(prior->name().getText(), *v);
    }
    if (auto cv = ev.eval(expr)) {
        globalVar->setConstValue(std::move(*cv));
        DEBUG_LOG_VAL("  LetGlobal val (const-eval)", name->getText());
    }

    file->addGlobalVar(globalVar);

    DEBUG_LOG_VAL("  LetGlobal val", name->getText() << " : " << typeNode->getType().name);
    return globalVar;
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
                            string grandchildMod = childMod;
                            grandchildMod += '.';
                            grandchildMod += child;
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
                            string subsubMod = childMod;
                            subsubMod += '.';
                            subsubMod += sub;
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
                    // rename 非空时以 rename 作为导出名（name as alias）
                    string exportedName = exportItem.rename.empty() ? exportItem.name : exportItem.rename;
                    auto childPathKind = _yux.modulePathKind(childMod);
                    bool alreadyWildcard = file->wildcardAliasSources(exportedName) != nullptr;

                    if (!alreadyWildcard && file->hasSymbol(exportedName)) {
                        DEBUG_LOG_VAL("    skip export (symbol exists)", exportedName);
                        continue;
                    }

                    if (childPathKind == Yux::ModulePathKind::File) {
                        // 文件模块：注册为模块别名
                        if (!alreadyWildcard) {
                            auto target = _yux.loadModule(childMod, line);
                            SymbolInfo aliasSym(SymbolKind::Module, exportedName, TypeInfo());
                            aliasSym.moduleName = childMod;
                            file->registerSymbol(exportedName, aliasSym);
                            file->addModuleAlias(exportedName, target);
                            DEBUG_LOG_VAL("    export module", exportedName << " -> " << childMod);
                        }
                        file->addWildcardAliasSource(exportedName, childMod);
                    } else if (childPathKind == Yux::ModulePathKind::Package) {
                        // 包：注册为包别名
                        if (!alreadyWildcard) {
                            SymbolInfo aliasSym(SymbolKind::Package, exportedName, TypeInfo());
                            aliasSym.moduleName = childMod;
                            file->registerSymbol(exportedName, aliasSym);
                            file->addPackageAlias(exportedName, childMod);
                            preloadPackageChildren(file, exportedName, childMod, "", line);
                            DEBUG_LOG_VAL("    export package", exportedName << " -> " << childMod);
                        }
                        file->addWildcardAliasSource(exportedName, childMod);
                    }
                }
            }
        } else {
            // 没有 pkg 文件：导出所有 .yux 和子目录（原有逻辑）
            // 通配注入的别名在 _wildcardAliasSources 中记录来源；已注入过的同名别名不
            // 立即报错，而是追加来源，留到使用点检查歧义（Phase 5）。
            for (auto& child : _yux.listPackageYuxChildren(modName)) {
                string childMod = modName;
                childMod += '.';
                childMod += child;
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
                string subMod = modName;
                subMod += '.';
                subMod += sub;
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

    auto aliasDecl = createWithLine<AliasDeclNode>(ctx, file, nameTok, static_cast<p<TypeNode>>(nullptr));
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

// ==================== E3155 辅助函数 ====================

// 递归检查表达式树中是否包含 try/catch 节点
// 用于全局 let 和 #Static 字段 init 表达式校验（DRAFT-static-vars §6）
bool ASTBuilder::exprContainsTryCatch(p<ExprNode> expr) {
    if (!expr) return false;

    // 直接命中 try/catch
    if (dynamic_cast<ExprTryCatchNode*>(expr)) return true;

    // 二元运算族（AddSub / MulDivMod / Shift / Compare / Eq / Bool / BinOp）
    if (auto* bin = dynamic_cast<ExprBinOpNode*>(expr)) {
        return exprContainsTryCatch(bin->left()) || exprContainsTryCatch(bin->right());
    }

    // a ?? b（Nullable 回退，不继承 ExprBinOpNode）
    if (auto* ne = dynamic_cast<ExprNullElseNode*>(expr)) {
        return exprContainsTryCatch(ne->left()) || exprContainsTryCatch(ne->right());
    }

    // 一元运算
    if (auto* un = dynamic_cast<ExprUnaryNode*>(expr)) {
        return exprContainsTryCatch(un->right());
    }

    // 函数调用
    if (auto* call = dynamic_cast<ExprCallNode*>(expr)) {
        if (exprContainsTryCatch(call->getCalleeExpr())) return true;
        for (auto& a : call->getArgs()) {
            if (exprContainsTryCatch(a)) return true;
        }
        return false;
    }

    // 成员访问
    if (auto* dot = dynamic_cast<ExprDotNode*>(expr)) {
        return exprContainsTryCatch(dot->baseExpr());
    }

    // if-else 表达式 —— condition / 单行分支直接是 ExprNode 可递归
    // 多行块 if-else 的分支是 StatementBlockNode，其内部 try/catch 暂不递归
    // （全局 init 中直接写块式 if-else 已属罕见，嵌套 try/catch 更是边缘）
    if (auto* ol = dynamic_cast<ExprOneLineIfElseNode*>(expr)) {
        if (exprContainsTryCatch(ol->condition())) return true;
        if (exprContainsTryCatch(ol->trueValue())) return true;
        if (exprContainsTryCatch(ol->falseValue())) return true;
        return false;
    }

    // 括号
    if (auto* paren = dynamic_cast<ExprParenNode*>(expr)) {
        return exprContainsTryCatch(paren->expr());
    }

    // 数组字面量（ArrayInitNode 的 value 是 LiteralNode，不含 try/catch，跳过）
    if (auto* arr = dynamic_cast<ExprArrayNode*>(expr)) {
        for (auto& e : arr->elements()) {
            if (exprContainsTryCatch(e)) return true;
        }
        return false;
    }

    // match 表达式
    if (auto* m = dynamic_cast<ExprMatchNode*>(expr)) {
        if (exprContainsTryCatch(m->scrutinee())) return true;
        for (auto& arm : m->arms()) {
            if (arm->hasBlock()) {
                auto& blk = arm->block();
                if (blk->hasResult() && exprContainsTryCatch(blk->resultExpr())) return true;
                for (auto& s : blk->statements()) {
                    if (auto se = dynamic_cast<StatementExprNode*>(s)) {
                        if (exprContainsTryCatch(se->expr())) return true;
                    }
                }
            } else if (exprContainsTryCatch(arm->body())) {
                return true;
            }
        }
        return false;
    }

    // 构造器形态
    if (auto* sl = dynamic_cast<ExprStructLitNode*>(expr)) {
        for (auto& fi : sl->fields()) {
            if (exprContainsTryCatch(fi->value())) return true;
        }
        return false;
    }
    if (auto* dynCtor = dynamic_cast<ExprDynCtorNode*>(expr)) {
        return exprContainsTryCatch(dynCtor->arg());
    }
    if (auto* pc = dynamic_cast<ExprPathCallNode*>(expr)) {
        for (auto& a : pc->args()) {
            if (exprContainsTryCatch(a)) return true;
        }
        return false;
    }

    // 下标 / 取值（a[i] / &a / *a）
    if (auto* g = dynamic_cast<ExprGetNode*>(expr)) {
        if (exprContainsTryCatch(g->arrayExpr())) return true;
        for (auto& idx : g->indices()) {
            if (exprContainsTryCatch(idx)) return true;
        }
        return false;
    }

    // 元组
    if (auto* tup = dynamic_cast<ExprTupleNode*>(expr)) {
        for (auto& e : tup->elements()) {
            if (exprContainsTryCatch(e)) return true;
        }
        return false;
    }

    // 字面量 / 变量引用 / ExprGetRefNode / lambda 等 → 叶子，不含 try/catch
    return false;
}
