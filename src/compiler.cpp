// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 编译器核心实现
// 
// 本文件包含 Compiler 类的主要实现:
// - 构造函数: 初始化基本类型映射
// - compile(): 编译主入口，编排整个编译流程
// - compileGlobalConsts(): 编译全局常量
// - compileStructDecls(): 编译结构体声明
// - compileStructImpls(): 编译结构体实现
// - compileFn()/compileMethod(): 编译函数和方法
// - 泛型单态化相关函数

#include "compiler.h"
#include "mangler.h"
#include "node/fn_node.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "types.h"
#include "compiler_runtime.h"
#include <utility>
#include <algorithm>
#include <regex>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>


// ==================== 构造函数 ====================
// 初始化编译器，建立基本类型到 LLVM 类型的映射
Compiler::Compiler(
    llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file, Yux* yux, bool isSdk) :
    _context(context), _builder(builder), _module(mod), _file(file), _yux(yux), _isSdk(isSdk) {
    // 初始化基本类型映射表
    // 注意: i8/u8, i16/u16 等使用相同的 LLVM 类型，语义区分在 TypeInfo 中
    _typeMap.insert({"", _builder.getVoidTy()});        // void 类型
    _typeMap.insert({"bool", _builder.getInt1Ty()});    // 布尔类型 (1 bit)
    _typeMap.insert({"i8", _builder.getInt8Ty()});      // 有符号 8 位整数
    _typeMap.insert({"u8", _builder.getInt8Ty()});      // 无符号 8 位整数
    _typeMap.insert({"i16", _builder.getInt16Ty()});    // 有符号 16 位整数
    _typeMap.insert({"u16", _builder.getInt16Ty()});    // 无符号 16 位整数
    _typeMap.insert({"i32", _builder.getInt32Ty()});    // 有符号 32 位整数
    _typeMap.insert({"u32", _builder.getInt32Ty()});    // 无符号 32 位整数
    _typeMap.insert({"i64", _builder.getInt64Ty()});    // 有符号 64 位整数
    _typeMap.insert({"u64", _builder.getInt64Ty()});    // 无符号 64 位整数
    _typeMap.insert({"f32", _builder.getFloatTy()});    // 32 位浮点数
    _typeMap.insert({"f64", _builder.getDoubleTy()});   // 64 位浮点数
}

// ==================== 编译主入口 ====================
// 编译一个源文件，按顺序执行:
// 1. 编译全局常量
// 2. 编译结构体声明
// 3. 编译结构体实现 (方法)
// 4. 如果是 SDK，生成运行时辅助函数
// 5. 编译普通函数
// 6. 生成泛型实例的方法
// 7. 生成泛型函数实例
// 8. 如果不是 SDK 且有 main 函数，生成启动代码
void Compiler::compile(p<FileNode> file) {
    DEBUG_LOG("=== Starting compilation ===");
    DEBUG_LOG_VAL("  isSdk", _isSdk);

    DEBUG_LOG("Compiling global constants...");
    compileGlobalConsts();

    DEBUG_LOG("Compiling struct declarations...");
    compileStructDecls();

    DEBUG_LOG("Compiling struct implementations...");
    compileStructImpls();

    // SDK 需要生成运行时辅助函数
    // 这些函数用于 Box、Array 等类型的内存管理
    if (_isSdk) {
        DEBUG_LOG("Emitting runtime helpers (yux module)");
        runtime::emitRuntimeHelpers(_builder, _module);
        runtime::emitBoxHelpers(_context, _builder, _module);
        runtime::emitArrayHelpers(_context, _builder, _module);
    }

    // 编译所有非泛型、非 CompilerInner 的函数
    auto functions = file->getFunctions();
    DEBUG_LOG_VAL("Compiling functions", functions.size());
    for (auto fn : functions) {
        // 跳过泛型函数模板，它们会在使用时单态化
        if (fn->header()->isGeneric()) {
            DEBUG_LOG_VAL("  Skipping generic function template", fn->header()->name().getText());
            continue;
        }
        // 跳过编译器内部函数，它们由编译器特殊处理
        if (fn->header()->hasAnno("CompilerInner")) {
            DEBUG_LOG_VAL("  Skipping #CompilerInner function (compiler handles)", fn->header()->name().getText());
            continue;
        }
        auto func = getFunction(fn->header());
        compileFn(fn, func);
    }

    // 生成所有延迟的泛型实例方法
    DEBUG_LOG("Emitting generic instance methods");
    emitInstanceMethods();

    // 生成所有延迟的泛型函数实例
    DEBUG_LOG("Emitting generic function instances");
    emitFnInstances();

    // 非 SDK 程序需要生成 main 启动代码
    // main 函数会被重命名为 yux_main，真正的 main 由 mainStartup 提供
    if (!_isSdk && _file->getFunction("main")) {
        DEBUG_LOG("Emitting main startup");
        runtime::emitMainStartup(_context, _builder, _module);
    }
    DEBUG_LOG("=== Compilation complete ===");
}

