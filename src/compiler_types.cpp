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
#include "node/alias_node.h"
#include "node/draft_node.h"
#include "node/struct_node.h"
#include "node/enum_node.h"
#include "node/fn_node.h"
#include <llvm/IR/DerivedTypes.h>
#include <set>

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
    throw YuxError(e.getLineNumber(), e.getColumn(), ErrorCode::E3099, what, ctx);
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
// 用于泛型实例化过程中的类型参数替换；最后再走透明别名解析，使所有
// 后续 LLVM 类型查找 / 结构体查找看到的都是规范化后的目标类型
TypeInfo Compiler::applySubst(const TypeInfo& t) const {
    TypeInfo result = t;
    if (!_substStack.empty()) {
        auto& frame = _substStack.back();
        // 使用 TypeInfo::substitute 进行类型参数替换
        result = result.substitute(frame.subst);
        // 特殊处理: 将泛型原名替换为实例名
        // 例如: Box -> Box$i32
        if (result.kind == TypeKind::Normal && !frame.baseStructName.empty()
            && result.name == frame.baseStructName) {
            result.name = frame.effStructName;
        }
    }
    // 顶层透明类型别名替换：alias 名透明等价于目标类型
    return resolveAlias(result);
}

// ==================== 别名解析 ====================

namespace {
// 内部递归实现：visited 用于环检测
TypeInfo resolveAliasImpl(const TypeInfo& t, FileNode* file, std::set<std::string>& visited) {
    if (!file) return t;
    if (t.kind == TypeKind::Normal) {
        auto* alias = file->getAliasDecl(t.name);
        if (!alias) return t;
        // 别名是泛型而引用位置不带类型实参 → 不替换（让后续 arity 检查报错）
        if (alias->isGeneric()) return t;
        if (visited.count(t.name)) {
            throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
        }
        visited.insert(t.name);
        if (!alias->target()) return t;
        TypeInfo target = alias->target()->getType();
        return resolveAliasImpl(target, file, visited);
    }
    if (t.kind == TypeKind::Generic) {
        // 泛型别名实例化：Pair<T> = (T, T) 遇 Pair<i32> → (i32, i32)
        auto* alias = file->getAliasDecl(t.name);
        if (alias && alias->isGeneric() && alias->typeParams().size() == t.genericArgs.size() && alias->target()) {
            if (visited.count(t.name)) {
                throw YuxError(static_cast<int>(alias->name().getLine()), ErrorCode::E2016, t.name);
            }
            visited.insert(t.name);
            std::map<std::string, TypeInfo> subst;
            for (size_t i = 0; i < alias->typeParams().size(); ++i) {
                subst[alias->typeParams()[i]] = t.genericArgs[i] ? *t.genericArgs[i] : TypeInfo();
            }
            TypeInfo inst = alias->target()->getType().substitute(subst);
            return resolveAliasImpl(inst, file, visited);
        }
        // 普通泛型：递归解析每个实参中的别名
        vector<sp<TypeInfo>> newArgs;
        newArgs.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newArgs.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, file, sub)));
            } else {
                newArgs.push_back(nullptr);
            }
        }
        return TypeInfo(t.name, std::move(newArgs));
    }
    if (t.kind == TypeKind::Array && t.elementType) {
        std::set<std::string> sub = visited;
        TypeInfo inner = resolveAliasImpl(*t.elementType, file, sub);
        return TypeInfo(std::make_shared<TypeInfo>(std::move(inner)), t.arraySize);
    }
    if (t.kind == TypeKind::Tuple) {
        vector<sp<TypeInfo>> newElems;
        newElems.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newElems.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, file, sub)));
            } else {
                newElems.push_back(nullptr);
            }
        }
        return TypeInfo(TupleTag{}, std::move(newElems));
    }
    return t;
}
} // namespace

TypeInfo Compiler::resolveAlias(const TypeInfo& t) const {
    std::set<std::string> visited;
    return resolveAliasImpl(t, _file ? _file : nullptr, visited);
}

