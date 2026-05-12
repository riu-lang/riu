// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 函数调用编译实现
// 
// 本文件包含所有与函数/方法调用相关的编译逻辑:
// - 函数重载解析
// - 方法调用编译 (结构体方法、内置类型方法、数组方法)
// - 构造函数调用
// - 泛型函数调用
// - 外部函数调用

#include "compiler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "compiler_runtime.h"
#include "analyzer/draft_impl_checker.h"
#include "analyzer/draft_registry.h"
#include "ast/mangler.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <functional>

// ==================== 辅助函数: 参数类型匹配 ====================

// 检查参数类型是否可以接受
// 支持精确匹配和引用类型匹配
static bool paramAccepts(const TypeInfo& param, const TypeInfo& argType) {
    if (param == argType) return true;  // 精确匹配
    if (param.isRef()) {
        auto ref = param.refElementType();
        if (ref && *ref == argType) return true;  // 引用参数接受值类型
    }
    if (param.isPtr() && argType.isRef()) return true;  // 指针参数接受引用
    return false;
}

// 灵活的函数重载匹配
// 对灵活整数字面量 (如 42) 允许匹配任何整数类型
static bool overloadMatchesFlexible(const vector<p<ExprNode>>& args, const vector<TypeInfo>& params) {
    if (params.size() != args.size()) return false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (isFlexibleIntExpr(args[i])) {
            // 灵活整数可以匹配任何整数类型
            if (isIntTypeName(params[i].name)) continue;
            try {
                if (paramAccepts(params[i], args[i]->getType())) continue;
            } catch (...) {}
            return false;
        }
        try {
            if (!paramAccepts(params[i], args[i]->getType())) return false;
        } catch (...) { return false; }
    }
    return true;
}

// 默认的函数重载匹配
// 灵活整数字面量默认匹配 i32
static bool overloadMatchesDefault(const vector<p<ExprNode>>& args, const vector<TypeInfo>& params) {
    if (params.size() != args.size()) return false;
    TypeInfo i32Type("i32");
    for (size_t i = 0; i < args.size(); ++i) {
        TypeInfo argType;
        if (isFlexibleIntExpr(args[i])) {
            argType = i32Type;  // 灵活整数默认为 i32
        } else {
            try { argType = args[i]->getType(); } catch (...) { return false; }
        }
        if (!paramAccepts(params[i], argType)) return false;
    }
    return true;
}

// ==================== 构造函数重载解析 ====================
// 与 resolveFnOverload 同思路，但 ctor 在符号表中以 `S.S` 注册，且 params[0] 是
// 接收者（结构体类型本身）。匹配时跳过 params[0]，按用户写的实参列表推断未带后缀
// 的整数字面量类型，避免后续在 LLVM 后端因 i32→i64 形参不匹配而走到外部函数路径
// 触发 `isSized` 断言（见 BUGS.md「构造函数 i64 形参传 untyped int 字面量」）。
static void resolveCtorOverload(FileNode* file, const string& structName,
                                const vector<p<ExprNode>>& args, int line) {
    string ctorFullName = structName + "." + structName;
    vector<FnSymbolInfo*> candidates;
    file->collectFnOverloads(ctorFullName, candidates);
    if (candidates.empty()) return;

    auto matchesDefault = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        TypeInfo i32Type("i32");
        for (size_t i = 0; i < args.size(); ++i) {
            TypeInfo argType;
            if (isFlexibleIntExpr(args[i])) {
                argType = i32Type;
            } else {
                try { argType = args[i]->getType(); } catch (...) { return false; }
            }
            if (!paramAccepts(c->params[i + 1], argType)) return false;
        }
        return true;
    };
    auto matchesFlexible = [&](FnSymbolInfo* c) {
        if (c->params.size() != args.size() + 1) return false;
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i])) {
                if (isIntTypeName(c->params[i + 1].name)) continue;
                try {
                    if (paramAccepts(c->params[i + 1], args[i]->getType())) continue;
                } catch (...) {}
                return false;
            }
            try {
                if (!paramAccepts(c->params[i + 1], args[i]->getType())) return false;
            } catch (...) { return false; }
        }
        return true;
    };

    vector<FnSymbolInfo*> defaultMatches;
    for (auto c : candidates) if (matchesDefault(c)) defaultMatches.push_back(c);

    vector<FnSymbolInfo*> matches;
    if (defaultMatches.size() == 1) {
        matches = defaultMatches;
    } else if (defaultMatches.empty()) {
        for (auto c : candidates) if (matchesFlexible(c)) matches.push_back(c);
    } else {
        matches = defaultMatches;
    }

    if (matches.size() == 1) {
        // 唯一匹配：把每个灵活整数实参推断到对应 ctor 形参类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->params[i + 1].name)) {
                tryInferIntType(args[i], fn->params[i + 1]);
            }
        }
    } else if (matches.size() > 1) {
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + structName + "(";
            for (size_t i = 1; i < m->params.size(); ++i) {
                if (i > 1) sigs += ", ";
                sigs += m->params[i].name;
            }
            sigs += ")";
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try { argSigs += args[i]->getType().name; } catch(...) { argSigs += "?"; }
        }
        throw YuxError(line, ErrorCode::E6014, structName, argSigs, matches.size(), sigs);
    }
}

// ==================== 函数重载解析 ====================
// 解析函数重载，确定应该调用哪个版本
// 如果有歧义，抛出错误要求用户添加类型后缀
static void resolveFnOverload(FileNode* file, FileNode* sdkFile, const string& fnName,
                              const vector<p<ExprNode>>& args, int line) {
    (void)sdkFile;
    vector<FnSymbolInfo*> candidates;
    file->collectFnOverloads(fnName, candidates);
    if (candidates.empty()) return;

    // 首先尝试默认匹配 (灵活整数 -> i32)
    vector<FnSymbolInfo*> defaultMatches;
    for (auto c : candidates) {
        if (overloadMatchesDefault(args, c->params)) defaultMatches.push_back(c);
    }

    vector<FnSymbolInfo*> matches;
    if (defaultMatches.size() == 1) {
        matches = defaultMatches;
    } else if (defaultMatches.empty()) {
        // 如果默认匹配失败，尝试灵活匹配
        for (auto c : candidates) {
            if (overloadMatchesFlexible(args, c->params)) matches.push_back(c);
        }
    } else {
        matches = defaultMatches;
    }

    if (matches.size() == 1) {
        // 唯一匹配: 推断灵活整数的类型
        auto fn = matches[0];
        for (size_t i = 0; i < args.size(); ++i) {
            if (isFlexibleIntExpr(args[i]) && isIntTypeName(fn->params[i].name)) {
                tryInferIntType(args[i], fn->params[i]);
            }
        }
    } else if (matches.size() > 1) {
        // 多个匹配: 报告歧义错误
        string sigs;
        for (auto m : matches) {
            sigs += "\n  " + fnName + "(";
            for (size_t i = 0; i < m->params.size(); ++i) {
                if (i) sigs += ", ";
                sigs += m->params[i].name;
            }
            sigs += ")";
        }
        string argSigs;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) argSigs += ", ";
            try { argSigs += args[i]->getType().name; } catch(...) { argSigs += "?"; }
        }
        throw YuxError(line, ErrorCode::E6014, fnName, argSigs, matches.size(), sigs);
    }
}

// ==================== 函数获取 ====================

// 获取或创建 LLVM 函数
// 处理 main 函数重命名为 yux_main
llvm::Function* Compiler::getFunction(p<FnHeaderNode> header) {
    auto name = header->name().getText();
    DEBUG_LOG_VAL("  getFunction", name);

    // main 函数重命名为 yux_main，真正的 main 由 mainStartup 提供
    if (name == "main") {
        name = "yux_main";
        DEBUG_LOG("    -> renamed to yux_main");
    } else {
        // 其他函数使用 mangle 名称
        // v0.6 Phase 2b: 透明类型别名先解析再 mangle，使声明 / 调用两侧 mangle 名一致
        vector<TypeInfo> paramTypes;
        for (auto param : header->params()) {
            if (param->type()) {
                paramTypes.push_back(applySubst(param->type()->getType()));
            }
        }
        bool isPriv = !name.empty() && name[0] == '_';
        name = Mangler::function(_file->moduleName(), name, paramTypes, isPriv);
        DEBUG_LOG_VAL("    -> mangled name", name);
    }

    auto fnType = getLLVMFunctionType(header);
    auto func = _module->getFunction(name);
    if (!func) {
        func = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, name, _module);
        DEBUG_LOG("    -> created new function");
    } else {
        DEBUG_LOG("    -> found existing function");
    }
    return func;
}

// 获取或创建方法函数
// 方法名包含结构体名，如 "Foo.bar"
llvm::Function* Compiler::getMethodFunction(
    const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes, const TypeInfo& retType,
    const string& fallibleErrType) {
    DEBUG_LOG_VAL("  getMethodFunction", structName << "." << methodName);

    bool isCtor = methodName == structName;  // 构造函数名与结构体名相同
    bool isPriv = !methodName.empty() && methodName[0] == '_';

    // 确定方法所属的模块
    // 泛型实例：使用消费方模块（每个使用方模块各自生成一份实例 IR，避免重复符号）
    // 普通结构体：使用 baseDecl owner 模块（跨模块仍引用同一份定义）
    string ownerModule = _file->moduleName();
    if (auto instIt = _structInstances.find(structName); instIt != _structInstances.end()) {
        ownerModule = instIt->second.consumerModule;
    } else if (!isBuiltinType(structName)) {
        auto* owner = _file->getStructOwner(structName);
        if (owner && owner != _file) {
            ownerModule = owner->moduleName();
        } else if (!owner && _yux && _yux->sdkFile() && _yux->sdkFile() != _file
                   && _yux->sdkFile()->getStructDecl(structName)) {
            ownerModule = _yux->sdkFile()->moduleName();
        }
    }
    // 生成 mangle 名称
    string mangledName = isCtor
        ? Mangler::ctor(ownerModule, structName, paramTypes)
        : Mangler::method(ownerModule, structName, methodName, paramTypes, isPriv);
    DEBUG_LOG_VAL("    -> mangled name", mangledName);

    auto func = _module->getFunction(mangledName);
    if (func) {
        DEBUG_LOG("    -> found existing function");
        return func;
    }

    // 构建参数类型列表
    vector<llvm::Type*> llvmParamTypes;

    // 第一个参数是当前实例（用户层 `$`）
    if (isBuiltinType(structName) || TypeInfo(structName).isPtr() || TypeInfo(structName).isRef()) {
        llvmParamTypes.push_back(getLLVMType(TypeInfo(structName)));
    } else {
        llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));  // 结构体通过指针传递
    }

    // 其他参数
    for (auto& paramType : paramTypes) {
        if (paramType.isPtr() || paramType.isRef()) {
            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
            continue;
        }
        if (structParamUsesPointer(paramType.name)) {
            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            llvmParamTypes.push_back(getLLVMType(paramType));
        }
    }

    auto llvmRetType = wrapFallibleRetType(retType, fallibleErrType);
    DEBUG_LOG_VAL("    -> return type", (retType.empty() ? "void" : retType.name));
    auto fnType = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
    DEBUG_LOG("    -> created new function");
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

// 获取或创建析构函数
llvm::Function* Compiler::getDestructorFunction(const string& structName) {
    DEBUG_LOG_VAL("  getDestructorFunction", structName);

    // 确定析构函数所属的模块
    // 泛型实例：用消费方模块（每个使用方模块各自一份）
    // 普通结构体：仍走 owner 模块
    string ownerModule = _file->moduleName();
    if (auto instIt = _structInstances.find(structName); instIt != _structInstances.end()) {
        ownerModule = instIt->second.consumerModule;
    } else {
        auto* owner = _file->getStructOwner(structName);
        if (owner && owner != _file) {
            ownerModule = owner->moduleName();
        } else if (!owner && _yux && _yux->sdkFile()
                   && _yux->sdkFile()->getStructDecl(structName)) {
            ownerModule = _yux->sdkFile()->moduleName();
        }
    }
    string mangledName = Mangler::dtor(ownerModule, structName);
    DEBUG_LOG_VAL("    -> mangled name", mangledName);

    auto func = _module->getFunction(mangledName);
    if (func) {
        DEBUG_LOG("    -> found existing function");
        return func;
    }

    // 析构函数签名: void (Struct*)
    vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));

    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), llvmParamTypes, false);
    DEBUG_LOG("    -> created new function");
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

// ==================== 编译器内部方法检查 ====================
// 检查方法是否标记为 #CompilerInner
// CompilerInner 方法由编译器特殊处理，不生成普通 IR
bool Compiler::isCompilerInnerMethod(const string& structName, const string& methodName) {
    if (!_yux || !_yux->sdkFile()) return false;
    
    auto structImpl = _yux->sdkFile()->getStructImpl(structName);
    if (!structImpl) return false;
    
    for (auto& method : structImpl->methods()) {
        if (method->header()->name().getText() == methodName) {
            return method->header()->hasAnno("CompilerInner");
        }
    }
    
    return false;
}