// ==================== 全局常量编译 ====================
// 编译文件中的所有全局常量
// 全局常量在编译时确定值，存储在模块的全局变量表中
void Compiler::compileGlobalConsts() {
    for (auto globalConst : _file->getGlobalConsts()) {
        string name = globalConst->name().getText();
        bool isPriv = !name.empty() && name[0] == '_';  // 以下划线开头的是私有常量
        string mangledName = Mangler::global(_file->moduleName(), name, isPriv);
        TypeInfo type = globalConst->getType();
        auto llvmType = getLLVMType(type);

        llvm::Constant* initValue = nullptr;
        auto literal = globalConst->value();
        auto text = literal->getValue().getText();

        // 处理整数字面量
        // 支持多种格式: 十进制、二进制 (0b)、八进制 (0o)、十六进制 (0x)
        // 支持类型后缀 (i32, u64 等) 和下划线分隔符
        if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
            string numStr = text;
            static const std::regex suffix_regex(R"([iu](?:8|16|32|64)?$)");
            numStr = std::regex_replace(numStr, suffix_regex, "");
            
            int base = 10;
            string parseStr = numStr;
            if (numStr.size() >= 2) {
                if (numStr[0] == '0' && (numStr[1] == 'b' || numStr[1] == 'B')) {
                    base = 2;
                    parseStr = numStr.substr(2);
                } else if (numStr[0] == '0' && (numStr[1] == 'o' || numStr[1] == 'O')) {
                    base = 8;
                    parseStr = numStr.substr(2);
                } else if (numStr[0] == '0' && (numStr[1] == 'x' || numStr[1] == 'X')) {
                    base = 16;
                    parseStr = numStr.substr(2);
                }
            }
            parseStr.erase(std::remove(parseStr.begin(), parseStr.end(), '_'), parseStr.end());
            i64 numVal = std::stoll(parseStr, nullptr, base);
            initValue = llvm::ConstantInt::get(llvmType, numVal, true);
        } 
        // 处理浮点数字面量
        // FLOAT 词法形如: [-]?(INT_10|INT.INT|INT.INT 'e' '-'? INT) ('f32'|'f64')?
        // 需保留科学计数法 (e[-]?\d+)，仅剥掉类型后缀 f32/f64
        else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
            string numStr = text;
            if (numStr.size() >= 3) {
                string suf = numStr.substr(numStr.size() - 3);
                if (suf == "f32" || suf == "f64") {
                    numStr = numStr.substr(0, numStr.size() - 3);
                }
            }
            f64 numVal = stod(numStr);
            initValue = llvm::ConstantFP::get(llvmType, numVal);
        }
        // 处理布尔字面量
        else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
            bool boolVal = (text == "true");
            initValue = llvm::ConstantInt::get(llvmType, boolVal ? 1 : 0, false);
        } else {
            throw YuxError(globalConst->getLineNumber(), "Unsupported literal type for global constant: {}", type.name);
        }

        // 设置链接类型: 私有常量使用内部链接，公开常量使用外部链接
        auto linkage = globalConst->isPrivate()
                           ? llvm::GlobalValue::InternalLinkage
                           : llvm::GlobalValue::ExternalLinkage;

        new llvm::GlobalVariable(
            *_module,
            llvmType,
            true,           // isConstant = true
            linkage,
            initValue,
            mangledName
        );

        DEBUG_LOG_VAL("Created global constant", mangledName << " : " << type.name);
    }
}

