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
// NoReturn / Fallible 由 DRAFT-错误.md 引入（spec §11.5.1）：
//   #NoReturn        零参；标在 fn / structImpl 内方法上
//   #Fallible(E)     单参；E 为错误 enum 类型名（语义校验推 10e）
const set<string>& knownAnnos() {
    static const set<string> s = {"CompilerInner", "Test", "DraftLike", "NoReturn", "Fallible"};
    return s;
}

// 单参注解白名单（spec §11.1.1.1）。其它注解出现 (arg) 形式视为非法（E2005 形式错配）。
const set<string>& argAnnos() {
    static const set<string> s = {"Fallible"};
    return s;
}

// 注解可附着位置的限定集合
// fn 之外的位置（structDecl / structImpl / extern / globalConst）只接受 #CompilerInner，
// 不接受 #Test（spec §11.3.1.2）
const set<string>& nonFnAllowedAnnos() {
    static const set<string> s = {"CompilerInner"};
    return s;
}

// 注解名 + 单参槽位（与 _annoArgs 对齐）。无参注解的 args[i] 为空字符串。
struct AnnoList {
    vector<string> names;
    vector<string> args;
};

// 校验单参 / 零参形态：argAnnos() 中的注解必须带 (ID)，否则缺参；其他注解出现 (ID) 视为多余。
template<typename A>
static void checkAnnoArity(A* a, const string& name, bool hasArg) {
    bool needArg = argAnnos().contains(name);
    if (needArg && !hasArg) {
        throw YuxError(
            static_cast<int>(a->name->getLine()),
            static_cast<int>(a->name->getCharPositionInLine()) + 1,
            ErrorCode::E2005, name);
    }
    if (!needArg && hasArg) {
        throw YuxError(
            static_cast<int>(a->name->getLine()),
            static_cast<int>(a->name->getCharPositionInLine()) + 1,
            ErrorCode::E2005, name);
    }
}

template<typename AnnoVec>
AnnoList collectAnnos(const AnnoVec& annos) {
    AnnoList out;
    for (auto* a : annos) {
        string name = a->name->getText();
        if (!knownAnnos().contains(name)) {
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E2005, name);
        }
        // §12.4.1.1：DraftLike 只能标在 draft 声明；其它位置（fn / struct / impl / extern / global）报 E1110
        if (name == "DraftLike") {
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E1110);
        }
        string arg;
        if (a->arg) arg = a->arg->getText();
        checkAnnoArity(a, name, !arg.empty());
        out.names.push_back(std::move(name));
        out.args.push_back(std::move(arg));
    }
    return out;
}

// 仅 visitDraftDecl 使用：白名单同 collectAnnos，但保留 DraftLike
template<typename AnnoVec>
AnnoList collectAnnosForDraft(const AnnoVec& annos) {
    AnnoList out;
    for (auto* a : annos) {
        string name = a->name->getText();
        if (!knownAnnos().contains(name)) {
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E2005, name);
        }
        // draft 声明上 #Test 不合法（§11.3.1.2）
        if (name == "Test") {
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E2011, name);
        }
        string arg;
        if (a->arg) arg = a->arg->getText();
        checkAnnoArity(a, name, !arg.empty());
        out.names.push_back(std::move(name));
        out.args.push_back(std::move(arg));
    }
    return out;
}

// 用于非 fn 位置（struct / extern / globalConst）：进一步收紧到 fn-only 注解清单
template<typename AnnoVec>
AnnoList collectAnnosNonFn(const AnnoVec& annos) {
    AnnoList out = collectAnnos(annos);
    for (size_t i = 0; i < out.names.size(); ++i) {
        if (!nonFnAllowedAnnos().contains(out.names[i])) {
            // 取对应的 token 用于行列号
            auto* a = annos[i];
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E2011, out.names[i]);
        }
    }
    return out;
}

// 用于 extern 块内 fnHeader：允许 `CompilerInner` 与 `#NoReturn`（DRAFT-错误.md §8.3）。
// `#Fallible` 在 extern 上仍被推迟（[#7]），不在白名单。
const set<string>& externFnAllowedAnnos() {
    static const set<string> s = {"CompilerInner", "NoReturn"};
    return s;
}

template<typename AnnoVec>
AnnoList collectAnnosExternFn(const AnnoVec& annos) {
    AnnoList out = collectAnnos(annos);
    for (size_t i = 0; i < out.names.size(); ++i) {
        if (!externFnAllowedAnnos().contains(out.names[i])) {
            auto* a = annos[i];
            throw YuxError(
                static_cast<int>(a->name->getLine()),
                static_cast<int>(a->name->getCharPositionInLine()) + 1,
                ErrorCode::E2011, out.names[i]);
        }
    }
    return out;
}

// Phase 10d-1：`#NoReturn` 头部级语义校验（E7012 / E7013）
// 不依赖 fn body，仅看 header 注解 + retType。E7014（流终止）与调用点流终止注册推 10d-2。
//   E7012 — `#NoReturn` 函数声明带返回类型
//   E7013 — `#NoReturn` 与 `#Fallible(E)` 互斥
static void checkNoReturnHeader(p<FnHeaderNode> header) {
    if (!header->hasAnno("NoReturn")) return;
    int line = header->getLineNumber();
    int col = header->getColumn();
    if (header->retType()) {
        throw YuxError(line, col, ErrorCode::E7012, header->name().getText());
    }
    if (header->hasAnno("Fallible")) {
        // 取 #Fallible 的单参 E（若解析得到则填，否则空字符串）
        auto eOpt = header->getAnnoArg("Fallible");
        string e = eOpt.value_or("");
        throw YuxError(line, col, ErrorCode::E7013, e);
    }
}

} // namespace

