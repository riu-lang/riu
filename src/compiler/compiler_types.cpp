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
#include <array>
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
// 返回类似 "instantiated as 'Rc$i32' at module:line" 的字符串
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
        // 例如: Rc -> Rc$i32
        if (result.kind == TypeKind::Normal && !frame.baseStructName.empty() && result.name == frame.baseStructName) {
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
        return {t.name, std::move(newArgs)};
    }
    if (t.kind == TypeKind::Array && t.elementType) {
        std::set<std::string> sub = visited;
        TypeInfo inner = resolveAliasImpl(*t.elementType, file, sub);
        return {std::make_shared<TypeInfo>(std::move(inner)), t.arraySize};
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
    // 函数类型：递归解析每个形参 / 返回类型中的别名
    if (t.kind == TypeKind::Fn) {
        vector<sp<TypeInfo>> newParams;
        newParams.reserve(t.genericArgs.size());
        for (auto& a : t.genericArgs) {
            if (a) {
                std::set<std::string> sub = visited;
                newParams.push_back(std::make_shared<TypeInfo>(resolveAliasImpl(*a, file, sub)));
            } else {
                newParams.push_back(nullptr);
            }
        }
        sp<TypeInfo> newRet = nullptr;
        if (t.elementType) {
            std::set<std::string> sub = visited;
            newRet = std::make_shared<TypeInfo>(resolveAliasImpl(*t.elementType, file, sub));
        }
        return TypeInfo(FnTag{}, std::move(newParams), newRet, t.fnNullable);
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
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("struct"), name);
        }
        // 与本文件 draft 同名
        if (auto* d = _file->getSpecDecl(name)) {
            (void)d;
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("draft"), name);
        }
        // 重复 alias
        size_t cnt = 0;
        for (auto& b : aliases) {
            if (b->name().getText() == name) ++cnt;
        }
        if (cnt > 1) {
            throw YuxError(static_cast<int>(a->name().getLine()), ErrorCode::E2017, name, string("type alias"), name);
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
// 返回 mangle 后的实例名 (如 "Rc$i32")
string Compiler::ensureStructInstance(p<StructDeclNode> baseDecl, const vector<sp<TypeInfo>>& args,
                                      p<FileNode> ownerFile, int sourceLine) {
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
        // 调用方未提供位置（getLLVMType 路径常见）时，退回到 struct 声明行，避免 assert(line>0) 触发 abort
        int errLine = sourceLine > 0 ? sourceLine : static_cast<int>(baseDecl->name().getLine());
        if (errLine <= 0) errLine = 1;
        throw YuxError(errLine, ErrorCode::E6011, baseName, baseDecl->typeParams().size(), args.size())
            .withHint(std::format("实例化时的类型实参个数需与声明匹配；改写为 `{}<{}>` 形式补齐 {} 个类型", baseName,
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
    for (auto& a : args)
        inst.args.push_back(a ? *a : TypeInfo());
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
                auto substituted = applySubst(fieldType);
                throw YuxError(static_cast<int>(field->name().getLine()), ErrorCode::E3098, substituted.getFullName(),
                               field->name().getText(), baseName);
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

// B-3: Array<T> 新布局辅助 —— 去 Block，字段内联
// Array 实例 layout：{ ptr _data @0, u64 _len @8, u64 _cap @16 }

// 获取 Array struct 的 LLVM 类型 { ptr, i64, i64 }，用于 GEP
static llvm::StructType* getArrayStructTypeForGEP(llvm::LLVMContext& ctx) {
    std::array<llvm::Type*, 3> fields = {llvm::PointerType::get(ctx, 0),
                                         llvm::Type::getInt64Ty(ctx),
                                         llvm::Type::getInt64Ty(ctx)};
    return llvm::StructType::get(ctx, llvm::ArrayRef(fields.data(), fields.size()));
}

// _data 字段指针（field 0，ptr*）
llvm::Value* Compiler::arrayDataFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP(_context);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, zero}, name + ".data_field");
}

// _len 字段指针（field 1，i64*）
llvm::Value* Compiler::arrayLenFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP(_context);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, one}, name + ".len_field");
}