// ==================== 结构体声明编译 ====================
// 编译所有结构体声明，创建对应的 LLVM 结构体类型
// 处理顺序: SDK 结构体 -> 导入的结构体 -> 当前文件的结构体
void Compiler::compileStructDecls() {
    // 首先编译 SDK 中的结构体 (如果当前文件不是 SDK)
    // 这确保 SDK 类型在其他文件之前可用
    if (_yux && _yux->sdkFile() && _file != _yux->sdkFile()) {
        DEBUG_LOG_VAL("Compiling SDK struct declarations", _yux->sdkFile()->getStructDecls().size());
        for (auto structDecl : _yux->sdkFile()->getStructDecls()) {
            if (structDecl->isGeneric()) continue;  // 跳过泛型结构体，它们会在使用时单态化
            DEBUG_LOG_VAL("  SDK struct", structDecl->name().getText());
            getOrCreateStructType(structDecl, _yux->sdkFile());
        }
    }
    // 编译通配符导入的结构体
    for (auto* imp : _file->wildcardImports()) {
        if (imp == _yux->sdkFile()) continue;  // 避免重复处理 SDK
        DEBUG_LOG_VAL("Compiling imported struct declarations from", imp->moduleName());
        for (auto structDecl : imp->getStructDecls()) {
            if (structDecl->isGeneric()) continue;
            DEBUG_LOG_VAL("  imported struct", structDecl->name().getText());
            getOrCreateStructType(structDecl, imp);
        }
    }
    // 最后编译当前文件的结构体
    DEBUG_LOG_VAL("Compiling file struct declarations", _file->getStructDecls().size());
    for (auto structDecl : _file->getStructDecls()) {
        if (structDecl->isGeneric()) continue;
        DEBUG_LOG_VAL("  struct", structDecl->name().getText());
        getOrCreateStructType(structDecl);
    }
}

// ==================== 结构体实现编译 ====================
// 编译所有结构体实现 (方法和析构函数)
// 同时为需要析构函数但没有显式定义的结构体生成默认析构函数
void Compiler::compileStructImpls() {
    auto& impls = _file->getStructImpls();
    DEBUG_LOG_VAL("  compileStructImpls", impls.size() << " implementations");

    set<string> processedStructs;       // 已处理的结构体
    set<string> hasExplicitDestructor;  // 有显式析构函数的结构体

    for (auto structImpl : impls) {
        if (structImpl->isGeneric()) continue;  // 泛型结构体在实例化时处理
        string structName = structImpl->structName();
        processedStructs.insert(structName);
        DEBUG_LOG_VAL("    Processing struct impl", structName);

        // 处理显式定义的析构函数
        if (structImpl->hasDestructor()) {
            hasExplicitDestructor.insert(structName);
            DEBUG_LOG("      Has destructor");
            auto destructor = structImpl->destructor();
            vector<TypeInfo> paramTypes;
            paramTypes.emplace_back(structName);

            auto func = getDestructorFunction(structName);
            compileMethod(destructor, func, structName, true);
        }

        // 编译所有方法 (跳过 CompilerInner 方法)
        auto& methods = structImpl->methods();
        DEBUG_LOG_VAL("      Methods count", methods.size());
        for (auto method : methods) {
            string methodName = method->header()->name().getText();
            
            // CompilerInner 方法由编译器特殊处理，不生成 IR
            if (method->header()->hasAnno("CompilerInner")) {
                DEBUG_LOG_VAL("        Skipping #CompilerInner method (compiler handles)", structName << "." << methodName);
                continue;
            }
            
            DEBUG_LOG_VAL("        Compiling method", methodName);
            vector<TypeInfo> paramTypes;
            for (auto param : method->header()->params()) {
                if (param->type()) {
                    paramTypes.push_back(param->type()->getType());
                }
            }
            TypeInfo retType;
            if (method->header()->retType()) {
                retType = method->header()->retType()->getType();
            }
            auto func = getMethodFunction(structName, methodName, paramTypes, retType);
            compileMethod(method, func, structName);
        }
    }

    // 为需要析构函数但没有显式定义的结构体生成默认析构函数
    // 默认析构函数会递归调用所有字段的析构函数
    for (auto structDecl : _file->getStructDecls()) {
        if (structDecl->isGeneric()) continue;
        string structName = structDecl->name().getText();
        if (hasExplicitDestructor.find(structName) == hasExplicitDestructor.end()) {
            if (structNeedsDestructor(structName)) {
                DEBUG_LOG_VAL("  Generating default destructor for struct", structName);
                generateDefaultDestructor(structName);
            }
        }
    }
}

