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
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "compiler_runtime.h"
#include "mangler.h"
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

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
        throw YuxError(line, "Ambiguous call to '{}({})': {} overloads match; add type suffix to disambiguate:{}", fnName, argSigs, matches.size(), sigs);
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
        vector<TypeInfo> paramTypes;
        for (auto param : header->params()) {
            if (param->type()) {
                paramTypes.push_back(param->type()->getType());
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
    const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes, const TypeInfo& retType) {
    DEBUG_LOG_VAL("  getMethodFunction", structName << "." << methodName);

    bool isCtor = methodName == structName;  // 构造函数名与结构体名相同
    bool isPriv = !methodName.empty() && methodName[0] == '_';

    // 确定方法所属的模块
    string ownerModule = _file->moduleName();
    if (!isBuiltinType(structName)) {
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

    // 第一个参数是 self
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
        auto structDecl = _file->getStructDecl(paramType.name);
        if (structDecl && !isBuiltinType(paramType.name)) {
            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            llvmParamTypes.push_back(getLLVMType(paramType));
        }
    }

    auto llvmRetType = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
    DEBUG_LOG_VAL("    -> return type", (retType.empty() ? "void" : retType.name));
    auto fnType = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
    DEBUG_LOG("    -> created new function");
    return llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
}

// 获取或创建析构函数
llvm::Function* Compiler::getDestructorFunction(const string& structName) {
    DEBUG_LOG_VAL("  getDestructorFunction", structName);

    // 确定析构函数所属的模块
    string ownerModule = _file->moduleName();
    auto* owner = _file->getStructOwner(structName);
    if (owner && owner != _file) {
        ownerModule = owner->moduleName();
    } else if (!owner && _yux && _yux->sdkFile()
               && _yux->sdkFile()->getStructDecl(structName)) {
        ownerModule = _yux->sdkFile()->moduleName();
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
                    throw YuxError(callNode->getLineNumber(),
                        "module `{}` not found in package `{}`", childKey, aliasSym->moduleName);
                }
                const string& fnName = segs.back();
                auto* fnSym = target->lookupFnSymbolWithParams(fnName, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(),
                        "function `{}` not found in module `{}.{}`", fnName, aliasSym->moduleName, childKey);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(),
                        "Cannot call private function `{}` via package alias", fnName);
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
                    throw YuxError(callNode->getLineNumber(),
                        "module `{}` (alias `{}`) not loaded", aliasSym->moduleName, aliasName);
                }
                auto* fnSym = targetMod->lookupFnSymbolWithParams(member, argTypes);
                if (!fnSym) {
                    throw YuxError(callNode->getLineNumber(),
                        "function `{}` not found in module `{}`", member, aliasSym->moduleName);
                }
                if (fnSym->isPrivate) {
                    throw YuxError(callNode->getLineNumber(),
                        "Cannot call private function `{}` via module alias", member);
                }
                return compileKnownFunctionCall(callNode, member, args, argTypes, fnSym);
            }
        }
    }

    auto baseType = baseExpr->getType();

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
llvm::Value* Compiler::compileCallExpr(p<ExprCallNode> node) {
    auto calleeExpr = node->getCalleeExpr();

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
                        throw YuxError(node->getLineNumber(),
                            "Generic function '{}' expects {} type args, got {}",
                            fnName, typeParams.size(), explicitTypeArgs.size());
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
                            auto sd = _file->getStructDecl(instParamType.name);
                            if (!sd && _yux && _yux->sdkFile()) {
                                sd = _yux->sdkFile()->getStructDecl(instParamType.name);
                            }
                            if (sd && !isBuiltinType(instParamType.name)) {
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

    throw YuxError(node->getLineNumber(), "Unsupported call expression");
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
            throw YuxError(callNode->getLineNumber(), "Cannot use private struct '{}' in constructor", fnName);
        }
        string effName = fnName;
        bool isGenericCtor = false;
        if (structDecl->isGeneric()) {
            const auto& typeArgs = callNode->getTypeArgs();
            if (typeArgs.empty()) {
                throw YuxError(
                    callNode->getLineNumber(),
                    "Generic struct '{}' constructor requires explicit type arguments", fnName);
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
            for (auto& a : args) ctorArgs.push_back(a);
            string ownerMod = structOwner->moduleName();
            string cName = Mangler::ctor(ownerMod, effName, argTypes);
            auto fn = _module->getFunction(cName);
            if (!fn) {
                vector<llvm::Type*> paramTypes;
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                for (auto& t : argTypes) {
                    auto sd = _file->getStructDecl(t.name);
                    if (!sd && _yux && _yux->sdkFile()) {
                        sd = _yux->sdkFile()->getStructDecl(t.name);
                    }
                    if (sd && !isBuiltinType(t.name)) {
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
        auto result = compileConstructorCall(fnName, effName, args, argTypes);
        if (result) {
            return result;
        }
    }

    auto fnSymbol = _file->lookupFnSymbolWithParams(fnName, argTypes);

    auto genericFn = _file->getFunction(fnName);
    p<FileNode> fnOwner = _file;
    if (!genericFn && _yux && _yux->sdkFile()) {
        genericFn = _yux->sdkFile()->getFunction(fnName);
        if (genericFn) fnOwner = _yux->sdkFile();
    }

    if (genericFn && genericFn->header()->isGeneric()) {
        return compileGenericFunctionCall(callNode, fnName, args, argTypes, genericFn, fnOwner);
    }

    if (fnName == "ptr_from_addr") {
        DEBUG_LOG("    Expr: PtrFromAddr");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), "ptr_from_addr expects 1 argument");
        }
        return _builder.CreateIntToPtr(args[0], llvm::PointerType::get(_context, 0), "ptr_from_addr");
    }

    if (fnSymbol) {
        if (fnSymbol->isPrivate && !fnSymbol->moduleName.empty() && fnSymbol->moduleName != _file->moduleName()) {
            throw YuxError(callNode->getLineNumber(), "Cannot call private function '{}'", fnName);
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
            throw YuxError(callNode->getLineNumber(),
                "Generic function '{}' expects {} type args, got {}",
                fnName, typeParams.size(), explicitTypeArgs.size());
        }
        for (auto& tn : explicitTypeArgs) {
            typeArgs.push_back(applySubst(tn->getType()));
        }
    } else {
        auto params = genericFn->header()->params();
        if (params.size() != argTypes.size()) {
            throw YuxError(callNode->getLineNumber(),
                "Generic function '{}' expects {} params, got {} args",
                fnName, params.size(), argTypes.size());
        }

        map<string, TypeInfo> inferred;
        for (size_t i = 0; i < params.size(); ++i) {
            auto paramType = params[i]->type();
            if (!paramType) continue;
            TypeInfo pType = paramType->getType();
            TypeInfo aType = argTypes[i];

            if (pType.isNormal() && !isBuiltinType(pType.name)) {
                bool isTypeParam = false;
                for (auto& tp : typeParams) {
                    if (pType.name == tp) {
                        isTypeParam = true;
                        break;
                    }
                }
                if (isTypeParam) {
                    inferred[pType.name] = aType;
                }
            }
        }

        for (auto& tp : typeParams) {
            auto it = inferred.find(tp);
            if (it == inferred.end()) {
                throw YuxError(callNode->getLineNumber(),
                    "Cannot infer type parameter '{}' for generic function '{}'",
                    tp, fnName);
            }
            typeArgs.push_back(it->second);
        }
    }

    if (genericFn->header()->hasAnno("CompilerInner")) {
        if (fnName == "size_of") {
            if (typeArgs.empty()) {
                throw YuxError(callNode->getLineNumber(),
                    "Cannot determine type argument for size_of");
            }
            auto llvmType = getLLVMType(typeArgs[0]);
            if (!llvmType) {
                throw YuxError(callNode->getLineNumber(),
                    "Cannot determine LLVM type for '{}'", typeArgs[0].getFullName());
            }
            auto size = _module->getDataLayout().getTypeAllocSize(llvmType);
            return _builder.getInt64(size);
        }
        throw YuxError(callNode->getLineNumber(),
            "Unknown #CompilerInner function '{}'", fnName);
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
            auto sd = _file->getStructDecl(t.name);
            if (!sd && _yux && _yux->sdkFile()) {
                sd = _yux->sdkFile()->getStructDecl(t.name);
            }
            if (sd && !isBuiltinType(t.name) && !t.isPtr() && !t.isRef()) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
            } else {
                paramTypes.push_back(getLLVMType(t));
            }
        }
        auto retType = instRetType.empty() ? _builder.getVoidTy() : getLLVMType(instRetType);
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
        auto sd = _file->getStructDecl(at.name);
        if (!sd && _yux && _yux->sdkFile()) {
            sd = _yux->sdkFile()->getStructDecl(at.name);
        }
        if (sd && !isBuiltinType(at.name)) {
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
    auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    auto two = llvm::ConstantInt::get(_builder.getInt32Ty(), 2);

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
                    auto boxStructType = getLLVMType(outerType);
                    llvm::Value* bIndices[] = {zero, zero};
                    auto dataPtrField = _builder.CreateGEP(boxStructType, outerPtr, bIndices, "box.data_ptr_field");
                    dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataPtrField, "box.data_ptr");
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
        llvm::Value* indices[] = {zero, one};
        auto lenField = _builder.CreateGEP(arrayStructType, ptr, indices, "len_field");
        return _builder.CreateLoad(_builder.getInt64Ty(), lenField, "array.len");
    }
    if (member == "cap") {
        DEBUG_LOG("    Expr: Array.cap()");
        auto ptr = getReadPtr();
        llvm::Value* indices[] = {zero, two};
        auto capField = _builder.CreateGEP(arrayStructType, ptr, indices, "cap_field");
        return _builder.CreateLoad(_builder.getInt64Ty(), capField, "array.cap");
    }

    if (!elemType) {
        throw YuxError(callNode->getLineNumber(), "Array type requires element type");
    }
    auto elemLLVMType = getLLVMType(*elemType);

    if (member == "is_empty") {
        DEBUG_LOG("    Expr: Array.is_empty()");
        auto ptr = getReadPtr();
        llvm::Value* indices[] = {zero, one};
        auto lenField = _builder.CreateGEP(arrayStructType, ptr, indices, "len_field");
        auto lenVal = _builder.CreateLoad(_builder.getInt64Ty(), lenField, "array.len");
        return _builder.CreateICmpEQ(lenVal, _builder.getInt64(0), "array.is_empty");
    }

    if (member == "at") {
        DEBUG_LOG("    Expr: Array.at()");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), "at requires 1 argument");
        }
        auto ptr = getReadPtr();
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, ptr, dataIndices, "a.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {args[0]}, "at.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "at.elem");
    }

    if (member == "first") {
        DEBUG_LOG("    Expr: Array.first()");
        auto ptr = getReadPtr();
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, ptr, dataIndices, "a.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {_builder.getInt64(0)}, "first.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "first.elem");
    }

    if (member == "last") {
        DEBUG_LOG("    Expr: Array.last()");
        auto ptr = getReadPtr();
        llvm::Value* lenIndices[] = {zero, one};
        auto lenField = _builder.CreateGEP(arrayStructType, ptr, lenIndices, "len_field");
        auto lenVal = _builder.CreateLoad(_builder.getInt64Ty(), lenField, "array.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "last.idx");
        
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, ptr, dataIndices, "a.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "last.elem.ptr");
        return _builder.CreateLoad(elemLLVMType, elemPtr, "last.elem");
    }

    if (member == "pop") {
        DEBUG_LOG("    Expr: Array.pop()");
        if (!arrayPtr) {
            throw YuxError(callNode->getLineNumber(), "Array.pop() requires an lvalue array");
        }
        llvm::Value* lenIndices[] = {zero, one};
        auto lenFieldPtr = _builder.CreateGEP(arrayStructType, arrayPtr, lenIndices, "a.len.field");
        auto lenVal = _builder.CreateLoad(_builder.getInt64Ty(), lenFieldPtr, "a.len");
        auto lastIdx = _builder.CreateSub(lenVal, _builder.getInt64(1), "pop.idx");
        
        llvm::Value* dataIndices[] = {zero, zero};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, arrayPtr, dataIndices, "a.data.field");
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data");
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {lastIdx}, "pop.elem.ptr");
        auto elemVal = _builder.CreateLoad(elemLLVMType, elemPtr, "pop.elem");
        
        auto newLen = _builder.CreateSub(lenVal, _builder.getInt64(1), "pop.new_len");
        _builder.CreateStore(newLen, lenFieldPtr);
        return elemVal;
    }

    if (member == "push" || member == "set_len" || member == "clear") {
        if (!arrayPtr) {
            throw YuxError(callNode->getLineNumber(),
                "Array mutation method '{}' requires an lvalue array", member);
        }
        auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);

        llvm::Value* dataIdx[] = {zero, zero};
        llvm::Value* lenIdx[] = {zero, one};
        llvm::Value* capIdx[] = {zero, two};
        auto dataFieldPtr = _builder.CreateGEP(arrayStructType, arrayPtr, dataIdx, "a.data.field");
        auto lenFieldPtr = _builder.CreateGEP(arrayStructType, arrayPtr, lenIdx, "a.len.field");
        auto capFieldPtr = _builder.CreateGEP(arrayStructType, arrayPtr, capIdx, "a.cap.field");

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
                throw YuxError(callNode->getLineNumber(), "set_len requires 1 argument");
            }
            _builder.CreateStore(args[0], lenFieldPtr);
            return voidResult();
        }
        DEBUG_LOG("    Expr: Array.push()");
        if (args.size() != 1) {
            throw YuxError(callNode->getLineNumber(), "push requires 1 argument");
        }
        auto elemVal = args[0];
        auto lenVal = _builder.CreateLoad(_builder.getInt64Ty(), lenFieldPtr, "a.len");
        auto capVal = _builder.CreateLoad(_builder.getInt64Ty(), capFieldPtr, "a.cap");

        auto needGrow = _builder.CreateICmpUGE(lenVal, capVal, "push.need_grow");
        auto growBB = llvm::BasicBlock::Create(_context, "push.grow", _currentFn);
        auto storeBB = llvm::BasicBlock::Create(_context, "push.store", _currentFn);
        _builder.CreateCondBr(needGrow, growBB, storeBB);

        _builder.SetInsertPoint(growBB);
        auto oldData = _builder.CreateLoad(
            llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data.old");
        auto capIsZero = _builder.CreateICmpEQ(capVal, _builder.getInt64(0), "cap.is_zero");
        auto doubled = _builder.CreateMul(capVal, _builder.getInt64(2), "cap.dbl");
        auto newCap = _builder.CreateSelect(capIsZero, _builder.getInt64(4), doubled, "new.cap");
        auto newBytes = _builder.CreateMul(newCap, _builder.getInt64(elemSize), "new.bytes");
        auto growFn = runtime::getArrayGrowFn(_module, _builder);
        auto newData = _builder.CreateCall(growFn, {oldData, newBytes}, "a.data.new");
        _builder.CreateStore(newData, dataFieldPtr);
        _builder.CreateStore(newCap, capFieldPtr);
        _builder.CreateBr(storeBB);

        _builder.SetInsertPoint(storeBB);
        auto curData = _builder.CreateLoad(
            llvm::PointerType::get(_context, 0), dataFieldPtr, "a.data.cur");
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
            auto retType = sdkMethodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(sdkMethodSymbol->retType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }
    
    throw YuxError(callNode->getLineNumber(), "Unknown method '{}' for builtin type '{}'", member, baseType.name);
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

                    string ownerMod = owner->moduleName();
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
                            auto sd = _file->getStructDecl(t.name);
                            if (!sd && _yux && _yux->sdkFile()) {
                                sd = _yux->sdkFile()->getStructDecl(t.name);
                            }
                            if (sd && !isBuiltinType(t.name)) {
                                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                            } else {
                                paramTypes.push_back(getLLVMType(t));
                            }
                        }
                        TypeInfo retType;
                        if (chosen->header()->retType()) {
                            retType = chosen->header()->retType()->getType().substitute(subst);
                        }
                        auto llvmRetType = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
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
                throw YuxError(callNode->getLineNumber(), "Cannot call private method '{}' of struct '{}'", member, actualType.name);
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
            auto boxStructType = getLLVMType(baseType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            llvm::Value* indices[] = {zero, zero};
            auto dataPtrField = _builder.CreateGEP(boxStructType, basePtr, indices, "box.data_ptr_field");
            dataPtr = _builder.CreateLoad(
                llvm::PointerType::get(_context, 0), dataPtrField, "box.data_ptr");
        }

        vector<llvm::Value*> methodArgs;
        methodArgs.push_back(dataPtr);
        for (size_t i = 0; i < args.size(); ++i) {
            auto& at = argTypes[i];
            auto argStructDecl = _file->getStructDecl(at.name);
            if (!argStructDecl && _yux && _yux->sdkFile()) {
                argStructDecl = _yux->sdkFile()->getStructDecl(at.name);
            }
            if (argStructDecl && !isBuiltinType(at.name)) {
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
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            for (auto& t : argTypes) {
                auto sd = _file->getStructDecl(t.name);
                if (!sd && _yux && _yux->sdkFile()) {
                    sd = _yux->sdkFile()->getStructDecl(t.name);
                }
                if (sd && !isBuiltinType(t.name)) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(t));
                }
            }
            auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
            auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
            fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
        }
        return _builder.CreateCall(fn, methodArgs);
    }

    return nullptr;
}