// ==================== 方法调用编译 ====================
// 编译方法调用表达式 (obj.method(args))
// 处理多种情况: 包别名调用、模块别名调用、内置类型方法、数组方法、结构体方法
llvm::Value* Compiler::compileMethodCall(
    p<ExprCallNode> callNode, p<ExprDotNode> dotNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    auto baseExpr = dotNode->baseExpr();
    auto member = dotNode->member();

    // 处理包别名调用: package.module.fn(args)
    {
        string aliasName;
        vector<string> segs;
        if (ExprDotNode::parseChain(dotNode, aliasName, segs) && segs.size() >= 2) {
            auto aliasSym = _file->lookupSymbol(aliasName);
            if (aliasSym && (aliasSym->kind == SymbolKind::Package || aliasSym->kind == SymbolKind::Module)
                && _file->isAmbiguousAlias(aliasName)) {
                _file->throwAmbiguousAlias(aliasName, callNode->getLineNumber());
            }
            if (aliasSym && aliasSym->kind == SymbolKind::Package) {
                string childKey;
                for (size_t i = 0; i + 1 < segs.size(); ++i) {
                    if (i) childKey += ".";
                    childKey += segs[i];
                }
                auto* target = _file->packageChild(aliasName, childKey);
                if (!target) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6001, childKey, aliasSym->moduleName);
                }
                const string& fnName = segs.back();
                auto* fnSym = target->lookupFnSymbolWithParams(fnName, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6002, fnName, aliasSym->moduleName + "." + childKey);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6003, fnName);
                }
                return compileKnownFunctionCall(callNode, fnName, args, argTypes, fnSym);
            }
        }
    }

    // 处理模块别名调用: module.fn(args)
    if (auto baseLit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
            auto aliasName = objLit->getValue().getText();
            auto aliasSym = _file->lookupSymbol(aliasName);
            if (aliasSym && aliasSym->kind == SymbolKind::Module) {
                auto targetMod = _yux ? _yux->module(aliasSym->moduleName) : nullptr;
                if (!targetMod) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6005, aliasSym->moduleName, aliasName);
                }
                auto* fnSym = targetMod->lookupFnSymbolWithParams(member, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6002, member, aliasSym->moduleName);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6004, member);
                }
                return compileKnownFunctionCall(callNode, member, args, argTypes, fnSym);
            }
        }
    }

    auto baseType = baseExpr->getType();
    // §12.4 / §6.4.4：若 baseExpr 类型是当前替换栈中的泛型形参 T，
    // 应用替换得到具体类型（T -> i32 / Counter / ...），后续按具体类型分发
    // 边界 (E1106) 已在调用点 compileGenericFunctionCall 校验过。
    baseType = applySubst(baseType);

    // Dyn<D> / Dyn<D&> 方法调用 (Phase 2d 静态检查 + Phase 3d vtable codegen)
    if (baseType.isDyn()) {
        return compileDynMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    // 处理内置类型方法
    if (isBuiltinType(baseType.name)) {
        return compileBuiltinTypeMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
    }

    TypeInfo actualType = baseType;

    // 处理指针类型方法
    if (baseType.isPtr()) {
        if (member == "to_int") {
            DEBUG_LOG("    Expr: PtrMethod - to_int");
            auto selfVal = compileExpr(baseExpr);
            return _builder.CreatePtrToInt(selfVal, _builder.getInt64Ty(), "ptr_to_int");
        }
    }

    // 处理数组方法
    if (baseType.isArrayGeneric()) {
        auto result = compileArrayMethodCall(callNode, baseExpr, baseType, member, args, argTypes);
        if (result) return result;
    }

    // 处理 Box 类型: 解包获取实际类型
    if (baseType.isBox()) {
        auto boxElemType = baseType.boxElementType();
        if (boxElemType) {
            actualType = *boxElemType;
        }
    }

    // 处理结构体方法
    auto structMethodResult = compileStructMethodCall(callNode, baseExpr, baseType, actualType, member, args, argTypes);
    if (structMethodResult) return structMethodResult;

    // 处理内部函数调用 (obj.fn(args) 其中 obj 是函数名)
    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto fnSymbol = _file->lookupFnSymbolWithParams(objName, argTypes);
            if (fnSymbol) {
                string ownerMod = fnSymbol->moduleName.empty() ? _file->moduleName() : fnSymbol->moduleName;
                bool fnPriv = !objName.empty() && objName[0] == '_';
                auto cName = Mangler::function(ownerMod, objName, argTypes, fnPriv);
                DEBUG_LOG_VAL("    Expr: InnerFnCall (dot)", objName << " -> " << cName);
                auto fn = _module->getFunction(cName);
                if (!fn) {
                    vector<llvm::Type*> paramTypes;
                    for (auto& t : argTypes) {
                        paramTypes.push_back(getLLVMType(t));
                    }
                    auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
                    fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
                }
                return _builder.CreateCall(fn, args);
            }
        }
    }
    return nullptr;
}

// ==================== 调用表达式编译 ====================
// 编译函数调用表达式的主入口
// 处理类型推断、参数编译、分发到具体调用类型
// Phase 10e：调用点 `!` 错误传播 / 裸调可失败函数的语义校验
// （DRAFT-错误.md [#4.B]，错误码 E7001 / E7004 / E7006）
//
// 仅处理 ID-callee（最常见形态）；方法 / fn-value 路径的 `!` 校验推 10g（届时
// IR 路由完成）。
//
// caller 的 #Fallible(E) 通过 `_currentFnNode->header()` 取注解；callee 的 #Fallible(E)
// 从 FnSymbolInfo.fallibleErrType 读出（ast_builder 在 visitProgram 预扫时填好）。
//
// Phase 10f：新增 try-catch 块路径分流（[#4.H]）：
//   - inTryBlock=true 时：裸调用 #Fallible callee 合法（错误自动路由到 catch 子句），
//     不再触发 E7001 / E7006；callee 错误类型记入 try ctx 的 seenErrTypes（穷尽性 / 多余
//     用，10f-5）；写了 ! 给 E7016 警告（语义不变，编译器视同义）
//   - inTryBlock=false 时：沿用原 10e 逻辑（E7001 / E7004 / E7006）
static void checkErrPropagateForIdCall(
    p<FnNode> currentFnNode,
    p<ExprCallNode> node,
    const string& fnName,
    const FnSymbolInfo* calleeSym,
    Compiler::TryCatchCtx* tryCtx) {
    bool hasBang = node->errPropagate();
    string callerErr;
    if (currentFnNode) {
        if (auto eOpt = currentFnNode->header()->getAnnoArg("Fallible")) {
            callerErr = *eOpt;
        }
    }
    string calleeErr = calleeSym ? calleeSym->fallibleErrType : "";

    // Phase 10f：try block 内的 #Fallible 调用 → 路由到 catch 子句
    if (tryCtx && !calleeErr.empty()) {
        tryCtx->seenErrTypes.push_back(calleeErr);
        if (hasBang) {
            // E7016：try block 内 ! 冗余（语义不变，警告）
            // TODO(10f-4): 接入诊断警告通道；当前仅记录注释
            // 当前编译器无独立 warning 通道，待 spec §11 诊断分级落地时切换；
            // 这里保持 silent 以不阻塞编译（[#4.H] 表："写出来不会改变语义"）
        }
        return;
    }

    if (hasBang) {
        // E7001：caller 不在 #Fallible(E) 函数内 + 不在 try block 内
        if (callerErr.empty()) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E7001);
        }
        // E7004：caller / callee 错误类型不一致（同 ! 不可跨类型透传）
        if (!calleeErr.empty() && calleeErr != callerErr) {
            throw YuxError(node->getLineNumber(), node->getColumn(),
                ErrorCode::E7004, calleeErr, callerErr, calleeErr, callerErr, calleeErr);
        }
        // callee 不是 #Fallible 但写了 ! ：暂不在 10e/10f 报；保留给后续考虑
    } else {
        // E7006：调用 `#Fallible` 函数但未加 `!`（不在 try block 内）
        if (!calleeErr.empty()) {
            throw YuxError(node->getLineNumber(), node->getColumn(),
                ErrorCode::E7006, fnName);
        }
    }
}

