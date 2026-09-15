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

#include "ast/mangler.h"
#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/struct_node.h"
#include "compiler.h"
#include "sema/name_resolver.h"
#include <array>
#include <llvm/IR/DerivedTypes.h>
#include <set>

// ==================== 错误报告辅助 ====================

// 重新抛出异常并附加实例化上下文
// 用于在泛型实例化过程中提供更详细的错误信息
void Compiler::rethrowWithInstantiationContext(const RiuError& e) const {
    string ctx = formatInstantiationContext();
    string what = e.what();
    // 避免重复添加上下文
    if (ctx.empty() || what.find("instantiated as '") != string::npos) {
        throw e;
    }
    throw RiuError(e.getLineNumber(), e.getColumn(), ErrorCode::E3099, what, ctx);
}

// 格式化泛型实例化上下文信息
// 返回类似 "instantiated as 'Rc<i32>' at module:line" 的字符串
string Compiler::formatInstantiationContext() const {
    return _substStack.formatInstantiationContext();
}

// ==================== 类型替换 ====================

TypeInfo Compiler::bindStructSelfType(const TypeInfo& t, const string& baseName, const string& effName) const {
    if (!generic::bindsStructSelf(t, baseName, effName)) return t;
    return typeInfoForNamedStruct(effName);
}

// 应用当前类型替换；最后再走透明别名解析，使后续 LLVM / 结构体查找看到规范化类型。
TypeInfo Compiler::applySubst(const TypeInfo& t) const {
    auto named = [](const void* ctx, const string& name) -> TypeInfo {
        return static_cast<const Compiler*>(ctx)->typeInfoForNamedStruct(name);
    };
    return generic::applySubst(t, _substStack, _currentStructName, _file, _riu ? _riu->sdkFile() : nullptr, named,
                               this);
}

// 从 target type 递归推断灵活整数类型（含 tuple/泛型别名展开）
// 当 target 解析为 tuple 且 expr 为 tuple 字面量时，逐元素递归推断；
// 否则委托给 AST 层的 tryInferIntType。
void Compiler::inferFlexibleInts(ExprNode* expr, const TypeInfo& target) {
    // 先展开别名（Triple<i64> → (i64,i64,i64)）
    TypeInfo resolved = applySubst(target);
    // tuple 目标 + tuple 字面量 → 逐元素递归
    if (resolved.isTuple()) {
        if (auto tupleExpr = dynamic_cast<ExprTupleNode*>(expr)) {
            auto& targetElems = resolved.tupleElements();
            auto& srcElems = tupleExpr->elements();
            if (targetElems.size() == srcElems.size()) {
                for (size_t i = 0; i < srcElems.size(); ++i) {
                    if (targetElems[i]) {
                        inferFlexibleInts(srcElems[i], *targetElems[i]);
                    }
                }
            }
        }
        return;
    }
    // 标量整数目标 → 委托 AST 层
    if (isIntTypeName(resolved.name) && isFlexibleIntExpr(expr)) {
        tryInferIntType(expr, resolved);
    }
}

// ==================== 别名解析 ====================

TypeInfo Compiler::resolveAlias(const TypeInfo& t) const {
    return sema::resolveAlias(t, _file, _riu ? _riu->sdkFile() : nullptr);
}

namespace {
bool skipMangleOwnerFill(const TypeInfo& t) {
    if (t.name.empty()) return true;
    if (isBuiltinType(t.name)) return true;
    if (t.name == "Ptr" || t.name == "Self" || t.name == "Function") return true;
    if (kindForBuiltinWrapper(t.name) != TypeKind::Generic) return true;
    if (t.kind == TypeKind::Tuple || t.kind == TypeKind::Fn || t.kind == TypeKind::Array) return true;
    return false;
}
} // namespace

FileNode* Compiler::fileForMangleModule(const string& module) const {
    if (_riu && !module.empty()) {
        if (auto f = _riu->module(module)) return f;
        if (auto sdk = _riu->sdkFile()) {
            if (auto f = sdk->relatedFile(module)) return f;
        }
    }
    if (_file) {
        if (!module.empty() && _file->moduleName() == module) return _file;
        if (auto f = _file->relatedFile(module)) return f;
    }
    return _file;
}

TypeInfo Compiler::withMangleOwners(const TypeInfo& t, FileNode* fromFile) const {
    FileNode* search = fromFile ? fromFile : _file;
    TypeInfo r = sema::resolveAlias(t, search, _riu ? _riu->sdkFile() : nullptr);
    if (r.elementType) {
        r.elementType = make_shared<TypeInfo>(withMangleOwners(*r.elementType, fromFile));
    }
    if (!r.genericArgs.empty()) {
        vector<sp<TypeInfo>> args;
        args.reserve(r.genericArgs.size());
        for (auto& a : r.genericArgs) {
            args.push_back(make_shared<TypeInfo>(a ? withMangleOwners(*a, fromFile) : TypeInfo()));
        }
        r.genericArgs = std::move(args);
    }
    if (r.ownerModule.empty() && !skipMangleOwnerFill(r)) {
        FileNode* search = fromFile ? fromFile : _file;
        sema::NameResolver nr(search, _riu ? _riu->sdkFile() : nullptr);
        if (auto* d = nr.lookupStruct(r, /*includeBuiltin=*/false, nullptr)) {
            if (auto* ef = d->enclosingFile()) r.ownerModule = ef->moduleName();
        } else if (auto* e = nr.lookupEnum(r, nullptr)) {
            if (auto* ef = e->enclosingFile()) r.ownerModule = ef->moduleName();
        }
    }
    return r;
}