ASTBuilder::ASTBuilder(Yux& yux, const string& moduleName, bool isSdk,
                       bool isTestFile, const string& sourcePath) :
    _yux(yux), _isSdk(isSdk), _isTestFile(isTestFile),
    _moduleName(moduleName), _sourcePath(sourcePath) {
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
            throw YuxError(
                static_cast<int>(header->name->getLine()),
                static_cast<int>(header->name->getCharPositionInLine()) + 1,
                ErrorCode::E7012, header->name->getText());
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
                throw YuxError(header->getStart()->getLine(), ErrorCode::E2031,
                               fnName, "parameters");
            }
        }
        if (retType.isFn()) {
            throw YuxError(header->getStart()->getLine(), ErrorCode::E2031,
                           fnName, "return type");
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

std::any ASTBuilder::visitGlobalConst(yux::yuxParser::GlobalConstContext* ctx) {
    DEBUG_LOG("Visit: GlobalConst");
    auto file = any_cast_p<FileNode>(stack.back());
    // globalConst 不接受 #Test（spec §11.3.1.2）
    (void)collectAnnosNonFn(ctx->buildAnnos);

    auto name = ctx->name;
    auto typeNode = any_cast_p<TypeNode>(visit(ctx->type()));
    auto literal = any_cast_p<LiteralNode>(visit(ctx->literal()));
    
    auto globalConst = createWithLine<GlobalConstNode>(ctx, file, name, typeNode, literal);
    file->addGlobalConst(globalConst);
    
    DEBUG_LOG_VAL("  GlobalConst", name->getText() << " : " << typeNode->getType().name);
    return p<GlobalConstNode>(globalConst);
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
        // 预扫 #NoReturn：避免在头部注册阶段重复 collectAnnos 校验
        for (auto* a : header->buildAnnos) {
            if (a->name->getText() == "NoReturn") {
                fnFnSym.isNoReturn = true;
                break;
            }
        }
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
            methodFnSym.isNoReturn = method->header()->hasAnno("NoReturn");
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

    // ==================== #NoReturn 头部校验 (E7012 / E7013，DRAFT-错误.md §8.3) ====================
    checkNoReturnHeader(header);

    // ==================== #Test 注解校验 (spec §11.3) ====================
    // 仅 *.test.yux 允许；与 #CompilerInner 互斥；签名 `fn name(): void`、必须有体。
    if (header->hasAnno("Test")) {
        // 注意：header->name() 返回 Token 值类型，getText() 返回的 const string& 绑定到临时对象会悬挂；按值拷贝
        const string fnName = header->name().getText();
        int annoLine = header->getLineNumber();
        int annoCol = header->getColumn();

        if (!_isTestFile) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2014,
                           _sourcePath.empty() ? _moduleName : _sourcePath);
        }
        if (header->hasAnno("CompilerInner")) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2013, fnName);
        }
        bool sigOk = true;
        // 不允许有参数
        if (auto fnParamsCtx = ctx->fnHeader()->fnParams()) {
            if (!fnParamsCtx->fnParam().empty()) sigOk = false;
        }
        // 不允许有返回类型标注
        if (ctx->fnHeader()->retType) sigOk = false;
        // 必须有函数体
        if (!ctx->fnBody()) sigOk = false;
        if (!sigOk) {
            throw YuxError(annoLine, annoCol, ErrorCode::E2012, fnName, fnName);
        }
    }

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
                header->getLineNumber(), header->getColumn(),
                ErrorCode::E2006, header->name().getText())
                .withHint("普通函数必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#CompilerInner` 注解");
        }
        DEBUG_LOG("  Body: (compiler-synthesized)");
    } else if (ctx->fnBody()->fnExprkBody()) {
        DEBUG_LOG("  Body: Expression");
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
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
            retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
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
        retType = buildTypeWithRef(ctx->retType, file);
        DEBUG_LOG_VAL("    Return type", retType->getType().getFullName());
    }

    auto header = createWithLine<FnHeaderNode>(ctx, file, ctx->name, retType);
    {
        auto al = collectAnnos(ctx->buildAnnos);
        header->setAnnos(std::move(al.names), std::move(al.args));
    }

    // spec §6.3.X：返回 T& 受溯源约束（根须为 $ 或某 T& 形参），由 borrow_checker 在
    // fn body 检查时强制（E4010）；此处只放过 #CompilerInner 与有"潜在源"的用户函数。
    // 顶层 free fn 的 "无 T& 形参" 这种 0 源情况此处看不到（我们还没解析完形参），
    // 同样交给 borrow_checker 在拿到完整 fn 后判定。
    (void)retType; // 闸门已撤；保留语义校验给后续阶段

    if (auto gd = ctx->genericDef()) {
        vector<string> typeParams;
        vector<vector<string>> typeParamBounds;
        for (auto pCtx : gd->params) {
            // 形参名：取 typeParam.type 的 typeNormal 分支 ID
            string paramName;
            if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(pCtx->type(0))) {
                paramName = tn->ID()->getText();
            }
            typeParams.push_back(paramName);

            // 边界：typeParam.bounds 中每个 type → 取名（仅支持 typeNormal / typeGeneric 的基名）
            vector<string> bounds;
            for (auto bCtx : pCtx->bounds) {
                if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(bCtx)) {
                    bounds.push_back(tn->ID()->getText());
                } else if (auto tg = dynamic_cast<yux::yuxParser::TypeGenericContext*>(bCtx)) {
                    bounds.push_back(tg->ID()->getText());
                }
            }
            typeParamBounds.push_back(std::move(bounds));
        }
        header->setTypeParams(typeParams);
        header->setTypeParamBounds(typeParamBounds);
        for (size_t i = 0; i < header->typeParams().size(); ++i) {
            DEBUG_LOG_VAL("    TypeParam", header->typeParams()[i]);
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
    auto type = buildTypeWithRef(ctx->typeWithRef(), parent);
    DEBUG_LOG_VAL("    Param", ctx->name->getText() << " : " << type->getType().name);
    
    vector<p<FnParamNode>> params;
    params.push_back(p<FnParamNode>(createWithLine<FnParamNode>(ctx, parent, ctx->name, type)));
    return params;
}

