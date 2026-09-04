// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 构造表达式编译 (Enum / Dyn / Heap ctor)：从 compiler_expr.cpp 拆出 (P1 Phase 4)。
// 方法体一字不动。

#include "../compiler.h"
#include "../compiler_runtime.h"
#include "analyzer/spec_impl_checker.h"
#include "analyzer/spec_registry.h"
#include "ast/mangler.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/literal_node.h"
#include "ast/yux.h"
#include "sema/call_resolve.h"
#include "sema/const_eval.h"
#include <algorithm>
#include <cassert>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <regex>
#include <set>

// #Cval #Inline 静态字段 init 直接求值为 llvm::Constant*。
// 不经过 ConstEvaluator，避免其内部 parseIntLiteral 对大 u64 字面量（>= 2^63）
// 走 stoll 溢出导致返回 nullopt。直接按字段类型决定 signedness 解析。
llvm::Constant* Compiler::evalInlineFieldInit(p<ExprNode> init, const TypeInfo& fieldType, llvm::Type* llvmType) {
    if (!init || !llvmType) return nullptr;

    // ---- 处理一元负号（ExprUnaryNode Neg）：用于 MIN = -128 等形式 ----
    bool negate = false;
    if (auto* unary = dynamic_cast<ExprUnaryNode*>(init)) {
        if (unary->op() == ExprUnaryNode::Op::Neg) {
            negate = true;
            init = unary->right();
        }
    }

    // ---- 叶：字面量 ----
    if (auto* litExpr = dynamic_cast<ExprLiteralNode*>(init)) {
        auto* lit = litExpr->literal();
        if (!lit) return nullptr;

        if (auto* intLit = dynamic_cast<LiteralIntNode*>(lit)) {
            string text = intLit->getValue().getText();
            // 按字段类型决定 signedness（而非文本后缀），避免无后缀大 u64 走 stoll 溢出
            bool isUnsigned = fieldType.isUnsigned();
            try {
                // 去掉文本后缀再按 fieldType 解析
                string numStr = text;
                static const std::regex suffixRe(R"([iu](?:8|16|32|64|size)?$)");
                numStr = std::regex_replace(numStr, suffixRe, "");

                int base = 10;
                string parseStr = numStr;
                if (numStr.size() >= 2 && numStr[0] == '0') {
                    if (numStr[1] == 'b' || numStr[1] == 'B') {
                        base = 2;
                        parseStr = numStr.substr(2);
                    } else if (numStr[1] == 'o' || numStr[1] == 'O') {
                        base = 8;
                        parseStr = numStr.substr(2);
                    } else if (numStr[1] == 'x' || numStr[1] == 'X') {
                        base = 16;
                        parseStr = numStr.substr(2);
                    }
                }
                std::erase(parseStr, '_');

                u64 bits;
                if (isUnsigned) {
                    bits = std::stoull(parseStr, nullptr, base);
                } else {
                    i64 v = std::stoll(parseStr, nullptr, base);
                    bits = static_cast<u64>(v);
                }
                if (negate) {
                    i64 negV = -static_cast<i64>(bits);
                    bits = static_cast<u64>(negV);
                }
                return llvm::ConstantInt::get(llvmType, bits, false);
            } catch (...) {
                return nullptr;
            }
        }

        if (auto* floatLit = dynamic_cast<LiteralFloatNode*>(lit)) {
            string text = floatLit->getValue().getText();
            // 去掉 f32/f64 后缀
            if (text.size() >= 3) {
                string suf = text.substr(text.size() - 3);
                if (suf == "f32" || suf == "f64") text = text.substr(0, text.size() - 3);
            }
            try {
                double v = std::stod(text);
                if (fieldType.name == "f32") v = static_cast<float>(v);
                if (negate) v = -v;
                return llvm::ConstantFP::get(llvmType, v);
            } catch (...) {
                return nullptr;
            }
        }

        if (auto* boolLit = dynamic_cast<LiteralBoolNode*>(lit)) {
            bool v = boolLit->getValue().getText() == "true";
            if (negate) v = !v;
            return llvm::ConstantInt::get(llvmType, v ? 1 : 0, false);
        }
    }

    return nullptr;
}