vector<TypeInfo> Compiler::withMangleOwners(const vector<TypeInfo>& ts, FileNode* fromFile) const {
    vector<TypeInfo> out;
    out.reserve(ts.size());
    for (const auto& t : ts)
        out.push_back(withMangleOwners(t, fromFile));
    return out;
}

string Compiler::mangleFallibleErr(const string& err, FileNode* fromFile) const {
    if (err.empty()) return err;
    return withMangleOwners(TypeInfo(err), fromFile).getMangleName();
}

string Compiler::mangleFunction(const string& module, const string& name, const vector<TypeInfo>& params,
                                bool isPrivate, const TypeInfo& retType, const string& fallibleErrType) const {
    FileNode* from = fileForMangleModule(module);
    return Mangler::function(module, name, withMangleOwners(params, from), isPrivate, withMangleOwners(retType, from),
                             mangleFallibleErr(fallibleErrType, from));
}

string Compiler::mangleMethod(const string& module, const string& structName, const string& methodName,
                              const vector<TypeInfo>& params, bool isPrivate, const TypeInfo& retType,
                              const string& fallibleErrType) const {
    FileNode* from = fileForMangleModule(module);
    return Mangler::method(module, structName, methodName, withMangleOwners(params, from), isPrivate,
                           withMangleOwners(retType, from), mangleFallibleErr(fallibleErrType, from));
}

string Compiler::mangleStaticMethod(const string& module, const string& structName, const string& methodName,
                                    const vector<TypeInfo>& params, const TypeInfo& retType,
                                    const string& fallibleErrType) const {
    FileNode* from = fileForMangleModule(module);
    return Mangler::staticMethod(module, structName, methodName, withMangleOwners(params, from),
                                 withMangleOwners(retType, from), mangleFallibleErr(fallibleErrType, from));
}

// ==================== 泛型结构体实例化 ====================

// 确保泛型结构体实例存在
// 返回 mangle 后的实例名 (如 "Rc<i32>")
string Compiler::ensureStructInstance(StructDeclNode* baseDecl, const vector<sp<TypeInfo>>& args, FileNode* ownerFile,
                                      int sourceLine) {
    string baseName = baseDecl->name().getText();
    // 实例 key / LLVM 类型名：定义模块全限定 + `<>`（与 riu 类型写法同形）
    FileNode* instOwner = ownerFile ? ownerFile : _file;
    string mangledName;
    if (instOwner && !instOwner->moduleName().empty()) {
        mangledName = instOwner->moduleName() + ".";
    }
    mangledName += baseName + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) mangledName += ',';
        mangledName += args[i] ? withMangleOwners(*args[i], instOwner).getMangleName() : string("?");
    }
    mangledName += '>';

    if (_structInstances.contains(mangledName)) return mangledName;

    // 验证类型参数数量
    if (args.size() != baseDecl->typeParams().size()) {
        // 调用方未提供位置（getLLVMType 路径常见）时，退回到 struct 声明行，避免 assert(line>0) 触发 abort
        int errLine = sourceLine > 0 ? sourceLine : static_cast<int>(baseDecl->name().getLine());
        if (errLine <= 0) errLine = 1;
        // E6011 由 SemaPass 声明处 / turbofish 先抛；此处防 IR 实例化 arity 不一致。
        throwSemaGap(errLine);
    }

    vector<TypeInfo> instArgs;
    instArgs.reserve(args.size());
    for (auto& a : args)
        instArgs.push_back(a ? *a : TypeInfo());
    // 先建记录、后入表：字段 getLLVMType 可能递归 ensure 其它实例；同名须等 LLVM 类型建完。
    auto inst = generic::makeStructInstance(baseDecl, std::move(instArgs), ownerFile, _file,
                                            _riu ? _riu->sdkFile() : nullptr, mangledName, sourceLine);

    // 建立类型参数替换映射
    map<string, TypeInfo> subst = inst.substMap();

    // 压入替换栈帧
    _substStack.push_back(SubstFrame{.subst = subst,
                                     .baseStructName = baseName,
                                     .effStructName = mangledName,
                                     .sourceFile = inst.sourceFile,
                                     .sourceLine = inst.sourceLine});

    // 计算实例化后的字段类型
    vector<llvm::Type*> fieldTypes;
    try {
        for (auto field : baseDecl->fields()) {
            auto fieldType = field->getType();
            auto llvmTy = getLLVMType(fieldType);
            if (!llvmTy) {
                throwSemaGap(static_cast<size_t>(field->name().getLine()));
            }
            fieldTypes.push_back(llvmTy);
        }
    } catch (const RiuError& e) {
        _substStack.pop_back();
        rethrowWithInstantiationContext(e);
    }

    // 创建 LLVM 结构体类型（mangledName 已是定义模块全限定）
    auto structType = llvm::StructType::create(_context, fieldTypes, mangledName);
    _structTypes[mangledName] = structType;
    DEBUG_LOG_VAL("Created generic struct instance", mangledName);

    _substStack.pop_back();

    _structInstances.insert(std::move(inst));
    return mangledName;
}