// _cap 字段指针（field 2，i64*）
llvm::Value* Compiler::arrayCapFieldPtr(llvm::Value* arrayStructPtr, const string& name) {
    auto ty = getArrayStructTypeForGEP(_context);
    auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto two = llvm::ConstantInt::get(_builder.getInt32Ty(), 2);
    return _builder.CreateGEP(ty, arrayStructPtr, {zero, two}, name + ".cap_field");
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

    // 内建泛型类型（Rc/Weak/Array/Nullable）写成 Normal 形式（即不带 `<T>`）：
    // 这些类型没有 StructDecl 兜底，落到下方各分支也只剩 `Unknown type (null)`，
    // 返回 null 会让调用方在后续 SEH/段错误时崩。这里早抛 E6011 以给出诊断。
    if (type.isNormal()) {
        static constexpr std::array<std::pair<const char*, size_t>, 5> kBuiltinGenerics{{
            {"Rc", 1},
            {"Weak", 1},
            {"Array", 1},
            {"Nullable", 1},
            {"Heap", 1},
        }};
        for (auto [bname, arity] : kBuiltinGenerics) {
            if (type.name == bname) {
                throw YuxError(1, ErrorCode::E6011, type.name, arity, static_cast<size_t>(0))
                    .withHint(std::format("`{}` 是泛型类型，使用时须带类型实参：改写为 `{}<T>`", type.name, type.name));
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
            // E4025: Rc<Heap<T>> 禁止 (DRAFT-heap-types §8.3a.5.1)
            if (elemType->isHeap()) {
                auto innerSp = elemType->heapElementType();
                throw YuxError(1, ErrorCode::E4025, std::string("Rc"), innerSp ? innerSp->name : std::string("?"));
            }
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
            // E4025: Weak<Heap<T>> 禁止 (DRAFT-heap-types §8.3a.5.1)
            if (elemType->isHeap()) {
                auto innerSp = elemType->heapElementType();
                throw YuxError(1, ErrorCode::E4025, std::string("Weak"), innerSp ? innerSp->name : std::string("?"));
            }
            DEBUG_LOG_VAL("    -> WeakType (struct)", "Weak<" << elemType->name << ">");
            vector<llvm::Type*> weakFields;
            weakFields.push_back(llvm::PointerType::get(_context, 0)); // handle: Block*
            return llvm::StructType::get(_context, weakFields);
        }
        return llvm::PointerType::get(_context, 0);
    }

    // Array<T> 类型 (动态数组，B-3 新布局：去 Builtin/去 Block)
    // 结构: { ptr _data, u64 _len, u64 _cap }，24 字节
    // _data 为直接 HeapAlloc 的数据缓冲指针；_data == null 表示空数组（无分配）
    if (type.isArrayGeneric()) {
        auto elemType = type.arrayGenericElementType();
        if (elemType) {
            // E4025: Array<Heap<T>> 禁止 (DRAFT-heap-types §8.3a.5.1)
            if (elemType->isHeap()) {
                auto innerSp = elemType->heapElementType();
                throw YuxError(1, ErrorCode::E4025, std::string("Array"), innerSp ? innerSp->name : std::string("?"));
            }
            DEBUG_LOG_VAL("    -> ArrayGeneric (struct)", "Array<" << elemType->name << ">");
            vector<llvm::Type*> arrayFields;
            arrayFields.push_back(llvm::PointerType::get(_context, 0)); // _data: Ptr
            arrayFields.push_back(_builder.getInt64Ty());               // _len: u64
            arrayFields.push_back(_builder.getInt64Ty());               // _cap: u64
            return llvm::StructType::get(_context, arrayFields);
        }
        return llvm::PointerType::get(_context, 0);
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

    // 函数类型字面量 fn(P1,...) R / fn?(...) R → 16 字节 fat-ptr 占位（spec §5.2）
    // layout: { ptr fn_ptr, ptr captures }；captures 为 Rc<CapturesT>? handle，
    // Phase 1 仅占位（不生成调用），调用 / RC / 闭包推迟 Phase 2/3/4
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

    // DRAFT-spec-reflect: Type / Field / Method / Variant — #Builtin struct,
    // LLVM layout 由编译器硬编码 (SDK 声明仅 name String 字段可见, 其余 slot 隐藏).
    {
        auto* ptrTy = llvm::PointerType::get(_context, 0);
        // Resolve String LLVM type: try cache first, then getLLVMType, then build directly.
        // String = { Array_u32 } = { { ptr } } (Array_u32 is a single-ptr struct).
        auto* stringTy = [&]() -> llvm::Type* {
            auto cit = _structTypes.find("String");
            if (cit != _structTypes.end()) return cit->second;
            auto* resolved = getLLVMType(TypeInfo("String"));
            if (resolved) return resolved;
            // Fallback: build String type directly (test context, SDK not yet loaded)
            vector<llvm::Type*> arrFields = {ptrTy};
            auto* arrU32 = llvm::StructType::get(_context, arrFields);
            vector<llvm::Type*> strFields = {arrU32};
            return llvm::StructType::get(_context, strFields);
        }();
        if (type.name == "Field" || type.name == "Method" || type.name == "Variant") {
            vector<llvm::Type*> fields = {stringTy};
            auto* st = llvm::StructType::create(_context, fields, "reflect." + type.name);
            _structTypes[type.name] = st;
            DEBUG_LOG_VAL("    -> Reflect struct (builtin)", type.name);
            return st;
        }
        if (type.name == "Type") {
            // layout: { String name } — fields/methods/variants refs 为独立全局
            vector<llvm::Type*> fields = {stringTy};
            auto* st = llvm::StructType::create(_context, fields, "reflect.Type");
            _structTypes[type.name] = st;
            DEBUG_LOG_VAL("    -> Reflect Type struct (builtin)", "Type");
            return st;
        }
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
        // 泛型 struct 但用法没带 `<T>`：避免落进 getOrCreateStructType 把未实例化的类型参数当成
        // 实类型 → 字段类型 null → llvm::StructType::create 段错误
        if (structDecl->isGeneric()) {
            int errLine = static_cast<int>(structDecl->name().getLine());
            if (errLine <= 0) errLine = 1;
            throw YuxError(errLine, ErrorCode::E6011, type.name, structDecl->typeParams().size(),
                           static_cast<size_t>(0))
                .withHint(std::format("`{}` 是泛型类型，使用时须带类型实参：改写为 `{}<{}>`", type.name, type.name,
                                      std::string(structDecl->typeParams().size() == 1 ? "T" : "T1, T2, ...")));
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
        string cacheKey = "$enum$" + type.name;
        // 先查缓存：泛型实例方法 emit 时 _file 会切到 SDK，lookupEnumDecl 找不到用户文件里的 enum，
        // 但 LLVM 类型其实已经在用户文件 emit 阶段建过缓存，直接返回即可，避免落到 null 上层崩。
        if (auto cit = _structTypes.find(cacheKey); cit != _structTypes.end()) {
            DEBUG_LOG_VAL("    -> Enum (cached)", type.name);
            return cit->second;
        }
        p<FileNode> enumOwner = nullptr;
        auto enumDecl = lookupEnumDecl(type.name, enumOwner);
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
    // DRAFT-spec-reflect: Field/Type/Method/Variant 的 LLVM 布局由编译器硬编码,
    // 不从 yux 声明构建, 避免 getOrCreateStructType 缓存旧布局覆盖硬编码版本.
    if (name == "Field" || name == "Type" || name == "Method" || name == "Variant") {
        DEBUG_LOG_VAL("Skipping reflect #Builtin struct (hardcoded layout)", name);
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
    // DRAFT-错误.md [#10.A]：#Fallible(E) 函数返回类型包成 { i1, T_ok?, ErrEnum }
    string fallibleErr;
    if (auto e = header->getAnnoArg("Fallible")) {
        fallibleErr = *e;
    }
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
        return retType.empty() ? _builder.getVoidTy() : getLLVMType(retType);
    }
    return getFallibleRetStructType(retType, errTypeName);
}

llvm::StructType* Compiler::getFallibleRetStructType(const TypeInfo& retType, const string& errTypeName) {
    // ErrEnum 必为已声明 enum（10e 静态层已校 + E7011）；通过 TypeInfo 走 getLLVMType
    auto errLLVMType = getLLVMType(TypeInfo(errTypeName));
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
//   1. 取 yux_main —— 此时签名已是 { i1 isErr, EnumLLVM err }（因 main 必返 void）
//   2. 设置控制台代码页（沿用普通 startup）
//   3. call yux_main → 取 isErr → CondBr ok / err
//   4. err 分支：extract err 字段 → switch on err.tag → 各 variant BB 写 stderr + ExitProcess(1)
//   5. ok 分支：ret 0
void Compiler::emitMainStartupFallible(const string& fallibleErrName) {
    auto setConsoleOutputCP = runtime::getSetConsoleOutputCPFn(_module, _builder);
    auto setConsoleCP = runtime::getSetConsoleCPFn(_module, _builder);
    auto getStdHandle = runtime::getOrCreateWindowsAPI(_module, _builder, "GetStdHandle");
    auto writeFile = runtime::getOrCreateWindowsAPI(_module, _builder, "WriteFile");
    auto exitProcess = runtime::getOrCreateWindowsAPI(_module, _builder, "ExitProcess");

    auto yuxMain = _module->getFunction("yux_main");
    if (!yuxMain) {
        // 防御：理论 compileFn 已发射 yux_main
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

    // DRAFT-static-vars Phase 6: 按模块拓扑序调用 _yux_global_init_<Mod>()
    auto voidFnType = llvm::FunctionType::get(_builder.getVoidTy(), {}, false);
    if (_yux && !_yux->loadOrder().empty()) {
        for (auto& modName : _yux->loadOrder()) {
            string fnName = "_yux_global_init_" + modName;
            for (auto& c : fnName) {
                if (c == '.') c = '_';
            }
            auto callee = _module->getOrInsertFunction(fnName, voidFnType);
            _builder.CreateCall(callee, {});
        }
    } else {
        // 兼容旧路径（单文件模式）：遍历当前 Module 内所有 init 函数
        for (auto& func : _module->getFunctionList()) {
            auto funcName = func.getName();
            if (funcName.starts_with("_yux_global_init_")) {
                _builder.CreateCall(&func, {});
            }
        }
    }

    // 调用 yux_main 拿 { i1, EnumLLVM }
    auto callRet = _builder.CreateCall(yuxMain, {}, "main.ret");
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
    p<FileNode> enumOwner = nullptr;
    auto enumDecl = lookupEnumDecl(fallibleErrName, enumOwner);
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
        msg += "\n";

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