llvm::Value* Compiler::compileCallExpr(p<ExprCallNode> node) {
    auto calleeExpr = node->getCalleeExpr();

    // Phase 10e：错误传播语义校验（仅 ID-callee 路径；方法 / fn-value 推 10f）
    if (auto litCallee = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(litCallee->literal())) {
            string fnName = objLit->getValue().getText();
            vector<FnSymbolInfo*> cands;
            _file->collectFnOverloads(fnName, cands);
            if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                _yux->sdkFile()->collectFnOverloads(fnName, cands);
            }
            // 取第一个候选项的 fallibleErrType；多重载形态在 10e 视为同质（spec 后续收口）
            const FnSymbolInfo* sym = cands.empty() ? nullptr : cands.front();
            // 仅当能识别为 fn 调用时校验；构造函数 / 类型构造走 callee 路径，但 FnSymbolInfo 也可能有
            TryCatchCtx* tryCtx = _tryCatchStack.empty() ? nullptr : &_tryCatchStack.back();
            checkErrPropagateForIdCall(_currentFnNode, node, fnName, sym, tryCtx);
        } else if (node->errPropagate()) {
            // 非 ID-literal 但带 `!`：极少见路径（如 nullable 字面量调用），按 caller / try 状态判 E7001
            // 在 try block 内 → 暂放过（路由到 catch 由 10g 实施）
            if (_tryCatchStack.empty()) {
                string callerErr;
                if (_currentFnNode) {
                    if (auto eOpt = _currentFnNode->header()->getAnnoArg("Fallible")) callerErr = *eOpt;
                }
                if (callerErr.empty()) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E7001);
                }
            }
        }
    } else if (node->errPropagate() && !dynamic_cast<ExprDotNode*>(calleeExpr)) {
        // fn-value 调用 + `!`：caller 未 fallible 且不在 try 内时报 E7001
        if (_tryCatchStack.empty()) {
            string callerErr;
            if (_currentFnNode) {
                if (auto eOpt = _currentFnNode->header()->getAnnoArg("Fallible")) callerErr = *eOpt;
            }
            if (callerErr.empty()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E7001);
            }
        }
    }

    // Phase 2b: callee 静态类型为 Fn(...)R → 走 fat-ptr 路径
    // 涵盖：lambda IIFE `((x i32) i32 => ...)(5)`、fn-typed 变量 / 字段 / 调用结果
    // 普通 ID callee（普通函数名）走 ExprLiteralNode 路径，那里返回 "fn() <ret>" 字符串
    // 编码（kind=Normal），不会命中 isFn()
    // Phase 3c: callee 为 Box<fn(...)R> → 自动解引取 fat-ptr 后走同款 fn-value-call
    {
        TypeInfo calleeStaticType;
        try { calleeStaticType = calleeExpr->getType(); } catch (...) {}
        if (calleeStaticType.isFn()) {
            return compileFnValueCall(node);
        }
        if (calleeStaticType.isBox()) {
            auto inner = calleeStaticType.boxElementType();
            if (inner && inner->isFn()) {
                return compileBoxFnValueCall(node, *inner);
            }
        }
    }

    // 预处理: 类型推断和泛型参数处理
    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            string fnName = objLiteral->getValue().getText();
            
            // 检查是否为泛型函数
            auto genericFn = _file->getFunction(fnName);
            p<FileNode> fnOwner = _file;
            if (!genericFn && _yux && _yux->sdkFile()) {
                genericFn = _yux->sdkFile()->getFunction(fnName);
                if (genericFn) fnOwner = _yux->sdkFile();
            }

            // 处理显式类型参数的泛型函数调用
            if (genericFn && genericFn->header()->isGeneric()) {
                const auto& typeParams = genericFn->header()->typeParams();
                vector<TypeInfo> typeArgs;

                const auto& explicitTypeArgs = node->getTypeArgs();
                if (!explicitTypeArgs.empty()) {
                    // 验证类型参数数量
                    if (explicitTypeArgs.size() != typeParams.size()) {
                        throw YuxError(node->getLineNumber(), node->getColumn(),
                            ErrorCode::E6010, fnName, typeParams.size(), explicitTypeArgs.size())
                            .withHint(std::format("调用处的类型实参个数需与声明匹配；改写为 `{}:<{}>(...)` 形式补齐 {} 个类型",
                                fnName,
                                std::string(typeParams.size() == 1 ? "T" : "T1, T2, ..."),
                                typeParams.size()));
                    }
                    for (auto& tn : explicitTypeArgs) {
                        typeArgs.push_back(applySubst(tn->getType()));
                    }

                    // 建立类型替换映射并推断参数类型
                    map<string, TypeInfo> subst;
                    for (size_t i = 0; i < typeParams.size(); ++i) {
                        subst[typeParams[i]] = typeArgs[i];
                    }
                    _substStack.push_back(SubstFrame{subst, "", ""});

                    auto params = genericFn->header()->params();
                    for (size_t i = 0; i < params.size(); ++i) {
                        auto paramType = params[i]->type();
                        if (paramType) {
                            TypeInfo instParamType = applySubst(paramType->getType());
                            // 推断灵活整数的类型
                            if (isIntTypeName(instParamType.name) && i < node->getArgs().size()) {
                                tryInferIntType(node->getArgs()[i], instParamType);
                            }
                        }
                    }

                    _substStack.pop_back();
                }
            } else {
                // 检查是否为泛型结构体构造函数
                auto structDecl = _file->getStructDecl(fnName);
                p<FileNode> structOwner = _file;
                if (!structDecl && _yux && _yux->sdkFile()) {
                    auto sdkDecl = _yux->sdkFile()->getStructDecl(fnName);
                    if (sdkDecl) {
                        structDecl = sdkDecl;
                        structOwner = _yux->sdkFile();
                    }
                }
                // 处理泛型结构体构造函数的类型推断
                if (structDecl && structDecl->isGeneric() && !node->getTypeArgs().empty()) {
                    const auto& explicitTypeArgs = node->getTypeArgs();
                    if (explicitTypeArgs.size() == structDecl->typeParams().size()) {
                        vector<sp<TypeInfo>> instArgs;
                        instArgs.reserve(explicitTypeArgs.size());
                        for (auto& tn : explicitTypeArgs) {
                            instArgs.push_back(make_shared<TypeInfo>(applySubst(tn->getType())));
                        }
                        string effName = fnName;
                        for (auto& a : instArgs) {
                            effName += "$" + a->getGenericMangleName();
                        }
                        
                        map<string, TypeInfo> subst;
                        for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                            subst[structDecl->typeParams()[i]] = *instArgs[i];
                        }
                        
                        auto structImpl = structOwner->getStructImpl(fnName);
                        if (!structImpl && _yux && _yux->sdkFile() && _yux->sdkFile() != structOwner) {
                            structImpl = _yux->sdkFile()->getStructImpl(fnName);
                        }
                        
                        // 推断构造函数参数类型
                        if (structImpl) {
                            for (auto& method : structImpl->methods()) {
                                if (method->header()->name().getText() == fnName) {
                                    auto params = method->header()->params();
                                    for (size_t i = 0; i < params.size() && i < node->getArgs().size(); ++i) {
                                        auto paramType = params[i]->type();
                                        if (paramType) {
                                            TypeInfo instParamType = paramType->getType().substitute(subst);
                                            if (isIntTypeName(instParamType.name)) {
                                                tryInferIntType(node->getArgs()[i], instParamType);
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                } else if (structDecl) {
                    // 非泛型结构体构造函数：按 `S.S` 解析重载，推断未带后缀的整数字面量
                    // 否则会带着 i32 实参落到下方 lookupFnSymbolWithParams 的严格匹配
                    // 失败，再退化到 external function call 路径触发 LLVM 断言。
                    resolveCtorOverload(_file, fnName, node->getArgs(), node->getLineNumber());
                    if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                        resolveCtorOverload(_yux->sdkFile(), fnName, node->getArgs(), node->getLineNumber());
                    }
                } else {
                    // 解析函数重载
                    resolveFnOverload(_file, _yux ? _yux->sdkFile() : nullptr, fnName,
                                      node->getArgs(), node->getLineNumber());
                }
            }
        }
    }

    // 编译参数
    vector<llvm::Value*> args;
    vector<TypeInfo> argTypes;
    
    bool isGenericCtorCall = false;
    map<string, TypeInfo> ctorSubst;
    p<StructImplNode> ctorStructImpl;
    p<FileNode> ctorStructOwner;
    string ctorFnName;
    
    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            ctorFnName = objLiteral->getValue().getText();
            auto structDecl = _file->getStructDecl(ctorFnName);
            p<FileNode> structOwner = _file;
            if (!structDecl && _yux && _yux->sdkFile()) {
                auto sdkDecl = _yux->sdkFile()->getStructDecl(ctorFnName);
                if (sdkDecl) {
                    structDecl = sdkDecl;
                    structOwner = _yux->sdkFile();
                }
            }
            if (structDecl && structDecl->isGeneric() && !node->getTypeArgs().empty()) {
                const auto& explicitTypeArgs = node->getTypeArgs();
                if (explicitTypeArgs.size() == structDecl->typeParams().size()) {
                    isGenericCtorCall = true;
                    ctorStructOwner = structOwner;
                    
                    vector<sp<TypeInfo>> instArgs;
                    instArgs.reserve(explicitTypeArgs.size());
                    for (auto& tn : explicitTypeArgs) {
                        instArgs.push_back(make_shared<TypeInfo>(applySubst(tn->getType())));
                    }
                    
                    for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                        ctorSubst[structDecl->typeParams()[i]] = *instArgs[i];
                    }
                    
                    ctorStructImpl = structOwner->getStructImpl(ctorFnName);
                    if (!ctorStructImpl && _yux && _yux->sdkFile() && _yux->sdkFile() != structOwner) {
                        ctorStructImpl = _yux->sdkFile()->getStructImpl(ctorFnName);
                    }
                }
            }
        }
    }
    
    // Phase 2b：实参位置 lambda 类型反推
    // 对 ID-callee 的普通函数调用，按 fnName 找匹配 FnSymbol；若 params[i] 是 Fn 类型且
    // arg[i] 是 LambdaExprNode，则按 params[i] 预先 emit lambda function（mangle 缓存）。
    // 多重载场景：当前先按 args.size() 唯一匹配；若多匹配，靠后面的 lookupFnSymbolWithParams
    // 进一步消歧（lambda arg 在重载解析中按 Fn 类型已统一）。
    if (auto calleeLit = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLit = dynamic_cast<LiteralObjNode*>(calleeLit->literal())) {
            string fnName = objLit->getValue().getText();
            vector<FnSymbolInfo*> candidates;
            _file->collectFnOverloads(fnName, candidates);
            if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                _yux->sdkFile()->collectFnOverloads(fnName, candidates);
            }
            // 按实参 arity 过滤
            vector<FnSymbolInfo*> aritied;
            for (auto* c : candidates) {
                if (c->params.size() == node->getArgs().size()) aritied.push_back(c);
            }
            if (aritied.size() == 1) {
                auto* fnSym = aritied[0];
                for (size_t i = 0; i < node->getArgs().size(); ++i) {
                    auto lambdaArg = dynamic_cast<LambdaExprNode*>(node->getArgs()[i]);
                    if (!lambdaArg) continue;
                    if (i >= fnSym->params.size()) break;
                    if (!fnSym->params[i].isFn()) continue;
                    // 设置反推类型，让后续 lambdaArg->getType() 返回完整 Fn TypeInfo
                    // （影响 argTypes 收集 / lookupFnSymbolWithParams 重载消歧）
                    lambdaArg->setInferredFnType(fnSym->params[i]);
                    // 同步刷新 body scope 中形参符号的 type，让 body 内符号查找拿到正确类型
                    if (auto sc = lambdaArg->bodyScope()) {
                        const auto& fps = fnSym->params[i].fnParamTypes();
                        for (size_t k = 0; k < lambdaArg->params().size() && k < fps.size(); ++k) {
                            if (lambdaArg->params()[k].type) continue;  // 显式标注尊重源
                            if (auto* psym = sc->lookupSymbol(lambdaArg->params()[k].name.getText())) {
                                if (fps[k]) psym->type = *fps[k];
                            }
                        }
                    }
                    // 预 emit；后续 compileLambdaExpr 走 mangle 缓存命中同一 Function*
                    emitLambdaFunction(lambdaArg, fnSym->params[i]);
                }
            }
        }
    }

    for (auto& arg : node->getArgs()) {
        auto argType = arg->getType();
        argTypes.push_back(argType);

        bool passByPtr = false;
        if (isGenericCtorCall && ctorStructImpl) {
            for (auto& method : ctorStructImpl->methods()) {
                if (method->header()->name().getText() == ctorFnName) {
                    auto params = method->header()->params();
                    size_t argIdx = args.size();
                    if (argIdx < params.size()) {
                        auto paramType = params[argIdx]->type();
                        if (paramType) {
                            TypeInfo instParamType = paramType->getType().substitute(ctorSubst);
                            if (structParamUsesPointer(instParamType.name)) {
                                passByPtr = true;
                            }
                        }
                    }
                    break;
                }
            }
        }
        
        if (passByPtr) {
            if (auto litExpr = dynamic_cast<ExprLiteralNode*>(arg)) {
                if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                    auto varName = objLit->getValue().getText();
                    if (_localVarPtrs.contains(varName)) {
                        args.push_back(_localVarPtrs[varName]);
                        continue;
                    }
                }
            }
            auto val = compileExpr(arg);
            auto tmpAlloca = _builder.CreateAlloca(getLLVMType(argType), nullptr, "arg_tmp");
            _builder.CreateStore(val, tmpAlloca);
            args.push_back(tmpAlloca);
        } else {
            args.push_back(compileExpr(arg));
        }
    }

    if (auto dotNode = dynamic_cast<ExprDotNode*>(calleeExpr)) {
        auto result = compileMethodCall(node, dotNode, args, argTypes);
        if (result) {
            return result;
        }
    }

    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            return compileFunctionCall(node, objLiteral->getValue().getText(), args, argTypes);
        }
    }

    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E6015);
}

llvm::Value* Compiler::compileFunctionCall(
    p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    if (_castFunctions.contains(fnName)) {
        DEBUG_LOG_VAL("    Expr: CastFunction", fnName);
        auto& castInfo = _castFunctions[fnName];
        return createCast(castInfo.value, castInfo.srcType, castInfo.dstType);
    }

    auto structDecl = _file->getStructDecl(fnName);
    p<FileNode> structOwner = _file;
    if (!structDecl && _yux && _yux->sdkFile()) {
        auto sdkDecl = _yux->sdkFile()->getStructDecl(fnName);
        if (sdkDecl) {
            structDecl = sdkDecl;
            structOwner = _yux->sdkFile();
        }
    }
    if (structDecl) {
        if (structDecl->isPrivate()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6008, fnName);
        }
        string effName = fnName;
        bool isGenericCtor = false;
        if (structDecl->isGeneric()) {
            const auto& typeArgs = callNode->getTypeArgs();
            if (typeArgs.empty()) {
                throw YuxError(
                    callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6009, fnName);
            }
            vector<sp<TypeInfo>> instArgs;
            instArgs.reserve(typeArgs.size());
            for (auto& tn : typeArgs) {
                instArgs.push_back(make_shared<TypeInfo>(applySubst(tn->getType())));
            }
            effName = ensureStructInstance(structDecl, instArgs, structOwner);
            isGenericCtor = true;
        }
        if (isGenericCtor) {
            auto structType = _structTypes[effName];
            auto alloca = _builder.CreateAlloca(structType, nullptr, effName + "_tmp");
            vector<llvm::Value*> ctorArgs;
            ctorArgs.push_back(alloca);
            for (size_t i = 0; i < args.size(); ++i) {
                // Phase 3c.2.a: 泛型构造器调用点 retain
                // Phase 8c: fresh 实参（call/array literal）已自带 +1，跳过 retain
                // Phase 8d.1: fresh 实参从临时帧消费
                if (!isFreshHandleExpr(callNode->getArgs()[i])) {
                    retainHandleAtCallSite(args[i], argTypes[i]);
                } else {
                    consumeTemp(args[i]);
                }
                ctorArgs.push_back(args[i]);
            }
            // 泛型实例构造器：用消费方模块作前缀（与 emit / 方法调用一致）
            string ownerMod = _structInstances[effName].consumerModule;
            string cName = Mangler::ctor(ownerMod, effName, argTypes);
            auto fn = _module->getFunction(cName);
            if (!fn) {
                vector<llvm::Type*> paramTypes;
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                for (auto& t : argTypes) {
                    if (structParamUsesPointer(t.name)) {
                        paramTypes.push_back(llvm::PointerType::get(_context, 0));
                    } else {
                        paramTypes.push_back(getLLVMType(t));
                    }
                }
                auto fnType = llvm::FunctionType::get(_builder.getVoidTy(), paramTypes, false);
                fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
            }
            _builder.CreateCall(fn, ctorArgs);
            return _builder.CreateLoad(structType, alloca);
        }
        // Phase 8c: 计算每个实参的 fresh 标志，传给构造器调用点用于跳过 retain
        vector<bool> argFresh;
        argFresh.reserve(callNode->getArgs().size());
        for (auto& a : callNode->getArgs()) argFresh.push_back(isFreshHandleExpr(a));
        auto result = compileConstructorCall(fnName, effName, args, argTypes, argFresh);
        if (result) {
            return result;
        }
    }

    // v0.6 Phase 2b: 透明类型别名解析，使 alias 名实参 / 形参在重载查找上视为同一类型
    // 函数符号表已在 validateAliases 中归一化；这里再把 argTypes 也走一遍，匹配两侧
    vector<TypeInfo> resolvedArgTypes;
    resolvedArgTypes.reserve(argTypes.size());
    for (auto& t : argTypes) resolvedArgTypes.push_back(applySubst(t));
    auto fnSymbol = _file->lookupFnSymbolWithParams(fnName, resolvedArgTypes);

    // Phase 4b: 当存在同名 generic + 非泛型重载时，参数严格匹配的非泛型优先；
    // 仅在 fnSymbol 没匹配到时才走泛型路径。这样 `assert_eq(s1 String, s2 String)`
    // 命中 SDK assert.yux 的 yux 重载，而不会跑到 #CompilerInner 的 compileTestAssertEq。
    auto genericFn = _file->getGenericFunction(fnName);
    p<FileNode> fnOwner = _file;
    if (!genericFn && _yux && _yux->sdkFile()) {
        genericFn = _yux->sdkFile()->getGenericFunction(fnName);
        if (genericFn) fnOwner = _yux->sdkFile();
    }

    if (genericFn && !fnSymbol) {
        return compileGenericFunctionCall(callNode, fnName, args, argTypes, genericFn, fnOwner);
    }

    if (fnName == "ptr_from_addr") {
        DEBUG_LOG("    Expr: PtrFromAddr");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6020);
        }
        return _builder.CreateIntToPtr(args[0], llvm::PointerType::get(_context, 0), "ptr_from_addr");
    }

    if (fnName == "rc_leak_count") {
        DEBUG_LOG("    Expr: rc_leak_count");
        if (!args.empty()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6021);
        }
        auto g = runtime::getRcBlockCountGlobal(_module, _builder);
        return _builder.CreateLoad(_builder.getInt64Ty(), g, "rc_leak");
    }

    if (fnName == "_ptr_offset") {
        DEBUG_LOG("    Expr: _ptr_offset");
        if (fnSymbol && fnSymbol->isPrivate && !fnSymbol->moduleName.empty() &&
            fnSymbol->moduleName != _file->moduleName()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6023);
        }
        if (args.size() != 2) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6022);
        }
        auto i8Ty = _builder.getInt8Ty();
        return _builder.CreateGEP(i8Ty, args[0], args[1], "ptr_off");
    }

    // 测试断言内建（spec §11.3.5）：非泛型分支
    // assert_eq:<T> 走 compileGenericFunctionCall #CompilerInner 分支
    if (fnName == "assert_true") {
        DEBUG_LOG("    Expr: assert_true");
        return compileTestAssertTrue(callNode, args, argTypes);
    }
    if (fnName == "assert_false") {
        DEBUG_LOG("    Expr: assert_false");
        return compileTestAssertFalse(callNode, args, argTypes);
    }
    if (fnName == "fail") {
        DEBUG_LOG("    Expr: fail");
        return compileTestFail(callNode, args, argTypes);
    }

    if (fnSymbol) {
        if (fnSymbol->isPrivate && !fnSymbol->moduleName.empty() && fnSymbol->moduleName != _file->moduleName()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6006, fnName);
        }
        return compileKnownFunctionCall(callNode, fnName, args, argTypes, fnSymbol);
    }

    DEBUG_LOG_VAL("    Expr: ExternalFunctionCall", fnName);
    auto retType = callNode->getType();
    bool retIsPtr = retType.isPtr();

    auto fn = _module->getFunction(fnName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (size_t i = 0; i < argTypes.size(); ++i) {
            if (argTypes[i].isPtr()) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(args[i]->getType());
            }
        }
        auto llvmRetType = retIsPtr ? llvm::PointerType::get(_context, 0) : _builder.getVoidTy();
        auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, fnName, _module);
    }

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        bool paramIsPtrInSignature = (i < fn->getFunctionType()->getNumParams()) &&
            fn->getFunctionType()->getParamType(i)->isPointerTy();

        if (paramIsPtrInSignature && args[i]->getType()->isPointerTy()) {
            callArgs.push_back(_builder.CreateBitCast(
                args[i], llvm::PointerType::get(_context, 0), "ptr_cast"));
        } else {
            callArgs.push_back(args[i]);
        }
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    if (retIsPtr) {
        return callResult;
    }

    return callResult;
}

