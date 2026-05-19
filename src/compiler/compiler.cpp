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
#include "analyzer/borrow_checker.h"
#include "analyzer/const_mut_checker.h"
#include "analyzer/flow_terminate_checker.h"
#include "sema/sema_pass.h"
#include "ast/mangler.h"
#include "ast/node/fn_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/literal_node.h"
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

    // §12.2 / §12.3 / §12.5 显式 draft 实现校验 (Phase 3.2.e):
    // 第一次 compile() 时跑一次, 覆盖 SDK + 所有用户文件; 后续重入空操作.
    if (_yux) {
        _yux->validateSpecImpls();
    }

    // 透明类型别名的一次性校验：名称冲突 + 环检测
    DEBUG_LOG("Validating type aliases...");
    validateAliases();

    // Sema/Codegen 拆分骨架（Phase 3.1）：当前是 no-op, 仅落位走 AST 全树。
    // 3.2 起 visitExpr 写 resolvedType, 与 compile<Foo>Expr 入口的写并存校验;
    // 3.3+ 按子系统迁 throw, 让 codegen 不再抛语义错。
    DEBUG_LOG("Running SemaPass...");
    string semaSourcePath = (_yux && _file) ? _yux->modulePath(_file->moduleName()) : "";
    SemaPass(_file, _yux ? _yux->sdkFile() : nullptr, semaSourcePath).run();

    DEBUG_LOG("Compiling global constants...");
    compileGlobalConsts();

    DEBUG_LOG("Compiling struct declarations...");
    compileStructDecls();

    DEBUG_LOG("Compiling struct implementations...");
    compileStructImpls();

    DEBUG_LOG("Compiling enum destructors...");
    compileEnumDtors();

    // SDK 需要生成运行时辅助函数
    // 这些函数用于 Rc、Array 等类型的内存管理
    if (_isSdk) {
        DEBUG_LOG("Emitting runtime helpers (yux module)");
        runtime::emitRuntimeHelpers(_builder, _module);
        runtime::emitRcHelpers(_context, _builder, _module);
        runtime::emitWeakHelpers(_context, _builder, _module);
        runtime::emitArrayHelpers(_context, _builder, _module);
        runtime::emitHeapHandleHelpers(_context, _builder, _module);
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
    if (!_isSdk) {
        auto mainFn = _file->getFunction("main");
        if (mainFn) {
            // 10g-7：main 是否标 #Fallible(E)？
            string mainFallibleErr;
            if (mainFn->header()) {
                if (auto e = mainFn->header()->getAnnoArg("Fallible")) {
                    mainFallibleErr = *e;
                }
            }
            if (!mainFallibleErr.empty()) {
                DEBUG_LOG_VAL("Emitting main startup (Fallible)", mainFallibleErr);
                emitMainStartupFallible(mainFallibleErr);
            } else {
                DEBUG_LOG("Emitting main startup");
                runtime::emitMainStartup(_context, _builder, _module);
            }
        }
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
            // 识别后缀以选 signed/unsigned 解析路径
            static const std::regex suffix_regex(R"([iu](?:8|16|32|64)?$)");
            std::smatch m;
            string suffix;
            if (std::regex_search(numStr, m, suffix_regex)) {
                suffix = m.str();
            }
            bool isUnsigned = !suffix.empty() && suffix[0] == 'u';
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
            i64 numVal = 0;
            try {
                if (isUnsigned) {
                    numVal = static_cast<i64>(std::stoull(parseStr, nullptr, base));
                } else {
                    numVal = std::stoll(parseStr, nullptr, base);
                }
            } catch (const std::out_of_range&) {
                int line = literal->getLineNumber();
                throw YuxError(line > 0 ? line : 1, literal->getColumn(),
                    ErrorCode::E3103, text, suffix.empty() ? string("i64") : suffix);
            } catch (const std::invalid_argument&) {
                int line = literal->getLineNumber();
                throw YuxError(line > 0 ? line : 1, literal->getColumn(),
                    ErrorCode::E3103, text, suffix.empty() ? string("i64") : suffix);
            }
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
            throw YuxError(globalConst->getLineNumber(), globalConst->getColumn(), ErrorCode::E3082, type.name);
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
            
            bool isStatic = method->header()->isStatic();
            DEBUG_LOG_VAL("        Compiling method", methodName << (isStatic ? " [#Static]" : ""));
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
            string mFallibleErr;
            if (auto e = method->header()->getAnnoArg("Fallible")) mFallibleErr = *e;
            auto func = getMethodFunction(structName, methodName, paramTypes, retType, mFallibleErr, isStatic);
            compileMethod(method, func, structName, false, isStatic);
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

            // 编译方法体时把 _file 临时切换到泛型结构体的定义文件（owner）。
            // 这样体内对其它私有 SDK 函数（如 _exit）的调用，私有可见性检查能通过。
            // 否则 _file 仍是用户文件，跨模块访问会被拦截。
            auto savedFile = _file;
            if (inst.ownerFile && inst.ownerFile != _file) {
                _file = inst.ownerFile;
            }

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
                    bool isStatic = method->header()->isStatic();
                    // Phase 6D-tail：同名 ctor 已砍 (E3130)；泛型 impl 在实例化时
                    // 兜底拦截同名非 #Static 方法（sema 跳过 generic impl，故只能在这里抓）。
                    if (!isStatic && methodName == baseName) {
                        throw YuxError(method->header()->getLineNumber(),
                                       ErrorCode::E3130, baseName, baseName, baseName);
                    }
                    string mFallibleErr;
                    if (auto e = method->header()->getAnnoArg("Fallible")) mFallibleErr = *e;
                    auto func = getMethodFunction(structName, methodName, paramTypes, retType, mFallibleErr, isStatic);
                    compileMethod(method, func, structName, false, isStatic);
                }

                // 如果没有显式析构函数但需要，生成默认析构函数
                if (!inst.baseImpl->hasDestructor() && structNeedsDestructor(structName)) {
                    generateDefaultDestructor(structName);
                }
            } catch (const YuxError& e) {
                _file = savedFile;
                _substStack.pop_back();
                rethrowWithInstantiationContext(e);  // 附加实例化上下文后重新抛出
            }

            _file = savedFile;
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
        throw YuxError(sourceLine, ErrorCode::E6010,
            baseName, typeParams.size(), typeArgs.size())
            .withHint(std::format("实例化时的类型实参个数需与声明匹配；调用处补齐 {} 个类型 `:<{}>`",
                typeParams.size(),
                std::string(typeParams.size() == 1 ? "T" : "T1, T2, ...")));
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
                    string fallibleErr;
                    if (auto e = baseFn->header()->getAnnoArg("Fallible")) fallibleErr = *e;
                    auto llvmRetType = wrapFallibleRetType(retType, fallibleErr);
                    auto fnType = llvm::FunctionType::get(llvmRetType, llvmParamTypes, false);
                    fn = llvm::Function::Create(fnType, llvm::Function::LinkOnceODRLinkage, mangledFnName, _module);
                }
                // 泛型实例跨 TU 由 mangle 名保证 ODR 等价，定义点统一标 linkonce_odr +
                // COMDAT，让 LLD 在多模块各自实例化时合并同名定义（修 P1-MMP）。
                // COFF 平台下 linkonce_odr 必须显式 COMDAT，否则仍按强符号 emit。
                fn->setLinkage(llvm::Function::LinkOnceODRLinkage);
                fn->setVisibility(llvm::GlobalValue::DefaultVisibility);
                fn->setComdat(_module->getOrInsertComdat(mangledFnName));

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

    // Phase 4d: 借用静态检查（寿命 + 根对象重赋禁）
    checkBorrows(node);

    // P1-2: DRAFT-const-mut §3.3 局部 cval 初值约束（E3104）
    checkConstMut(node);

    // Phase 10d-2：`#NoReturn` 流终止分析（E7014，DRAFT-错误.md §8.3）
    checkFlowTerminate(node);

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
        } else if (structParamUsesPointer(paramType.name)) {
            // Phase 3c.1: 非平凡结构体仍走指针 ABI
            _localVarPtrs[paramName] = &arg;
            DEBUG_LOG_VAL("  Param (struct ptr)", paramName << " : " << paramType.name << "*");
        } else {
            // 基本类型 / 平凡结构体: 创建 alloca 并存储 by-value 参数
            auto llvmType = getLLVMType(paramType);
            auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
            _builder.CreateStore(&arg, alloca);
            _localVarPtrs[paramName] = alloca;
            DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);

            // Phase 3a: 堆句柄参数（Rc/Array/Weak）按 callee-clean 协议
            // 在作用域结束时 release，与局部变量同路径
            if (typeNeedsDestructor(paramType)) {
                _scopeVars.push_back(paramName);
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
        // #Fallible(E) void-return 函数体走到末尾：补隐式成功-void ret struct
        // （与 compileRetVoidStatement 同形；[#10.A] T_ok=void）
        string fallibleErrName;
        if (auto e = node->header()->getAnnoArg("Fallible")) fallibleErrName = *e;
        bool fnRetVoid = !node->header()->retType();
        if (!fallibleErrName.empty() && fnRetVoid) {
            callDestructorsForScope();
            auto retStructTy = getFallibleRetStructType(TypeInfo(), fallibleErrName);
            auto errLLVMTy = getLLVMType(TypeInfo(fallibleErrName));
            llvm::Value* rs = llvm::UndefValue::get(retStructTy);
            rs = _builder.CreateInsertValue(rs, _builder.getInt1(0), {0});
            rs = _builder.CreateInsertValue(rs, llvm::Constant::getNullValue(errLLVMTy), {1});
            _builder.CreateRet(rs);
            DEBUG_LOG("  Added implicit #Fallible void-success return");
        } else if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();  // 在返回前调用析构函数
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling function", node->header()->name().getText());
}