std::any ASTBuilder::visitFnParamGroup(yux::yuxParser::FnParamGroupContext* ctx) {
    p<Node> parent = any_cast_p<FnHeaderNode>(stack.back());
    auto type = buildTypeWithRef(ctx->typeWithRef(), parent);
    
    vector<p<FnParamNode>> params;
    for (auto nameToken : ctx->names) {
        DEBUG_LOG_VAL("    Param (group)", nameToken->getText() << " : " << type->getType().name);
        params.push_back(p<FnParamNode>(createWithLine<FnParamNode>(ctx, parent, nameToken, type)));
    }
    return params;
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
                throw YuxError(
                    tk ? (int)tk->getSymbol()->getLine() : 0,
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
    DEBUG_LOG_VAL("Visit: AliasDecl",
        nameTok->getText() << " -> " << (target ? target->getType().name : string("?")));
    file->addAliasDecl(aliasDecl);
    return p<AliasDeclNode>(aliasDecl);
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
            throw YuxError(
                static_cast<int>(vCtx->name->getLine()),
                static_cast<int>(vCtx->name->getCharPositionInLine()) + 1,
                ErrorCode::E2018,
                variant->name().getText(), ctx->name->getText());
        }
    }

    _scopeStack.pop_back();
    stack.pop_back();

    file->addEnumDecl(enumDecl);
    return p<EnumDeclNode>(enumDecl);
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
    return p<EnumVariantNode>(variant);
}