llvm::Value* Compiler::compileGenericFunctionCall(
    p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
    p<FnNode> genericFn, p<FileNode> fnOwner) {
    
    const auto& typeParams = genericFn->header()->typeParams();
    vector<TypeInfo> typeArgs;

    const auto& explicitTypeArgs = callNode->getTypeArgs();
    if (!explicitTypeArgs.empty()) {
        if (explicitTypeArgs.size() != typeParams.size()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                ErrorCode::E6010, fnName, typeParams.size(), explicitTypeArgs.size())
                .withHint(std::format("调用处的类型实参个数需与声明匹配；改写为 `{}:<{}>(...)` 形式补齐 {} 个类型",
                    fnName,
                    std::string(typeParams.size() == 1 ? "T" : "T1, T2, ..."),
                    typeParams.size()));
        }
        for (auto& tn : explicitTypeArgs) {
            typeArgs.push_back(applySubst(tn->getType()));
        }
    } else {
        auto params = genericFn->header()->params();
        if (params.size() != argTypes.size()) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                ErrorCode::E6012, fnName, params.size(), argTypes.size());
        }

        map<string, TypeInfo> inferred;
        // 递归 unify：参数 pType 与实参 aType 匹配；遇到形如 T 的裸类型参数则记录推断
        std::function<void(const TypeInfo&, const TypeInfo&)> unify =
            [&](const TypeInfo& pType, const TypeInfo& aType) {
                if (pType.isNormal() && !isBuiltinType(pType.name)) {
                    for (auto& tp : typeParams) {
                        if (pType.name == tp) {
                            inferred[tp] = aType;
                            return;
                        }
                    }
                }
                // Generic vs Generic：同名同元数则递归各 typeArg
                if (pType.kind == TypeKind::Generic && aType.kind == TypeKind::Generic
                    && pType.name == aType.name
                    && pType.genericArgs.size() == aType.genericArgs.size()) {
                    for (size_t i = 0; i < pType.genericArgs.size(); ++i) {
                        if (pType.genericArgs[i] && aType.genericArgs[i]) {
                            unify(*pType.genericArgs[i], *aType.genericArgs[i]);
                        }
                    }
                }
            };
        for (size_t i = 0; i < params.size(); ++i) {
            auto paramType = params[i]->type();
            if (!paramType) continue;
            unify(paramType->getType(), argTypes[i]);
        }

        for (auto& tp : typeParams) {
            auto it = inferred.find(tp);
            if (it == inferred.end()) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6013, tp, fnName);
            }
            typeArgs.push_back(it->second);
        }
    }

    if (genericFn->header()->hasAnno("CompilerInner")) {
        // 测试断言泛型分支（spec §11.3.5）：assert_eq:<T> T ∈ 数值/bool
        if (fnName == "assert_eq") {
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6026, fnName);
            }
            return compileTestAssertEq(callNode, args, argTypes, typeArgs[0]);
        }
        if (fnName == "size_of") {
            if (typeArgs.empty()) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6018);
            }
            auto llvmType = getLLVMType(typeArgs[0]);
            if (!llvmType) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6019, typeArgs[0].getFullName());
            }
            auto size = _module->getDataLayout().getTypeAllocSize(llvmType);
            return _builder.getInt64(size);
        }
        if (fnName == "upgrade") {
            // Phase 1d.2：Weak<T> → Box<T>?
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6024);
            }
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6025);
            }
            auto& T = typeArgs[0];
            auto tShared = make_shared<TypeInfo>(T);
            TypeInfo weakTy("Weak", {tShared});
            TypeInfo boxTy("Box", {tShared});
            auto boxShared = make_shared<TypeInfo>(boxTy);
            TypeInfo nullableBoxTy("Nullable", {boxShared});

            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            // 取 Weak.handle
            auto weakStructTy = getLLVMType(weakTy);
            auto weakTmp = _builder.CreateAlloca(weakStructTy, nullptr, "upgrade.weak_tmp");
            _builder.CreateStore(args[0], weakTmp);
            auto handleField = _builder.CreateGEP(weakStructTy, weakTmp, {zero, zero}, "upgrade.handle_field");
            auto handle = _builder.CreateLoad(ptrTy, handleField, "upgrade.handle");

            // 调 _box_upgrade(handle) → handle_or_null
            auto upgradeFn = runtime::getBoxUpgradeFn(_module, _builder);
            auto resultHandle = _builder.CreateCall(upgradeFn, {handle}, "upgrade.result");

            // 构造 Nullable<Box<T>> = { i1 _has, { ptr handle } _value }
            auto nullableLLVMTy = getLLVMType(nullableBoxTy);
            auto resultAlloca = _builder.CreateAlloca(nullableLLVMTy, nullptr, "upgrade.nullable");
            auto hasField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, zero}, "upgrade.has_field");
            auto valueField = _builder.CreateGEP(nullableLLVMTy, resultAlloca, {zero, one}, "upgrade.value_field");

            auto isNotNull = _builder.CreateICmpNE(resultHandle, llvm::ConstantPointerNull::get(ptrTy), "upgrade.has");
            _builder.CreateStore(isNotNull, hasField);
            // Box<T> = { ptr handle }；不论 has 与否都写 handle（null 时 _has=false 已表示无效）
            auto boxStructTy = getLLVMType(boxTy);
            auto innerHandleField = _builder.CreateGEP(boxStructTy, valueField, {zero, zero}, "upgrade.inner_handle");
            _builder.CreateStore(resultHandle, innerHandleField);

            return _builder.CreateLoad(nullableLLVMTy, resultAlloca, "upgrade.value");
        }
        if (fnName == "same_ref" || fnName == "ptr_of") {
            // Phase 7：地址相等 / 显式取裸指针 builtin
            // T 必须是堆句柄类型 (Box / Weak / Array / String) 或 T&
            // 标量 / 用户 struct / Nullable 等其他类型在此报错
            size_t expectedArgs = (fnName == "same_ref" ? 2 : 1);
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6026, fnName);
            }
            if (args.size() != expectedArgs) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6027, fnName, expectedArgs);
            }
            auto& T = typeArgs[0];
            auto ptrTy = llvm::PointerType::get(_context, 0);

            // 工具：从第 i 个实参提取一个"裸指针"（handle / data / ref-ptr），按 T 的源类型决定如何抽
            // ptr_of 模式：当 T = Box/Array/String 时，需要进一步跳过 RC 头或读 data 字段；same_ref 不跳头
            auto extractRawPtr = [&](size_t i, bool forPtrOf) -> llvm::Value* {
                if (T.isBox() || T.isWeak() || T.isArrayGeneric()) {
                    // args[i] 为 { ptr handle } 结构体值；ExtractValue 0 取 handle
                    auto handle = _builder.CreateExtractValue(args[i], {0}, "handle");
                    if (!forPtrOf || T.isBox()) {
                        if (forPtrOf && T.isBox()) {
                            // Box payload 偏移 8（u32 strong + u32 weak）
                            return _builder.CreateInBoundsGEP(
                                _builder.getInt8Ty(), handle,
                                {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                                "box.payload");
                        }
                        return handle;
                    }
                    // Array<U>：读 Block.data（offset 24：8 字节 RC 头 + 16 字节 len/cap）
                    auto dataAddr = _builder.CreateInBoundsGEP(
                        _builder.getInt8Ty(), handle,
                        {llvm::ConstantInt::get(_builder.getInt64Ty(), 24)},
                        "array.data.addr");
                    return _builder.CreateLoad(ptrTy, dataAddr, "array.data");
                }
                if (T.name == "String" && T.kind == TypeKind::Normal) {
                    // String layout = { data: Array<u32> } = { { ptr handle } }
                    auto handle = _builder.CreateExtractValue(args[i], {0, 0}, "string.handle");
                    if (!forPtrOf) return handle;
                    auto dataAddr = _builder.CreateInBoundsGEP(
                        _builder.getInt8Ty(), handle,
                        {llvm::ConstantInt::get(_builder.getInt64Ty(), 24)},
                        "string.data.addr");
                    return _builder.CreateLoad(ptrTy, dataAddr, "string.data");
                }
                if (T.isRef()) {
                    // T& 路径：args[i] 是 compileExpr 自动 deref 后的 U 值，需要回溯 AST 拿原始指针
                    auto argNode = callNode->getArgs()[i];
                    if (auto litExpr = dynamic_cast<ExprLiteralNode*>(argNode)) {
                        if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                            auto name = objLit->getValue().getText();
                            auto it = _localVarPtrs.find(name);
                            if (it != _localVarPtrs.end()) return it->second;
                        }
                    }
                    if (auto refExpr = dynamic_cast<ExprGetRefNode*>(argNode)) {
                        return compileGetRefExpr(refExpr);
                    }
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6028, fnName);
                }
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6029, fnName, T.getFullName());
            };

            if (fnName == "same_ref") {
                auto p0 = extractRawPtr(0, false);
                auto p1 = extractRawPtr(1, false);
                return _builder.CreateICmpEQ(p0, p1, "same_ref");
            }
            // ptr_of
            return extractRawPtr(0, true);
        }
        if (fnName == "as_ref") {
            // spec §8.3.5.5：as_ref:<T>(box Box<T>) T&
            // 返回 box payload 起点的非空指针（跳过 8 字节 RC 头）
            // 寿命检查在 borrow_checker 处理（识别 ExprCallNode 形如 as_ref(x)）
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6026, fnName);
            }
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6027, fnName, (size_t)1);
            }
            auto& T = typeArgs[0];
            // 实参必须是 Box<T>（不接受 Box<T>?、Array、String、Weak 等）
            auto argType = callNode->getArgs()[0]->getType();
            if (!argType.isBox() || argType.isNullable()) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6029, fnName, argType.getFullName());
            }
            // args[0] 为 Box<T> = { ptr handle } 结构体值；ExtractValue 0 取 handle
            auto handle = _builder.CreateExtractValue(args[0], {0}, "as_ref.handle");
            // payload 偏移 8（u32 strong + u32 weak）
            return _builder.CreateInBoundsGEP(
                _builder.getInt8Ty(), handle,
                {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                "as_ref.payload");
        }
        if (fnName == "copy_of") {
            // spec §12.7.3 / DRAFT-const-mut [#1.I]：copy_of:<T>(x T&) T
            // 返回独立 owned T；值类型 memcpy，含 Box / Array / String / Weak 字段时按字段 retain
            // 含 Ref<U> 字段 → 报 E6032（DRAFT-const-mut §5.3 决议 #2：脱 const 出口不放任 ref）
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6026, fnName);
            }
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6027, fnName, (size_t)1);
            }
            auto& T = typeArgs[0];

            // 递归扫描 T 是否（深度）含 Ref 字段；命中即报 E6032。
            // 当前 grammar 禁止 Ref 出现在 struct 字段、turbofish 类型实参中
            // （field 走 `type` 规则、turbofish 走 `genericDef` 都不接 `&`），
            // 所以这个分支主要是防御性的，承接将来语法放宽 / 中间 TypeInfo 携带 Ref 的场景。
            // 不展开 Box / Array / Weak / Nullable / Dyn / Ptr / Fn 的类型参数：
            //   这些是堆句柄包装，copy_of 仅 retain handle，不深拷其内部，
            //   即便其内部含 Ref，也由那个内部 owned 实例自己承担约束。
            std::function<bool(const TypeInfo&, string&)> hasRefDeep;
            hasRefDeep = [&](const TypeInfo& t, string& path) -> bool {
                if (t.isRef()) { path = t.getFullName(); return true; }
                if (t.isBox() || t.isArrayGeneric() || t.isWeak() ||
                    t.isNullable() || t.isDyn() || t.isPtr() || t.isFn()) {
                    return false;
                }
                if (isBuiltinType(t.name)) return false;
                // 用户 struct：递归走字段
                auto sd = _file ? _file->getStructDecl(t.name) : nullptr;
                if (!sd && _yux && _yux->sdkFile()) {
                    sd = _yux->sdkFile()->getStructDecl(t.name);
                }
                if (!sd) return false;  // 找不到声明：保守放过（跨模块 / 类型参数等）
                for (auto& field : sd->fields()) {
                    auto ft = field->getType();
                    string inner;
                    if (hasRefDeep(ft, inner)) {
                        path = t.name + "." + field->name().getText() + " : " + inner;
                        return true;
                    }
                }
                return false;
            };
            string refPath;
            if (hasRefDeep(T, refPath)) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6032, refPath);
            }

            // args[0] 是 T 的 struct value（来自 compileExpr 自动 deref T&）
            // 把所有 RC 子结构 +1：Box/Array/Weak 抽 handle 调对应 retain；
            // struct 走 retainStructFieldsAtCallSite 递归；含 RC enum 走其分支。
            // 内置 / Ptr / 平凡 struct：no-op，直接返回 args[0]。
            retainHandleAtCallSite(args[0], T);

            // 登记为 fresh +1 句柄/struct，未被消费时帧弹出自动释放
            recordTemp(args[0], T);
            return args[0];
        }
        if (fnName == "weak") {
            // spec §4.8.3.1 / §9：weak:<T>(box Box<T>?) Weak<T>
            // 接受 Box<T> 或 Box<T>?；null/哨兵输入返回空 Weak（永远 upgrade 失败）
            // 复用 _weak_retain：复制 handle 指针 + weak 计数 +1
            if (typeArgs.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6026, fnName);
            }
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6027, fnName, (size_t)1);
            }
            auto& T = typeArgs[0];
            auto argType = callNode->getArgs()[0]->getType();

            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

            // 提取源 handle：
            //   Box<T>      → args[0] = { ptr handle }，直接抽 field 0
            //   Box<T>?     → args[0] = Nullable<Box<T>> = { i1 _has, { ptr handle } _value }
            //                  按 _has 选 inner.handle / null
            llvm::Value* srcHandle = nullptr;
            if (argType.isBox() && !argType.isNullable()) {
                srcHandle = _builder.CreateExtractValue(args[0], {0}, "weak.src.handle");
            } else if (argType.isNullable()) {
                auto inner = argType.nullableInnerType();
                if (!inner || !inner->isBox()) {
                    throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                        ErrorCode::E6029, fnName, argType.getFullName());
                }
                auto hasFlag = _builder.CreateExtractValue(args[0], {0}, "weak.has");
                auto innerHandle = _builder.CreateExtractValue(args[0], {1, 0}, "weak.inner.handle");
                srcHandle = _builder.CreateSelect(hasFlag, innerHandle, nullPtr, "weak.handle");
            } else {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                    ErrorCode::E6029, fnName, argType.getFullName());
            }

            // _weak_retain(handle)：null / 哨兵跳过；否则 weak++
            auto weakRetainFn = runtime::getWeakRetainFn(_module, _builder);
            _builder.CreateCall(weakRetainFn, {srcHandle});

            // 构造 Weak<T> = { ptr handle }
            auto tShared = make_shared<TypeInfo>(T);
            TypeInfo weakTy("Weak", {tShared});
            auto weakStructTy = getLLVMType(weakTy);
            auto resultAlloca = _builder.CreateAlloca(weakStructTy, nullptr, "weak.result");
            auto handleField = _builder.CreateGEP(weakStructTy, resultAlloca, {zero, zero},
                "weak.result.handle_field");
            _builder.CreateStore(srcHandle, handleField);
            auto result = _builder.CreateLoad(weakStructTy, resultAlloca, "weak.result.val");

            // Phase 8d.1：fresh +1 weak 句柄，登记到当前语句临时帧，
            // 未被消费时帧弹出自动 _weak_release。
            recordTemp(result, weakTy);
            return result;
        }
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
            ErrorCode::E6017, fnName);
    }

    // §6.4.4.4 / §12.4 边界单态化校验 (Phase 3.3): 对每个 <T : D1 + D2>,
    // 解析每个 D 的限定名并校验 typeArgs[i] 是否满足 D (显式 impl 或
    // #DraftLike 结构匹配); 不满足报 E1106. 不影响 ensureFnInstance 的
    // mangle (单态化静态分发, 边界仅做静态检查).
    if (_yux) {
        const auto& bounds = genericFn->header()->typeParamBounds();
        if (!bounds.empty()) {
            auto& reg = _yux->draftRegistry();
            auto& checker = _yux->draftImplChecker();
            for (size_t i = 0; i < typeParams.size() && i < bounds.size(); ++i) {
                for (auto& boundName : bounds[i]) {
                    auto resolved = reg.resolve(boundName, fnOwner);
                    if (!resolved) {
                        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                            ErrorCode::E3032, boundName);
                    }
                    // v0.5: 函数声明位 draftBound 暂未携带类型实参 (ast_builder
                    // 仅取基名), draftTypeArgs 传空; 草案 §6.4.4.1 文法允许
                    // `D<T>` 形态留待后续扩展.
                    std::vector<TypeInfo> draftTypeArgs;
                    if (!checker.boundSatisfied(typeArgs[i], resolved->decl,
                                                 resolved->qualifiedName, draftTypeArgs)) {
                        throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                            ErrorCode::E1106,
                            typeArgs[i].getFullName(),
                            resolved->qualifiedName,
                            typeParams[i]);
                    }
                }
            }
        }
    }

    string mangledName = ensureFnInstance(genericFn, typeArgs, fnOwner, callNode->getLineNumber());

    map<string, TypeInfo> subst;
    for (size_t i = 0; i < typeParams.size(); ++i) {
        subst[typeParams[i]] = typeArgs[i];
    }
    _substStack.push_back(SubstFrame{subst, "", ""});

    vector<TypeInfo> instParamTypes;
    for (auto param : genericFn->header()->params()) {
        if (param->type()) {
            instParamTypes.push_back(applySubst(param->type()->getType()));
        }
    }

    TypeInfo instRetType;
    if (genericFn->header()->retType()) {
        instRetType = applySubst(genericFn->header()->retType()->getType());
    }

    _substStack.pop_back();

    bool isPrivate = !fnName.empty() && fnName[0] == '_';
    string cName = Mangler::function(fnOwner->moduleName(), mangledName, instParamTypes, isPrivate);
    DEBUG_LOG_VAL("    Expr: GenericFunctionCall", fnName << " -> " << cName);

    auto fn = _module->getFunction(cName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (auto& t : instParamTypes) {
            if (!t.isPtr() && !t.isRef() && structParamUsesPointer(t.name)) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(getLLVMType(t));
            }
        }
        string gFallibleErr;
        if (auto e = genericFn->header()->getAnnoArg("Fallible")) gFallibleErr = *e;
        auto retType = wrapFallibleRetType(instRetType, gFallibleErr);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size(); ++i) {
        auto& at = instParamTypes[i];
        if (at.isPtr() || at.isRef()) {
            callArgs.push_back(args[i]);
            continue;
        }
        // Phase 3a: Box/Array/Weak 实参传前 retain（callee-clean）
        // Phase 8c: fresh 实参（call/array literal）已自带 +1，跳过 retain
        // Phase 8d.1: fresh 实参的 +1 移交给 callee，从临时帧消费掉，避免帧末多余 release
        bool isFresh = isFreshHandleExpr(callNode->getArgs()[i]);
        if (typeNeedsDestructor(at)) {
            if (!isFresh) {
                retainHandleAtCallSite(args[i], at);
            } else {
                consumeTemp(args[i]);
            }
            callArgs.push_back(args[i]);
            continue;
        }
        if (structParamUsesPointer(at.name)) {
            auto structType = getLLVMType(at);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
        } else {
            callArgs.push_back(args[i]);
        }
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    if (!instRetType.empty()) {
        if (instRetType.isPtr() || instRetType.isRef()) {
            return callResult;
        }
        auto sd = _file->getStructDecl(instRetType.name);
        if (!sd && _yux && _yux->sdkFile()) {
            sd = _yux->sdkFile()->getStructDecl(instRetType.name);
        }
        if (sd && !isBuiltinType(instRetType.name)) {
            auto structType = getLLVMType(instRetType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "ret_tmp");
            _builder.CreateStore(callResult, alloca);
            return _builder.CreateLoad(structType, alloca);
        }
    }

    return callResult;
}