// ==================== 泛型结构体实例方法生成 ====================
// 生成所有泛型结构体实例的方法
// 使用迭代方式处理，因为一个实例的方法可能触发新实例的创建
void Compiler::emitInstanceMethods() {
    bool progress = true;
    while (progress) {
        progress = false;
        // 收集所有键，避免在迭代时修改 map
        vector<string> keys;
        keys.reserve(_structInstances.size());
        for (auto& [k, _] : _structInstances) keys.push_back(k);

        for (auto& key : keys) {
            auto& inst = _structInstances[key];
            if (inst.methodsEmitted) continue;  // 已处理
            inst.methodsEmitted = true;
            progress = true;

            if (!inst.baseImpl) continue;  // 没有实现则跳过

            // 建立类型参数替换映射
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < inst.args.size(); ++i) {
                subst[inst.baseDecl->typeParams()[i]] = inst.args[i];
            }
            string baseName = inst.baseDecl->name().getText();
            _substStack.push_back(SubstFrame{subst, baseName, inst.mangledName, inst.sourceFile, inst.sourceLine});

            string structName = inst.mangledName;
            DEBUG_LOG_VAL("  Emitting generic instance methods", structName);

            try {
                // 编译析构函数 (如果有)
                if (inst.baseImpl->hasDestructor()) {
                    auto destructor = inst.baseImpl->destructor();
                    auto func = getDestructorFunction(structName);
                    compileMethod(destructor, func, structName, true);
                }

                // 编译所有方法
                for (auto method : inst.baseImpl->methods()) {
                    // 跳过 CompilerInner 方法
                    if (method->header()->hasAnno("CompilerInner")) {
                        DEBUG_LOG_VAL("        Skipping #CompilerInner method (compiler handles)", baseName << "." << method->header()->name().getText());
                        continue;
                    }

                    string methodName = method->header()->name().getText();
                    vector<TypeInfo> paramTypes;
                    for (auto param : method->header()->params()) {
                        if (param->type()) {
                            paramTypes.push_back(applySubst(param->type()->getType()));
                        }
                    }
                    TypeInfo retType;
                    if (method->header()->retType()) {
                        retType = applySubst(method->header()->retType()->getType());
                    }
                    // 构造函数名需要使用实例名
                    string effMethodName = (methodName == baseName) ? structName : methodName;
                    auto func = getMethodFunction(structName, effMethodName, paramTypes, retType);
                    compileMethod(method, func, structName);
                }

                // 如果没有显式析构函数但需要，生成默认析构函数
                if (!inst.baseImpl->hasDestructor() && structNeedsDestructor(structName)) {
                    generateDefaultDestructor(structName);
                }
            } catch (const YuxError& e) {
                _substStack.pop_back();
                rethrowWithInstantiationContext(e);  // 附加实例化上下文后重新抛出
            }

            _substStack.pop_back();
        }
    }
}

// ==================== 泛型函数实例管理 ====================
// 确保泛型函数实例存在，返回 mangle 后的名称
// 如果实例不存在，创建一个新的实例记录
string Compiler::ensureFnInstance(p<FnNode> baseFn, const vector<TypeInfo>& typeArgs, p<FileNode> ownerFile, int sourceLine) {
    string baseName = baseFn->header()->name().getText();
    // 生成 mangle 名称: foo$i32$i64
    string mangledName = baseName;
    for (auto& a : typeArgs) {
        mangledName += "$" + a.getGenericMangleName();
    }

    // 检查是否已存在
    auto it = _fnInstances.find(mangledName);
    if (it != _fnInstances.end()) return mangledName;

    // 验证类型参数数量
    auto& typeParams = baseFn->header()->typeParams();
    if (typeArgs.size() != typeParams.size()) {
        throw YuxError(sourceLine,
            "Generic function '{}' expects {} type args, got {}",
            baseName, typeParams.size(), typeArgs.size());
    }

    // 创建实例记录
    FnInstance inst;
    inst.baseFn = baseFn;
    inst.ownerFile = ownerFile ? ownerFile : _file;
    inst.typeArgs = typeArgs;
    inst.mangledName = mangledName;

    _fnInstances[mangledName] = std::move(inst);
    DEBUG_LOG_VAL("Created generic function instance", mangledName);
    return mangledName;
}