// 编译枚举构造表达式 E::V / E::V() / E::V(args)
// Phase 5: 支持零参 + tuple-payload variant
//
// 步骤：
// 1. 通过 ExprPathCallNode::getType() 解析后的 enum 名（已透传别名）查 EnumDecl
// 2. 验证 variant 存在 / arity 匹配
// 3. 取 enum 的 LLVM 类型（{ i32 tag } 或 { i32, [N x i8] }）
// 4. 在栈上 alloca，写入 tag = variant index
// 5. tuple-payload variant：把 payload buffer 重解释为 variant 的 tuple struct，
//    逐元素 store 实参值（实参类型与 payload 元素类型严格匹配；callee-clean
//    入参规则下，Rc/Array/Weak 已是 +1 fresh 句柄，直接交付给 enum 拥有）
// 6. 零参 variant 不动 payload buffer（spec §6.5）
// 7. 加载整体 struct value 作为表达式结果返回
llvm::Value* Compiler::compileEnumCtorExpr(p<ExprPathCallNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    string enumName = node->getType().name; // 经别名解析后的真实 enum 名
    string variantName = node->variantName().getText();
    int line = node->getLineNumber();
    int col = node->getColumn();

    const bool selfLhs = node->enumName().getText() == "Self";
    string lookupLhs = node->resolvedLhsName();
    if (selfLhs) {
        if (!_currentStructName.empty()) {
            auto instIt = _structInstances.find(_currentStructName);
            if (instIt != _structInstances.end() && instIt->second.baseDecl) {
                lookupLhs = instIt->second.baseDecl->name().getText();
            } else if (lookupLhs == "Self") {
                lookupLhs = _currentStructName;
            }
        } else if (lookupLhs == "Self") {
            // E3123 由 SemaPass PathCall 先抛。
            throwSemaGap(line, col);
        }
    }

    // DRAFT-spec-reflect Phase 4: `<Struct>::type` / `<Struct>::fields` /
    // `<Struct>::methods` / `<Struct>::variants`
    //   * type     → 整个 reflect Type 全局值 (by-value 拷贝)
    //   * fields   → Type 的第 1 槽 ([Field& * N]& = ptr to [N x ptr])
    //   * methods  → Type 的第 2 槽 ([Method& * 0]&, 当前 null)
    //   * variants → Type 的第 3 槽 ([Variant& * 0]&, 当前 null)
    // sema 已校验 LHS 是已知 struct, 这里直接 emit load.
    {
        string lhsRaw = lookupLhs;
        string rhsName = node->variantName().getText();
        if (node->args().empty() &&
            (rhsName == "type" || rhsName == "fields" || rhsName == "methods" || rhsName == "variants")) {
            FileNode* sdkF = _yux ? _yux->sdkFile() : nullptr;
            auto* sd = _file ? _file->getStructDecl(lhsRaw) : nullptr;
            if (!sd && sdkF && sdkF != _file) sd = sdkF->getStructDecl(lhsRaw);
            if (sd) {
                // DRAFT-spec-reflect §2: variants 仅 enum 可访问; struct 上访问 → E3135.
                // E3135 由 SemaPass PathCall 先抛；此处防 IR 把 struct::variants 当 reflect 槽。
                if (rhsName == "variants") {
                    throwSemaGap(line, col);
                }

                llvm::GlobalVariable* fieldsRefGV = nullptr;
                auto* gv = ensureReflectTypeGlobal(TypeInfo(lhsRaw), &fieldsRefGV);
                if (!gv) {
                    throw YuxError(line, col, ErrorCode::E6019, lhsRaw);
                }
                if (rhsName == "type") {
                    auto typeStructTy = getLLVMType(TypeInfo("Type"));
                    return _builder.CreateLoad(typeStructTy, gv, "reflect.type");
                }
                // fields: 返回 [N x ptr] ref 数组的指针 ([Field& * N]&).
                // fieldsRefGV 是 [N x ptr] 全局常量, 其地址即为数组引用.
                if (rhsName == "fields") {
                    if (!fieldsRefGV) {
                        throw YuxError(line, col, ErrorCode::E6019, lhsRaw + ".fields (no instance fields)");
                    }
                    return fieldsRefGV;
                }
                // methods: not yet populated → null reference
                auto ptrTy = llvm::PointerType::get(_context, 0);
                return llvm::ConstantPointerNull::get(ptrTy);
            }
            // LHS 是 enum: variants 放行（v1 尚未填充 variants 数据, 返回 null）
            if (rhsName == "variants") {
                auto* enumDecl = _file ? _file->getEnumDecl(lhsRaw) : nullptr;
                if (!enumDecl && sdkF && sdkF != _file) enumDecl = sdkF->getEnumDecl(lhsRaw);
                if (enumDecl) {
                    // v1: variants data 尚未实现, 返回 null
                    auto ptrTy = llvm::PointerType::get(_context, 0);
                    return llvm::ConstantPointerNull::get(ptrTy);
                }
                // 既不是 struct 也不是 enum → fallthrough
            }
        }
    }

    // Phase 3c 构造模型重构: 若 LHS 是 struct, 走 #Static fn 调用路径.
    // sema 已先做形态校验 (#Static 命中 / 缺失 / 实例方法误用), 这里直接 emit call.
    {
        string lhsRaw = lookupLhs;
        auto* structImpl = _file ? _file->getStructImpl(lhsRaw) : nullptr;
        FileNode* sdk = _yux ? _yux->sdkFile() : nullptr;
        if (!structImpl && sdk && sdk != _file) {
            structImpl = sdk->getStructImpl(lhsRaw);
        }

        // DRAFT-static-vars Phase 4: 静态字段读路径
        // 若 LHS 是 struct 且 RHS 无 args（零参无括号），先查静态字段
        if (node->args().empty()) {
            string fieldName = node->variantName().getText();
            // includeBuiltin=true：允许 #Builtin struct（如 i8）上的静态字段
            auto* structDecl = _file ? _file->getStructDecl(lhsRaw, /*includeBuiltin=*/true) : nullptr;
            if (!structDecl && sdk && sdk != _file) {
                structDecl = sdk->getStructDecl(lhsRaw, /*includeBuiltin=*/true);
            }
            if (structDecl) {
                if (auto* sf = structDecl->staticField(fieldName)) {
                    // #Cval #Inline：使用处直接内联常量值，不产生 GlobalVariable / 符号
                    if (sf->isCval && sf->isInline) {
                        auto llvmType = getLLVMType(sf->type->getType());
                        llvm::Constant* constVal = evalInlineFieldInit(sf->init, sf->type->getType(), llvmType);
                        if (constVal) {
                            DEBUG_LOG_VAL("    Expr: InlineStaticField",
                                          lhsRaw << "::" << fieldName << " = [inline constant]");
                            return constVal;
                        }
                        // 求值失败是编译器 bug（#Cval 字段的 init 必须是 const-evaluable）
                        throw YuxError(line, col, ErrorCode::E3140, lhsRaw + "::" + fieldName);
                    }
                    string ownerMod = _file ? _file->moduleName() : "";
                    auto mangledName = Mangler::staticField(ownerMod, lhsRaw, fieldName);
                    auto* gv = _module->getGlobalVariable(mangledName, true);
                    if (!gv && sdk) {
                        // 静态字段在 SDK 模块中（跨模块访问）：用 SDK 模块名查找
                        string sdkMod = sdk->moduleName();
                        mangledName = Mangler::staticField(sdkMod, lhsRaw, fieldName);
                        gv = _module->getGlobalVariable(mangledName, true);
                        if (!gv) {
                            // 跨文件引用：创建外部声明供链接时解析
                            auto llvmType = getLLVMType(sf->type->getType());
                            gv = new llvm::GlobalVariable(*_module, llvmType, true, llvm::GlobalValue::ExternalLinkage,
                                                          nullptr, mangledName);
                        }
                    }
                    if (gv) {
                        return _builder.CreateLoad(gv->getValueType(), gv, "static.field.load");
                    }
                }
            }
        }

        if (structImpl) {
            string methodName = node->variantName().getText();
            p<FnHeaderNode> methodHeader = nullptr;
            for (auto& m : structImpl->methods()) {
                if (m->header()->name().getText() == methodName) {
                    methodHeader = m->header();
                    break;
                }
            }
            // E3120/E3121 由 SemaPass PathCall 先抛；此处防 IR 无 #Static 可降。
            if (!methodHeader || !methodHeader->isStatic()) {
                throwSemaGap(line, col);
            }
            // Array 是 #Builtin 空字段结构体，Self { ... } 不适用；
            // #Builtin #Static 工厂走 kBuiltinMethods，调用点内联合成。
            if (methodHeader->hasAnno("Builtin") && methodHeader->isStatic()) {
                if (auto* spec = sema::lookupStaticBuiltin(lhsRaw, methodName)) {
                    if (spec->lower == sema::BuiltinLower::ArrayWithCapacity) {
                        DEBUG_LOG("    Expr: Array::with_capacity");
                        return compileArrayWithCapacity(node);
                    }
                }
            }
            // Phase 6E.4-B: 泛型 struct turbofish 形态 `Type:<T>::name(...)`
            // 消费 lhsTypeArgs, 触发 ensureStructInstance, 切到实例 mangled 名;
            // 同时压一帧 SubstFrame 让 paramTypes / retType 的 T / Self 替换生效.
            string effLhs = lhsRaw;
            bool pushedFrame = false;
            const auto& lhsTArgs = node->lhsTypeArgs();
            if (!lhsTArgs.empty()) {
                p<StructDeclNode> baseDecl = _file ? _file->getStructDecl(lhsRaw) : nullptr;
                p<FileNode> baseOwner = _file;
                if (!baseDecl && _yux && _yux->sdkFile() && _yux->sdkFile() != _file) {
                    baseDecl = _yux->sdkFile()->getStructDecl(lhsRaw);
                    baseOwner = _yux->sdkFile();
                }
                if (!baseDecl || !baseDecl->isGeneric()) {
                    // E6011 由 SemaPass 非泛型 struct turbofish 先抛。
                    throwSemaGap(line, col);
                }
                if (lhsTArgs.size() != baseDecl->typeParams().size()) {
                    // E6011 由 SemaPass #Static turbofish 先抛。
                    throwSemaGap(line, col);
                }
                vector<sp<TypeInfo>> instArgs;
                instArgs.reserve(lhsTArgs.size());
                for (auto& ta : lhsTArgs) {
                    instArgs.push_back(std::make_shared<TypeInfo>(applySubst(ta->getType())));
                }
                effLhs = ensureStructInstance(baseDecl, instArgs, baseOwner, line);
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < instArgs.size(); ++i) {
                    subst[baseDecl->typeParams()[i]] = instArgs[i] ? *instArgs[i] : TypeInfo();
                }
                _substStack.push_back(SubstFrame{.subst = std::move(subst),
                                                 .baseStructName = lhsRaw,
                                                 .effStructName = effLhs,
                                                 .sourceFile = _file ? _file->moduleName() : "",
                                                 .sourceLine = line});
                pushedFrame = true;
            } else if (selfLhs) {
                // 泛型实例方法体内 `Self::name`：绑当前单态 mangled 名。
                // 从栈顶向下找 struct 实例帧（跳过压在上面的泛型 fn 帧）。
                for (auto it = _substStack.rbegin(); it != _substStack.rend(); ++it) {
                    if (it->baseStructName == lhsRaw && !it->effStructName.empty()) {
                        effLhs = it->effStructName;
                        break;
                    }
                }
                if (effLhs == lhsRaw) {
                    auto instIt = _structInstances.find(_currentStructName);
                    if (instIt != _structInstances.end()) {
                        if (!instIt->second.baseDecl || instIt->second.baseDecl->name().getText() == lhsRaw) {
                            effLhs = _currentStructName;
                        }
                    }
                }
            }
            // SubstFrame 在异常路径上必须 pop, 否则后续 applySubst 误用本帧 → 类型污染.
            try {
                vector<TypeInfo> paramTypes;
                for (auto p : methodHeader->params()) {
                    if (p->type()) paramTypes.push_back(applySubst(p->type()->getType()));
                }
                TypeInfo retType;
                if (methodHeader->retType()) retType = applySubst(methodHeader->retType()->getType());
                string mFallibleErr;
                mFallibleErr = methodHeader->resolvedFallibleErr();
                auto fn = getMethodFunction(effLhs, methodName, paramTypes, retType, mFallibleErr, /*isStatic=*/true);
                vector<llvm::Value*> argVals;
                argVals.reserve(node->args().size());
                // Phase 6E: 灵活整数字面量按形参类型回填 (如 `S::make(1)` 推 1 为 i64)
                for (size_t i = 0; i < node->args().size() && i < paramTypes.size(); ++i) {
                    tryInferIntType(node->args()[i], paramTypes[i]);
                }
                // Phase 6E: 调用站点实参 arity / 类型校验 (替代 6D 删除的 E6033).
                // 先抛 E3131 比让 LLVM signature-mismatch 断言崩好得多.
                if (node->args().size() != paramTypes.size()) {
                    throwSemaGap(line, col);
                }
                for (size_t i = 0; i < node->args().size(); ++i) {
                    auto actualTy = applySubst(node->args()[i]->getType());
                    if (!actualTy.empty() && !(actualTy == paramTypes[i])) {
                        throwSemaGap(line, col);
                    }
                }
                for (auto& a : node->args()) {
                    argVals.push_back(compileExpr(a));
                }
                // Phase 4c: 静态 fn 调用点的句柄实参所有权转移，与 ExprCallNode 路径对齐
                // (compiler_call.cpp:667-670). 不做这步会让 callee 拿到 caller 唯一 +1,
                // callee 析构释放后 caller 的 alloca 变成 use-after-free.
                for (size_t i = 0; i < argVals.size() && i < paramTypes.size(); ++i) {
                    passAsArg(argVals[i], paramTypes[i], node->args()[i]);
                    // B-4: Array<T> 等 struct-by-pointer 实参做指针转换,
                    // 对齐 getMethodFunction 中 structParamUsesPointer 的 LLVM 签名。
                    // 优先复用源变量 alloca（避免副本导致 caller 析构时 double-free），
                    // 找不到则创临时 alloca（表达式结果等场景）。
                    if (structParamUsesPointer(paramTypes[i])) {
                        llvm::Value* ptrAlloca = nullptr;
                        auto& arg = node->args()[i];
                        if (auto lit = dynamic_cast<ExprLiteralNode*>(arg)) {
                            if (auto objLit = dynamic_cast<LiteralObjNode*>(lit->literal())) {
                                auto varName = objLit->getValue().getText();
                                auto it = _localVarPtrs.find(varName);
                                if (it != _localVarPtrs.end()) {
                                    ptrAlloca = it->second;
                                    // 所有权转移给 callee（callee 内部 move 会 zero _data），
                                    // 标记 moved 防 caller 析构 double-free
                                    _movedVars.insert(varName);
                                    eraseScopeVar(varName);
                                }
                            }
                        }
                        if (!ptrAlloca) {
                            auto structType = getLLVMType(paramTypes[i]);
                            ptrAlloca = _builder.CreateAlloca(structType, nullptr, "struct_arg_tmp");
                            _builder.CreateStore(argVals[i], ptrAlloca);
                        }
                        argVals[i] = ptrAlloca;
                    }
                }
                auto callResult = _builder.CreateCall(fn, argVals, retType.empty() ? "" : methodName + ".ret");
                if (pushedFrame) {
                    _substStack.pop_back();
                }
                return handleFallibleCallResult(callResult, mFallibleErr, retType, nullptr);
            } catch (...) {
                if (pushedFrame) _substStack.pop_back();
                throw;
            }
        }
    }

    p<FileNode> owner = nullptr;
    auto enumDecl = names().lookupEnum(enumName, &owner);
    auto variant = enumDecl->variant(variantName);

    size_t givenArity = node->args().size();
    size_t declArity = variant->payloadArity();
    (void)givenArity; // arity 已由 sema::validateEnumCtorShape 校验

    int tagIndex = enumDecl->variantIndex(variantName);
    TypeInfo enumTy(enumName);
    auto enumLLVMType = getLLVMType(enumTy);
    if (!enumLLVMType) {
        throw YuxError(line, col, ErrorCode::E3096, enumName);
    }

    // 在栈上 alloca、写入 tag
    auto alloca = _builder.CreateAlloca(enumLLVMType, nullptr, "enum.ctor");
    auto tagPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 0, "enum.tag.ptr");
    _builder.CreateStore(_builder.getInt32(tagIndex), tagPtr);

    // tuple-payload variant：把 payload buffer 重解释为 variant 自身的 tuple struct，
    // 按位置写入每个实参；payload 字段（即 enum LLVM type 的第 1 个字段）在 enum 类型有
    // payload 时一定存在，layout 形如 { i32 tag, [N x i8] payload }
    if (declArity > 0) {
        // 收集 variant 的 payload 元素 LLVM 类型，构造 variant tuple struct 类型
        vector<llvm::Type*> elemTys;
        elemTys.reserve(declArity);
        for (auto t : variant->payloadTypes()) {
            auto ll = getLLVMType(t->getType());
            if (!ll) {
                std::string msg = enumName;
                msg += "::";
                msg += variantName;
                msg += " payload";
                throw YuxError(line, col, ErrorCode::E3096, msg);
            }
            elemTys.push_back(ll);
        }
        auto payloadStruct = llvm::StructType::get(_context, elemTys);

        // payload buffer 字段地址（enum struct 的字段 1）
        auto payloadBufPtr = _builder.CreateStructGEP(enumLLVMType, alloca, 1, "enum.payload.ptr");

        // 编译实参并按 variant tuple struct 的字段位置 store.
        // 实参类型与 variant payload 类型的严格匹配 (E2032) 已由 sema::validateEnumCtorShape
        // 接管, 这里只走 IR emit.
        for (size_t i = 0; i < declArity; ++i) {
            auto argExpr = node->args()[i];
            auto argType = argExpr->getType();
            auto argVal = compileExpr(argExpr);
            if (!argVal) {
                std::string msg = enumName;
                msg += "::";
                msg += variantName;
                msg += " arg#";
                msg += std::to_string(i);
                throw YuxError(line, col, ErrorCode::E3096, msg);
            }
            auto fieldPtr =
                _builder.CreateStructGEP(payloadStruct, payloadBufPtr, static_cast<unsigned>(i), "enum.payload.elem");
            storeIntoSlot(fieldPtr, argVal, argType, argExpr, SlotStore::Init);
        }
    }

    auto loaded = _builder.CreateLoad(enumLLVMType, alloca, "enum.val");

    DEBUG_LOG_VAL("    Expr: EnumCtor",
                  enumName << "::" << variantName << " tag=" << tagIndex << " arity=" << declArity);
    return loaded;
}