llvm::Value* Compiler::compileArrayMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    auto elemType = baseType.arrayGenericElementType();
    auto arrayStructType = getLLVMType(baseType);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto i64Ty = _builder.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(_context, 0);

    llvm::Value* arrayPtr = nullptr;
    if (auto baseLit = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto obj = dynamic_cast<LiteralObjNode*>(baseLit->literal())) {
            auto it = _localVarPtrs.find(obj->getValue().getText());
            if (it != _localVarPtrs.end()) {
                arrayPtr = it->second;
            }
        }
    } else if (auto dotBase = dynamic_cast<ExprDotNode*>(baseExpr)) {
        auto outerBase = dotBase->baseExpr();
        auto outerType = outerBase->getType();
        TypeInfo outerActual = outerType;
        if (outerType.isRef()) {
            auto t = outerType.refElementType();
            if (t) outerActual = *t;
        }
        if (outerType.isBox()) {
            auto t = outerType.boxElementType();
            if (t) outerActual = *t;
        }
        llvm::Value* outerPtr = nullptr;
        if (auto ol = dynamic_cast<ExprLiteralNode*>(outerBase)) {
            if (auto oobj = dynamic_cast<LiteralObjNode*>(ol->literal())) {
                auto it = _localVarPtrs.find(oobj->getValue().getText());
                if (it != _localVarPtrs.end()) {
                    outerPtr = it->second;
                }
            }
        }
        auto outerStructDecl = _file->getStructDecl(outerActual.name);
        if (!outerStructDecl && _yux && _yux->sdkFile()) {
            outerStructDecl = _yux->sdkFile()->getStructDecl(outerActual.name);
        }
        if (outerPtr && outerStructDecl) {
            int fi = outerStructDecl->fieldIndex(dotBase->member());
            if (fi >= 0) {
                llvm::Value* dataPtr = outerPtr;
                if (outerType.isBox()) {
                    // Box.field：load handle，payload = handle + 8
                    auto boxStructType = getLLVMType(outerType);
                    auto handleField = _builder.CreateGEP(boxStructType, outerPtr, {zero, zero}, "box.handle_field");
                    auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "box.handle");
                    dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "box.payload");
                }
                auto outerLLVM = getLLVMType(outerActual);
                auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fi);
                llvm::Value* indices[] = {zero, idx};
                arrayPtr = _builder.CreateGEP(outerLLVM, dataPtr, indices, "array.field.ptr");
            }
        }
    }

    auto getReadPtr = [&]() -> llvm::Value* {
        if (arrayPtr) return arrayPtr;
        auto baseVal = compileExpr(baseExpr);
        auto tmp = _builder.CreateAlloca(arrayStructType, nullptr, "array_tmp");
        _builder.CreateStore(baseVal, tmp);
        return tmp;
    };

    if (member == "len") {
        DEBUG_LOG("    Expr: Array.len()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenField = arrayBlockLenPtr(handle);
        return _builder.CreateLoad(i64Ty, lenField, "array.len");
    }
    if (member == "cap") {
        DEBUG_LOG("    Expr: Array.cap()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto capField = arrayBlockCapPtr(handle);
        return _builder.CreateLoad(i64Ty, capField, "array.cap");
    }

    if (!elemType) {
        throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E3055);
    }
    auto elemLLVMType = getLLVMType(*elemType);

    if (member == "is_empty") {
        DEBUG_LOG("    Expr: Array.is_empty()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenField = arrayBlockLenPtr(handle);
        auto lenVal = _builder.CreateLoad(i64Ty, lenField, "array.len");
        return _builder.CreateICmpEQ(lenVal, _builder.getInt64(0), "array.is_empty");
    }

    if (member == "at") {
        DEBUG_LOG("    Expr: Array.at()");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6040);
        }
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {args[0]}, "at.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "at.elem");
    }

    if (member == "first") {
        DEBUG_LOG("    Expr: Array.first()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {_builder.getInt64(0)}, "first.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "first.elem");
    }

    if (member == "last") {
        DEBUG_LOG("    Expr: Array.last()");
        auto ptr = getReadPtr();
        auto handle = loadArrayHandle(ptr);
        auto lenVal = _builder.CreateLoad(i64Ty, arrayBlockLenPtr(handle), "array.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "last.idx");
        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "last.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "last.elem");
    }

    if (member == "pop") {
        DEBUG_LOG("    Expr: Array.pop()");
        if (!arrayPtr) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6041);
        }
        auto handle = loadArrayHandle(arrayPtr);
        auto lenFieldPtr = arrayBlockLenPtr(handle);
        auto lenVal = _builder.CreateLoad(i64Ty, lenFieldPtr, "a.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "pop.idx");

        auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "pop.elem.ptr");
        auto elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "pop.elem");

        _builder.CreateStore(lastIdx, lenFieldPtr);
        return elemVal;
    }

    if (member == "push" || member == "set_len" || member == "clear") {
        if (!arrayPtr) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(),
                ErrorCode::E6042, member);
        }
        auto handle = loadArrayHandle(arrayPtr);
        auto lenFieldPtr = arrayBlockLenPtr(handle);
        auto capFieldPtr = arrayBlockCapPtr(handle);

        auto voidResult = [&]() -> llvm::Value* {
            return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        };

        if (member == "clear") {
            DEBUG_LOG("    Expr: Array.clear()");
            _builder.CreateStore(_builder.getInt64(0), lenFieldPtr);
            return voidResult();
        }
        if (member == "set_len") {
            DEBUG_LOG("    Expr: Array.set_len()");
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6043);
            }
            _builder.CreateStore(args[0], lenFieldPtr);
            return voidResult();
        }
        DEBUG_LOG("    Expr: Array.push()");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6044);
        }
        auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
        auto elemVal = args[0];
        auto lenVal = _builder.CreateLoad(i64Ty, lenFieldPtr, "a.len");
        auto capVal = _builder.CreateLoad(i64Ty, capFieldPtr, "a.cap");

        auto needGrow = _builder.CreateICmpUGE(lenVal, capVal, "push.need_grow");
        auto growBB = llvm::BasicBlock::Create(_context, "push.grow", _currentFn);
        auto storeBB = llvm::BasicBlock::Create(_context, "push.store", _currentFn);
        _builder.CreateCondBr(needGrow, growBB, storeBB);

        // 扩容路径：通过 _array_grow 在 Block 内原地更新 cap、data
        _builder.SetInsertPoint(growBB);
        auto capIsZero = _builder.CreateICmpEQ(capVal, _builder.getInt64(0), "cap.is_zero");
        auto doubled = _builder.CreateMul(capVal, _builder.getInt64(2), "cap.dbl");
        auto newCap = _builder.CreateSelect(capIsZero, _builder.getInt64(4), doubled, "new.cap");
        auto growFn = runtime::getArrayGrowFn(_module, _builder);
        _builder.CreateCall(growFn, {handle, _builder.getInt64(elemSize), newCap});
        _builder.CreateBr(storeBB);

        // 写入新元素并 len++
        _builder.SetInsertPoint(storeBB);
        auto curData = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(handle), "a.data.cur");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, curData, {lenVal}, "push.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
        auto newLen = _builder.CreateAdd(lenVal, _builder.getInt64(1), "new.len");
        _builder.CreateStore(newLen, lenFieldPtr);
        return voidResult();
    }

    return nullptr;
}