std::any ASTBuilder::visitStructDecl(yux::yuxParser::StructDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto* stCtx = ctx->structType();
    auto structDecl = createWithLine<StructDeclNode>(ctx, file, stCtx->name);
    // structDecl 不接受 #Test（spec §11.3.1.2）
    {
        auto al = collectAnnosNonFn(ctx->buildAnnos);
        structDecl->setAnnos(std::move(al.names), std::move(al.args));
    }

    DEBUG_LOG_VAL("Visit: StructDecl", stCtx->name->getText());

    vector<string> typeParams;
    for (auto tCtx : stCtx->types) {
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
    auto* stCtx = ctx->structType();
    auto structImpl = createWithLine<StructImplNode>(ctx, file, stCtx->name);
    // structImpl 块本身不接受 #Test（spec §11.3.1.2）；其内部方法通过 visitFn 处理
    {
        auto al = collectAnnosNonFn(ctx->buildAnnos);
        structImpl->setAnnos(std::move(al.names), std::move(al.args));
    }

    DEBUG_LOG_VAL("Visit: StructImpl", stCtx->name->getText());

    vector<string> typeParams;
    for (auto tCtx : stCtx->types) {
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

    string structName = stCtx->name->getText();

    // spec §12.2 收集 `Type : D1 + D2` 中的 draft 列表
    {
        vector<DraftRef> refs;
        for (auto* dCtx : ctx->drafts) {
            DraftRef r;
            r.name = dCtx->name->getText();
            for (auto* tCtx : dCtx->types) {
                auto tn = any_cast_p<TypeNode>(visit(tCtx));
                r.typeArgs.push_back(tn->getType());
            }
            if (auto* st = dCtx->getStart()) {
                r.line = (int)st->getLine();
                r.col = static_cast<int>(st->getCharPositionInLine()) + 1;
            }
            refs.push_back(std::move(r));
        }
        structImpl->setDraftRefs(std::move(refs));
    }

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

        // #NoReturn 头部校验（E7012 / E7013）也覆盖 structImpl 内方法
        checkNoReturnHeader(header);

        stack.emplace_back(fn);
        _scopeStack.push_back(fn);

        for (auto& tp : structImpl->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }
        for (auto& tp : header->typeParams()) {
            fn->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        }

        // Phase 4e: receiver `$` 类型登记为 Self&（Ref<Self>）。IR 层仍是非空指针；
        // sym.type 走 Ref 让 §3 借用规则统一适用（&$.field、传 Self& 形参等）。
        // 字段 / 方法访问点已就位 Ref 自动剥皮（compileDotExpr / compileGetRefExpr / LiteralObjNode::getType）。
        {
            vector<sp<TypeInfo>> selfArgs;
            selfArgs.push_back(make_shared<TypeInfo>(structName));
            fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
        }

        for (auto param : header->params()) {
            TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
            fn->registerSymbol(param->name().getText(), {SymbolKind::Variable, param->name().getText(), paramType});
        }

        if (!fnCtx->fnBody()) {
            if (!header->hasAnno("CompilerInner")) {
                throw YuxError(
                    header->getLineNumber(), header->getColumn(),
                    ErrorCode::E2007, structName, header->name().getText())
                    .withHint("结构体方法必须有函数体；若仅声明（由编译器内部提供实现），在签名上加 `#CompilerInner` 注解");
            }
        } else if (fnCtx->fnBody()->fnExprkBody()) {
            auto exprBody = fnCtx->fnBody()->fnExprkBody();
            auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
            auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
            retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
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
                retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
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

std::any ASTBuilder::visitDraftDecl(yux::yuxParser::DraftDeclContext* ctx) {
    auto file = any_cast_p<FileNode>(stack.back());
    auto* dt = ctx->draftType();
    auto draft = createWithLine<DraftDeclNode>(ctx, file, dt->name);
    // draft 声明位允许 #DraftLike + #CompilerInner（§11.4）；#Test 不合法
    {
        auto al = collectAnnosForDraft(ctx->buildAnnos);
        draft->setAnnos(std::move(al.names), std::move(al.args));
    }

    DEBUG_LOG_VAL("Visit: DraftDecl", dt->name->getText());

    // §12.1.1.3 draft 自身可带泛型形参；体内 fn 不得再有泛型（在 visitFnHeader 后校验）
    vector<string> typeParams;
    for (auto* tCtx : dt->types) {
        if (auto tn = dynamic_cast<yux::yuxParser::TypeNormalContext*>(tCtx)) {
            typeParams.push_back(tn->ID()->getText());
        }
    }
    draft->setTypeParams(typeParams);

    stack.emplace_back(draft);
    _scopeStack.push_back(draft);

    for (auto& tp : draft->typeParams()) {
        draft->registerSymbol(tp, {SymbolKind::TypeParam, tp, TypeInfo(tp)});
        DEBUG_LOG_VAL("    TypeParam", tp);
    }

    bool isDraftLike = draft->isDraftLike();
    string draftName = dt->name->getText();

    for (auto* fnHeaderCtx : ctx->fnHeader()) {
        auto header = any_cast_p<FnHeaderNode>(visitFnHeader(fnHeaderCtx));

        // §12.3.2 draft 体内单个 fn 不得引入本地泛型；§12.4.2 #DraftLike 也不得共用
        if (header->isGeneric()) {
            int line = header->getLineNumber();
            int col = header->getColumn();
            if (isDraftLike) {
                throw YuxError(line, col, ErrorCode::E1112, draftName);
            }
            throw YuxError(line, col, ErrorCode::E1104, draftName, header->name().getText());
        }
        draft->addSignature(header);
    }

    _scopeStack.pop_back();
    stack.pop_back();

    file->addDraftDecl(draft);
    return p<DraftDeclNode>(draft);
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
        // Phase 4e: 析构函数 receiver 同 §4e：Self&
        vector<sp<TypeInfo>> selfArgs;
        selfArgs.push_back(make_shared<TypeInfo>(structName));
        fn->registerSymbol("$", {SymbolKind::Variable, "$", TypeInfo("Ref", selfArgs)});
    }
    
    if (ctx->fnBody()->fnExprkBody()) {
        auto exprBody = ctx->fnBody()->fnExprkBody();
        auto expr = any_cast_p<ExprNode>(visit(exprBody->expr()));
        auto retStmt = createWithLine<StatementRetNode>(ctx, fn, expr);
        retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
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
            retStmt->setLocation(resultExpr->resolveLineNumber(), resultExpr->resolveColumn());
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
    if (auto twr = ctx->typeWithRef(); twr) {
        type = buildTypeWithRef(twr, scope);
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

// 元组解构声明：var (a, b, ...) = expr 或 var (a, b) (T1, T2) = expr
// Phase 5：仅支持一层平铺 ID，不支持嵌套和 _
std::any ASTBuilder::visitStatementDeclareAssignTuple(yux::yuxParser::StatementDeclareAssignTupleContext* ctx) {
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

    p<TypeNode> type = nullptr;
    if (auto twr = ctx->typeWithRef(); twr) {
        type = buildTypeWithRef(twr, scope);
    }

    // 元组类型来源：显式标注 > expr 推断
    TypeInfo wholeType;
    if (type) {
        wholeType = type->getType();
    } else {
        wholeType = expr->getType();
    }

    vector<Token> names;
    for (auto idTok : ctx->names) {
        names.emplace_back(idTok);
    }

    // 元素数 / 类型校验在 codegen 阶段（compileDeclareAssignTupleStatement）做，
    // 因为这里 wholeType 可能是 alias 名，未走 applySubst
    // 提前注册符号：每个 ID 走元组对应位置的元素类型
    if (scope && wholeType.isTuple() && wholeType.tupleElements().size() == names.size()) {
        const auto& elems = wholeType.tupleElements();
        for (size_t i = 0; i < names.size(); ++i) {
            scope->registerSymbol(names[i].getText(),
                {SymbolKind::Variable, names[i].getText(), *elems[i], declType == DeclareType::Var});
        }
    } else if (scope) {
        // alias 或非元组：先用 wholeType 占位（codegen 时会再校验），按未知类型挂到符号表
        // TODO: alias 透明替换的元素类型在 ast_builder 阶段不易解析，留给 compiler 验证 + 报错
        for (auto& n : names) {
            scope->registerSymbol(n.getText(),
                {SymbolKind::Variable, n.getText(), TypeInfo(), declType == DeclareType::Var});
        }
    }

    DEBUG_LOG_VAL("  Statement: DeclareTuple", names.size() << " names, expr type=" << wholeType.name);

    return p<StatementNode>(createWithLine<StatementDeclareAssignTupleNode>(ctx, scope, declType, names, type, expr));
}

std::any ASTBuilder::visitStatementAssign(yux::yuxParser::StatementAssignContext* ctx) {
    auto scope = currentScope();
    auto expr = any_cast_p<ExprNode>(visit(ctx->expr()));
    vector<Token> subs;
    // DOT_NUM token (如 ".0") 文本带前导 '.'，剥掉以保持 sub 仅是字段名 / 数字
    for (auto sub : ctx->subs) {
        auto text = sub->getText();
        if (!text.empty() && text[0] == '.') {
            subs.push_back(Token(text.substr(1), sub->getLine()));
        } else {
            subs.push_back(Token(sub));
        }
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
    retStmt->setLocation(expr->resolveLineNumber(), expr->resolveColumn());
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

// 收集 lambdaParams 上下文为 LambdaParamSlot 列表
// g4 lambdaParam 两 alt：
//   - lambdaParamGroup：names+= ID (',' names+= ID)+ typeWithRef?  → 组糖，N 形参共享同类型
//   - lambdaParamStd：name=ID typeWithRef?                          → 单形参可省类型
// 类型省时 type=nullptr，由调用 / 赋值点的 fn 类型反推（Phase 2b）
static vector<LambdaParamSlot> collectLambdaParams(
    ASTBuilder* self, yux::yuxParser::LambdaParamsContext* params,
    p<Node> parent,
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
                out.push_back(LambdaParamSlot{Token(idTok->getText(), (int)idTok->getLine()), sharedType});
            }
        } else if (auto* s = dynamic_cast<yux::yuxParser::LambdaParamStdContext*>(lp)) {
            p<TypeNode> ty = nullptr;
            if (auto* twr = s->typeWithRef()) {
                ty = (self->*buildTwr)(twr, parent);
            }
            out.push_back(LambdaParamSlot{Token(s->name->getText(), (int)s->name->getLine()), ty});
        }
    }
    return out;
}

// 把 trailingLambda 上下文转为 LambdaExprNode（块形）
// trailingLambdaBlock：'{' lambdaParams '=>' stmts '}'  → Form::Block
// trailingLambdaZeroBlock：'{' stmts '}'                → Form::ZeroBlock
static p<LambdaExprNode> buildTrailingLambda(
    ASTBuilder* self, yux::yuxParser::TrailingLambdaContext* tl,
    p<ScopeNode> scope,
    p<TypeNode> (ASTBuilder::*buildTwr)(yux::yuxParser::TypeWithRefContext*, p<Node>),
    std::function<p<LambdaExprNode>(yux::yuxParser::TrailingLambdaContext*,
                                     LambdaExprNode::Form,
                                     vector<LambdaParamSlot>,
                                     vector<p<StatementNode>>)> create) {
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
                throw YuxError(
                    tk ? (int)tk->getSymbol()->getLine() : 0,
                    tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                    ErrorCode::E2015);
            }
            typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
        }
        call->setTypeArgs(std::move(typeArgs));
    }
    // 尾随 lambda 糖 §4.5：f(args) { ... } → 等价 f(args, { ... })
    if (auto* tl = ctx->trailing) {
        auto lambda = buildTrailingLambda(this, tl, scope,
            &ASTBuilder::buildTypeWithRef,
            [this](auto* tlCtx, LambdaExprNode::Form form,
                   vector<LambdaParamSlot> params, vector<p<StatementNode>> stmts) {
                return createWithLine<LambdaExprNode>(tlCtx, currentScope(), form,
                    std::move(params), nullptr, nullptr, std::move(stmts));
            });
        if (lambda) call->addArg(p<ExprNode>(lambda));
    }
    return p<ExprNode>(call);
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
                throw YuxError(
                    tk ? (int)tk->getSymbol()->getLine() : 0,
                    tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                    ErrorCode::E2015);
            }
            typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
        }
        call->setTypeArgs(std::move(typeArgs));
    }
    auto lambda = buildTrailingLambda(this, ctx->trailing, scope,
        &ASTBuilder::buildTypeWithRef,
        [this](auto* tlCtx, LambdaExprNode::Form form,
               vector<LambdaParamSlot> params, vector<p<StatementNode>> stmts) {
            return createWithLine<LambdaExprNode>(tlCtx, currentScope(), form,
                std::move(params), nullptr, nullptr, std::move(stmts));
        });
    if (lambda) call->addArg(p<ExprNode>(lambda));
    return p<ExprNode>(call);
}