// ==================== 方法编译 ====================
// 编译结构体方法
// 与普通函数类似，但需要处理当前实例参数（`$`）
void Compiler::compileMethod(p<FnNode> node, llvm::Function* func, const string& structName, bool isDestructor, bool isStatic) {
    _currentFn = func;
    _currentFnNode = node;
    _currentStructName = structName;
    _localVarPtrs.clear();
    _scopeVars.clear();

    DEBUG_LOG_VAL("Compiling method", structName << "." << node->header()->name().getText());

    // Phase 4d: 借用静态检查
    checkBorrows(node, structName);

    // P1-2: DRAFT-const-mut §3.3 局部 cval 初值约束（E3104）
    checkConstMut(node);

    // Phase 10d-2：`#NoReturn` 流终止分析（E7014）
    checkFlowTerminate(node);

    DEBUG_LOG_VAL("  Method params count", node->header()->params().size());
    DEBUG_LOG_VAL("  LLVM args count", func->arg_size());

    // 创建入口基本块
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(_context, "entry", func);
    _builder.SetInsertPoint(entry);
    DEBUG_LOG("Created entry basic block");

    auto args = func->args();
    auto argIt = args.begin();

    // 处理当前实例参数（方法的第一个参数，对应用户层 `$`）
    // DRAFT-static-fn: #Static fn 无 receiver, 整段跳过.
    llvm::Value* thisPtr = nullptr;
    if (!isStatic && argIt != args.end()) {
        string thisName = "$";

        if (isBuiltinType(structName)) {
            // 内置类型：值类型，需要创建 alloca
            auto thisAlloca = _builder.CreateAlloca(getLLVMType(TypeInfo(structName)), nullptr, "this.addr");
            _builder.CreateStore(argIt, thisAlloca);
            _localVarPtrs[thisName] = thisAlloca;
            thisPtr = thisAlloca;
            DEBUG_LOG_VAL("  Param ($ - builtin value)", thisName << " : " << structName);
        } else {
            // 结构体类型：指针类型
            _localVarPtrs[thisName] = argIt;
            thisPtr = argIt;
            DEBUG_LOG_VAL("  Param ($)", thisName << " : " << structName << "*");
        }
        ++argIt;
    }

    // 处理其他参数
    for (auto& param : node->header()->params()) {
        if (argIt == args.end()) break;

        auto paramName = param->name().getText();
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        auto llvmType = getLLVMType(paramType);

        if (paramType.isRef()) {
            // 引用类型直接使用传入的指针（与 compileFn 同路径）；
            // 不另开 alloca，否则 `other.field` 会 GEP 到 alloca 自身而非被引用的 struct
            _localVarPtrs[paramName] = argIt;
            DEBUG_LOG_VAL("  Method param (ref)", paramName << " : " << paramType.getFullName());
        } else if (structParamUsesPointer(paramType.name)) {
            // Phase 3c.1: 非平凡结构体仍走指针 ABI
            _localVarPtrs[paramName] = argIt;
            DEBUG_LOG_VAL("  Param (struct ptr)", paramName << " : " << paramType.name << "*");
        } else {
            // 基本类型 / 平凡结构体: 创建 alloca 并存储 by-value 参数
            auto alloca = _builder.CreateAlloca(llvmType, nullptr, paramName);
            _builder.CreateStore(argIt, alloca);
            _localVarPtrs[paramName] = alloca;
            DEBUG_LOG_VAL("  Param", paramName << " : " << paramType.name);

            // Phase 3a: 堆句柄参数（Rc/Array/Weak）按 callee-clean 协议在作用域末 release
            if (typeNeedsDestructor(paramType)) {
                _scopeVars.push_back(paramName);
            }
        }
        ++argIt;
    }

    // 编译方法体
    for (auto s : node->body()) {
        compileStatement(s);
    }

    // 处理隐式返回
    if (!_builder.GetInsertBlock()->getTerminator()) {
        // 方法上的 #Fallible(E) void：补隐式成功-void ret struct（与 fn 同型）
        string fallibleErrName;
        if (auto e = node->header()->getAnnoArg("Fallible")) fallibleErrName = *e;
        bool methRetVoid = !node->header()->retType();
        if (!fallibleErrName.empty() && methRetVoid && !isDestructor) {
            callDestructorsForScope();
            auto retStructTy = getFallibleRetStructType(TypeInfo(), fallibleErrName);
            auto errLLVMTy = getLLVMType(TypeInfo(fallibleErrName));
            llvm::Value* rs = llvm::UndefValue::get(retStructTy);
            rs = _builder.CreateInsertValue(rs, _builder.getInt1(0), {0});
            rs = _builder.CreateInsertValue(rs, llvm::Constant::getNullValue(errLLVMTy), {1});
            _builder.CreateRet(rs);
            DEBUG_LOG("  Added implicit #Fallible void-success return (method)");
        } else if (func->getReturnType()->isVoidTy()) {
            callDestructorsForScope();
            // 析构函数需要在返回前调用字段析构函数
            if (isDestructor && thisPtr) {
                callFieldDestructor(thisPtr, structName);
            }
            _builder.CreateRetVoid();
            DEBUG_LOG("  Added implicit void return");
        }
    }
    DEBUG_LOG_VAL("Finished compiling method", structName << "." << node->header()->name().getText());
}