llvm::Value* Compiler::compileBuiltinTypeMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        if (isBuiltinType(dstType)) {
            DEBUG_LOG_VAL("    Expr: CastCall (to_)", dstType);
            auto baseVal = compileExpr(baseExpr);
            auto srcType = baseExpr->getType();
            return createCast(baseVal, srcType, TypeInfo(dstType));
        }
    }
    
    // 处理 #CompilerInner 运算符方法：直接生成 LLVM IR
    if (isCompilerInnerMethod(baseType.name, member)) {
        auto baseVal = compileExpr(baseExpr);
        bool isFloat = baseType.startsWith('f');
        bool isUnsigned = baseType.startsWith('u');
        
        // 算术运算符
        if (member == "plus") {
            DEBUG_LOG_VAL("    Expr: CompilerInner plus", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "plus");
            }
            if (isFloat) {
                return _builder.CreateFAdd(baseVal, args[0], "add");
            }
            return _builder.CreateAdd(baseVal, args[0], "add");
        }
        if (member == "minus") {
            DEBUG_LOG_VAL("    Expr: CompilerInner minus", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "minus");
            }
            if (isFloat) {
                return _builder.CreateFSub(baseVal, args[0], "sub");
            }
            return _builder.CreateSub(baseVal, args[0], "sub");
        }
        if (member == "mul") {
            DEBUG_LOG_VAL("    Expr: CompilerInner mul", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "mul");
            }
            if (isFloat) {
                return _builder.CreateFMul(baseVal, args[0], "mul");
            }
            return _builder.CreateMul(baseVal, args[0], "mul");
        }
        if (member == "div") {
            DEBUG_LOG_VAL("    Expr: CompilerInner div", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "div");
            }
            if (isFloat) {
                return _builder.CreateFDiv(baseVal, args[0], "div");
            }
            if (isUnsigned) {
                return _builder.CreateUDiv(baseVal, args[0], "div");
            }
            return _builder.CreateSDiv(baseVal, args[0], "div");
        }
        if (member == "mod") {
            DEBUG_LOG_VAL("    Expr: CompilerInner mod", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "mod");
            }
            if (isFloat) {
                return _builder.CreateFRem(baseVal, args[0], "mod");
            }
            if (isUnsigned) {
                return _builder.CreateURem(baseVal, args[0], "mod");
            }
            return _builder.CreateSRem(baseVal, args[0], "mod");
        }
        
        // 比较运算符
        if (member == "eq") {
            DEBUG_LOG_VAL("    Expr: CompilerInner eq", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "eq");
            }
            if (isFloat) {
                return _builder.CreateFCmpOEQ(baseVal, args[0], "eq");
            }
            return _builder.CreateICmpEQ(baseVal, args[0], "eq");
        }
        if (member == "ne") {
            DEBUG_LOG_VAL("    Expr: CompilerInner ne", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "ne");
            }
            if (isFloat) {
                return _builder.CreateFCmpONE(baseVal, args[0], "ne");
            }
            return _builder.CreateICmpNE(baseVal, args[0], "ne");
        }
        if (member == "lt") {
            DEBUG_LOG_VAL("    Expr: CompilerInner lt", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "lt");
            }
            if (isFloat) {
                return _builder.CreateFCmpOLT(baseVal, args[0], "lt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULT(baseVal, args[0], "lt");
            }
            return _builder.CreateICmpSLT(baseVal, args[0], "lt");
        }
        if (member == "le") {
            DEBUG_LOG_VAL("    Expr: CompilerInner le", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "le");
            }
            if (isFloat) {
                return _builder.CreateFCmpOLE(baseVal, args[0], "le");
            }
            if (isUnsigned) {
                return _builder.CreateICmpULE(baseVal, args[0], "le");
            }
            return _builder.CreateICmpSLE(baseVal, args[0], "le");
        }
        if (member == "gt") {
            DEBUG_LOG_VAL("    Expr: CompilerInner gt", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "gt");
            }
            if (isFloat) {
                return _builder.CreateFCmpOGT(baseVal, args[0], "gt");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGT(baseVal, args[0], "gt");
            }
            return _builder.CreateICmpSGT(baseVal, args[0], "gt");
        }
        if (member == "ge") {
            DEBUG_LOG_VAL("    Expr: CompilerInner ge", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "ge");
            }
            if (isFloat) {
                return _builder.CreateFCmpOGE(baseVal, args[0], "ge");
            }
            if (isUnsigned) {
                return _builder.CreateICmpUGE(baseVal, args[0], "ge");
            }
            return _builder.CreateICmpSGE(baseVal, args[0], "ge");
        }
        
        // 位运算符
        if (member == "and") {
            DEBUG_LOG_VAL("    Expr: CompilerInner and", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "and");
            }
            return _builder.CreateAnd(baseVal, args[0], "and");
        }
        if (member == "or") {
            DEBUG_LOG_VAL("    Expr: CompilerInner or", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "or");
            }
            return _builder.CreateOr(baseVal, args[0], "or");
        }
        if (member == "xor") {
            DEBUG_LOG_VAL("    Expr: CompilerInner xor", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "xor");
            }
            return _builder.CreateXor(baseVal, args[0], "xor");
        }
        if (member == "shl") {
            DEBUG_LOG_VAL("    Expr: CompilerInner shl", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "shl");
            }
            return _builder.CreateShl(baseVal, args[0], "shl");
        }
        if (member == "shr") {
            DEBUG_LOG_VAL("    Expr: CompilerInner shr", baseType.name);
            if (args.size() != 1) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6045, "shr");
            }
            if (isUnsigned) {
                return _builder.CreateLShr(baseVal, args[0], "shr");
            }
            return _builder.CreateAShr(baseVal, args[0], "shr");
        }
        
        // 一元运算符
        if (member == "neg") {
            DEBUG_LOG_VAL("    Expr: CompilerInner neg", baseType.name);
            if (isFloat) {
                return _builder.CreateFNeg(baseVal, "neg");
            }
            return _builder.CreateNeg(baseVal, "neg");
        }
        if (member == "inv") {
            DEBUG_LOG_VAL("    Expr: CompilerInner inv", baseType.name);
            if (isFloat) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E3070, baseType.name);
            }
            return _builder.CreateNot(baseVal, "inv");
        }
        if (member == "not") {
            DEBUG_LOG_VAL("    Expr: CompilerInner not", baseType.name);
            return _builder.CreateNot(baseVal, "not");
        }
    }
    
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(baseType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }
    
    string methodFullName = baseType.name + "." + member;
    FnSymbolInfo* sdkMethodSymbol = nullptr;
    if (_yux && _yux->sdkFile()) {
        sdkMethodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (sdkMethodSymbol) {
        DEBUG_LOG_VAL("    Expr: BuiltinTypeMethodCall (SDK)", methodFullName);
        
        auto baseVal = compileExpr(baseExpr);
        
        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(baseVal);
        for (auto& arg : args) {
            methodArgs.push_back(arg);
        }
        
        string ownerMod = _yux->sdkFile()->moduleName();
        bool methPriv = !member.empty() && member[0] == '_';
        string mangledName = Mangler::method(ownerMod, baseType.name, member, argTypes, methPriv);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(getLLVMType(baseType));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = wrapFallibleRetType(sdkMethodSymbol->retType, sdkMethodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6016, member, baseType.name);
}

llvm::Value* Compiler::compileStructMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType, const TypeInfo& actualType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    
    if (actualType.isGeneric()) {
        auto baseDecl = _file->getStructDecl(actualType.name);
        p<FileNode> owner = _file;
        if (!baseDecl && _yux && _yux->sdkFile()) {
            baseDecl = _yux->sdkFile()->getStructDecl(actualType.name);
            if (baseDecl) owner = _yux->sdkFile();
        }
        if (baseDecl && baseDecl->isGeneric()) {
            string effName = ensureStructInstance(baseDecl, actualType.genericArgs, owner);
            auto& inst = _structInstances[effName];
            if (inst.baseImpl) {
                p<FnNode> chosen = nullptr;
                for (auto m : inst.baseImpl->methods()) {
                    if (m->header()->name().getText() != member) continue;
                    if (m->header()->params().size() != argTypes.size()) continue;
                    chosen = m;
                    break;
                }
                if (chosen) {
                    llvm::Value* basePtr = nullptr;
                    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                            auto varName = objLiteral->getValue().getText();
                            auto it = _localVarPtrs.find(varName);
                            if (it != _localVarPtrs.end()) basePtr = it->second;
                        }
                    }
                    if (!basePtr) {
                        auto baseVal = compileExpr(baseExpr);
                        auto structType = _structTypes[effName];
                        auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
                        _builder.CreateStore(baseVal, alloca);
                        basePtr = alloca;
                    }

                    vector<llvm::Value*> methodArgs;
                    methodArgs.push_back(basePtr);
                    for (auto& a : args) methodArgs.push_back(a);

                    // 泛型实例方法：用消费方模块作为前缀（与 emit 端一致）
                    string ownerMod = inst.consumerModule;
                    bool methPriv = !member.empty() && member[0] == '_';
                    string mangledName = Mangler::method(ownerMod, effName, member, argTypes, methPriv);
                    auto fn = _module->getFunction(mangledName);
                    if (!fn) {
                        map<string, TypeInfo> subst;
                        for (size_t i = 0; i < inst.args.size(); ++i) {
                            subst[inst.baseDecl->typeParams()[i]] = inst.args[i];
                        }
                        vector<llvm::Type*> paramTypes;
                        paramTypes.push_back(llvm::PointerType::get(_context, 0));
                        for (auto& t : argTypes) {
                            if (structParamUsesPointer(t.name)) {
                                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                            } else {
                                paramTypes.push_back(getLLVMType(t));
                            }
                        }
                        TypeInfo retType;
                        if (chosen->header()->retType()) {
                            retType = chosen->header()->retType()->getType().substitute(subst);
                        }
                        string mFallibleErr;
                        if (auto e = chosen->header()->getAnnoArg("Fallible")) mFallibleErr = *e;
                        auto llvmRetType = wrapFallibleRetType(retType, mFallibleErr);
                        auto fnType = llvm::FunctionType::get(llvmRetType, paramTypes, false);
                        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
                    }
                    return _builder.CreateCall(fn, methodArgs);
                }
            }
        }
    }

    string methodFullName = actualType.name + "." + member;

    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(actualType);
    for (auto& t : argTypes) {
        methodParamTypes.push_back(t);
    }
    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);

    if (methodSymbol) {
        DEBUG_LOG_VAL("    Expr: MethodCall", methodFullName);

        if (methodSymbol->isPrivate) {
            string currentBase = _currentStructName;
            auto dollarPos = currentBase.find('$');
            if (dollarPos != string::npos) currentBase = currentBase.substr(0, dollarPos);
            if (currentBase != actualType.name) {
                throw YuxError(callNode->getLineNumber(), callNode->getColumn(), ErrorCode::E6007, member, actualType.name);
            }
        }

        llvm::Value* basePtr = nullptr;
        if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
            if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                auto varName = objLiteral->getValue().getText();
                auto it = _localVarPtrs.find(varName);
                if (it != _localVarPtrs.end()) {
                    basePtr = it->second;
                }
            }
        }

        if (!basePtr) {
            auto baseVal = compileExpr(baseExpr);
            auto structType = getLLVMType(actualType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "method_tmp");
            _builder.CreateStore(baseVal, alloca);
            basePtr = alloca;
        }

        llvm::Value* dataPtr = basePtr;

        if (baseType.isBox()) {
            // Box 方法 receiver：load handle，payload = handle + 8
            auto boxStructType = getLLVMType(baseType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(boxStructType, basePtr, {zero, zero}, "box.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "box.handle");
            dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "box.payload");
        }

        // Box<primitive> 方法调用：内置类型方法的 receiver 走 by-value ABI
        // （见 getMethodFunction line 291：isBuiltinType(structName) 时第 0 槽用
        // getLLVMType(structName)，对应 compileMethod line 672-678 把首参 alloca + store
        // 作为 `$`）。这里要把 payload load 出来按值传，否则与 callee 签名不一致：
        // - 单跑 b.to_string()：callee 把 ptr 当 i32 读 → 栈上残值；
        // - 与 42i32.to_string() 共存：fn 已被前者按 (i32)→T 声明，此处再按 (ptr)→T
        //   call 触发 LLVM "Calling a function with a bad signature!" assert。
        bool receiverByValue = isBuiltinType(actualType.name);
        llvm::Value* receiverArg = dataPtr;
        if (receiverByValue) {
            auto receiverTy = getLLVMType(actualType);
            receiverArg = _builder.CreateLoad(receiverTy, dataPtr, "box.payload.val");
        }

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(receiverArg);
        for (size_t i = 0; i < args.size(); ++i) {
            auto& at = argTypes[i];
            if (structParamUsesPointer(at.name)) {
                auto structType = getLLVMType(at);
                auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
                _builder.CreateStore(args[i], alloca);
                methodArgs.push_back(alloca);
            } else {
                methodArgs.push_back(args[i]);
            }
        }

        string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
        bool methPriv = !member.empty() && member[0] == '_';
        string mangledName = Mangler::method(ownerMod, actualType.name, member, argTypes, methPriv);
        auto fn = _module->getFunction(mangledName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            if (receiverByValue) {
                paramTypes.push_back(getLLVMType(actualType));
            } else {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            }
            for (auto& t : argTypes) {
                if (structParamUsesPointer(t.name)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto retType = wrapFallibleRetType(methodSymbol->retType, methodSymbol->fallibleErrType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    return nullptr;
}

// Phase 2d: Dyn<D> / Dyn<D&> 方法调用静态检查 (vtable 间接调用 codegen 推 Phase 3d).
//
// 流程:
//   1. 从 baseType (Dyn<D> / Dyn<D&>) 取 D, 在 draft 注册表按 _file 可见性解析.
//      解析失败 (理论上 Phase 2b/2c 已拦截) → 直接 throw E1131.
//   2. 在 D 的 signatures 中按 member 名查找; 失败 → 抛"未定义方法"风格诊断
//      (沿用 E6016 段位, type = baseType.getFullName(), 与 builtin 未定义方法一致).
//   3. arity 严格匹配 sig->params().size() 与 argTypes.size(); 不匹配 → E6012.
//   4. 参数类型按 D 签名 (不是具体实现签名) 逐位比对; 不匹配 → E3001 风格暂复用 E6015
//      (后续 4c 落 E3xxx 明确码; 此处先用通用 E6015 + hint, 保证 Phase 2d 闭环).
//   5. Phase 2d 不接 codegen: 命中合法调用统一抛 E6015 + hint「Phase 3d pending」.
//      Phase 3d 把第 5 步替换为 load vtable[i] + indirect call.
llvm::Value* Compiler::compileDynMethodCall(
    p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
    const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
    int line = callNode->getLineNumber();
    int col = callNode->getColumn();

    // 1. 取 D 名 (剥 Dyn<D&> 的内层 Ref); 解析为 draft decl.
    auto draftInner = baseType.dynDraftType();
    string draftBare = draftInner ? draftInner->name : string();
    DraftDeclNode* draftDecl = nullptr;
    string draftQualified;
    if (_yux && _file && !draftBare.empty()) {
        auto& reg = _yux->draftRegistry();
        if (auto resolved = reg.resolve(draftBare, _file)) {
            draftDecl = resolved->decl;
            draftQualified = resolved->qualifiedName;
        }
    }
    if (!draftDecl) {
        throw YuxError(line, col, ErrorCode::E1131,
            draftBare.empty() ? string("?") : draftBare);
    }

    // 2. 找方法签名 (按名 + arity 匹配; yux 暂无方法名重载, 第一处即终)
    FnHeaderNode* sig = nullptr;
    for (auto& s : draftDecl->signatures()) {
        if (!s) continue;
        if (s->name().getText() != member) continue;
        sig = s;
        break;
    }
    if (!sig) {
        throw YuxError(line, col, ErrorCode::E6016, member, baseType.getFullName());
    }

    // 3. arity 校验
    if (sig->params().size() != argTypes.size()) {
        throw YuxError(line, col, ErrorCode::E6012, member,
            (int)sig->params().size(), (int)argTypes.size());
    }

    // 4. 参数类型按 D 签名比对 (类型名 + 全名相等; 与 yux 名义类型一致)
    for (size_t i = 0; i < sig->params().size(); ++i) {
        auto sp = sig->params()[i];
        if (!sp || !sp->type()) continue;
        TypeInfo expected = sp->type()->getType();
        if (expected.getFullName() != argTypes[i].getFullName()) {
            throw YuxError(line, col, ErrorCode::E6015)
                .withHint("Dyn<" + draftQualified + ">." + member + " arg#"
                    + std::to_string(i) + ": 期望 " + expected.getFullName()
                    + ", 实际 " + argTypes[i].getFullName()
                    + " (Dyn 方法调用参数类型按 draft 签名静态匹配)");
        }
    }

    // 5. Phase 3d: load fat_ptr.vtable → GEP slot[i+1] → load fn ptr → indirect call.
    //    receiver:
    //      - Dyn<D>  (owned)  : data + 8（跳过 Box RC 头，与 compileStructMethodCall 的
    //                           Box receiver 一致；layout 见 compileDynCtorExpr）
    //      - Dyn<D&> (借用)   : data 直接是实例指针（裸 ref）
    //    fn 签名按 D.sig 还原：(ptr receiver, P1, ..., Pn) -> R
    //    （对象安全确保 sig 不含 Self / 自身名，所以 D.sig 形参/返回类型与 U.impl 一致）

    // 找到方法在 D.signatures() 中的下标（vtable 槽 0 是 dtor，方法从 1 开始）
    size_t methodIdx = 0;
    for (size_t i = 0; i < draftDecl->signatures().size(); ++i) {
        if (draftDecl->signatures()[i] == sig) { methodIdx = i; break; }
    }

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto i32Ty = _builder.getInt32Ty();

    // 5.1 编译 baseExpr → 落到 alloca 以便 GEP 出 vtable / data 字段
    auto fatStructTy = getLLVMType(baseType);  // { ptr, ptr }
    llvm::Value* fatAlloca = nullptr;
    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) fatAlloca = it->second;
        }
    }
    if (!fatAlloca) {
        auto baseVal = compileExpr(baseExpr);
        fatAlloca = _builder.CreateAlloca(fatStructTy, nullptr, "dyn.tmp");
        _builder.CreateStore(baseVal, fatAlloca);
    }

    auto zero = llvm::ConstantInt::get(i32Ty, 0);
    auto one = llvm::ConstantInt::get(i32Ty, 1);
    auto vtableFieldPtr = _builder.CreateGEP(fatStructTy, fatAlloca, {zero, zero}, "dyn.vtable.field");
    auto vtablePtr = _builder.CreateLoad(ptrTy, vtableFieldPtr, "dyn.vtable.load");
    auto dataFieldPtr = _builder.CreateGEP(fatStructTy, fatAlloca, {zero, one}, "dyn.data.field");
    auto dataPtr = _builder.CreateLoad(ptrTy, dataFieldPtr, "dyn.data.load");

    // 5.2 GEP vtable[methodIdx + 1] → load fn ptr
    // vtable 是 i8* 数组，按 ptr 步长 GEP 即可
    auto slotIdx = llvm::ConstantInt::get(_builder.getInt64Ty(), (uint64_t)(methodIdx + 1));
    auto slotPtr = _builder.CreateGEP(ptrTy, vtablePtr, {slotIdx}, "dyn.slot.ptr");
    auto fnPtr = _builder.CreateLoad(ptrTy, slotPtr, "dyn.fn.ptr");

    // 5.3 receiver：owned → data + 8（跳 RC 头）；borrow → data 直接是实例指针
    llvm::Value* receiver = dataPtr;
    if (baseType.isDynOwned()) {
        receiver = _builder.CreateGEP(_builder.getInt8Ty(), dataPtr,
                                       {_builder.getInt64(8)}, "dyn.payload");
    }

    // 5.4 构建 indirect call 的 FunctionType（与 vtable 端 forward-declare 一致）
    std::vector<llvm::Type*> llvmParamTypes;
    llvmParamTypes.push_back(ptrTy);  // receiver
    for (auto& p : sig->params()) {
        if (!p || !p->type()) continue;
        auto pt = p->type()->getType();
        if (pt.isPtr() || pt.isRef()) {
            llvmParamTypes.push_back(ptrTy);
        } else {
            llvmParamTypes.push_back(getLLVMType(pt));
        }
    }
    llvm::Type* llvmRetType = _builder.getVoidTy();
    TypeInfo retType;
    if (sig->retType()) {
        retType = sig->retType()->getType();
        if (!retType.empty()) llvmRetType = getLLVMType(retType);
    }
    auto fnTy = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);

    // 5.5 组装实参并 indirect call
    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(receiver);
    for (size_t i = 0; i < args.size(); ++i) {
        auto& at = argTypes[i];
        if (structParamUsesPointer(at.name)) {
            auto stTy = getLLVMType(at);
            auto alloca = _builder.CreateAlloca(stTy, nullptr, "dyn.arg.tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
        } else {
            callArgs.push_back(args[i]);
        }
    }

    const char* callName = llvmRetType->isVoidTy() ? "" : "dyn.call";
    return _builder.CreateCall(fnTy, fnPtr, callArgs, callName);
}

llvm::Value* Compiler::compileConstructorCall(
    const string& baseName, const string& effName,
    vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
    const vector<bool>& argFresh) {
    string ctorFullName = baseName + "." + baseName;

    vector<TypeInfo> ctorParamTypes;
    ctorParamTypes.push_back(TypeInfo(baseName));
    for (auto& t : argTypes) {
        ctorParamTypes.push_back(t);
    }
    auto ctorSymbol = _file->lookupFnSymbolWithParams(ctorFullName, ctorParamTypes);

    if (ctorSymbol) {
        DEBUG_LOG_VAL("    Expr: ConstructorCall", effName);

        llvm::Type* structType = nullptr;
        if (auto it = _structTypes.find(effName); it != _structTypes.end()) {
            structType = it->second;
        } else {
            structType = getLLVMType(TypeInfo(effName));
        }
        auto alloca = _builder.CreateAlloca(structType, nullptr, effName + "_tmp");

        vector<llvm::Value*> ctorArgs;
        ctorArgs.push_back(alloca);
        for (size_t i = 0; i < args.size(); ++i) {
            // Phase 3c.2.a: 构造器调用点 retain；与函数调用同协议
            // Phase 8c: fresh 实参跳过 retain
            // Phase 8d.1: fresh 实参从临时帧消费
            if (!argFresh.empty() && argFresh[i]) {
                consumeTemp(args[i]);
                ctorArgs.push_back(args[i]);
                continue;
            }
            retainHandleAtCallSite(args[i], argTypes[i]);
            ctorArgs.push_back(args[i]);
        }

        // 若 effName 是泛型实例，按消费方模块取前缀；否则按 ctorSymbol 的模块。
        string ownerMod;
        if (auto instIt = _structInstances.find(effName); instIt != _structInstances.end()) {
            ownerMod = instIt->second.consumerModule;
        } else {
            ownerMod = ctorSymbol->moduleName.empty() ? _file->moduleName() : ctorSymbol->moduleName;
        }
        string cName = Mangler::ctor(ownerMod, effName, argTypes);
        auto fn = _module->getFunction(cName);
        if (!fn) {
            vector<llvm::Type*> paramTypes;
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            for (auto& t : argTypes) {
                paramTypes.push_back(getLLVMType(t));
            }
            auto retType = _builder.getVoidTy();
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
        }
        _builder.CreateCall(fn, ctorArgs);

        return _builder.CreateLoad(structType, alloca);
    }
    return nullptr;
}

llvm::Value* Compiler::compileKnownFunctionCall(
    p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
    FnSymbolInfo* fnSymbol) {
    string cName;
    if (fnSymbol->isExternal) {
        cName = fnName;
    } else if (fnName == "main") {
        cName = "yux_main";
    } else {
        string ownerMod = fnSymbol->moduleName.empty() ? _file->moduleName() : fnSymbol->moduleName;
        bool isPriv = !fnName.empty() && fnName[0] == '_';
        cName = Mangler::function(ownerMod, fnName, fnSymbol->params, isPriv);
    }

    DEBUG_LOG_VAL("    Expr: FunctionCall", fnName << " -> " << cName);
    auto fn = _module->getFunction(cName);

    bool needPtrConversion = fnSymbol->isExternal;
    for (auto& param : fnSymbol->params) {
        if (param.isPtr()) {
            needPtrConversion = true;
            break;
        }
    }

    if (!fn) {
        vector<llvm::Type*> paramTypes;
        for (size_t i = 0; i < fnSymbol->params.size(); ++i) {
            if (fnSymbol->params[i].isPtr() || fnSymbol->params[i].isRef()) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else if (structParamUsesPointer(fnSymbol->params[i].name)) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(getLLVMType(fnSymbol->params[i]));
            }
        }
        // extern fn 禁 #Fallible（[#7]）—— extern 路径走原 isPtr 分支不包装；
        // 用户 fn 走 wrapFallibleRetType，按 fnSymbol->fallibleErrType 决定是否包成 struct
        llvm::Type* retType;
        if (fnSymbol->isExternal && !fnSymbol->retType.empty() && TypeInfo(fnSymbol->retType).isPtr()) {
            retType = llvm::PointerType::get(_context, 0);
        } else {
            retType = wrapFallibleRetType(TypeInfo(fnSymbol->retType), fnSymbol->fallibleErrType);
        }
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, cName, _module);
    }

    DEBUG_LOG_VAL("    Function signature check", "numParams=" << fn->getFunctionType()->getNumParams());
    for (size_t i = 0; i < fn->getFunctionType()->getNumParams() && i < args.size(); ++i) {
        auto expectedType = fn->getFunctionType()->getParamType(i);
        auto actualType = args[i]->getType();
        DEBUG_LOG_VAL(
            "    Param type", i << " expected=" << expectedType->getTypeID() << " actual=" << actualType->getTypeID());
        if (expectedType->isIntegerTy() && actualType->isIntegerTy()) {
            DEBUG_LOG_VAL(
                "    Integer bit width",
                "expected=" << expectedType->getIntegerBitWidth() << " actual=" << actualType->getIntegerBitWidth());
        }
        if (expectedType != actualType) {
            DEBUG_LOG_VAL("    TYPE MISMATCH", "need conversion");
        }
    }

    vector<llvm::Value*> callArgs;
    for (size_t i = 0; i < args.size() && i < fnSymbol->params.size(); ++i) {
        DEBUG_LOG_VAL("    Param", i << " argType=" << argTypes[i].name << " paramType=" << fnSymbol->params[i].name);
        DEBUG_LOG_VAL("    Param isPtr", argTypes[i].isPtr() << " paramIsPtr=" << fnSymbol->params[i].isPtr());
        if (fnSymbol->params[i].isRef() && !fnSymbol->isExternal) {
            if (auto literalNode = dynamic_cast<ExprLiteralNode*>(callNode->getArgs()[i])) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        callArgs.push_back(it->second);
                        continue;
                    }
                }
            }
            // Phase 4b: 非局部变量（字符串字面量 / 调用结果 / 字段访问等）传给 T& 形参时，
            // alloca 一个 T 临时存放，再把 alloca 的指针作为 T& 传入。
            // args[i] 已是 T 值（compileExpr 对 T& 形参会把 ref 自解；对 T 直接给值）。
            if (args[i]->getType()->isPointerTy()) {
                callArgs.push_back(args[i]);
            } else {
                auto tmpAlloca = _builder.CreateAlloca(args[i]->getType(), nullptr, "ref_arg_tmp");
                _builder.CreateStore(args[i], tmpAlloca);
                callArgs.push_back(tmpAlloca);
            }
            continue;
        }

        if (fnSymbol->params[i].isPtr()) {
            if (args[i]->getType()->isPointerTy()) {
                auto ptrVal = _builder.CreateBitCast(
                    args[i], llvm::PointerType::get(_context, 0), "ptr_cast");
                callArgs.push_back(ptrVal);
                continue;
            }
            // Phase 7c (DRAFT §9.3): extern 边界自动转 Ptr
            // T& / Box<T> / Weak<T> / Array<T> / String 作实参传给 Ptr 形参时自动转换
            // 转换规则与 ptr_of 一致：Box → payload (跳 RC 头)；Array/String → data 区
            if (fnSymbol->isExternal) {
                auto& aType = argTypes[i];
                auto ptrTy = llvm::PointerType::get(_context, 0);
                if (aType.isRef()) {
                    // T& → 原始指针：从 AST 回溯（args[i] 已被 compileExpr 自动 deref 为 U 值）
                    auto argNode = callNode->getArgs()[i];
                    llvm::Value* refPtr = nullptr;
                    if (auto litExpr = dynamic_cast<ExprLiteralNode*>(argNode)) {
                        if (auto objLit = dynamic_cast<LiteralObjNode*>(litExpr->literal())) {
                            auto name = objLit->getValue().getText();
                            auto it = _localVarPtrs.find(name);
                            if (it != _localVarPtrs.end()) refPtr = it->second;
                        }
                    }
                    if (!refPtr) {
                        if (auto refExpr = dynamic_cast<ExprGetRefNode*>(argNode)) {
                            refPtr = compileGetRefExpr(refExpr);
                        }
                    }
                    if (refPtr) {
                        callArgs.push_back(refPtr);
                        continue;
                    }
                }
                if (aType.isBox() || aType.isWeak() || aType.isArrayGeneric()) {
                    auto handle = _builder.CreateExtractValue(args[i], {0}, "handle");
                    if (aType.isWeak()) {
                        callArgs.push_back(handle);
                        continue;
                    }
                    if (aType.isBox()) {
                        // Box payload 偏移 8（跳过 RC 头）
                        auto payload = _builder.CreateInBoundsGEP(
                            _builder.getInt8Ty(), handle,
                            {llvm::ConstantInt::get(_builder.getInt64Ty(), 8)},
                            "box.payload");
                        callArgs.push_back(payload);
                        continue;
                    }
                    // Array：读 Block.data (offset 24)
                    auto dataAddr = _builder.CreateInBoundsGEP(
                        _builder.getInt8Ty(), handle,
                        {llvm::ConstantInt::get(_builder.getInt64Ty(), 24)},
                        "array.data.addr");
                    auto dataPtr = _builder.CreateLoad(ptrTy, dataAddr, "array.data");
                    callArgs.push_back(dataPtr);
                    continue;
                }
                if (aType.name == "String" && aType.kind == TypeKind::Normal) {
                    auto handle = _builder.CreateExtractValue(args[i], {0, 0}, "string.handle");
                    auto dataAddr = _builder.CreateInBoundsGEP(
                        _builder.getInt8Ty(), handle,
                        {llvm::ConstantInt::get(_builder.getInt64Ty(), 24)},
                        "string.data.addr");
                    auto dataPtr = _builder.CreateLoad(ptrTy, dataAddr, "string.data");
                    callArgs.push_back(dataPtr);
                    continue;
                }
            }
        }

        // callee-clean (DRAFT §7.3)：传参前 retain；callee 末尾析构 release 抵消
        // Phase 8c: fresh 实参（call/array literal）已自带 +1，跳过 retain
        // Phase 8d.1: fresh 实参的 +1 移交给 callee，从临时帧消费掉
        if (typeNeedsDestructor(argTypes[i])) {
            if (i < callNode->getArgs().size() && !isFreshHandleExpr(callNode->getArgs()[i])) {
                retainHandleAtCallSite(args[i], argTypes[i]);
            } else if (i < callNode->getArgs().size()) {
                consumeTemp(args[i]);
            }
            callArgs.push_back(args[i]);
            continue;
        }

        if (structParamUsesPointer(fnSymbol->params[i].name)) {
            DEBUG_LOG_VAL("    Passing struct by pointer", "arg " << i << " : " << fnSymbol->params[i].name);
            auto structType = getLLVMType(argTypes[i]);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
            _builder.CreateStore(args[i], alloca);
            callArgs.push_back(alloca);
            continue;
        }

        callArgs.push_back(args[i]);
    }

    auto callResult = _builder.CreateCall(fn, callArgs);

    if (fnSymbol->isExternal && !fnSymbol->retType.empty() && TypeInfo(fnSymbol->retType).isPtr()) {
        return callResult;
    }

    // [#10.A] / [#10.C]：callee 标 #Fallible 时分流 isErr → 透传 / 提取 T_ok
    return handleFallibleCallResult(callResult, fnSymbol->fallibleErrType,
        TypeInfo(fnSymbol->retType), callNode);
}

