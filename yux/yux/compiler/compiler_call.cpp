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

#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
#include "compiler.h"
#include "compiler_runtime.h"
#include "sema/call_resolve.h"
#include <functional>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>

// 重载解析 / 灵活整数推断 / E6014 歧义诊断已迁至 yux/frontend/sema/call_resolve.cpp,
// 由 namespace sema 提供 resolveFnOverload / resolveCtorOverload, 不依赖 LLVM.

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
llvm::Function* Compiler::getMethodFunction(const string& structName, const string& methodName,
                                            const vector<TypeInfo>& paramTypes, const TypeInfo& retType,
                                            const string& fallibleErrType, bool isStatic) {
    DEBUG_LOG_VAL("  getMethodFunction", structName << "." << methodName << (isStatic ? " [#Static]" : ""));

    bool isPriv = !methodName.empty() && methodName[0] == '_';

    // 确定方法所属的模块
    // 泛型实例：使用消费方模块（每个使用方模块各自生成一份实例 IR，避免重复符号）
    // 普通结构体：使用 baseDecl owner 模块（跨模块仍引用同一份定义）
    string ownerModule = _file->moduleName();
    if (auto instIt = _structInstances.find(structName); instIt != _structInstances.end()) {
        ownerModule = instIt->second.consumerModule;
    } else {
        auto* owner = _file->getStructOwner(structName);
        if (owner && owner != _file) {
            ownerModule = owner->moduleName();
        } else if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
            // SDK 平铺文件拆分后，_sdkFile 空壳通过 wildcardImports 找到真实 owner
            auto* sdkOwner = _yux->sdkFile()->getStructOwner(structName);
            if (sdkOwner) {
                ownerModule = sdkOwner->moduleName();
            } else if (_yux->sdkFile()->getStructDecl(structName)) {
                ownerModule = _yux->sdkFile()->moduleName();
            }
        }
    }
    // 生成 mangle 名称
    string mangledName;
    if (isStatic) {
        mangledName = Mangler::staticMethod(ownerModule, structName, methodName, paramTypes);
    } else {
        mangledName = Mangler::method(ownerModule, structName, methodName, paramTypes, isPriv);
    }
    DEBUG_LOG_VAL("    -> mangled name", mangledName);

    auto func = _module->getFunction(mangledName);
    if (func) {
        DEBUG_LOG("    -> found existing function");
        return func;
    }

    // 构建参数类型列表
    vector<llvm::Type*> llvmParamTypes;

    // 第一个参数是当前实例（用户层 `$`） —— #Static fn 没有 receiver, 跳过
    if (!isStatic) {
        TypeInfo recvTy = typeInfoForNamedStruct(structName);
        if (isBuiltinType(structName) || recvTy.isPtr() || recvTy.isRef()) {
            llvmParamTypes.push_back(getLLVMType(recvTy));
        } else {
            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0)); // 结构体通过指针传递
        }
    }

    // 其他参数
    for (auto& paramType : paramTypes) {
        if (paramType.isPtr() || paramType.isRef()) {
            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
            continue;
        }
        if (structParamUsesPointer(paramType)) {
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
        } else if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
            auto* sdkOwner = _yux->sdkFile()->getStructOwner(structName);
            if (sdkOwner) {
                ownerModule = sdkOwner->moduleName();
            } else if (_yux->sdkFile()->getStructDecl(structName)) {
                ownerModule = _yux->sdkFile()->moduleName();
            }
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
// 检查方法是否标记为 #Builtin
// Builtin 方法由编译器特殊处理，不生成普通 IR
bool Compiler::isBuiltinMethod(const string& structName, const string& methodName) {
    if (!_yux || !_yux->sdkFile()) return false;

    auto structImpl = _yux->sdkFile()->getStructImpl(structName);
    if (!structImpl) return false;

    for (auto& method : structImpl->methods()) {
        if (method->header()->name().getText() == methodName) {
            return method->header()->hasAnno("Builtin");
        }
    }

    return false;
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
// Phase 10e/10f 的 ID-callee 错误传播校验 (E7001/E7004/E7006/E7016) 已迁至
// yux/frontend/sema/call_resolve.cpp 的 sema::checkErrPropagateForIdCall, 形参改成
// `vector<string>* tryBlockSeenErrs` 解开 Compiler::TryCatchCtx 的 LLVM 耦合.

llvm::Value* Compiler::compileCallExpr(p<ExprCallNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
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
            string srcPath = (_yux && _file) ? _yux->modulePath(_file->moduleName()) : "";
            sema::checkErrPropagateForIdCall(_currentFnNode, node, fnName, sym,
                                             tryCtx ? &tryCtx->seenErrTypes : nullptr, srcPath);
        } else if (node->errPropagate()) {
            // 非 ID-literal 但带 `!`：极少见路径（如 nullable 字面量调用），按 caller / try 状态判 E7001
            // 在 try block 内 → 暂放过（路由到 catch 由 10g 实施）
            if (_tryCatchStack.empty()) {
                sema::checkBangWithoutFallibleCaller(_currentFnNode, node);
            }
        }
    } else if (node->errPropagate() && !dynamic_cast<ExprDotNode*>(calleeExpr)) {
        // fn-value 调用 + `!`：caller 未 fallible 且不在 try 内时报 E7001
        if (_tryCatchStack.empty()) {
            sema::checkBangWithoutFallibleCaller(_currentFnNode, node);
        }
    }

    // Phase 2b: callee 静态类型为 Fn(...)R → 走 fat-ptr 路径
    // 涵盖：lambda IIFE `((x i32) i32 => ...)(5)`、fn-typed 变量 / 字段 / 调用结果
    // 排除：函数名字面量（LiteralObjNode 引用 Function 符号）——即使现在返回准确
    // 的 Fn TypeInfo，仍走下方字面量路径以完成重载解析和形参类型检查。
    // Phase 3c: callee 为 Rc<fn(...)R> → 自动解引取 fat-ptr 后走同款 fn-value-call
    // v0.16: callee 为 Ref<fn(...)R>（如 arr[i] 返回 fn&）→ Load 引用得 fat-ptr 后调用
    {
        TypeInfo calleeStaticType;
        try {
            calleeStaticType = resolvedOrInferredType(calleeExpr);
        } catch (...) { // NOLINT(bugprone-empty-catch)
        }
        if (calleeStaticType.isFn() || resolveAlias(calleeStaticType).isFn()) {
            // 函数名字面量不能走 fn-value-call：下方字面量路径负责重载解析和形参类型检查
            bool isFnNameLiteral = false;
            if (auto* lit = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
                if (auto* objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                    auto scope = objLit->findNearestScope();
                    if (scope) {
                        auto* sym = scope->lookupSymbol(objLit->getValue().getText());
                        if (sym && sym->kind == SymbolKind::Function) {
                            isFnNameLiteral = true;
                        }
                    }
                }
            }
            // 方法点 getType 也返回 TypeKind::Fn（仅编码返回类型），不是 fat-ptr 值；
            // 真正的 Fn 字段才走 compileFnValueCall。
            bool isMethodDot = false;
            if (auto* dot = dynamic_cast<ExprDotNode*>(calleeExpr)) {
                try {
                    isMethodDot = !dot->isFieldAccess();
                } catch (...) { // NOLINT(bugprone-empty-catch)
                    isMethodDot = true;
                }
            }
            if (!isFnNameLiteral && !isMethodDot) {
                return compileFnValueCall(node);
            }
        }
        if (calleeStaticType.isRc() || resolveAlias(calleeStaticType).isRc()) {
            auto inner = calleeStaticType.isRc() ? calleeStaticType.rcElementType()
                                                 : resolveAlias(calleeStaticType).rcElementType();
            if (inner && inner->isFn()) {
                return compileRcFnValueCall(node, *inner);
            }
        }
        if (calleeStaticType.isRef() || resolveAlias(calleeStaticType).isRef()) {
            auto inner = calleeStaticType.isRef() ? calleeStaticType.refElementType()
                                                  : resolveAlias(calleeStaticType).refElementType();
            if (inner && inner->isFn()) {
                return compileRefFnValueCall(node, *inner);
            }
        }
    }

    // 预处理: 类型推断和泛型参数处理
    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            string fnName = objLiteral->getValue().getText();

            // 检查是否为泛型函数
            // getFunctionWithOwner 已搜索本地 + wildcardImports，不再需要手动 SDK 回退
            auto [genericFn, fnOwner] = _file->getFunctionWithOwner(fnName);
            if (!fnOwner) fnOwner = _file;

            // 处理显式类型参数的泛型函数调用
            if (genericFn && genericFn->header()->isGeneric()) {
                const auto& typeParams = genericFn->header()->typeParams();
                vector<TypeInfo> typeArgs;

                const auto& explicitTypeArgs = node->getTypeArgs();
                if (!explicitTypeArgs.empty()) {
                    // 验证类型参数数量 (E6010 已迁至 sema::validateGenericTypeArgsArity)
                    sema::validateGenericTypeArgsArity(fnName, typeParams.size(), explicitTypeArgs.size(),
                                                       node->getLineNumber(), node->getColumn());
                    for (auto& tn : explicitTypeArgs) {
                        typeArgs.push_back(applySubst(tn->getType()));
                    }

                    // 建立类型替换映射并推断参数类型
                    map<string, TypeInfo> subst;
                    for (size_t i = 0; i < typeParams.size(); ++i) {
                        subst[typeParams[i]] = typeArgs[i];
                    }
                    _substStack.push_back(SubstFrame{.subst = subst, .baseStructName = "", .effStructName = ""});

                    auto params = genericFn->header()->params();
                    for (size_t i = 0; i < params.size(); ++i) {
                        auto paramType = params[i]->type();
                        if (paramType) {
                            TypeInfo instParamType = applySubst(paramType->getType());
                            // 推断灵活整数的类型（含 tuple/泛型别名展开）
                            if (i < node->getArgs().size()) {
                                if (isIntTypeName(instParamType.name)) {
                                    tryInferIntType(node->getArgs()[i], instParamType);
                                } else if (instParamType.isNullable()) {
                                    // Nullable<T> 形参：按内层 T 推断灵活整数
                                    auto inner = instParamType.nullableInnerType();
                                    if (inner && isIntTypeName(inner->name)) {
                                        tryInferIntType(node->getArgs()[i], *inner);
                                    }
                                }
                                inferFlexibleInts(node->getArgs()[i], instParamType);
                            }
                            // 推断灵活 null 的类型
                            if (instParamType.isNullable() && i < node->getArgs().size()) {
                                tryInferNullType(node->getArgs()[i], instParamType);
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
                        string effName = fnName + "<";
                        for (size_t i = 0; i < instArgs.size(); ++i) {
                            if (i > 0) effName += ",";
                            effName += instArgs[i]->getMangleName();
                        }
                        effName += ">";

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
                                            // 推断 tuple 字面量中灵活整数的类型（含泛型别名展开）
                                            inferFlexibleInts(node->getArgs()[i], instParamType);
                                            if (instParamType.isNullable()) {
                                                tryInferNullType(node->getArgs()[i], instParamType);
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
                    sema::resolveCtorOverload(_file, fnName, node->getArgs(), node->getLineNumber());
                    if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                        sema::resolveCtorOverload(_yux->sdkFile(), fnName, node->getArgs(), node->getLineNumber());
                    }
                } else {
                    // 解析函数重载
                    sema::resolveFnOverload(_file, _yux ? _yux->sdkFile() : nullptr, fnName, node->getArgs(),
                                            node->getLineNumber());
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
                            if (lambdaArg->params()[k].type) continue; // 显式标注尊重源
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

    // 方法调用：在 arg 编译前解析重载并推断灵活整数类型
    // 普通函数调用在上方 resolveFnOverload 已处理，构造器在 resolveCtorOverload 已处理；
    // 方法调用（ExprDotNode callee）此前缺少这一步，导致如 s.get(0) 中 0 默认 i32
    // 无法匹配 String::get(usize) → compileStructMethodCall 返回 nullptr → E6015。
    // 仅对结构体类型方法做重载解析；内置类型 / Array / Ptr / Dyn 有各自 codegen 分派路径。
    if (auto dotNode = dynamic_cast<ExprDotNode*>(calleeExpr)) {
        auto baseType = dotNode->baseExpr()->getType();
        baseType = applySubst(baseType);

        // 安全方法调用 a?.foo()：从 Nullable<T> 解出 T 做重载解析
        if (dotNode->isSafe() && baseType.isNullable()) {
            auto inner = baseType.nullableInnerType();
            if (inner) baseType = *inner;
        }

        if (baseType.isRef()) {
            auto inner = baseType.refElementType();
            if (inner) baseType = *inner;
        }

        // Unwrap Rc<T> → T：方法在 inner type 上查找（对齐 compileMethodCall 逻辑）
        string effectiveTypeName = baseType.name;
        if (baseType.isRc()) {
            auto rcElem = baseType.rcElementType();
            if (rcElem) effectiveTypeName = rcElem->name;
        }

        // 仅结构体类型方法走重载解析；内置类型 / Array / Ptr / Dyn 跳过
        if (!effectiveTypeName.empty() && !isBuiltinType(effectiveTypeName) && !baseType.isArrayGeneric() &&
            !baseType.isPtr() && !baseType.isDyn()) {
            string member = dotNode->member();
            if (dotNode->hasSpecQualifier()) {
                auto bt = dotNode->baseExpr()->getType();
                if (!bt.isDyn()) {
                    member = member + "__at__" + dotNode->specQualifier();
                }
            }
            sema::resolveMethodOverload(_file, _yux ? _yux->sdkFile() : nullptr, effectiveTypeName, member,
                                        node->getArgs(), node->getLineNumber());
        }
    }

    bool delaySafeMethodArgs = false;
    if (auto delayDot = dynamic_cast<ExprDotNode*>(calleeExpr)) {
        // §4.1.1.3：a?.m(args) 在 a 为空时不求值实参，推迟到 then 分支
        delaySafeMethodArgs = delayDot->isSafe();
    }

    for (auto& arg : node->getArgs()) {
        auto argType = arg->getType();
        argTypes.push_back(argType);
        if (delaySafeMethodArgs) continue;

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
                            if (structParamUsesPointer(instParamType)) {
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
        if (dotNode->isSafe()) {
            auto result = compileSafeDotMethodCall(node, dotNode, argTypes);
            if (result) {
                return result;
            }
        } else {
            auto result = compileMethodCall(node, dotNode, args, argTypes);
            if (result) {
                return result;
            }
        }
    }

    if (auto calleeLiteral = dynamic_cast<ExprLiteralNode*>(calleeExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(calleeLiteral->literal())) {
            return compileFunctionCall(node, objLiteral->getValue().getText(), args, argTypes);
        }
    }

    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E6015);
}

// ==================== #Fallible 调用侧分流 ====================

// 处理 #Fallible 调用结果（DRAFT-错误.md [#10.A] / [#10.C]）
// callee 非 fallible 时直接返回 callResult。否则生成 isErr 分流：
//   - 错误分支：构外层 fn 错误返回 struct + ret（透传到 caller 的 #Fallible 通道）
//   - 成功分支：extract T_ok，caller 在 okBB 继续编译；返回 T_ok（void 时 nullptr）
//
// 10g-4 仅覆盖 `!` 透传 + ID-callee；try-catch 错误路由推 10g-5（届时根据
// _tryCatchStack 把 errBB 改为跳到匹配的 catch arm entry block）。
llvm::Value* Compiler::handleFallibleCallResult(llvm::Value* callResult, const string& calleeFallibleErr,
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
        outerRet = _builder.CreateInsertValue(outerRet, _builder.getInt1(true), {0});
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
        return nullptr; // void
    }
    return _builder.CreateExtractValue(callResult, {1}, "call.ok");
}