// ==================== 泛型函数实例生成 ====================
// 生成所有泛型函数实例的 IR
// 使用迭代方式处理，因为一个函数可能调用另一个泛型函数
void Compiler::emitFnInstances() {
    bool progress = true;
    while (progress) {
        progress = false;
        // 收集所有键，避免在迭代时修改 map
        vector<string> keys;
        keys.reserve(_fnInstances.size());
        for (auto& [k, _] : _fnInstances) keys.push_back(k);

        for (auto& key : keys) {
            auto& inst = _fnInstances[key];
            if (inst.emitted) continue;  // 已处理
            inst.emitted = true;
            progress = true;

            auto baseFn = inst.baseFn;
            auto& typeParams = baseFn->header()->typeParams();

            // 建立类型参数替换映射
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < typeParams.size(); ++i) {
                subst[typeParams[i]] = inst.typeArgs[i];
            }

            string srcFile = _file ? _file->moduleName() : "";
            _substStack.push_back(SubstFrame{subst, "", inst.mangledName, srcFile, 0});

            try {
                // 计算实例化后的参数类型
                vector<TypeInfo> paramTypes;
                for (auto param : baseFn->header()->params()) {
                    if (param->type()) {
                        paramTypes.push_back(applySubst(param->type()->getType()));
                    }
                }

                // 计算实例化后的返回类型
                TypeInfo retType;
                if (baseFn->header()->retType()) {
                    retType = applySubst(baseFn->header()->retType()->getType());
                }

                // 生成 mangle 后的函数名
                bool isPrivate = !inst.mangledName.empty() && inst.mangledName[0] == '_';
                string mangledFnName = Mangler::function(
                    inst.ownerFile->moduleName(), inst.mangledName, paramTypes, isPrivate);

                // 获取或创建 LLVM 函数
                auto fn = _module->getFunction(mangledFnName);
                if (!fn) {
                    vector<llvm::Type*> llvmParamTypes;
                    for (auto& t : paramTypes) {
                        // 结构体类型通过指针传递
                        if (t.isPtr() || t.isRef()) {
                            llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
                        } else {
                            auto sd = _file->getStructDecl(t.name);
                            if (!sd && _yux && _yux->sdkFile()) {
                                sd = _yux->sdkFile()->getStructDecl(t.name);
                            }
                            if (sd && !isBuiltinType(t.name)) {
                                llvmParamTypes.push_back(llvm::PointerType::get(_context, 0));
                            } else {
                                llvmParamTypes.push_back(getLLVMType(t));
                            }
                        }
                    }
                    auto llvmRetType = retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
                    auto fnType = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
                    fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledFnName, _module);
                }

                DEBUG_LOG_VAL("  Emitting generic function instance", inst.mangledName);
                compileFn(baseFn, fn);
            } catch (const YuxError& e) {
                _substStack.pop_back();
                rethrowWithInstantiationContext(e);
            }

            _substStack.pop_back();
        }
    }
}