// 编译入口处的别名一次性校验
// 1. 名称冲突：alias 名 vs 已存在的 struct / draft / 其他 alias
// 2. 环检测：每个别名 target 走一次 resolveAlias，触发遇环抛 E2016
void Compiler::validateAliases() {
    if (!_file) return;
    auto& aliases = _file->getAliasDecls();

    // 先做名称冲突检查（先于解析）
    // 注意：a->name() 返回 Token 值类型，绑定 .getText() 的引用会悬空，需复制为 string。
    for (auto& a : aliases) {
        string name = a->name().getText();
        // 与本文件 struct 同名
        if (auto* s = _file->getStructDecl(name)) {
            (void)s;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017,
                           name, string("struct"), name);
        }
        // 与本文件 draft 同名
        if (auto* d = _file->getDraftDecl(name)) {
            (void)d;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017,
                           name, string("draft"), name);
        }
        // 重复 alias
        size_t cnt = 0;
        for (auto& b : aliases) {
            if (b->name().getText() == name) ++cnt;
        }
        if (cnt > 1) {
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017,
                           name, string("type alias"), name);
        }
    }

    // 环检测：以每个别名为起点尝试解析
    for (auto& a : aliases) {
        if (!a->target()) continue;
        std::set<std::string> visited;
        visited.insert(a->name().getText());
        // 触发递归；若闭合则抛 E2016
        (void)resolveAliasImpl(a->target()->getType(), _file, visited);
    }

    // 校验通过后，对函数符号表的 params / retType 做一次性透明别名解析，
    // 避免后续 lookupFnSymbolWithParams 因 alias 名 vs 目标名的字面差异而错过重载
    auto resolver = [this](const TypeInfo& t) { return resolveAlias(t); };
    _file->normalizeFnSymbolTypes(resolver);
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
        throw YuxError(sourceLine, ErrorCode::E6011,
            baseName, baseDecl->typeParams().size(), args.size())
            .withHint(std::format("实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型",
                baseName,
                std::string(baseDecl->typeParams().size() == 1 ? "T" : "T1, T2, ..."),
                baseDecl->typeParams().size()));
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
                    static_cast<int>(field->name().getLine()), ErrorCode::E3098,
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

    // 元组类型 (T1, T2, ...) → 匿名 llvm::StructType（按结构等价）
    // Phase 3：透明 layout，不带 RC，元素按声明顺序排布
    if (type.isTuple()) {
        vector<llvm::Type*> fieldTypes;
        fieldTypes.reserve(type.tupleElements().size());
        for (auto& e : type.tupleElements()) {
            if (!e) return nullptr;
            auto fty = getLLVMType(*e);
            if (!fty) return nullptr;
            fieldTypes.push_back(fty);
        }
        DEBUG_LOG_VAL("    -> TupleType (anon struct)", type.name);
        return llvm::StructType::get(_context, fieldTypes);
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

    // 枚举类型：layout = { i32 tag } 或 { i32 tag, [N x i8] payload }
    // tag 按声明顺序从 0 起编号；payload 缓冲取所有 variant 的 tuple-payload 中最大字节数
    // 全部零参 variant 时省略 payload 字段（N==0）。详见 docs/spec/draft/DRAFT-枚举.md §6
    {
        p<FileNode> enumOwner = nullptr;
        auto enumDecl = lookupEnumDecl(type.name, enumOwner);
        if (enumDecl) {
            string cacheKey = "$enum$" + type.name;
            auto cit = _structTypes.find(cacheKey);
            if (cit != _structTypes.end()) {
                DEBUG_LOG_VAL("    -> Enum (cached)", type.name);
                return cit->second;
            }
            // 计算 max payload 字节数
            u64 maxPayload = 0;
            for (auto v : enumDecl->variants()) {
                if (!v->hasPayload()) continue;
                vector<llvm::Type*> elemTys;
                elemTys.reserve(v->payloadTypes().size());
                for (auto t : v->payloadTypes()) {
                    auto ll = getLLVMType(t->getType());
                    if (!ll) {
                        DEBUG_LOG_VAL("    -> Enum payload type unresolved", t->getType().name);
                        return nullptr;
                    }
                    elemTys.push_back(ll);
                }
                auto payloadStruct = llvm::StructType::get(_context, elemTys);
                auto sz = _module->getDataLayout().getTypeAllocSize(payloadStruct);
                if (sz.getFixedValue() > maxPayload) maxPayload = sz.getFixedValue();
            }
            vector<llvm::Type*> fields;
            fields.push_back(_builder.getInt32Ty());
            if (maxPayload > 0) {
                fields.push_back(llvm::ArrayType::get(_builder.getInt8Ty(), maxPayload));
            }
            string mangled = "Enum$" + (enumOwner ? enumOwner->moduleName() : string("")) + "$" + type.name;
            auto enumType = llvm::StructType::create(_context, fields, mangled);
            _structTypes[cacheKey] = enumType;
            DEBUG_LOG_VAL("    -> Enum (created)", mangled << " payload=" << maxPayload);
            return enumType;
        }
    }

    DEBUG_LOG_VAL("    -> Unknown type (null)", type.name);
    return nullptr;
}

// 查找 enum 声明：本文件 → SDK → wildcard imports
// outOwner 接收所属 FileNode，用于 mangle 名带模块前缀
p<EnumDeclNode> Compiler::lookupEnumDecl(const string& name, p<FileNode>& outOwner) {
    if (!_file) {
        outOwner = nullptr;
        return nullptr;
    }
    if (auto* d = _file->getEnumDecl(name)) {
        outOwner = _file;
        return d;
    }
    if (_yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
        if (auto* d = _yux->sdkFile()->getEnumDecl(name)) {
            outOwner = _yux->sdkFile();
            return d;
        }
    }
    for (auto* imp : _file->wildcardImports()) {
        if (auto* d = imp->getEnumDecl(name)) {
            outOwner = imp;
            return d;
        }
    }
    outOwner = nullptr;
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
        } else if (structParamUsesPointer(paramType.name)) {
            // Phase 3c.1: 非平凡结构体仍按指针传递；平凡结构体走 by-value
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name << " (struct ptr)");
        } else {
            paramTypes.push_back(getLLVMType(paramType));
            DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name);
        }
    }
    // 处理返回类型
    auto retType = header->retType();
    TypeInfo retTypeInfo = retType ? retType->getType() : TypeInfo();
    auto llvmRetType = getLLVMType(retTypeInfo);
    DEBUG_LOG_VAL("    return type", (retTypeInfo.empty() ? "void" : retTypeInfo.name));
    return llvm::FunctionType::get(llvmRetType, paramTypes, false);
}
