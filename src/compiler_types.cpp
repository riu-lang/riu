// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 类型系统编译实现
// 
// 本文件包含类型系统相关的编译逻辑:
// - TypeInfo 到 LLVM 类型的映射
// - 函数类型生成
// - 结构体类型创建和管理
// - 泛型结构体实例化
// - 类型替换 (泛型参数替换)

#include "compiler.h"
#include "mangler.h"
#include "node/struct_node.h"
#include "node/fn_node.h"
#include <llvm/IR/DerivedTypes.h>

// ==================== 错误报告辅助 ====================

// 重新抛出异常并附加实例化上下文
// 用于在泛型实例化过程中提供更详细的错误信息
void Compiler::rethrowWithInstantiationContext(const YuxError& e) const {
    string ctx = formatInstantiationContext();
    string what = e.what();
    // 避免重复添加上下文
    if (ctx.empty() || what.find("instantiated as '") != string::npos) {
        throw e;
    }
    throw YuxError(e.getLineNumber(), "{}\n  {}", what, ctx);
}

// 格式化泛型实例化上下文信息
// 返回类似 "instantiated as 'Box$i32' at module:line" 的字符串
string Compiler::formatInstantiationContext() const {
    if (_substStack.empty()) return "";
    string result;
    // 从栈顶向下遍历，显示完整的实例化链
    for (auto it = _substStack.rbegin(); it != _substStack.rend(); ++it) {
        const auto& frame = *it;
        if (!frame.effStructName.empty()) {
            if (!result.empty()) result += "\n  ";
            result += "instantiated as '" + frame.effStructName + "'";
            if (!frame.sourceFile.empty()) {
                result += " at " + frame.sourceFile;
                if (frame.sourceLine > 0) {
                    result += ":" + to_string(frame.sourceLine);
                }
            }
        }
    }
    return result;
}

// ==================== 类型替换 ====================

// 应用当前类型替换
// 用于泛型实例化过程中的类型参数替换
TypeInfo Compiler::applySubst(const TypeInfo& t) const {
    if (_substStack.empty()) return t;
    auto& frame = _substStack.back();
    // 使用 TypeInfo::substitute 进行类型参数替换
    TypeInfo result = t.substitute(frame.subst);
    // 特殊处理: 将泛型原名替换为实例名
    // 例如: Box -> Box$i32
    if (result.kind == TypeKind::Normal && !frame.baseStructName.empty()
        && result.name == frame.baseStructName) {
        result.name = frame.effStructName;
    }
    return result;
}

// ==================== 泛型结构体实例化 ====================