llvm::Value* Compiler::compileConstructorCall(
    const string& baseName, const string& effName,
    vector<llvm::Value*>& args, vector<TypeInfo>& argTypes) {
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
        for (auto& arg : args) {
            ctorArgs.push_back(arg);
        }

        string ownerMod = ctorSymbol->moduleName.empty() ? _file->moduleName() : ctorSymbol->moduleName;
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
            } else {
                auto paramStructDecl = _file->getStructDecl(fnSymbol->params[i].name);
                bool isStructType = paramStructDecl != nullptr && !isBuiltinType(fnSymbol->params[i].name);

                if (!isStructType && _yux && _yux->sdkFile()) {
                    isStructType = _yux->sdkFile()->getStructDecl(fnSymbol->params[i].name) != nullptr && !isBuiltinType(fnSymbol->params[i].name);
                }

                if (isStructType) {
                    paramTypes.push_back(llvm::PointerType::get(_context, 0));
                } else {
                    paramTypes.push_back(getLLVMType(fnSymbol->params[i]));
                }
            }
        }
        auto retType = fnSymbol->retType.empty()
                           ? _builder.getVoidTy()
                           : (fnSymbol->isExternal && TypeInfo(fnSymbol->retType).isPtr()
                                  ? llvm::PointerType::get(_context, 0)
                                  : getLLVMType(TypeInfo(fnSymbol->retType)));
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
        }

        if (fnSymbol->params[i].isPtr()) {
            if (args[i]->getType()->isPointerTy()) {
                auto ptrVal = _builder.CreateBitCast(
                    args[i], llvm::PointerType::get(_context, 0), "ptr_cast");
                callArgs.push_back(ptrVal);
                continue;
            }
        }

        if (argTypes[i].isBox()) {
            auto boxStructType = getLLVMType(argTypes[i]);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);

            auto boxAlloca = _builder.CreateAlloca(boxStructType, nullptr, "box_arg_tmp");
            _builder.CreateStore(args[i], boxAlloca);

            llvm::Value* indices1[] = {zero, one};
            auto refCountFieldPtr = _builder.CreateGEP(boxStructType, boxAlloca, indices1, "ref_count_field_ptr");
            auto refCountPtr = _builder.CreateLoad(
                llvm::PointerType::get(_context, 0), refCountFieldPtr, "ref_count_ptr");

            auto retainFn = runtime::getBoxRetainFn(_module, _builder);
            _builder.CreateCall(retainFn, {refCountPtr});

            callArgs.push_back(args[i]);
            continue;
        }

        auto paramStructDecl = _file->getStructDecl(fnSymbol->params[i].name);
        bool isStructType = paramStructDecl != nullptr && !isBuiltinType(fnSymbol->params[i].name);

        if (!isStructType && _yux && _yux->sdkFile()) {
            isStructType = _yux->sdkFile()->getStructDecl(fnSymbol->params[i].name) != nullptr && !isBuiltinType(fnSymbol->params[i].name);
        }

        if (!isStructType) {
            auto it = _structTypes.find(fnSymbol->params[i].name);
            isStructType = (it != _structTypes.end());
        }

        if (isStructType) {
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

    return callResult;
}