// 编译 Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
//
// Phase 1c：仅占位 codegen。目标是让 `Dyn<D>(x)` 在 parse + 类型推导 + IR 生成
// 全链路走通，落到 16 字节 fat pointer 值；真实 vtable 槽与对象安全检查留 Phase 2/3：
//   - Phase 2：E1131..E1134 静态检查（draft 名 / 嵌套 / 类型不满足 / 非对象安全）
//   - Phase 3：vtable 全局发射 + 槽 0 dtor wrapper + 构造时写真 vtable_ptr / 句柄消费
//
// 当前 emit 策略：
//   - vtable 槽：constant null（占位；Phase 3 替换为 `&__yux_vtable_<U>_<D>`）
//   - data 槽：
//       * arg.type = Rc<U>：从 Rc struct 中抽 handle（第 0 字段，指向 [RC head | U]）
//       * arg.type = U&    ：直接用借用 ptr（Phase 1c 不处理借用所有权，留 Phase 3c）
//       * 其它形态：暂用 null 占位，留 Phase 2 报 E1133
//
// 注：未做 retain / RC 转移。owned 形态意味着接管 Rc 的 +1，本应消费临时帧或 retain；
// 真路由（构造消费 Rc）随 vtable 落地一起补，所以这里 Rc 句柄"裸抽"——Phase 1c
// 的 smoke 只看编译能否过、IR 是否成型，不验运行时所有权。
llvm::Value* Compiler::compileDynCtorExpr(p<ExprDynCtorNode> node) {
    if (!node->hasResolvedType()) node->setResolvedType(node->getType());
    auto resultType = resolvedOrInferredType(node);
    int line = node->getLineNumber();
    int col = node->getColumn();

    // ── Phase 2b: Dyn<D>(x) 构造静态检查 ─────────────────────────────────
    // 顺序: E1131 (D 必须是 draft) → E1132 (嵌套 Dyn) → E1134 (对象安全)
    // → E1133 (参数形态 Rc<U> / U& + U:D)。
    auto specInner = resultType.dynSpecType();
    std::string specBareName = specInner ? specInner->name : std::string();

    // 走 parent() 链而不是 parentScope()：struct 方法的 FnNode 在 AST 构造时
    // 不一定挂上 parentScope，但 parent() 链一定连到 FileNode
    // （参考 expr_node.cpp::lookupDynMethodRetType）。
    Node* cur = node->parent();
    FileNode* file = nullptr;
    while (cur) {
        if (auto f = dynamic_cast<FileNode*>(cur)) {
            file = f;
            break;
        }
        cur = cur->parent();
    }

    SpecDeclNode* specDecl = nullptr;
    std::string specQualified;
    if (_yux && file && !specBareName.empty()) {
        auto& reg = _yux->specRegistry();
        if (auto resolved = reg.resolve(specBareName, file)) {
            specDecl = resolved->decl;
            specQualified = resolved->qualifiedName;
        }
    }
    if (!specDecl) {
        // E1131 由 SemaPass Dyn 构造先抛。
        throwSemaGap(line, col);
    }

    // E1132 由 SemaPass 先抛（内层仍是 Dyn）。
    if (specInner && specInner->isDyn()) {
        throwSemaGap(line, col);
    }

    // E1134 由 SemaPass 先抛（对象安全）。
    if (_yux) {
        auto& checker = _yux->specImplChecker();
        if (!checker.specIsObjectSafe(specDecl)) {
            throwSemaGap(line, col);
        }
    }

    // E1133: 参数形态 + U:D 满足
    auto argExpr = node->arg();
    auto argType = argExpr->getType();
    bool isBorrow = node->isBorrow();
    std::string concreteBare;
    if (isBorrow) {
        // Dyn<D&>(x): 接受 U& 或 Rc<U>
        if (argType.isRef()) {
            auto inner = argType.refElementType();
            if (inner) concreteBare = inner->name;
        } else if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    } else {
        // Dyn<D>(x): 仅接受 Rc<U>
        if (argType.isRc()) {
            auto inner = argType.rcElementType();
            if (inner) concreteBare = inner->name;
        }
    }
    if (concreteBare.empty()) {
        // E1133 由 SemaPass Dyn 构造先抛（参数形态）。
        throwSemaGap(line, col);
    }
    if (_yux) {
        auto& checker = _yux->specImplChecker();
        TypeInfo concreteTI(concreteBare);
        // boundSatisfied 同时覆盖显式 impl (_seen) 与 #DraftLike 结构匹配
        std::vector<TypeInfo> specTypeArgs;
        if (!checker.boundSatisfied(concreteTI, specDecl, specQualified, specTypeArgs)) {
            throwSemaGap(line, col);
        }
    }

    // ── codegen ──────────────────────────────────────────────────────
    // Phase 3a/3c：vtable 由 getOrEmitDynVTable 合成；data 槽按 Rc<U> / U& 形态抽取。
    // 注：Rc 的 +1 / 借用 RC 半权交接留 Phase 3c.2（消费临时帧 / retain 抵消），
    // 本 Phase 仅落 vtable 真值，临时帧路径与原占位等价。
    auto llvmDynTy = getLLVMType(resultType);

    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto nullPtr = llvm::ConstantPointerNull::get(ptrTy);

    auto argVal = compileExpr(argExpr);

    // 抽取 data 槽：Rc<U> 取 handle 字段；U& 直接用
    // Phase 3e RC 交接：
    //   owned (Rc<U>) 形态：源 Rc 若是 fresh 临时（G() 直构），consumeTemp 偷取 +1；
    //     否则（命名变量 / 字段读出）调 _box_retain 拷一份 +1，源 Rc 自己照常 release。
    //     Dyn 在自身 scope 退出时走 _dyn_release 抵消。
    //   borrow (U&) 形态：data_ptr 借用，不动 RC（由源 owner 维持）。
    llvm::Value* dataPtr = nullPtr;
    if (argType.isRc()) {
        // Rc layout = { ptr handle }；handle 指向 [RC head | payload]
        auto rcLLVMTy = getLLVMType(argType);
        auto tmp = _builder.CreateAlloca(rcLLVMTy, nullptr, "dyn.src.rc.tmp");
        _builder.CreateStore(argVal, tmp);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto handleField = _builder.CreateGEP(rcLLVMTy, tmp, {zero, zero}, "dyn.src.handle.ptr");
        dataPtr = _builder.CreateLoad(ptrTy, handleField, "dyn.src.handle");

        passAsArg(argVal, argType, argExpr);
    } else if (argType.isRef()) {
        // U& 已是裸指针类型，直接用
        dataPtr = argVal;
    }

    // vtable 槽：Phase 3a 真值（按 (U, D) 合成 linkonce_odr 全局）
    TypeInfo concreteTI(concreteBare);
    llvm::Value* vtablePtr = getOrEmitDynVTable(concreteTI, specQualified, specDecl);
    if (!vtablePtr) vtablePtr = nullPtr;

    // 组装 fat pointer struct value { vtable, data }
    llvm::Value* fatPtr = llvm::UndefValue::get(llvmDynTy);
    fatPtr = _builder.CreateInsertValue(fatPtr, vtablePtr, {0}, "dyn.vtable");
    fatPtr = _builder.CreateInsertValue(fatPtr, dataPtr, {1}, "dyn.fat");

    DEBUG_LOG_VAL("    Expr: DynCtor", resultType.getFullName() << " <- " << argType.getFullName());
    return fatPtr;
}
