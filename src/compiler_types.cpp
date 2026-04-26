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
    // Array<T> 特殊处理: 使用标准布局 { ptr, i64, i64 }
    if (baseName == "Array") {
        fieldTypes.clear();
        fieldTypes.push_back(llvm::PointerType::get(_context, 0));
        fieldTypes.push_back(_builder.getInt64Ty());
        fieldTypes.push_back(_builder.getInt64Ty());
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

    // Box<T> 类型 (智能指针)
    // 结构: { ptr data, ptr ref_count }
    if (type.isBox()) {
        auto elemType = type.boxElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> BoxType (struct)", "Box<" << elemType->name << ">");
            vector<llvm::Type*> boxFields;
            boxFields.push_back(llvm::PointerType::get(_context, 0));  // data pointer
            boxFields.push_back(llvm::PointerType::get(_context, 0));  // ref_count pointer
            return llvm::StructType::get(_context, boxFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Array<T> 类型 (动态数组)
    // 结构: { ptr data, i64 len, i64 cap }
    if (type.isArrayGeneric()) {
        auto elemType = type.arrayGenericElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> ArrayGeneric (struct)", "Array<" << elemType->name << ">");
            vector<llvm::Type*> arrayFields;
            arrayFields.push_back(llvm::PointerType::get(_context, 0));  // data pointer
            arrayFields.push_back(_builder.getInt64Ty());                 // length
            arrayFields.push_back(_builder.getInt64Ty());                 // capacity
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