// ==================== 函数编译 ====================
// 编译一个函数的完整实现
// 包括参数处理、函数体编译、隐式返回等
void Compiler::compileFn(p<FnNode> node, llvm::Function* func) {
    _currentFn = func;
    _currentFnNode = node;
    _currentStructName.clear();
    _localVarPtrs.clear();
    _scopeVars.clear();

    DEBUG_LOG_VAL("Compiling function", node->header()->name().getText());

    // 创建入口基本块
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    DEBUG_LOG("Created entry basic block");

    // 处理函数参数
    // 为每个参数创建栈上存储空间 (alloca)
    for (auto& arg : func->args()) {
        auto paramName = node->header()->params()[arg.getArgNo()]->name().getText();
        TypeInfo paramType = node->header()->params()[arg.getArgNo()]->type()
                                 ? node->header()->params()[arg.getArgNo()]->type()->getType()
                                 : TypeInfo();

        if (paramType.isRef()) {
            // 引用类型直接使用传入的指针
            _localVarPtrs[paramName] = &arg;
            DEBUG_LOG_VAL("  Param (ref)", paramName << " : " << paramType.getFullName());
        } else {
            auto llvmType = getLLVMType(paramType);
            auto structDecl = _file->getStructDecl(paramType.name);

            if (structDecl && !isBuiltinType(paramType.name)) {
                // 结构体类型通过指针传递 (避免复制)
                _localVarPtrs[paramName] = &arg;
                DEBUG_LOG_VAL("  Param (struct ptr)", paramName << " : " << paramType.name << "*");
            } else {
                // 基本类型: 创建 alloca 并存储参数值
                auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
                _builder.CreateStore(&arg, alloca);
                _localVarPtrs[paramName] = alloca;
                DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);
            }
        }
    }

    // 编译函数体
    for (auto s : node->body()) {
        compileStatement(s);
    }

    // 处理隐式返回
    // 如果函数没有显式返回语句，添加隐式 void 返回
    auto fnName = node->header()->name().getText();
    if (!_builder.GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();  // 在返回前调用析构函数
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling function", node->header()->name().getText());
}

// ==================== 方法编译 ====================
// 编译结构体方法
// 与普通函数类似，但需要处理 self 参数
void Compiler::compileMethod(p<FnNode> node, llvm::Function* func, const string& structName, bool isDestructor) {
    _currentFn = func;
    _currentFnNode = node;
    _currentStructName = structName;
    _localVarPtrs.clear();
    _scopeVars.clear();

    DEBUG_LOG_VAL("Compiling method", structName << "." << node->header()->name().getText());

    DEBUG_LOG_VAL("  Method params count", node->header()->params().size());
    DEBUG_LOG_VAL("  LLVM args count", func->arg_size());

    // 创建入口基本块
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    DEBUG_LOG("Created entry basic block");

    auto args = func->args();
    auto argIt = args.begin();

    // 处理 self 参数 (方法的第一个参数)
    llvm::Value* selfPtr = nullptr;
    if (argIt != args.end()) {
        string selfName = "self";
        
        if (isBuiltinType(structName)) {
            // 内置类型: self 是值类型，需要创建 alloca
            auto selfAlloca = _builder.CreateAlloca(getLLVMType(TypeInfo(structName)), nullptr, "self.addr");
            _builder.CreateStore(argIt, selfAlloca);
            _localVarPtrs[selfName] = selfAlloca;
            selfPtr = selfAlloca;
            DEBUG_LOG_VAL("  Param (self - builtin value)", selfName << " : " << structName);
        } else {
            // 结构体类型: self 是指针类型
            _localVarPtrs[selfName] = argIt;
            selfPtr = argIt;
            DEBUG_LOG_VAL("  Param (self)", selfName << " : " << structName << "*");
        }
        ++argIt;
    }

    // 处理其他参数
    for (auto& param : node->header()->params()) {
        if (argIt == args.end()) break;

        auto paramName = param->name().getText();
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        auto llvmType = getLLVMType(paramType);

        auto structDecl = _file->getStructDecl(paramType.name);
        if (structDecl && !isBuiltinType(paramType.name)) {
            // 结构体类型通过指针传递
            _localVarPtrs[paramName] = argIt;
            DEBUG_LOG_VAL("  Param (struct ptr)", paramName << " : " << paramType.name << "*");
        } else {
            // 基本类型: 创建 alloca 并存储参数值
            auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
            _builder.CreateStore(argIt, alloca);
            _localVarPtrs[paramName] = alloca;
            DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);
        }
        ++argIt;
    }

    // 编译方法体
    for (auto s : node->body()) {
        compileStatement(s);
    }

    // 处理隐式返回
    if (!_builder.GetInsertBlock()->getTerminator()) {
        if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();
            // 析构函数需要在返回前调用字段析构函数
            if (isDestructor && selfPtr) {
                callFieldDestructor(selfPtr, structName);
            }
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling method", structName << "." << node->header()->name().getText());
}