// 创建 lambda body 的内层作用域，登记形参符号；caller 负责 push/pop _scopeStack。
// 用 LambdaScopeNode 结构区分于普通 ScopeNode，便于未来 sema 区分（如自由变量诊断）。
namespace {
class LambdaScopeNode : public ScopeNode {
public:
    explicit LambdaScopeNode(const p<Node>& parent) : ScopeNode(parent) {}
};
}

static p<ScopeNode> makeLambdaBodyScope(
    const p<ScopeNode>& parentScope, const vector<LambdaParamSlot>& params) {
    auto scope = p<LambdaScopeNode>(new LambdaScopeNode(parentScope));
    scope->setParentScope(parentScope);
    for (auto& slot : params) {
        TypeInfo t = slot.type ? slot.type->getType() : TypeInfo();
        scope->registerSymbol(slot.name.getText(),
            {SymbolKind::Variable, slot.name.getText(), t});
    }
    return scope;
}

// Lambda 表达式形：x => expr  （裸单参，类型由上下文反推）
std::any ASTBuilder::visitExprLambdaSingle(yux::yuxParser::ExprLambdaSingleContext* ctx) {
    auto scope = currentScope();
    vector<LambdaParamSlot> params;
    params.push_back(LambdaParamSlot{
        Token(ctx->name->getText(), (int)ctx->name->getLine()), nullptr });
    auto bodyScope = makeLambdaBodyScope(scope, params);
    _scopeStack.push_back(bodyScope);
    auto bodyExpr = any_cast_p<ExprNode>(visit(ctx->body->expr()));
    _scopeStack.pop_back();
    DEBUG_LOG("    Expr: LambdaSingle");
    auto node = createWithLine<LambdaExprNode>(ctx, scope,
        LambdaExprNode::Form::Single, std::move(params),
        nullptr, bodyExpr, vector<p<StatementNode>>{});
    node->setBodyScope(bodyScope);
    return p<ExprNode>(node);
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
    auto node = createWithLine<LambdaExprNode>(ctx, scope,
        LambdaExprNode::Form::Paren, std::move(params),
        retType, bodyExpr, vector<p<StatementNode>>{});
    node->setBodyScope(bodyScope);
    return p<ExprNode>(node);
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
    auto node = createWithLine<LambdaExprNode>(ctx, scope,
        LambdaExprNode::Form::Block, std::move(params),
        nullptr, nullptr, std::move(stmts));
    node->setBodyScope(bodyScope);
    return p<ExprNode>(node);
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
    auto node = createWithLine<LambdaExprNode>(ctx, scope,
        LambdaExprNode::Form::ZeroBlock, vector<LambdaParamSlot>{},
        p<TypeNode>(nullptr), p<ExprNode>(nullptr), std::move(stmts));
    node->setBodyScope(bodyScope);
    return p<ExprNode>(node);
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

std::any ASTBuilder::visitLiteralStringLineRaw(yux::yuxParser::LiteralStringLineRawContext* ctx) {
    auto token = ctx->STR_LINE_RAW()->getSymbol();
    DEBUG_LOG_VAL("      Literal: StringRaw", token->getText());
    return p<LiteralNode>(createWithLine<LiteralStringNode>(ctx, token, true));
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
    for (size_t i = 0; i < raw.size(); ) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char esc = raw[i + 1];
            switch (esc) {
                case 'n': out += '\n'; i += 2; break;
                case 'r': out += '\r'; i += 2; break;
                case 't': out += '\t'; i += 2; break;
                case '0': out += '\0'; i += 2; break;
                case '\\': out += '\\'; i += 2; break;
                case '"': out += '"'; i += 2; break;
                case '$': out += '$'; i += 2; break;
                case 'x':
                    if (i + 3 < raw.size()) {
                        u8 v = static_cast<u8>(std::stoi(raw.substr(i + 2, 2), nullptr, 16));
                        out += static_cast<char>(v);
                        i += 4;
                    } else { out += raw[i]; ++i; }
                    break;
                case 'u':
                    if (i + 5 < raw.size()) {
                        u32 cp = static_cast<u32>(std::stoi(raw.substr(i + 2, 4), nullptr, 16));
                        appendUtf8(out, cp);
                        i += 6;
                    } else { out += raw[i]; ++i; }
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
            auto expr = createWithLine<ExprLiteralNode>(part, scope, p<LiteralNode>(obj));
            interps.push_back(p<ExprNode>(expr));
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
        return p<LiteralNode>(createWithLine<LiteralStringNode>(ctx, synTok, true));
    }

    return p<LiteralNode>(createWithLine<StringTemplateNode>(
        ctx, Token(openTok), std::move(parts), std::move(interps)));
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
    return p<ExprNode>(createWithLine<ExprTupleNode>(ctx, scope, std::move(elements)));
}

// 元组成员访问 a.0
// 复用 ExprDotNode（member token 为 DOT_NUM，文本形如 ".0"），不支持 ?. 安全访问
// 链式 t.0.0 由 parser 左递归后缀重复匹配，每个 DOT_NUM 单独一个 ExprDotNode
std::any ASTBuilder::visitExprTupleMember(yux::yuxParser::ExprTupleMemberContext* ctx) {
    DEBUG_LOG_VAL("    Expr: TupleMember", "member: " << ctx->member->getText());
    auto scope = currentScope();
    auto base = any_cast_p<ExprNode>(visit(ctx->left));
    return p<ExprNode>(createWithLine<ExprDotNode>(ctx, scope, base, ctx->member, false));
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

// 枚举构造表达式：E::V / E::V() / E::V(args)
// AST 不解析 enum 是否存在 / variant 是否合法 / arity 是否匹配；这些都留到编译期
// 别名透传（C::V => E::V）由 ExprEnumCtorNode::getType 在查询时解析
std::any ASTBuilder::visitExprEnumCtor(yux::yuxParser::ExprEnumCtorContext* ctx) {
    DEBUG_LOG_VAL("    Expr: EnumCtor",
        ctx->enumName->getText() << "::" << ctx->variant->getText());
    auto scope = currentScope();
    auto node = createWithLine<ExprEnumCtorNode>(ctx, scope, ctx->enumName, ctx->variant);
    for (auto* aCtx : ctx->args) {
        node->addArg(any_cast_p<ExprNode>(visit(aCtx)));
    }
    return p<ExprNode>(node);
}

// match 模式：E::V / E::V() / E::V(b1, b2, ...)
// 绑定名重复在 ast 阶段不查（v1 留给 codegen 报 E2027）
std::any ASTBuilder::visitPatternEnum(yux::yuxParser::PatternEnumContext* ctx) {
    DEBUG_LOG_VAL("    Pattern: Enum",
        ctx->enumName->getText() << "::" << ctx->variant->getText());
    auto scope = currentScope();
    vector<Token> binds;
    binds.reserve(ctx->binds.size());
    for (auto* tk : ctx->binds) binds.emplace_back(tk);
    return p<EnumPatternNode>(createWithLine<EnumPatternNode>(
        ctx, scope, ctx->enumName, ctx->variant, std::move(binds)));
}

// match 模式：else 兜底
std::any ASTBuilder::visitPatternElse(yux::yuxParser::PatternElseContext* ctx) {
    DEBUG_LOG("    Pattern: Else");
    auto scope = currentScope();
    Token elseTok = ctx->Else()->getSymbol();
    return p<EnumPatternNode>(createWithLine<EnumPatternNode>(ctx, scope, elseTok));
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
    auto arm = createWithLine<MatchArmNode>(ctx, outer, pattern, p<ExprNode>(nullptr));
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

        EnumVariantNode* variant = enumDecl
            ? enumDecl->variant(pattern->variantName().getText())
            : nullptr;
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
    return p<MatchArmNode>(fullArm);
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
    return p<ExprNode>(createWithLine<ExprMatchNode>(ctx, scope, scrutinee, std::move(arms)));
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
    for (auto pCtx : ctx->genericDef()->params) {
        // 类型引用位不允许 bound（spec §B.2 / §12 仅声明位允许）
        if (!pCtx->bounds.empty()) {
            auto* tk = pCtx->SymbolColon();
            throw YuxError(
                tk ? (int)tk->getSymbol()->getLine() : 0,
                tk ? static_cast<int>(tk->getSymbol()->getCharPositionInLine()) + 1 : 0,
                ErrorCode::E2015);
        }
        typeArgs.push_back(any_cast_p<TypeNode>(visit(pCtx->type(0))));
    }

    // Weak<fn(...)> 禁（§3.7 / §5.5）：函数值是值类型，无 RC 头，不能 weak
    if (baseName->getText() == "Weak" && typeArgs.size() == 1) {
        if (dynamic_cast<TypeFnNode*>(typeArgs[0])) {
            throw YuxError((int)baseName->getLine(),
                (int)baseName->getCharPositionInLine() + 1,
                ErrorCode::E2001)
                .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
        }
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

// 函数类型字面量 fn(P1,...) R / fn?(...) R [#16][#23][#24]
// 形参槽位为 fnTypeParam（名可省 + 组糖），仅类型参与判等（§3.4(4)）
// retType 可省 → void；fnNullable 由可选 SymbolQuest 决定
std::any ASTBuilder::visitTypeFn(yux::yuxParser::TypeFnContext* ctx) {
    p<Node> parent = currentScope();
    bool nullable = ctx->SymbolQuest() != nullptr;

    vector<p<TypeNode>> paramTypes;
    if (auto* params = ctx->fnTypeParams()) {
        for (auto* p : params->fnTypeParam()) {
            // 三种 fnTypeParam alt 都以 typeWithRef 收尾：组糖 / 命名 / 无名
            if (auto* g = dynamic_cast<yux::yuxParser::FnTypeParamGroupContext*>(p)) {
                // a, b T → 展开为 N 份相同类型
                auto t = buildTypeWithRef(g->typeWithRef(), parent);
                size_t n = g->names.size();
                for (size_t i = 0; i < n; ++i) paramTypes.push_back(t);
            } else if (auto* n = dynamic_cast<yux::yuxParser::FnTypeParamNamedContext*>(p)) {
                paramTypes.push_back(buildTypeWithRef(n->typeWithRef(), parent));
            } else if (auto* u = dynamic_cast<yux::yuxParser::FnTypeParamUnnamedContext*>(p)) {
                paramTypes.push_back(buildTypeWithRef(u->typeWithRef(), parent));
            }
        }
    }

    p<TypeNode> retType = nullptr;
    if (auto* rt = ctx->retType) {
        retType = buildTypeWithRef(rt, parent);
    }

    DEBUG_LOG_VAL("    Type: Fn", (nullable ? "fn?(" : "fn(") << paramTypes.size() << " params)");
    return p<TypeNode>(createWithLine<TypeFnNode>(ctx, parent, std::move(paramTypes), retType, nullable));
}

// 元组类型 (T1, T2, ...)
// 元素列表至少 2 个（g4 语法保证），递归 visit 每个 type 子节点
std::any ASTBuilder::visitTypeTuple(yux::yuxParser::TypeTupleContext* ctx) {
    p<Node> parent = currentScope();
    vector<p<TypeNode>> elementTypes;
    elementTypes.reserve(ctx->types.size());
    for (auto* tCtx : ctx->types) {
        elementTypes.push_back(any_cast_p<TypeNode>(visit(tCtx)));
    }
    DEBUG_LOG_VAL("    Type: Tuple", "elements=" << elementTypes.size());
    return p<TypeNode>(createWithLine<TypeTupleNode>(ctx, parent, std::move(elementTypes)));
}

// Phase 4a: typeWithRef → TypeNode；SymbolAnd 存在则包成 Ref<inner>
// 语法已改：typeWithRef 现有 4 个分支，与 type 的 4 个分支结构对应，但每个内部位置（generic args / array elem）
// 也允许带 &，从而支持 Box<i32&> 这类嵌套引用类型作为参数 / 局部 var 类型。
p<TypeNode> ASTBuilder::buildTypeWithRef(yux::yuxParser::TypeWithRefContext* twr, p<Node> parent) {
    using namespace yux;
    p<TypeNode> inner;
    antlr4::tree::TerminalNode* andTok = nullptr;

    if (auto n = dynamic_cast<yuxParser::TypeNormalWithRefContext*>(twr)) {
        inner = p<TypeNode>(createWithLine<TypeNormalNode>(n, parent, n->ID()->getSymbol()));
        andTok = n->SymbolAnd();
    } else if (auto nul = dynamic_cast<yuxParser::TypeNullableWithRefContext*>(twr)) {
        // 内层是 type（不带 &），直接复用 visitType* 通路
        auto innerT = any_cast_p<TypeNode>(visit(nul->type()));
        if (innerT->getType().kind == TypeKind::Generic && innerT->getType().name == "Weak") {
            auto qt = nul->SymbolQuest()->getSymbol();
            throw YuxError(qt ? (int)qt->getLine() : 0,
                qt ? static_cast<int>(qt->getCharPositionInLine()) + 1 : 0,
                ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Box<T>?");
        }
        auto qt = nul->SymbolQuest()->getSymbol();
        Token nullableName(string("Nullable"), qt ? qt->getLine() : 0);
        vector<p<TypeNode>> args; args.push_back(innerT);
        inner = p<TypeNode>(createWithLine<TypeGenericNode>(nul, parent, nullableName, args));
        andTok = nul->SymbolAnd();
    } else if (auto g = dynamic_cast<yuxParser::TypeGenericWithRefContext*>(twr)) {
        auto baseName = g->ID()->getSymbol();
        vector<p<TypeNode>> typeArgs;
        for (auto innerCtx : g->genericDefWithRef()->types) {
            typeArgs.push_back(buildTypeWithRef(innerCtx, parent));
        }
        // Weak<fn(...)> 禁（§3.7 / §5.5）
        if (baseName->getText() == "Weak" && typeArgs.size() == 1) {
            if (dynamic_cast<TypeFnNode*>(typeArgs[0])) {
                throw YuxError((int)baseName->getLine(),
                    (int)baseName->getCharPositionInLine() + 1,
                    ErrorCode::E2001)
                    .withHint("函数值不是堆句柄、无 RC 头，不能用 Weak<...> 包裹（spec §3.7 / §5.5）");
            }
        }
        inner = p<TypeNode>(createWithLine<TypeGenericNode>(g, parent, baseName, typeArgs));
        andTok = g->SymbolAnd();
    } else if (auto a = dynamic_cast<yuxParser::TypeArrayWithRefContext*>(twr)) {
        auto elemType = buildTypeWithRef(a->typeWithRef(), parent);
        auto count = a->INT()->getSymbol();
        inner = p<TypeNode>(createWithLine<TypeArrayNode>(a, parent, elemType, count));
        andTok = a->SymbolAnd();
    } else if (auto t = dynamic_cast<yuxParser::TypeTupleWithRefContext*>(twr)) {
        // 元组 (T1, T2, ...)；每个元素本身可带 & 引用
        // 元组本身不带尾随 &（g4 中 typeTupleWithRef 没有 SymbolAnd?）
        vector<p<TypeNode>> elementTypes;
        elementTypes.reserve(t->types.size());
        for (auto* eCtx : t->types) {
            elementTypes.push_back(buildTypeWithRef(eCtx, parent));
        }
        inner = p<TypeNode>(createWithLine<TypeTupleNode>(t, parent, std::move(elementTypes)));
        andTok = nullptr;
    } else if (auto* fn = dynamic_cast<yuxParser::TypeFnWithRefContext*>(twr)) {
        // 函数类型字面量在 typeWithRef 位（fn 形参 / 返回值 / 局部 var）
        // 末尾 & 表借用一个函数值；fnTypeParam 三 alt 同 visitTypeFn 处理
        bool nullable = fn->SymbolQuest() != nullptr;
        vector<p<TypeNode>> paramTypes;
        if (auto* params = fn->fnTypeParams()) {
            for (auto* fp : params->fnTypeParam()) {
                if (auto* g = dynamic_cast<yuxParser::FnTypeParamGroupContext*>(fp)) {
                    auto t2 = buildTypeWithRef(g->typeWithRef(), parent);
                    size_t n = g->names.size();
                    for (size_t i = 0; i < n; ++i) paramTypes.push_back(t2);
                } else if (auto* n2 = dynamic_cast<yuxParser::FnTypeParamNamedContext*>(fp)) {
                    paramTypes.push_back(buildTypeWithRef(n2->typeWithRef(), parent));
                } else if (auto* u = dynamic_cast<yuxParser::FnTypeParamUnnamedContext*>(fp)) {
                    paramTypes.push_back(buildTypeWithRef(u->typeWithRef(), parent));
                }
            }
        }
        p<TypeNode> retType = nullptr;
        if (auto* rt = fn->retType) {
            retType = buildTypeWithRef(rt, parent);
        }
        inner = p<TypeNode>(createWithLine<TypeFnNode>(fn, parent, std::move(paramTypes), retType, nullable));
        andTok = fn->SymbolAnd();
    } else {
        throw YuxError(1, ErrorCode::E2002);
    }

    if (andTok) {
        auto sym = andTok->getSymbol();
        Token refName(string("Ref"), sym ? sym->getLine() : 0);
        vector<p<TypeNode>> args;
        args.push_back(inner);
        return p<TypeNode>(createWithLine<TypeGenericNode>(twr, parent, refName, args));
    }
    return inner;
}

// T? 解糖为 Nullable<T>
// 直接构造 TypeGenericNode("Nullable", [T])，复用现有泛型实例化通路
// "Nullable" 名字 token 用合成构造，line 取自 SymbolQuest
std::any ASTBuilder::visitTypeNullable(yux::yuxParser::TypeNullableContext* ctx) {
    p<Node> parent = currentScope();
    auto inner = any_cast_p<TypeNode>(visit(ctx->type()));

    auto questTok = ctx->SymbolQuest()->getSymbol();

    // Phase 1d.3：禁 Weak<T>?（DRAFT §5：Weak 已原生可空，再裹 Nullable 无意义）
    {
        auto innerTI = inner->getType();
        if (innerTI.kind == TypeKind::Generic && innerTI.name == "Weak") {
            int line = questTok ? (int)questTok->getLine() : 0;
            int col  = questTok ? (int)questTok->getCharPositionInLine() + 1 : 0;
            throw YuxError(line, col, ErrorCode::E2001)
                .withHint("Weak<T> 本身已可空；若需在持有者失效后取值，使用 `upgrade(weak)`，其结果即为 Box<T>?");
        }
    }

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