// ==================== usize 辅助 ====================

llvm::Type* Compiler::getSizeType() const {
    return _typeMap.at("usize");
}

// B-3: Array<T> 新布局辅助 —— 去 Block，字段内联
// Array 实例 layout：{ ptr _data @0, usize _len @8, usize _cap @16 }

// 获取 Array struct 的 LLVM 类型 { ptr, usize, usize }，用于 GEP
llvm::StructType* Compiler::getArrayStructTypeForGEP() const {
    auto* sizeTy = getSizeType();
    std::array<llvm::Type*, 3> fields = {llvm::PointerType::get(_context, 0), sizeTy, sizeTy};
    return llvm::StructType::get(_context, llvm::ArrayRef(fields.data(), fields.size()));
}

// _data 字段指针（field 0，ptr*）
llvm::Value* Compiler::arrayDataFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP();
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, zero}, name + ".data_field");
}

// _len 字段指针（field 1，usize*）
llvm::Value* Compiler::arrayLenFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP();
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, one}, name + ".len_field");
}

// _cap 字段指针（field 2，usize*）
llvm::Value* Compiler::arrayCapFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP();
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto two = llvm::ConstantInt::get(_builder.getInt32Ty(), 2);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, two}, name + ".cap_field");
}

// ==================== 类型映射 ====================

// 从 struct 名还原完整 TypeInfo。
// `_structInstances` 的 key 是 mangle（`Foo<i32>` / `Array<i32>`）；命中时带上 args
// 走 `TypeInfo(base, args)` 唯一名字分发，避免 `TypeInfo("Array")` 变成 Normal。
TypeInfo Compiler::typeInfoForNamedStruct(const string& name) const {
    if (const auto* inst = _structInstances.find(name); inst && inst->baseDecl) {
        return inst->typeInfo();
    }
    TypeInfo t(name);
    FileNode* owner = nullptr;
    if (names().lookupStruct(name, /*includeBuiltin=*/false, &owner) && owner) {
        t.ownerModule = owner->moduleName();
    }
    return t;
}