// 确保泛型结构体实例存在
// 返回 mangle 后的实例名 (如 "Box$i32")
string Compiler::ensureStructInstance(
    p<StructDeclNode> baseDecl, const vector<sp<TypeInfo>>& args, p<FileNode> ownerFile, int sourceLine) {
    string baseName = baseDecl->name().getText();
    // 生成 mangle 名称: StructName$T1$T2...
    string mangledName = baseName;
    for (auto& a : args) {
        mangledName += "$" + (a ? a->getGenericMangleName() : string("?"));
    }

    // 检查是否已存在
    auto it = _structInstances.find(mangledName);
    if (it != _structInstances.end()) return mangledName;

    // 验证类型参数数量
    if (args.size() != baseDecl->typeParams().size()) {
        throw YuxError(sourceLine,
            "Generic struct '{}' expects {} type args, got {}",
            baseName, baseDecl->typeParams().size(), args.size());
    }

    // 创建实例记录
    StructInstance inst;
    inst.baseDecl = baseDecl;
    inst.ownerFile = ownerFile ? ownerFile : _file;
    inst.mangledName = mangledName;
    // 查找结构体实现 (包含方法)
    inst.baseImpl = inst.ownerFile ? inst.ownerFile->getStructImpl(baseName) : nullptr;
    if (!inst.baseImpl && _yux && _yux->sdkFile() && _yux->sdkFile() != inst.ownerFile) {
        inst.baseImpl = _yux->sdkFile()->getStructImpl(baseName);
    }
    inst.args.reserve(args.size());
    for (auto& a : args) inst.args.push_back(a ? *a : TypeInfo());
    inst.sourceFile = _file ? _file->moduleName() : "";
    inst.sourceLine = sourceLine;
    // 关键：把当前编译模块记下来，作为本实例 IR 的符号前缀。
    // 即便后续 emitInstanceMethods 为了可见性把 _file 切到 ownerFile，
    // 实例方法的符号名仍用此处记录的消费方模块。
    inst.consumerModule = _file ? _file->moduleName() : "";

    // 建立类型参数替换映射
    map<string, TypeInfo> subst;
    for (size_t i = 0; i < args.size(); ++i) {
        subst[baseDecl->typeParams()[i]] = inst.args[i];
    }

    // 压入替换栈帧
    _substStack.push_back(SubstFrame{subst, baseName, mangledName, inst.sourceFile, inst.sourceLine});

    // 计算实例化后的字段类型
    vector<llvm::Type*> fieldTypes;
    try {
        for (auto field : baseDecl->fields()) {
            auto fieldType = field->getType();
            auto llvmTy = getLLVMType(fieldType);
            if (!llvmTy) {
                auto substituted = applySubst(fieldType);
                throw YuxError(
                    field->name().getLine(),
                    "Unknown type '{}' for field '{}' of generic struct '{}'",
                    substituted.getFullName(), field->name().getText(), baseName);
            }
            fieldTypes.push_back(llvmTy);
        }
    } catch (const YuxError& e) {
        _substStack.pop_back();
        rethrowWithInstantiationContext(e);
    }
    // Array<T> 特殊处理: 使用标准布局 { handle: Block* } 单字段（Phase 1b）
    // Block = { u32 strong, u32 weak, i64 len, i64 cap, *T data }；handle == null 表示空数组
    if (baseName == "Array") {
        fieldTypes.clear();
        fieldTypes.push_back(llvm::PointerType::get(_context, 0));
    }

    // 创建 LLVM 结构体类型
    string fullMangled = Mangler::structType(inst.ownerFile->moduleName(), mangledName);
    auto structType = llvm::StructType::create(_context, fieldTypes, fullMangled);
    _structTypes[mangledName] = structType;
    DEBUG_LOG_VAL("Created generic struct instance", fullMangled);

    _substStack.pop_back();

    _structInstances[mangledName] = std::move(inst);
    return mangledName;
}

// Phase 1b: Array<T> 的 RC Block 布局
// { u32 strong, u32 weak, i64 len, i64 cap, ptr data }
// 字段索引：0=strong, 1=weak, 2=len, 3=cap, 4=data
// 与元素类型 T 无关（data 是不透明指针，元素大小由 sizeof(T) 在调用方算）
llvm::StructType* Compiler::getArrayBlockType() {
    static const char* kName = "ArrayBlock";
    if (auto existing = llvm::StructType::getTypeByName(_context, kName)) {
        return existing;
    }
    vector<llvm::Type*> fields;
    fields.push_back(_builder.getInt32Ty());                       // strong
    fields.push_back(_builder.getInt32Ty());                       // weak
    fields.push_back(_builder.getInt64Ty());                       // len
    fields.push_back(_builder.getInt64Ty());                       // cap
    fields.push_back(llvm::PointerType::get(_context, 0));         // data
    return llvm::StructType::create(_context, fields, kName);
}

// ==================== Array<T> 句柄辅助（Phase 1b） ====================
// Array 实例 layout：{ ptr handle }（由 getLLVMType 返回 8 字节单字段 struct）
// Block layout：{ u32 strong @0, u32 weak @4, i64 len @8, i64 cap @16, ptr data @24 }

// 从 Array<T> 实例（栈上 alloca）加载句柄
// arrayStructPtr 指向 { ptr handle }，handle 字段在 offset 0，直接 load 即可
llvm::Value* Compiler::loadArrayHandle(llvm::Value* arrayStructPtr, const string& name) {
    auto ptrTy = llvm::PointerType::get(_context, 0);
    return _builder.CreateLoad(ptrTy, arrayStructPtr, name);
}

// 把句柄写回 Array<T> 实例
void Compiler::storeArrayHandle(llvm::Value* arrayStructPtr, llvm::Value* handle) {
    _builder.CreateStore(handle, arrayStructPtr);
}

// Block.len 字段指针（offset 8）
llvm::Value* Compiler::arrayBlockLenPtr(llvm::Value* handle) {
    return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "block.len_ptr");
}