// ==================== #Fallible 调用侧分流 ====================

// 处理 #Fallible 调用结果（DRAFT-错误.md [#10.A] / [#10.C]）
// callee 非 fallible 时直接返回 callResult。否则生成 isErr 分流：
//   - 错误分支：构外层 fn 错误返回 struct + ret（透传到 caller 的 #Fallible 通道）
//   - 成功分支：extract T_ok，caller 在 okBB 继续编译；返回 T_ok（void 时 nullptr）
//
// 10g-4 仅覆盖 `!` 透传 + ID-callee；try-catch 错误路由推 10g-5（届时根据
// _tryCatchStack 把 errBB 改为跳到匹配的 catch arm entry block）。
llvm::Value* Compiler::handleFallibleCallResult(
    llvm::Value* callResult, const string& calleeFallibleErr,
    const TypeInfo& calleeRetType, p<ExprCallNode> callNode) {
    if (calleeFallibleErr.empty()) return callResult;

    auto isErr = _builder.CreateExtractValue(callResult, {0}, "call.isErr");
    auto curFn = _builder.GetInsertBlock()->getParent();
    auto errBB = llvm::BasicBlock::Create(_context, "fallible.err", curFn);
    auto okBB = llvm::BasicBlock::Create(_context, "fallible.ok", curFn);
    _builder.CreateCondBr(isErr, errBB, okBB);

    // ===== errBB: 路由到 catch arm 或透传到外层 fn 错误返回 =====
    _builder.SetInsertPoint(errBB);

    // 10g-5：try-catch 内调用路由
    // 在最近一层 try 的 catchTypes 中查找匹配 callee err 的 arm；命中则
    // 把 ErrEnum store 到该 arm 的 e alloca，跳到 arm entry BB（不退出当前 fn）
    if (!_tryCatchStack.empty()) {
        auto& tryCtx = _tryCatchStack.back();
        for (size_t i = 0; i < tryCtx.catchTypes.size(); ++i) {
            if (tryCtx.catchTypes[i] == calleeFallibleErr) {
                unsigned errIdx = calleeRetType.empty() ? 1 : 2;
                auto errVal = _builder.CreateExtractValue(callResult, {errIdx}, "call.err");
                _builder.CreateStore(errVal, tryCtx.armEAllocas[i]);
                _builder.CreateBr(tryCtx.armEntryBBs[i]);
                // okBB 路径继续：提取 T_ok
                _builder.SetInsertPoint(okBB);
                if (calleeRetType.empty()) return nullptr;
                return _builder.CreateExtractValue(callResult, {1}, "call.ok");
            }
        }
        // 未命中：按 10f 静态校验 E7002，最近 try 必须覆盖；这里 fall-through 到透传
        // （理论上不应到达；防御性兜底）
    }

    string callerErr;
    TypeInfo callerRetType;
    if (_currentFnNode && _currentFnNode->header()) {
        if (auto e = _currentFnNode->header()->getAnnoArg("Fallible")) callerErr = *e;
        if (_currentFnNode->header()->retType()) {
            callerRetType = _currentFnNode->header()->retType()->getType();
        }
    }
    if (!callerErr.empty()) {
        // 取出 callee 的 ErrEnum 字段；T_ok 是否存在决定 idx
        unsigned errIdx = calleeRetType.empty() ? 1 : 2;
        auto errVal = _builder.CreateExtractValue(callResult, {errIdx}, "call.err");
        // 构外层 ret struct
        auto outerRetTy = getFallibleRetStructType(callerRetType, callerErr);
        llvm::Value* outerRet = llvm::UndefValue::get(outerRetTy);
        outerRet = _builder.CreateInsertValue(outerRet, _builder.getInt1(1), {0});
        unsigned outerErrIdx;
        if (!callerRetType.empty()) {
            auto okLLVMTy = getLLVMType(callerRetType);
            outerRet = _builder.CreateInsertValue(outerRet, llvm::Constant::getNullValue(okLLVMTy), {1});
            outerErrIdx = 2;
        } else {
            outerErrIdx = 1;
        }
        outerRet = _builder.CreateInsertValue(outerRet, errVal, {outerErrIdx});
        // 错误透传 = 提前 ret，必须释放当前作用域局部（[#10.B] U1）
        callDestructorsForScope();
        _builder.CreateRet(outerRet);
    } else {
        // 兜底：caller 既非 #Fallible 也不在 try 内 —— 实际应被 10e E7001 / E7006 拦下，
        // 这里 emit unreachable 防 LLVM verifier 报错
        _builder.CreateUnreachable();
    }

    // ===== okBB: 提取 T_ok =====
    _builder.SetInsertPoint(okBB);
    if (calleeRetType.empty()) {
        return nullptr;  // void
    }
    return _builder.CreateExtractValue(callResult, {1}, "call.ok");
}