// 将 TypeInfo 转换为 LLVM 类型
// 处理基本类型、数组、指针、引用、结构体、泛型实例等
llvm::Type* Compiler::getLLVMType(const TypeInfo& rawType) {
    // fallible 签名位 T ! E 的 ABI 仍按成功类型 T；剥后缀再映射 LLVM
    auto type = applySubst(rawType.withoutFallible());
    // 补声明模块：`IoErr` 与 `riu.io.IoErr` 必须是同一 LLVM 类型，否则
    // InsertValue 会因 owner 填/未填拆成两种 layout 而 abort。
    type = withMangleOwners(type, _file);
    DEBUG_LOG_VAL("  getLLVMType", type.identityKey() << " (kind=" << static_cast<int>(type.kind) << ")");

    // E4025 / E1132：别名展开与泛型 subst 之后拦截 Rc/Weak 内嵌 Heap/Dyn
    // （含 Rc<Rc<Heap<T>>>）。typed release 依赖此门，禁止回退 generic _box_release。
    validateRcContainerBans(type, 1, 0);

    // 空类型返回 void
    if (type.empty()) {
        DEBUG_LOG("    -> Void type");
        return _builder.getVoidTy();
    }

    // 内置泛型包装类型若被误写成 Normal 形式（即不带 `<T>`，kind 未正确分发）：
    // 这些类型没有 StructDecl 兜底，落到下方各分支只会得到 `Unknown type (null)`，
    // 返回 null 会让调用方在后续 SEH/段错误时崩溃。这里早抛 E6011 以给出诊断。
    // 注：正常情况下构造函数 kindForBuiltinWrapper() 已自动分发到正确 kind，
    // 此检查仅防御单参 TypeInfo(name) 误用或未来回归。
    if (type.isNormal()) {
        static constexpr std::array<std::pair<const char*, size_t>, 9> kBuiltinGenerics{{
            {"Rc", 1},
            {"Weak", 1},
            {"Array", 1},
            {"Nullable", 1},
            {"Heap", 1},
            {"Ref", 1},
            {"Dyn", 1},
            {"Function", 1},
            {"Ptr", 0},
        }};
        for (auto [bname, arity] : kBuiltinGenerics) {
            (void)arity;
            if (type.name == bname) {
                // E6011 由 SemaPass 声明处先抛；此处防 IR 对缺 typeArgs 的内置泛型建 LLVM 类型。
                throwSemaGap(1);
            }
        }
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

    // Rc<T> 类型 (智能指针，Phase 1a 新布局)
    // 结构: { ptr handle }
    // handle 指向 Block = { u32 strong, u32 weak, payload: T }；payload 始于偏移 8
    if (type.isRc()) {
        auto elemType = type.rcElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> RcType (struct)", "Rc<" << elemType->name << ">");
            vector<llvm::Type*> rcFields;
            rcFields.push_back(llvm::PointerType::get(_context, 0)); // handle: Block*
            return llvm::StructType::get(_context, rcFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Heap<T> 类型（DRAFT-heap-types §8.3a 堆作用域句柄）
    // layout = 裸 T*：单所有权、无 RC 头、作用域绑定析构、不参与 Rc/Weak
    if (type.isHeap()) {
        DEBUG_LOG_VAL("    -> HeapType (bare ptr)",
                      "Heap<" << (type.heapElementType() ? type.heapElementType()->name : "?") << ">");
        return llvm::PointerType::get(_context, 0);
    }

    // Weak<T> 类型 (Phase 1d.1 弱引用)
    // 结构: { ptr handle }，与 Rc<T> 同形；handle 指向同一 Block；只维护 block 存活
    if (type.isWeak()) {
        auto elemType = type.weakElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> WeakType (struct)", "Weak<" << elemType->name << ">");
            vector<llvm::Type*> weakFields;
            weakFields.push_back(llvm::PointerType::get(_context, 0)); // handle: Block*
            return llvm::StructType::get(_context, weakFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Array<T> 类型 (动态数组，B-3 新布局：去 Builtin/去 Block)
    // 结构: { ptr _data, usize _len, usize _cap }，24 字节
    // 必须与 getArrayStructTypeForGEP 共用同一 intern 类型，禁止退回裸 ptr
    // （退回 ptr 后按三字段 GEP → LLVM `Invalid GetElementPtrInst indices`）。
    if (type.isArrayGeneric()) {
        auto elemType = type.arrayGenericElementType();
        if (elemType) {
            DEBUG_LOG_VAL("    -> ArrayGeneric (struct)", "Array<" << elemType->name << ">");
        }
        return getArrayStructTypeForGEP();
    }

    // Nullable<T> 类型：layout = { bool _has, T _value }
    // T? 解糖后类型（spec §3.5 / DRAFT-nullable-types §8.2）
    if (type.isNullable()) {
        auto inner = type.nullableInnerType();
        if (inner) {
            DEBUG_LOG_VAL("    -> NullableType (struct)", type.name);
            vector<llvm::Type*> fields;
            fields.push_back(_builder.getInt1Ty()); // _has: bool
            fields.push_back(getLLVMType(*inner));  // _value: T
            return llvm::StructType::get(_context, fields);
        }
        return nullptr;
    }

    // Dyn<D> / Dyn<D&> 类型 (DRAFT-dyn-draft / 拟 §12.9)
    // 结构: { ptr vtable, ptr data }，16 字节 fat pointer
    // - vtable: 指向 (具体类型 U, draft D) 静态 vtable，槽 0 = dtor，槽 1..N = D 方法按声明序
    // - data:   owned 形态指向 [RC head | 实例]；借用形态借自栈或堆
    if (type.isDyn()) {
        DEBUG_LOG_VAL("    -> DynType (fat-ptr)", type.name);
        vector<llvm::Type*> dynFields;
        dynFields.push_back(llvm::PointerType::get(_context, 0)); // vtable
        dynFields.push_back(llvm::PointerType::get(_context, 0)); // data
        return llvm::StructType::get(_context, dynFields);
    }

    // 函数类型 Function<P..., Ret> / Function<...>? → 16 字节 fat-ptr
    // layout: { ptr fn_ptr, ptr captures }；可空仍是同一布局（fn_ptr == null）
    if (type.isFn()) {
        DEBUG_LOG_VAL("    -> FnType (fat-ptr placeholder)", type.name);
        vector<llvm::Type*> fnFields;
        fnFields.push_back(llvm::PointerType::get(_context, 0)); // fn_ptr
        fnFields.push_back(llvm::PointerType::get(_context, 0)); // captures (Rc?)
        return llvm::StructType::get(_context, fnFields);
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

    // 泛型类型实例 (如 Rc<i32>)
    if (type.isGeneric()) {
        FileNode* owner = nullptr;
        auto baseDecl = names().lookupStruct(type, /*includeBuiltin=*/false, &owner);
        if (!owner && _riu && !type.ownerModule.empty()) {
            owner = _riu->module(type.ownerModule);
            if (owner) baseDecl = owner->localStructDecl(type.baseStructName(), /*includeBuiltin=*/false);
        }
        if (baseDecl && baseDecl->isGeneric()) {
            // 确保实例存在
            string mangled = ensureStructInstance(baseDecl, type.genericArgs, owner ? owner : _file);
            return _structTypes[mangled];
        }
        // 从缓存查找
        string mangledKey = type.getMangleName();
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

    // 结构体类型：身份键优先；有 owner 时不回退短名（本文件 Map 与 SDK Map 不能共用 LLVM 类型）
    if (auto it = _structTypes.find(type.identityKey()); it != _structTypes.end()) {
        DEBUG_LOG_VAL("    -> Struct (cached)", type.identityKey());
        return it->second;
    }
    if (type.ownerModule.empty()) {
        if (auto it = _structTypes.find(type.name); it != _structTypes.end()) {
            DEBUG_LOG_VAL("    -> Struct (cached short)", type.name);
            return it->second;
        }
    }

    // DRAFT-spec-reflect: Type / Field / Method / Variant — #Builtin struct,
    // LLVM layout 由编译器硬编码 (SDK 声明仅 name String 字段可见, 其余 slot 隐藏).
    if (type.name == "Field" || type.name == "Method" || type.name == "Variant" || type.name == "Type") {
        if (auto it = _structTypes.find(type.name); it != _structTypes.end()) {
            _structTypes[type.identityKey()] = it->second;
            return it->second;
        }
        auto* ptrTy = llvm::PointerType::get(_context, 0);
        auto* stringTy = [&]() -> llvm::Type* {
            auto cit = _structTypes.find("String");
            if (cit != _structTypes.end()) return cit->second;
            auto* resolved = getLLVMType(TypeInfo("String"));
            if (resolved) return resolved;
            vector<llvm::Type*> rcFields = {ptrTy};
            auto* rcTy = llvm::StructType::get(_context, rcFields);
            vector<llvm::Type*> strFields = {rcTy};
            return llvm::StructType::get(_context, strFields);
        }();
        vector<llvm::Type*> fields = {stringTy};
        auto* st = llvm::StructType::create(_context, fields, "reflect." + type.name);
        _structTypes[type.name] = st;
        _structTypes[type.identityKey()] = st;
        DEBUG_LOG_VAL("    -> Reflect struct (builtin)", type.name);
        return st;
    }

    // 尝试查找并创建结构体类型
    FileNode* sourceFile = nullptr;
    StructDeclNode* structDecl = nullptr;
    if (!type.ownerModule.empty() && _riu) {
        sourceFile = _riu->module(type.ownerModule);
        if (sourceFile) structDecl = sourceFile->localStructDecl(type.name, /*includeBuiltin=*/false);
    }
    if (!structDecl) structDecl = names().lookupStruct(type, /*includeBuiltin=*/false, &sourceFile);
    if (!sourceFile) sourceFile = _file;

    if (structDecl) {
        // 泛型 struct 但用法没带 `<T>`：避免落进 getOrCreateStructType 把未实例化的类型参数当成
        // 实类型 → 字段类型 null → llvm::StructType::create 段错误
        if (structDecl->isGeneric()) {
            // 裸名 `Slot` 出现在当前单态方法体内（`Self` / TypeSelfNode.getType）时，
            // 应对应已建好的实例 LLVM 类型，而不是按未实例化泛型建类型。
            auto instLlvm = [&](const string& key) -> llvm::Type* {
                if (key.empty()) return nullptr;
                const auto* inst = _structInstances.find(key);
                if (!inst || !inst->baseDecl) return nullptr;
                if (inst->baseDecl->name().getText() != structDecl->name().getText()) return nullptr;
                auto cit = _structTypes.find(key);
                return cit != _structTypes.end() ? cit->second : nullptr;
            };
            for (auto it = _substStack.rbegin(); it != _substStack.rend(); ++it) {
                if (auto* ty = instLlvm(it->effStructName)) return ty;
            }
            if (auto* ty = instLlvm(_currentStructName)) return ty;
            int errLine = static_cast<int>(structDecl->name().getLine());
            if (errLine <= 0) errLine = 1;
            // E6011 由 SemaPass 声明处先抛；此处防 IR 对缺 typeArgs 的泛型 struct 建 LLVM 类型。
            throwSemaGap(errLine);
        }
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
        string shortKey = "$enum$" + type.name;
        string idKey = "$enum$" + type.identityKey();
        // 有 owner 只走身份键，避免本文件 enum IoErr 与 riu.io.IoErr 共用 LLVM 类型
        if (auto cit = _structTypes.find(idKey); cit != _structTypes.end()) {
            DEBUG_LOG_VAL("    -> Enum (cached)", type.name);
            return cit->second;
        }
        if (type.ownerModule.empty()) {
            if (auto cit = _structTypes.find(shortKey); cit != _structTypes.end()) {
                DEBUG_LOG_VAL("    -> Enum (cached short)", type.name);
                return cit->second;
            }
        }
        FileNode* enumOwner = nullptr;
        EnumDeclNode* enumDecl = nullptr;
        if (!type.ownerModule.empty() && _riu) {
            enumOwner = _riu->module(type.ownerModule);
            if (enumOwner) enumDecl = enumOwner->localEnumDecl(type.name);
        }
        if (!enumDecl) enumDecl = names().lookupEnum(type, &enumOwner);
        if (enumDecl) {
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
            string prefix = enumOwner && !enumOwner->moduleName().empty() ? enumOwner->moduleName() + "." : "";
            string mangled = prefix + type.name;
            auto enumType = llvm::StructType::create(_context, fields, mangled);
            _structTypes[idKey] = enumType;
            // 声明身份键：无 owner 的首次创建也登记 `mod.Name`，后续带 owner 的查找命中同一类型
            string declIdKey = "$enum$" + mangled;
            _structTypes[declIdKey] = enumType;
            if (!_structTypes.contains(shortKey)) {
                _structTypes[shortKey] = enumType;
            }
            DEBUG_LOG_VAL("    -> Enum (created)", mangled << " payload=" << maxPayload);
            return enumType;
        }
    }

    DEBUG_LOG_VAL("    -> Unknown type (null)", type.name);
    return nullptr;
}

// ==================== 结构体类型管理 ====================

// 获取或创建结构体类型
llvm::StructType* Compiler::getOrCreateStructType(StructDeclNode* structDecl, FileNode* sourceFile) {
    string name = structDecl->name().getText();

    // 跳过内置类型
    if (isBuiltinType(name)) {
        DEBUG_LOG_VAL("Skipping builtin type struct declaration", name);
        return nullptr;
    }
    // DRAFT-spec-reflect: Field/Type/Method/Variant 的 LLVM 布局由编译器硬编码,
    // 不从 riu 声明构建, 避免 getOrCreateStructType 缓存旧布局覆盖硬编码版本.
    if (name == "Field" || name == "Type" || name == "Method" || name == "Variant") {
        DEBUG_LOG_VAL("Skipping reflect #Builtin struct (hardcoded layout)", name);
        return nullptr;
    }

    auto file = sourceFile ? sourceFile : _file;
    string mangledName = Mangler::structType(file->moduleName(), name);

    // 检查缓存（身份键；短名仅在尚未被其它同名类型占用时作未填 owner 的回退）
    auto it = _structTypes.find(mangledName);
    if (it != _structTypes.end()) {
        return it->second;
    }
    if (file->moduleName().empty()) {
        it = _structTypes.find(name);
        if (it != _structTypes.end()) {
            return it->second;
        }
    }

    // 计算字段类型
    vector<llvm::Type*> fieldTypes;
    for (auto field : structDecl->fields()) {
        fieldTypes.push_back(getLLVMType(field->getType()));
    }

    // 创建结构体类型
    auto structType = llvm::StructType::create(_context, fieldTypes, mangledName);
    _structTypes[mangledName] = structType;
    if (!_structTypes.contains(name)) {
        _structTypes[name] = structType;
    }

    DEBUG_LOG_VAL("Created struct type", mangledName);
    return structType;
}

// ==================== 函数类型生成 ====================

// 获取函数的 LLVM 类型
llvm::FunctionType* Compiler::getLLVMFunctionType(FnHeaderNode* header) {
    DEBUG_LOG_VAL("  getLLVMFunctionType", header->name().getText());

    vector<llvm::Type*> paramTypes;
    for (auto param : header->params()) {
        TypeInfo paramType = param->type() ? param->type()->getType() : TypeInfo();
        // 指针和引用类型作为指针传递
        if (paramType.isPtr() || paramType.isRef()) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
            DEBUG_LOG_VAL("    param", param->name().getText() << " : " << paramType.name << " (pointer type)");
        } else if (structParamUsesPointer(paramType)) {
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
    // DRAFT-错误.md [#10.A]：#Fallible(E) 函数返回类型包成 { i1, T_ok?, ErrEnum }
    string fallibleErr = header->resolvedFallibleErr();
    auto llvmRetType = wrapFallibleRetType(retTypeInfo, fallibleErr);
    DEBUG_LOG_VAL("    return type", (retTypeInfo.empty() ? "void" : retTypeInfo.name)
                                         << (fallibleErr.empty() ? "" : (string(" #Fallible(") + fallibleErr + ")")));
    return llvm::FunctionType::get(llvmRetType, paramTypes, false);
}

// ==================== #Fallible 返回类型包装 ====================

// 把 #Fallible(E) 函数的返回类型包成 { i1 isErr, T_ok?, ErrEnum }
// 见 CURRENT.md 决议 [#10.A]：候选 A，anonymous struct 单返回值
llvm::Type* Compiler::wrapFallibleRetType(const TypeInfo& retType, const string& errTypeName) {
    if (errTypeName.empty()) {
        // 普通函数 / 非 Fallible：保持原行为
        return retType.empty() || retType.isUnit() ? _builder.getVoidTy() : getLLVMType(retType);
    }
    return getFallibleRetStructType(retType, errTypeName);
}

llvm::Value* Compiler::wrapFallibleSuccessRet(llvm::Value* okVal, const TypeInfo& successType,
                                              const string& fallibleErr) {
    if (fallibleErr.empty()) return okVal;
    auto retStructTy = getFallibleRetStructType(successType, fallibleErr);
    TypeInfo errTy = withMangleOwners(TypeInfo(fallibleErr), _file);
    auto errLLVMTy = getLLVMType(errTy);
    llvm::Value* retStruct = llvm::UndefValue::get(retStructTy);
    retStruct = _builder.CreateInsertValue(retStruct, _builder.getInt1(false), {0});
    unsigned errFieldIdx;
    if (!successType.empty()) {
        retStruct = _builder.CreateInsertValue(retStruct, okVal, {1});
        errFieldIdx = 2;
    } else {
        errFieldIdx = 1;
    }
    retStruct = _builder.CreateInsertValue(retStruct, llvm::Constant::getNullValue(errLLVMTy), {errFieldIdx});
    return retStruct;
}

llvm::StructType* Compiler::getFallibleRetStructType(const TypeInfo& retType, const string& errTypeName) {
    // ErrEnum 必为已声明 enum（10e 静态层已校 + E7011）；走 withMangleOwners 再 getLLVMType，
    // 避免短名 `IoErr` 与全限定拆成两种 LLVM 类型（InsertValue abort）。
    TypeInfo errType = withMangleOwners(TypeInfo(errTypeName), _file);
    auto errLLVMType = getLLVMType(errType);
    vector<llvm::Type*> fields;
    fields.push_back(_builder.getInt1Ty()); // 字段 0：isErr
    if (!retType.empty()) {
        // 字段 1：T_ok（void 时省略，便于 LLVM 寄存器返回 + extractvalue 索引稳定）
        // 注意：与参数传递不同，此处不做"按指针传"折叠——返回值按值聚合到 struct 内
        fields.push_back(getLLVMType(retType));
    }
    fields.push_back(errLLVMType); // 字段 2 (或 1，T=void 时)：ErrEnum
    return llvm::StructType::get(_context, fields);
}

// ==================== 10g-7：main #Fallible 出口 wrapper ====================

// DRAFT-错误.md §6.1：main 标 #Fallible(E) 的运行期呈现：
//   - 退出码：固定 _exit(1)
//   - stderr：`error: <module>.<EnumName>::<VariantName>[(payload.to_string())]\n`
//
// v1 实现：variant 名整字符串 .rodata 嵌入；payload 走 §12.7.1 ToString 推后续
// （含 RC payload 的 variant 当前打 `(...)` 占位，错误码 / 行为不受影响）。
//
// 流程：
//   1. 取 riu_main —— 此时签名已是 { i1 isErr, EnumLLVM err }（因 main 必返 void）
//   2. 设置控制台代码页（沿用普通 startup）
//   3. call riu_main → 取 isErr → CondBr ok / err
//   4. err 分支：extract err 字段 → switch on err.tag → 各 variant BB 写 stderr + ExitProcess(1)
//   5. ok 分支：ret 0
void Compiler::emitMainStartupFallible(const string& fallibleErrName) {
    auto setConsoleOutputCP = runtime::getSetConsoleOutputCPFn(_module, _builder);
    auto setConsoleCP = runtime::getSetConsoleCPFn(_module, _builder);
    auto getStdHandle = runtime::getOrCreateWindowsAPI(_module, _builder, "GetStdHandle");
    auto writeFile = runtime::getOrCreateWindowsAPI(_module, _builder, "WriteFile");
    auto exitProcess = runtime::getOrCreateWindowsAPI(_module, _builder, "ExitProcess");

    auto riuMain = _module->getFunction("riu_main");
    if (!riuMain) {
        // 防御：理论 compileFn 已发射 riu_main
        return;
    }

    // mainStartup() i32
    auto mainFnType = llvm::FunctionType::get(_builder.getInt32Ty(), {}, false);
    auto mainStartup = llvm::Function::Create(mainFnType, llvm::Function::ExternalLinkage, "mainStartup", _module);

    auto entry = llvm::BasicBlock::Create(_context, "entry", mainStartup);
    _builder.SetInsertPoint(entry);

    // 控制台 UTF-8 代码页
    auto cpUtf8 = _builder.getInt32(65001);
    _builder.CreateCall(setConsoleOutputCP, {cpUtf8});
    _builder.CreateCall(setConsoleCP, {cpUtf8});

    // DRAFT-static-vars Phase 6: 按模块拓扑序调用 __riu_global_init.<Mod>()
    auto voidFnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
    if (_riu && !_riu->loadOrder().empty()) {
        for (auto& modName : _riu->loadOrder()) {
            string fnName = "__riu_global_init." + modName;
            auto callee = _module->getOrInsertFunction(fnName, voidFnType);
            _builder.CreateCall(callee, {});
        }
    } else {
        // 兼容旧路径（单文件模式）：遍历当前 Module 内所有 init 函数
        for (auto& func : _module->getFunctionList()) {
            auto funcName = func.getName();
            if (funcName.starts_with("__riu_global_init.")) {
                _builder.CreateCall(&func, {});
            }
        }
    }

    // 调用 riu_main 拿 { i1, EnumLLVM }
    auto callRet = _builder.CreateCall(riuMain, {}, "main.ret");
    auto isErr = _builder.CreateExtractValue(callRet, {0}, "main.isErr");

    auto errBB = llvm::BasicBlock::Create(_context, "main.err", mainStartup);
    auto okBB = llvm::BasicBlock::Create(_context, "main.ok", mainStartup);
    _builder.CreateCondBr(isErr, errBB, okBB);

    // ===== err 分支 =====
    _builder.SetInsertPoint(errBB);

    // 取 stderr 句柄（STD_ERROR_HANDLE = -12）
    auto stderrHandle = _builder.CreateCall(getStdHandle, {_builder.getInt32(-12)}, "stderr.h");

    // 取 err 字段（字段 1，因 main retType 是 void → struct = { i1, EnumLLVM }）
    auto errVal = _builder.CreateExtractValue(callRet, {1}, "main.err.val");
    // err.tag = 字段 0
    auto tag = _builder.CreateExtractValue(errVal, {0}, "main.err.tag");

    // 查 enum decl 拿 variant 列表（含模块名修饰）
    FileNode* enumOwner = nullptr;
    auto enumDecl = names().lookupEnum(fallibleErrName, &enumOwner);
    if (!enumDecl) {
        // 防御：10e 已校 #Fallible 类型存在；走 unreachable 兜底
        _builder.CreateCall(exitProcess, {_builder.getInt32(1)});
        _builder.CreateUnreachable();
        _builder.SetInsertPoint(okBB);
        _builder.CreateRet(_builder.getInt32(0));
        return;
    }

    string moduleName = enumOwner ? enumOwner->moduleName() : _file->moduleName();
    string prefix = "error: " + moduleName + "." + fallibleErrName + "::";

    auto i32Ty = _builder.getInt32Ty();
    auto ptrTy = llvm::PointerType::get(_context, 0);

    // 共享 outWritten alloca（WriteFile 第 4 个参数 lpNumberOfBytesWritten）
    // 必须在 switch 终结符之前 emit，否则会被插到 switch 之后破坏块结构
    auto outWritten = _builder.CreateAlloca(i32Ty, nullptr, "out.written");

    // 默认分支：未知 tag（理论不可达）
    auto defaultBB = llvm::BasicBlock::Create(_context, "main.err.default", mainStartup);
    auto sw = _builder.CreateSwitch(tag, defaultBB, static_cast<unsigned>(enumDecl->variants().size()));

    auto emitWrite = [&](llvm::Value* msgGlobal, uint32_t len) {
        _builder.CreateCall(writeFile, {stderrHandle, msgGlobal, _builder.getInt32(len), outWritten,
                                        llvm::ConstantPointerNull::get(ptrTy)});
    };

    for (size_t i = 0; i < enumDecl->variants().size(); ++i) {
        auto& variant = enumDecl->variants()[i];
        auto vbb = llvm::BasicBlock::Create(_context, "main.err.v" + std::to_string(i), mainStartup);
        sw->addCase(_builder.getInt32(static_cast<int>(i)), vbb);
        _builder.SetInsertPoint(vbb);

        // TODO(10g-7): payload 走 §12.7.1 ToString —— 当前含 payload variant 打 "(...)" 占位
        string msg = prefix + variant->name().getText();
        if (variant->hasPayload()) {
            msg += "(...)";
        }
        msg += '\n';

        // .rodata 全局字符串（不带 NUL；len 单独传）
        auto strConst = llvm::ConstantDataArray::getString(_context, msg, /*addNull*/ false);
        auto strGlobal =
            new llvm::GlobalVariable(*_module, strConst->getType(), /*isConstant*/ true,
                                     llvm::GlobalValue::PrivateLinkage, strConst, "main.err.msg." + std::to_string(i));
        strGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

        emitWrite(strGlobal, static_cast<uint32_t>(msg.size()));

        _builder.CreateCall(exitProcess, {_builder.getInt32(1)});
        _builder.CreateUnreachable();
    }

    // default：tag 越界（理论不可达）；写一行 "error: <prefix><invalid tag>\n" 后退
    _builder.SetInsertPoint(defaultBB);
    {
        string msg = prefix + "<invalid tag>\n";
        auto strConst = llvm::ConstantDataArray::getString(_context, msg, false);
        auto strGlobal = new llvm::GlobalVariable(*_module, strConst->getType(), true,
                                                  llvm::GlobalValue::PrivateLinkage, strConst, "main.err.msg.default");
        strGlobal->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        emitWrite(strGlobal, static_cast<uint32_t>(msg.size()));
    }
    _builder.CreateCall(exitProcess, {_builder.getInt32(1)});
    _builder.CreateUnreachable();

    // ===== ok 分支 =====
    _builder.SetInsertPoint(okBB);
    _builder.CreateRet(_builder.getInt32(0));
}