// Block.cap 字段指针（offset 16）
llvm::Value* Compiler::arrayBlockCapPtr(llvm::Value* handle) {
    return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(16)}, "block.cap_ptr");
}

// Block.data 字段指针（offset 24，存放数据缓冲首地址）
llvm::Value* Compiler::arrayBlockDataFieldPtr(llvm::Value* handle) {
    return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(24)}, "block.data_field");
}

// 分配 Array<T> 的 Block
// initCap > 0 时同时分配数据缓冲；调用方负责把元素写入 block.data
llvm::Value* Compiler::allocArrayBlock(llvm::Type* elemLLVMType, llvm::Value* initCap, llvm::Value* initLen) {
    auto elemSize = _module->getDataLayout().getTypeAllocSize(elemLLVMType);
    auto allocFn = runtime::getArrayAllocFn(_module, _builder);
    return _builder.CreateCall(allocFn, {_builder.getInt64(elemSize), initCap, initLen}, "array.block");
}

// ==================== 类型映射 ====================

// 将 TypeInfo 转换为 LLVM 类型
// 处理基本类型、数组、指针、引用、结构体、泛型实例等
llvm::Type* Compiler::getLLVMType(const TypeInfo& rawType) {
    // 先应用类型替换
    auto type = applySubst(rawType);
    DEBUG_LOG_VAL("  getLLVMType", type.name << " (kind=" << static_cast<int>(type.kind) << ")");
    
    // 空类型返回 void
    if (type.empty()) {
        DEBUG_LOG("    -> Void type");
        return _builder.getVoidTy();
    }

    // 固定大小数组 [N]T
    if (type.isArray()) {
        if (type.elementType) {
            auto elementLLVMType = getLLVMType(*type.elementType);
            auto result = llvm::ArrayType::get(elementLLVMType, type.arraySize);
            DEBUG_LOG_VAL("    -> ArrayType", type.arraySize << " x " << type.elementType->name);
            return result;
        }
    }

    // 引用类型 (实现为指针)
    if (type.isRef()) {
        auto elemType = type.refElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> RefType (pointer)", "Ref<" << elemType->name << ">");
            return llvm::PointerType::get(_context, 0);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // 指针类型
    if (type.isPtr()) {
        DEBUG_LOG("    -> PtrType (void*)");
        return llvm::PointerType::get(_context, 0);
    }

    // Box<T> 类型 (智能指针，Phase 1a 新布局)
    // 结构: { ptr handle }
    // handle 指向 Block = { u32 strong, u32 weak, payload: T }；payload 始于偏移 8
    if (type.isBox()) {
        auto elemType = type.boxElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> BoxType (struct)", "Box<" << elemType->name << ">");
            vector<llvm::Type*> boxFields;
            boxFields.push_back(llvm::PointerType::get(_context, 0));  // handle: Block*
            return llvm::StructType::get(_context, boxFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Weak<T> 类型 (Phase 1d.1 弱引用)
    // 结构: { ptr handle }，与 Box<T> 同形；handle 指向同一 Block；只维护 block 存活
    if (type.isWeak()) {
        auto elemType = type.weakElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> WeakType (struct)", "Weak<" << elemType->name << ">");
            vector<llvm::Type*> weakFields;
            weakFields.push_back(llvm::PointerType::get(_context, 0));  // handle: Block*
            return llvm::StructType::get(_context, weakFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Array<T> 类型 (动态数组，Phase 1b 新布局)
    // 结构: { ptr handle }；handle 指向 Block = { u32 strong, u32 weak, i64 len, i64 cap, *T data }
    // handle == null 表示空数组（无分配）；data 间接指针，realloc 只换 data 不动 block
    if (type.isArrayGeneric()) {
        auto elemType = type.arrayGenericElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> ArrayGeneric (struct)", "Array<" << elemType->name << ">");
            vector<llvm::Type*> arrayFields;
            arrayFields.push_back(llvm::PointerType::get(_context, 0));  // handle: Block*
            return llvm::StructType::get(_context, arrayFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // 泛型类型实例 (如 Box<i32>)
    if (type.isGeneric()) {
        auto baseDecl = _file->getStructDecl(type.name);
        p<FileNode> owner = _file;
        if (!baseDecl && _yux && _yux->sdkFile()) {
            baseDecl = _yux->sdkFile()->getStructDecl(type.name);
            if (baseDecl) owner = _yux->sdkFile();
        }
        if (baseDecl && baseDecl->isGeneric()) {
            // 确保实例存在
            string mangled = ensureStructInstance(baseDecl, type.genericArgs, owner);
            return _structTypes[mangled];
        }
        // 从缓存查找
        string mangledKey = type.getGenericMangleName();
        auto it = _structTypes.find(mangledKey);
        if (it != _structTypes.end()) {
            DEBUG_LOG_VAL("    -> Generic struct (cached)", mangledKey);
            return it->second;
        }
        DEBUG_LOG_VAL("    -> Generic (fallback pointer)", type.getFullName());
        return llvm::PointerType::get(_context, 0);
    }

    // 基本类型 (从类型映射表查找)
    auto basicIt = _typeMap.find(type.name);
    if (basicIt != _typeMap.end()) {
        DEBUG_LOG_VAL("    -> Basic type", type.name);
        return basicIt->second;
    }

    // 结构体类型 (从缓存查找)
    auto it = _structTypes.find(type.name);
    if (it != _structTypes.end()) {
        DEBUG_LOG_VAL("    -> Struct (cached)", type.name);
        return it->second;
    }

    // 尝试查找并创建结构体类型
    auto structDecl = _file->getStructDecl(type.name);
    p<FileNode> sourceFile = _file;
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(type.name);
        sourceFile = _yux->sdkFile();
    }
    if (!structDecl) {
        for (auto* imp : _file->wildcardImports()) {
            structDecl = imp->getStructDecl(type.name);
            if (structDecl) {
                sourceFile = imp;
                break;
            }
        }
    }
    
    if (structDecl) {
        DEBUG_LOG_VAL("    -> Struct (creating on demand)", type.name);
        auto structType = getOrCreateStructType(structDecl, sourceFile);
        if (structType) {
            return structType;
        }
    }

    DEBUG_LOG_VAL("    -> Unknown type (null)", type.name);
    return nullptr;
}

// ==================== 结构体类型管理 ====================

// 获取或创建结构体类型
llvm::StructType* Compiler::getOrCreateStructType(p<StructDeclNode> structDecl, p<FileNode> sourceFile) {
    string name = structDecl->name().getText();

    // 跳过内置类型
    if (isBuiltinType(name)) {
        DEBUG_LOG_VAL("Skipping builtin type struct declaration", name);
        return nullptr;
    }

    auto file = sourceFile ? sourceFile : _file;
    string mangledName = Mangler::structType(file->moduleName(), name);

    // 检查缓存
    auto it = _structTypes.find(name);
    if (it != _structTypes.end()) {
        return it->second;
    }

    // 计算字段类型
    vector<llvm::Type*> fieldTypes;
    for (auto field : structDecl->fields()) {
        fieldTypes.push_back(getLLVMType(field->getType()));
    }

    // 创建结构体类型
    auto structType = llvm::StructType::create(_context, fieldTypes, mangledName);
    _structTypes[name] = structType;

    DEBUG_LOG_VAL("Created struct type", mangledName);
    return structType;
}

// ==================== 函数类型生成 ====================

// 获取函数的 LLVM 类型
llvm::FunctionType* Compiler::getLLVMFunctionType(p<FnHeaderNode> header) {
    DEBUG_LOG_VAL("  getLLVMFunctionType", header->name().getText());

    vector<llvm::Type*> paramTypes;
    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        // 指针和引用类型作为指针传递
        if (paramType.isPtr() || paramType.isRef()) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name << " (pointer type)");
        } else {
            // 结构体类型通过指针传递 (避免复制)
            auto structDecl = _file->getStructDecl(paramType.name);
            if (structDecl && !isBuiltinType(paramType.name)) {
                paramTypes.push_back(llvm::PointerType::get(_context, 0));
                DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name << " (struct ptr)");
            } else {
                paramTypes.push_back(getLLVMType(paramType));
                DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name);
            }
        }
    }
    // 处理返回类型
    auto retType = header->retType();
    TypeInfo retTypeInfo = retType ? retType->getType() : TypeInfo();
    auto llvmRetType = getLLVMType(retTypeInfo);
    DEBUG_LOG_VAL("    return type", (retTypeInfo.empty() ? "void" : retTypeInfo.name));
    return llvm::FunctionType::get(llvmRetType, paramTypes, false);
}
