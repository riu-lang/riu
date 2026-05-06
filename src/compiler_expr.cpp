// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 表达式编译实现
// 
// 本文件包含所有表达式类型的编译逻辑:
// - 字面量表达式 (整数、浮点数、布尔值、字符串)
// - 算术表达式 (加减乘除取模)
// - 位运算表达式 (与或异或左移右移)
// - 比较表达式 (相等、不等、大小比较)
// - 括号表达式
// - 函数调用表达式
// - 成员访问表达式
// - if-else 表达式
// - 数组表达式
// - 一元表达式 (取负、取反、取引用)

#include "compiler.h"
#include "node/expr_node.h"
#include "node/literal_node.h"
#include "compiler_runtime.h"
#include "mangler.h"
#include "symbol_suggest.h"
#include <algorithm>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <regex>

// ==================== 辅助函数 ====================

// 解析整数字面量
// 支持多种格式: 十进制、二进制 (0b)、八进制 (0o)、十六进制 (0x)
// 支持类型后缀 (i32, u64 等) 和下划线分隔符
namespace {
    i64 parseIntLiteral(const string& text) {
        string numStr = text;
        
        // 移除类型后缀 (如 i32, u64)
        static const std::regex suffix_regex(R"([iu](?:8|16|32|64)?$)");
        numStr = std::regex_replace(numStr, suffix_regex, "");
        
        int base = 10;
        string parseStr = numStr;
        
        // 检测进制前缀
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
        
        // 移除下划线分隔符
        parseStr.erase(std::remove(parseStr.begin(), parseStr.end(), '_'), parseStr.end());
        
        return std::stoll(parseStr, nullptr, base);
    }
}

// ==================== 数组初始化表达式编译 ====================

// 编译数组填充表达式 ([value ... Type] 语法)
// 使用指定值填充整个数组
llvm::Value* Compiler::compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType, llvm::Value* destPtr) {
    auto literal = node->value();
    auto literalType = literal->getType();
    auto text = literal->getValue().getText();

    // 确定元素类型
    TypeInfo elementType;
    if (node->explicitType()) {
        elementType = node->explicitType()->getType();
        // 类型检查
        if (literalType != elementType) {
            throw YuxError(node->getLineNumber(), node->getColumn(),
                ErrorCode::E3009, literalType.name, elementType.name);
        }
    } else {
        elementType = literalType;
    }

    // 验证元素类型与目标数组类型匹配
    if (targetType.elementType && *targetType.elementType != elementType) {
        throw YuxError(node->getLineNumber(), node->getColumn(),
            ErrorCode::E3010, targetType.elementType->name, elementType.name);
    }

    DEBUG_LOG_VAL("    Expr: ArrayInit", targetType.name);

    auto llvmArrayType = getLLVMType(targetType);
    bool needLoad = (destPtr == nullptr);
    if (needLoad) {
        destPtr = _builder.CreateAlloca(llvmArrayType, nullptr, "array.init");
    }

    // 解析填充值
    llvm::Value* fillValue;
    bool isZeroFill = false;
    i64 intFillVal = 0;
    f64 floatFillVal = 0.0;

    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        intFillVal = parseIntLiteral(text);
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), intFillVal, true);
        isZeroFill = (intFillVal == 0);  // 零值优化
    } else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        // FLOAT 词法可能含科学计数法 (e[-]?\d+) 与类型后缀 f32/f64
        // 仅剥掉后缀，其余交给 stod
        string numStr = text;
        if (numStr.size() >= 3) {
            string suf = numStr.substr(numStr.size() - 3);
            if (suf == "f32" || suf == "f64") {
                numStr = numStr.substr(0, numStr.size() - 3);
            }
        }
        floatFillVal = stod(numStr);
        fillValue = llvm::ConstantFP::get(getLLVMType(elementType), floatFillVal);
        isZeroFill = (floatFillVal == 0.0);  // 零值优化
    } else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        intFillVal = boolVal ? 1 : 0;
        fillValue = llvm::ConstantInt::get(getLLVMType(elementType), intFillVal, false);
        isZeroFill = !boolVal;  // false 值优化
    } else {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3081);
    }

    // 填充数组
    if (isZeroFill) {
        // 零值优化: 使用 LLVM 的零初始化
        auto zeroInit = llvm::ConstantAggregateZero::get(llvmArrayType);
        _builder.CreateStore(zeroInit, destPtr);
    } else if (elementType.name == "i8" || elementType.name == "u8" || elementType.name == "bool") {
        // 字节类型优化: 使用 memset
        auto size = llvm::ConstantInt::get(_builder.getInt64Ty(), targetType.arraySize);
        auto fillByte = _builder.getInt8(static_cast<u8>(intFillVal));
        _builder.CreateMemSetInline(destPtr, llvm::MaybeAlign(1), fillByte, size);
    } else {
        // 通用情况: 逐元素填充
        for (u64 i = 0; i < targetType.arraySize; ++i) {
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
            llvm::Value* indices[] = {zero, index};
            auto elemPtr = _builder.CreateGEP(llvmArrayType, destPtr, indices, "array.elem.ptr");
            _builder.CreateStore(fillValue, elemPtr);
        }
    }

    if (needLoad) {
        return _builder.CreateLoad(llvmArrayType, destPtr, "array.load");
    }
    return nullptr;
}

// ==================== 类型转换 ====================

// 创建类型转换
// 处理整数、浮点数、指针、引用等类型之间的转换
llvm::Value* Compiler::createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType) {
    // 相同类型无需转换
    if (srcType == dstType) {
        DEBUG_LOG_VAL("    Cast: no-op", srcType.name);
        return val;
    }

    DEBUG_LOG_VAL("    Cast", srcType.name << " -> " << dstType.name);

    // 指针类型转换
    if (dstType.isPtr()) {
        if (srcType.isRef()) {
            DEBUG_LOG("      Ref -> Ptr");
            return _builder.CreateBitCast(val, llvm::PointerType::get(_context, 0), "ref_to_ptr");
        }
        if (srcType.isBox()) {
            DEBUG_LOG("      Box -> Ptr");
            // Box -> Ptr：取 payload 首地址（DRAFT §9.4 跳过 RC 头）
            // payload = handle + 8
            auto boxStructType = getLLVMType(srcType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(boxStructType, val, {zero, zero}, "box.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "box.handle");
            return _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "box.payload");
        }
    }

    // 数值类型转换
    auto dstLLVMType = getLLVMType(dstType);
    bool srcIsFloat = srcType.startsWith('f');
    bool dstIsFloat = dstType.startsWith('f');
    bool srcIsUnsigned = srcType.startsWith('u');
    bool dstIsUnsigned = dstType.startsWith('u');

    if (srcIsFloat && dstIsFloat) {
        // 浮点数之间的转换
        if (srcType.name == "f64" && dstType.name == "f32") {
            DEBUG_LOG("      FPTrunc (f64 -> f32)");
            return _builder.CreateFPTrunc(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPExt (f32 -> f64)");
            return _builder.CreateFPExt(val, dstLLVMType);
        }
    } else if (!srcIsFloat && !dstIsFloat) {
        // 整数之间的转换
        auto srcLLVMType = getLLVMType(srcType);
        if (dstLLVMType->getIntegerBitWidth() > srcLLVMType->getIntegerBitWidth()) {
            // 扩展
            if (srcIsUnsigned) {
                DEBUG_LOG("      ZExt (unsigned int extension)");
                return _builder.CreateZExt(val, dstLLVMType);
            } else {
                DEBUG_LOG("      SExt (signed int extension)");
                return _builder.CreateSExt(val, dstLLVMType);
            }
        } else {
            // 截断
            DEBUG_LOG("      Trunc (int truncation)");
            return _builder.CreateTrunc(val, dstLLVMType);
        }
    } else if (!srcIsFloat && dstIsFloat) {
        // 整数转浮点数
        if (srcIsUnsigned) {
            DEBUG_LOG("      UIToFP (unsigned int to float)");
            return _builder.CreateUIToFP(val, dstLLVMType);
        } else {
            DEBUG_LOG("      SIToFP (signed int to float)");
            return _builder.CreateSIToFP(val, dstLLVMType);
        }
    } else {
        // 浮点数转整数
        if (dstIsUnsigned) {
            DEBUG_LOG("      FPToUI (float to unsigned int)");
            return _builder.CreateFPToUI(val, dstLLVMType);
        } else {
            DEBUG_LOG("      FPToSI (float to signed int)");
            return _builder.CreateFPToSI(val, dstLLVMType);
        }
    }
}

// ==================== 字面量表达式编译 ====================

// 编译字面量表达式
// 处理整数、浮点数、布尔值、对象名等
llvm::Value* Compiler::compileLiteralExpr(p<ExprLiteralNode> node) {
    auto literal = node->literal();
    auto type = literal->getType();
    auto text = literal->getValue().getText();

    if (auto intLiteral = dynamic_cast<LiteralIntNode*>(literal)) {
        i64 numVal = parseIntLiteral(text);
        DEBUG_LOG_VAL("    Expr: IntLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), numVal, true);
    } else if (auto floatLiteral = dynamic_cast<LiteralFloatNode*>(literal)) {
        // FLOAT 词法可能含科学计数法 (e[-]?\d+) 与类型后缀 f32/f64
        // 仅剥掉后缀，其余交给 stod
        string numStr = text;
        if (numStr.size() >= 3) {
            string suf = numStr.substr(numStr.size() - 3);
            if (suf == "f32" || suf == "f64") {
                numStr = numStr.substr(0, numStr.size() - 3);
            }
        }
        f64 numVal = stod(numStr);
        DEBUG_LOG_VAL("    Expr: FloatLiteral", text << " : " << type.name);
        return llvm::ConstantFP::get(getLLVMType(type), numVal);
    } else if (auto boolLiteral = dynamic_cast<LiteralBoolNode*>(literal)) {
        bool boolVal = (text == "true");
        DEBUG_LOG_VAL("    Expr: BoolLiteral", text << " : " << type.name);
        return llvm::ConstantInt::get(getLLVMType(type), boolVal ? 1 : 0, false);
    } else if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal)) {
        auto varName = text;
        auto sym = _currentFnNode->lookupSymbol(varName);

        if (sym && _localVarPtrs.contains(varName)) {
            DEBUG_LOG_VAL("    Expr: VariableLoad", varName << " : " << sym->type.name);
            // Phase 4a: T& 借用 — _localVarPtrs[name] 是底层 T 的地址（参数/局部统一），自动解引用
            if (sym->type.isRef()) {
                auto innerType = sym->type.refElementType();
                return _builder.CreateLoad(getLLVMType(*innerType), _localVarPtrs[varName]);
            }
            return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[varName]);
        }

        string ownerMod = (sym && !sym->moduleName.empty()) ? sym->moduleName : _file->moduleName();
        bool globPriv = !varName.empty() && varName[0] == '_';
        string mangledName = Mangler::global(ownerMod, varName, globPriv);
        auto globalVar = _module->getGlobalVariable(mangledName, true);
        if (globalVar) {
            DEBUG_LOG_VAL("    Expr: GlobalConstLoad", varName << " : " << (sym ? sym->type.name : "unknown"));
            return _builder.CreateLoad(globalVar->getValueType(), globalVar, "global.load");
        }

        SymbolSuggest::throwSymbolNotFound(_currentFnNode,
            node->getLineNumber(), node->getColumn(), ErrorCode::E3030, varName);
    } else if (auto cpLiteral = dynamic_cast<LiteralCodePointNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: CodePointLiteral", text << " : u32");
        return llvm::ConstantInt::get(getLLVMType(type), cpLiteral->codePoint(), false);
    } else if (auto nullLiteral = dynamic_cast<LiteralNullNode*>(literal)) {
        DEBUG_LOG("    Expr: NullLiteral");
        return llvm::ConstantPointerNull::get(llvm::PointerType::get(_context, 0));
    } else if (auto stringLiteral = dynamic_cast<LiteralStringNode*>(literal)) {
        DEBUG_LOG_VAL("    Expr: StringLiteral", text);
        return emitStringLiteralValue(stringLiteral->codePoints());
    } else if (auto tplLiteral = dynamic_cast<StringTemplateNode*>(literal)) {
        return compileStringTemplate(tplLiteral);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3080);
}

// 由码点向量发射 .rodata 哨兵 String 值
//
// Phase 1c.1：字面量走 .rodata 哨兵 Block，零启动开销。
// Block 字节布局匹配 Array<T>（compiler_runtime.cpp）：
//   { i32 strong=0xFFFFFFFF, i32 weak=0, i64 len, i64 cap, ptr data }
// strong = 0xFFFFFFFF 让 _array_retain / _array_release 直接跳过；
// String layout 仍是 { data: Array<u32> } = { { ptr handle } }，handle = &block。
//
// LiteralStringNode 与 StringTemplateNode（template parts）共用此发射路径。
llvm::Value* Compiler::emitStringLiteralValue(const vector<u32>& codePoints) {
    size_t len = codePoints.size();

    auto stringType = getLLVMType(TypeInfo("String"));
    auto alloca = _builder.CreateAlloca(stringType, nullptr, "str_tmp");

    auto i32Ty = _builder.getInt32Ty();
    auto i64Ty = _builder.getInt64Ty();
    auto ptrTy = llvm::PointerType::get(_context, 0);
    auto sentinel = llvm::ConstantInt::get(i32Ty, 0xFFFFFFFFu);
    auto i32Zero = llvm::ConstantInt::get(i32Ty, 0);

    // 数据缓冲：len > 0 时铺常量 u32 数组，否则用 null 指针（Block.data）。
    llvm::Constant* dataConst = llvm::ConstantPointerNull::get(ptrTy);
    if (len > 0) {
        auto arrType = llvm::ArrayType::get(i32Ty, len);
        vector<llvm::Constant*> elements;
        elements.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            elements.push_back(llvm::ConstantInt::get(i32Ty, codePoints[i]));
        }
        auto arrInit = llvm::ConstantArray::get(arrType, elements);

        static int strDataCounter = 0;
        string dataName = ".str.data." + to_string(strDataCounter++);
        dataConst = new llvm::GlobalVariable(
            *_module, arrType, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, arrInit, dataName);
    }

    // .rodata Block：32 字节精确匹配 Array Block layout。
    auto blockTy = llvm::StructType::get(_context, {i32Ty, i32Ty, i64Ty, i64Ty, ptrTy});
    auto lenC = llvm::ConstantInt::get(i64Ty, len);
    auto blockInit = llvm::ConstantStruct::get(
        blockTy, {sentinel, i32Zero, lenC, lenC, dataConst});

    // 空字面量共享同一全局，省 .rodata 体积。
    llvm::GlobalVariable* blockGlobal = nullptr;
    if (len == 0) {
        const char* sharedName = ".str.empty.block";
        blockGlobal = _module->getNamedGlobal(sharedName);
        if (!blockGlobal) {
            blockGlobal = new llvm::GlobalVariable(
                *_module, blockTy, /*isConstant=*/true,
                llvm::GlobalValue::PrivateLinkage, blockInit, sharedName);
        }
    } else {
        static int strBlockCounter = 0;
        string blockName = ".str.block." + to_string(strBlockCounter++);
        blockGlobal = new llvm::GlobalVariable(
            *_module, blockTy, /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, blockInit, blockName);
    }

    storeArrayHandle(alloca, blockGlobal);
    return _builder.CreateLoad(stringType, alloca, "str_val");
}

// 编译字符串模板（v0.6 Phase 2a / 2b）
// 把 StringTemplateNode lower 为：
//   sb StringBuilder = StringBuilder()
//   for each non-empty part: sb.append(part_literal)
//   for each interp:         sb.append(interp_value)
//   result = sb.build()
//   ~sb                     ; 释放 build() 后留下的空 Array<u32>
//
// Phase 2b：插值非 String 时合成 `interp.to_string()` 走现有方法分发，
// 类型未实现 ToString 时报 E3026。合成节点用临时 unique_ptr 持有，
// compileExpr 返回后立即释放。
llvm::Value* Compiler::compileStringTemplate(StringTemplateNode* node) {
    DEBUG_LOG("    Expr: StringTemplate -> StringBuilder lowering");
    const auto& parts = node->parts();
    const auto& interps = node->interps();

    // Phase 2b 类型校验：插值类型必须是 String 或在 SDK / 当前文件中可解析到
    // `<Type>.to_string()` —— 等价于实现了 ToString。
    auto canToString = [&](const TypeInfo& t) -> bool {
        if (t.name == "String") return true;
        string fullName = t.name + ".to_string";
        if (_yux && _yux->sdkFile()
            && _yux->sdkFile()->lookupFnSymbol(fullName)) {
            return true;
        }
        if (_file && _file->lookupFnSymbol(fullName)) return true;
        return false;
    };
    for (size_t i = 0; i < interps.size(); ++i) {
        auto t = interps[i]->getType();
        if (!canToString(t)) {
            throw YuxError(
                interps[i]->getLineNumber(), interps[i]->getColumn(),
                ErrorCode::E3026, t.name);
        }
    }

    // UTF-8 → u32 码点解码（parts 在 ast_builder 已展开转义，仅含原始 UTF-8 字节）
    auto decodeUtf8 = [](const string& s) -> vector<u32> {
        vector<u32> out;
        for (size_t i = 0; i < s.size(); ) {
            u8 c = static_cast<u8>(s[i]);
            u32 cp = 0;
            if (c < 0x80) {
                cp = c; i += 1;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
                cp = ((c & 0x1F) << 6) | (static_cast<u8>(s[i + 1]) & 0x3F);
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
                cp = ((c & 0x0F) << 12) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 6)
                   | (static_cast<u8>(s[i + 2]) & 0x3F);
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
                cp = ((c & 0x07) << 18) | ((static_cast<u8>(s[i + 1]) & 0x3F) << 12)
                   | ((static_cast<u8>(s[i + 2]) & 0x3F) << 6) | (static_cast<u8>(s[i + 3]) & 0x3F);
                i += 4;
            } else {
                cp = c; i += 1;
            }
            out.push_back(cp);
        }
        return out;
    };

    // 1. alloca StringBuilder + 调构造
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "tpl_sb");
    {
        vector<TypeInfo> noArgs;
        auto ctorFn = getMethodFunction("StringBuilder", "StringBuilder", noArgs, TypeInfo());
        _builder.CreateCall(ctorFn, {sbPtr});
    }

    // 2. emit sb.append(String) 帮手：普通 struct String 走 by-value 调用约定
    auto emitAppendString = [&](llvm::Value* strVal) {
        vector<TypeInfo> appendParams = {TypeInfo("String")};
        auto appendFn = getMethodFunction("StringBuilder", "append", appendParams, TypeInfo());
        _builder.CreateCall(appendFn, {sbPtr, strVal});
    };

    // 3. 交错追加 parts[i]、interps[i]
    //    Phase 2b：interp 非 String 时合成 `interp.to_string()` 节点，复用方法分发。
    //    合成节点的 _parent 取自 interp（其 parent 是 ScopeNode），保证 findNearestScope 可走到。
    vector<std::unique_ptr<Node>> synthHolder;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i].empty()) {
            auto cps = decodeUtf8(parts[i]);
            emitAppendString(emitStringLiteralValue(cps));
        }
        if (i < interps.size()) {
            auto interpExpr = interps[i];
            llvm::Value* strVal;
            if (interpExpr->getType().name == "String") {
                strVal = compileExpr(interpExpr);
            } else {
                Token memberTok("to_string", static_cast<size_t>(interpExpr->getLineNumber()));
                p<Node> synthParent = interpExpr->parent();
                auto dotNode = new ExprDotNode(synthParent, interpExpr, memberTok);
                synthHolder.emplace_back(dotNode);
                auto callNode = new ExprCallNode(synthParent, dotNode);
                synthHolder.emplace_back(callNode);
                strVal = compileExpr(callNode);
            }
            emitAppendString(strVal);
        }
    }

    // 4. sb.build() → String
    vector<TypeInfo> noArgs;
    auto buildFn = getMethodFunction("StringBuilder", "build", noArgs, TypeInfo("String"));
    auto result = _builder.CreateCall(buildFn, {sbPtr}, "tpl_built");

    // 5. SB 析构：释放 build() 后留下的空 Array<u32>（沿用作用域析构口径）
    releaseAtPtr(sbPtr, TypeInfo("StringBuilder"));

    return result;
}

// v0.6 Phase 2c：连续 String `+` 链整链 lower。
//
// 把左结合 `+` 树扁平化为叶子序列（自顶 right 先压、再沿 left 下钻直到 left 不再是
// 「结果为 String 的 Add」），然后用单条 StringBuilder 累加：
//   sb StringBuilder = StringBuilder()
//   for each leaf:
//     if leaf : String  -> sb.append(leaf)
//     else if leaf : ToString -> sb.append(leaf.to_string())
//     else -> E3026
//   result = sb.build()
//   ~sb
//
// 设计要点：
// - 仅扁平化 left spine。`a + (b + c)` 中 `(b + c)` 作为单个右叶子，由 compileExpr 递归
//   处理（若结果仍是 String，会再次进入本函数，独立开一条 SB；可接受，常见写法是左结合）。
// - 扁平化判据：内部节点必须是 ExprAddSubNode + Op::Add + getType().name == "String"。
//   只要任一操作数是 String，ExprAddSubNode::getType() 会返回 String（spec §4.4.1.4）。
llvm::Value* Compiler::compileStringPlusChain(ExprAddSubNode* node) {
    DEBUG_LOG("    Expr: String + chain -> StringBuilder lowering");

    // 1. 扁平化左脊：得到从左到右的叶子序列（shared_ptr，复用 AST 持有的所有权）
    vector<p<ExprNode>> leaves;
    {
        ExprAddSubNode* cur = node;
        while (true) {
            leaves.push_back(cur->right());
            auto leftExpr = cur->left();
            auto* innerAdd = dynamic_cast<ExprAddSubNode*>(leftExpr);
            bool isStringAdd = innerAdd != nullptr
                && innerAdd->op() == ExprAddSubNode::Op::Add
                && innerAdd->getType().name == "String";
            if (isStringAdd) {
                cur = innerAdd;
                continue;
            }
            leaves.push_back(leftExpr);
            break;
        }
        std::reverse(leaves.begin(), leaves.end());
    }

    // 2. 类型校验：每个叶子必须是 String 或实现 ToString
    auto canToString = [&](const TypeInfo& t) -> bool {
        if (t.name == "String") return true;
        string fullName = t.name + ".to_string";
        if (_yux && _yux->sdkFile()
            && _yux->sdkFile()->lookupFnSymbol(fullName)) {
            return true;
        }
        if (_file && _file->lookupFnSymbol(fullName)) return true;
        return false;
    };
    for (const auto& leaf : leaves) {
        auto t = leaf->getType();
        if (!canToString(t)) {
            throw YuxError(
                leaf->getLineNumber(), leaf->getColumn(),
                ErrorCode::E3026, t.name);
        }
    }

    // 3. alloca StringBuilder + 调构造
    auto sbType = getLLVMType(TypeInfo("StringBuilder"));
    auto sbPtr = _builder.CreateAlloca(sbType, nullptr, "plus_sb");
    {
        vector<TypeInfo> noArgs;
        auto ctorFn = getMethodFunction("StringBuilder", "StringBuilder", noArgs, TypeInfo());
        _builder.CreateCall(ctorFn, {sbPtr});
    }

    // 4. emit sb.append(String) 帮手
    vector<TypeInfo> appendParams = {TypeInfo("String")};
    auto appendFn = getMethodFunction("StringBuilder", "append", appendParams, TypeInfo());

    // 5. 逐叶 append；非 String 合成 `leaf.to_string()`（同 Phase 2b 模板）
    vector<std::unique_ptr<Node>> synthHolder;
    for (const auto& leaf : leaves) {
        llvm::Value* strVal;
        if (leaf->getType().name == "String") {
            strVal = compileExpr(leaf);
        } else {
            Token memberTok("to_string", static_cast<size_t>(leaf->getLineNumber()));
            p<Node> synthParent = leaf->parent();
            auto dotNode = new ExprDotNode(synthParent, leaf, memberTok);
            synthHolder.emplace_back(dotNode);
            auto callNode = new ExprCallNode(synthParent, dotNode);
            synthHolder.emplace_back(callNode);
            strVal = compileExpr(callNode);
        }
        _builder.CreateCall(appendFn, {sbPtr, strVal});
    }

    // 6. sb.build() → String
    vector<TypeInfo> noArgs;
    auto buildFn = getMethodFunction("StringBuilder", "build", noArgs, TypeInfo("String"));
    auto result = _builder.CreateCall(buildFn, {sbPtr}, "plus_built");

    // 7. SB 析构（同模板路径）
    releaseAtPtr(sbPtr, TypeInfo("StringBuilder"));

    return result;
}

// ==================== 自定义类型运算符方法调用 ====================

// 编译自定义类型的二元运算符方法调用
// 将运算符表达式转换为方法调用，如 a + b -> a.plus(b)
llvm::Value* Compiler::compileCustomTypeBinaryOp(
    p<ExprNode> leftExpr, p<ExprNode> rightExpr, const TypeInfo& leftType,
    const string& methodName, int lineNum) {
    
    DEBUG_LOG_VAL("    Expr: CustomTypeBinaryOp", leftType.name << "." << methodName);
    
    // 获取左操作数的指针
    llvm::Value* leftPtr = nullptr;
    if (auto leftLiteral = dynamic_cast<ExprLiteralNode*>(leftExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(leftLiteral->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                leftPtr = it->second;
            }
        }
    }
    
    if (!leftPtr) {
        auto leftVal = compileExpr(leftExpr);
        auto structType = getLLVMType(leftType);
        if (!structType) {
            throw YuxError(lineNum, ErrorCode::E3096, leftType.name);
        }
        auto alloca = _builder.CreateAlloca(structType, nullptr, "op_lhs_tmp");
        _builder.CreateStore(leftVal, alloca);
        leftPtr = alloca;
    }
    
    auto rightType = rightExpr->getType();
    // Phase 4b: 右操作数是 T& 字面变量时，从 _localVarPtrs 直接取裸 ptr，
    // 避免 compileExpr 对 ref 自动 load 出 struct 值后又 alloca 写回 —— 写回会
    // 把 struct 看成是 ptr 类型，触发 LLVM 签名校验失败。
    llvm::Value* rightVal = nullptr;
    if (rightType.isRef()) {
        if (auto rl = dynamic_cast<ExprLiteralNode*>(rightExpr)) {
            if (auto ol = dynamic_cast<LiteralObjNode*>(rl->literal())) {
                auto vn = ol->getValue().getText();
                auto rit = _localVarPtrs.find(vn);
                if (rit != _localVarPtrs.end()) rightVal = rit->second;
            }
        }
    }
    if (!rightVal) {
        rightVal = compileExpr(rightExpr);
    }

    // 查找方法（spec §7.2.3.3）：二元运算符方法形参强制 Self&，
    // 因此查表使用 [leftType, Ref<rightType>]，原 eq(other Self) 形态不再被运算符触发。
    // 运算符位置自动取址（spec §7.2.3.6）：右操作数自动包成 Ref，无需用户写 &。
    // Phase 4b: 操作数本身是 T& 时（如 fn 形参 `actual String&`），剥掉一层 Ref
    // 与方法注册的 [Self, Self&] 对齐；不剥则 lookup 失败导致调用方编译期崩溃。
    // v0.6 Phase 2b: 透明类型别名解析，使 `A = i32` 这类别名走到运算符方法时
    // 仍能匹配到 `i32.plus` 等内置方法。
    TypeInfo effLeftType = leftType.isRef() ? *leftType.refElementType() : leftType;
    TypeInfo effRightType = rightType.isRef() ? *rightType.refElementType() : rightType;
    effLeftType = applySubst(effLeftType);
    effRightType = applySubst(effRightType);
    string methodFullName = effLeftType.name + "." + methodName;
    TypeInfo rightRefType;
    rightRefType.kind = TypeKind::Generic;
    rightRefType.name = "Ref";
    rightRefType.genericArgs.push_back(make_shared<TypeInfo>(effRightType));
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(effLeftType);
    methodParamTypes.push_back(rightRefType);

    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (!methodSymbol) {
        throw YuxError(lineNum, ErrorCode::E3073,
                       leftType.name, methodName == "plus" ? "+" :
                       methodName == "minus" ? "-" :
                       methodName == "mul" ? "*" :
                       methodName == "div" ? "/" :
                       methodName == "mod" ? "%" :
                       methodName == "and" ? "&" :
                       methodName == "or" ? "|" :
                       methodName == "xor" ? "^" :
                       methodName == "shl" ? "<<" :
                       methodName == "shr" ? ">>" :
                       methodName == "eq" ? "==" :
                       methodName == "ne" ? "!=" :
                       methodName == "lt" ? "<" :
                       methodName == "le" ? "<=" :
                       methodName == "gt" ? ">" :
                       methodName == "ge" ? ">=" : methodName, methodName);
    }
    
    // 准备方法参数
    vector<llvm::Value*> methodArgs;
    methodArgs.push_back(leftPtr);
    
    // 检查右操作数是否需要通过指针传递
    auto rightStructDecl = _file->getStructDecl(effRightType.name);
    if (!rightStructDecl && _yux && _yux->sdkFile()) {
        rightStructDecl = _yux->sdkFile()->getStructDecl(effRightType.name);
    }
    if (rightStructDecl && !isBuiltinType(effRightType.name)) {
        // 当右操作数本身是 T&（即 rightVal 已是 ptr）时，直接传 ptr，避免错误的
        // alloca-then-store-into-Struct 路径（structType=Struct，但 rightVal=ptr，类型不匹配）
        if (rightType.isRef()) {
            methodArgs.push_back(rightVal);
        } else {
            auto structType = getLLVMType(effRightType);
            auto alloca = _builder.CreateAlloca(structType, nullptr, "op_rhs_tmp");
            _builder.CreateStore(rightVal, alloca);
            methodArgs.push_back(alloca);
        }
    } else {
        methodArgs.push_back(rightVal);
    }
    
    // 获取或创建方法函数
    // mangle 与 LLVM 签名都按 methodSymbol 实际声明的形参类型走（spec §7.2.3.3 后是 Ref<T>），
    // 与 SDK / 用户代码侧定义的方法符号一一对应。
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    TypeInfo declaredRhsType = methodSymbol->params.size() >= 2 ? methodSymbol->params[1] : rightType;
    vector<TypeInfo> argTypes;
    argTypes.push_back(declaredRhsType);
    string mangledName = Mangler::method(ownerMod, effLeftType.name, methodName, argTypes, methPriv);

    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        // 形参为 Ref<T> / 非 builtin struct 时按 ptr 传，其余按值
        bool rhsByPtr = declaredRhsType.isRef()
                        || (rightStructDecl && !isBuiltinType(declaredRhsType.name));
        if (rhsByPtr) {
            paramTypes.push_back(llvm::PointerType::get(_context, 0));
        } else {
            paramTypes.push_back(getLLVMType(declaredRhsType));
        }
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }
    
    return _builder.CreateCall(fn, methodArgs);
}

// 编译自定义类型的一元运算符方法调用
// 将运算符表达式转换为方法调用，如 -a -> a.neg()
llvm::Value* Compiler::compileCustomTypeUnaryOp(
    p<ExprNode> expr, const TypeInfo& type, const string& methodName, int lineNum) {
    
    DEBUG_LOG_VAL("    Expr: CustomTypeUnaryOp", type.name << "." << methodName);
    
    // 获取操作数的指针
    llvm::Value* ptr = nullptr;
    if (auto literal = dynamic_cast<ExprLiteralNode*>(expr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literal->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it != _localVarPtrs.end()) {
                ptr = it->second;
            }
        }
    }
    
    if (!ptr) {
        auto val = compileExpr(expr);
        auto structType = getLLVMType(type);
        auto alloca = _builder.CreateAlloca(structType, nullptr, "op_tmp");
        _builder.CreateStore(val, alloca);
        ptr = alloca;
    }
    
    // 查找方法
    string methodFullName = type.name + "." + methodName;
    vector<TypeInfo> methodParamTypes;
    methodParamTypes.push_back(type);
    
    auto methodSymbol = _file->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    if (!methodSymbol && _yux && _yux->sdkFile()) {
        methodSymbol = _yux->sdkFile()->lookupFnSymbolWithParams(methodFullName, methodParamTypes);
    }
    
    if (!methodSymbol) {
        throw YuxError(lineNum, ErrorCode::E3074,
                       type.name, methodName == "neg" ? "-" :
                       methodName == "inv" ? "~" :
                       methodName == "not" ? "!" : methodName, methodName);
    }
    
    // 获取或创建方法函数
    string ownerMod = methodSymbol->moduleName.empty() ? _file->moduleName() : methodSymbol->moduleName;
    bool methPriv = !methodName.empty() && methodName[0] == '_';
    vector<TypeInfo> argTypes;
    string mangledName = Mangler::method(ownerMod, type.name, methodName, argTypes, methPriv);
    
    auto fn = _module->getFunction(mangledName);
    if (!fn) {
        vector<llvm::Type*> paramTypes;
        paramTypes.push_back(llvm::PointerType::get(_context, 0));
        auto retType = methodSymbol->retType.empty() ? _builder.getVoidTy() : getLLVMType(methodSymbol->retType);
        auto fnType = llvm::FunctionType::get(retType, paramTypes, false);
        fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, mangledName, _module);
    }
    
    return _builder.CreateCall(fn, {ptr});
}

llvm::Value* Compiler::compileAddSubExpr(p<ExprAddSubNode> node) {
    // v0.6 Phase 2b: 透明别名解析，使 `A = i32` 后 `A + A` 仍走内置算子路径
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr = (node->op() == ExprAddSubNode::Op::Add) ? "+" : "-";
    DEBUG_LOG_VAL("    Expr: AddSub", opStr << " : " << type.name);

    // v0.6 Phase 2c：`+` 表达式整体类型为 String 时，
    // 走 StringBuilder 整链 lower（多段连续 `+` 合并为单次 builder）。
    if (node->op() == ExprAddSubNode::Op::Add && type.name == "String") {
        return compileStringPlusChain(node);
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName = (node->op() == ExprAddSubNode::Op::Add) ? "plus" : "minus";
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }
    
    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');

    if (node->op() == ExprAddSubNode::Op::Add) {
        if (isFloat) {
            return _builder.CreateFAdd(left, right);
        }
        return _builder.CreateAdd(left, right);
    } else {
        if (isFloat) {
            return _builder.CreateFSub(left, right);
        }
        return _builder.CreateSub(left, right);
    }
}

llvm::Value* Compiler::compileMulDivModExpr(p<ExprMulDivModNode> node) {
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul: opStr = "*";
        break;
    case ExprMulDivModNode::Op::Div: opStr = "/";
        break;
    case ExprMulDivModNode::Op::Mod: opStr = "%";
        break;
    }
    DEBUG_LOG_VAL("    Expr: MulDivMod", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprMulDivModNode::Op::Mul: methodName = "mul"; break;
        case ExprMulDivModNode::Op::Div: methodName = "div"; break;
        case ExprMulDivModNode::Op::Mod: methodName = "mod"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isUnsigned = type.startsWith('u');

    switch (node->op()) {
    case ExprMulDivModNode::Op::Mul:
        if (isFloat) {
            return _builder.CreateFMul(left, right);
        }
        return _builder.CreateMul(left, right);
    case ExprMulDivModNode::Op::Div:
        if (isFloat) {
            return _builder.CreateFDiv(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateUDiv(left, right);
        }
        return _builder.CreateSDiv(left, right);
    case ExprMulDivModNode::Op::Mod:
        if (isFloat) {
            return _builder.CreateFRem(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateURem(left, right);
        }
        return _builder.CreateSRem(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3077);
}

llvm::Value* Compiler::compileBinOpExpr(p<ExprBinOpNode> node) {
    auto type = applySubst(node->getType());
    auto leftType = applySubst(node->left()->getType());

    string opStr;
    switch (node->op()) {
    case ExprBinOpNode::Op::And: opStr = "&";
        break;
    case ExprBinOpNode::Op::Or: opStr = "|";
        break;
    case ExprBinOpNode::Op::Xor: opStr = "^";
        break;
    case ExprBinOpNode::Op::Shl: opStr = "<<";
        break;
    case ExprBinOpNode::Op::Shr: opStr = ">>";
        break;
    }
    DEBUG_LOG_VAL("    Expr: BinOp", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprBinOpNode::Op::And: methodName = "and"; break;
        case ExprBinOpNode::Op::Or: methodName = "or"; break;
        case ExprBinOpNode::Op::Xor: methodName = "xor"; break;
        case ExprBinOpNode::Op::Shl: methodName = "shl"; break;
        case ExprBinOpNode::Op::Shr: methodName = "shr"; break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());

    switch (node->op()) {
    case ExprBinOpNode::Op::And:
        return _builder.CreateAnd(left, right);
    case ExprBinOpNode::Op::Or:
        return _builder.CreateOr(left, right);
    case ExprBinOpNode::Op::Xor:
        return _builder.CreateXor(left, right);
    case ExprBinOpNode::Op::Shl:
        return _builder.CreateShl(left, right);
    case ExprBinOpNode::Op::Shr:
        if (type.startsWith('u')) {
            return _builder.CreateLShr(left, right);
        }
        return _builder.CreateAShr(left, right);
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3075);
}

llvm::Value* Compiler::compileParenExpr(p<ExprParenNode> node) {
    DEBUG_LOG("    Expr: Paren");
    return compileExpr(node->expr());
}

llvm::Value* Compiler::compileCompareExpr(p<ExprCompareNode> node) {
    (void)node->getType();
    auto leftType = applySubst(node->left()->getType());
    auto rightType = applySubst(node->right()->getType());

    if (leftType != rightType) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3004, leftType.name, rightType.name);
    }

    // Phase 1d.3：禁 Weak == / !=（DRAFT §5：v1 不暴露 handle 比较语义）
    if (leftType.isWeak()) {
        if (node->op() == ExprCompareNode::Op::Eq || node->op() == ExprCompareNode::Op::Ne) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3078)
                .withHint("先 `upgrade(weak)` 取得 Box<T>?，再用 `?.` / `??` / 相等比较判定目标对象");
        }
    }

    string opStr;
    switch (node->op()) {
    case ExprCompareNode::Op::Eq: opStr = "==";
        break;
    case ExprCompareNode::Op::Ne: opStr = "!=";
        break;
    case ExprCompareNode::Op::Lt: opStr = "<";
        break;
    case ExprCompareNode::Op::Le: opStr = "<=";
        break;
    case ExprCompareNode::Op::Gt: opStr = ">";
        break;
    case ExprCompareNode::Op::Ge: opStr = ">=";
        break;
    case ExprCompareNode::Op::AndAnd: opStr = "&&";
        break;
    case ExprCompareNode::Op::OrOr: opStr = "||";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Compare", opStr << " : " << leftType.name);

    // && 和 || 是逻辑运算符，不转换为方法调用
    if (node->op() == ExprCompareNode::Op::AndAnd || node->op() == ExprCompareNode::Op::OrOr) {
        auto left = compileExpr(node->left());
        auto right = compileExpr(node->right());
        if (node->op() == ExprCompareNode::Op::AndAnd) {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "and.rhs");
            return _builder.CreateAnd(leftBool, rightBool, "and");
        } else {
            auto leftBool = _builder.CreateICmpNE(left, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.lhs");
            auto rightBool = _builder.CreateICmpNE(right, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "or.rhs");
            return _builder.CreateOr(leftBool, rightBool, "or");
        }
    }

    // 检查是否为自定义类型
    if (!isBuiltinType(leftType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprCompareNode::Op::Eq: methodName = "eq"; break;
        case ExprCompareNode::Op::Ne: methodName = "ne"; break;
        case ExprCompareNode::Op::Lt: methodName = "lt"; break;
        case ExprCompareNode::Op::Le: methodName = "le"; break;
        case ExprCompareNode::Op::Gt: methodName = "gt"; break;
        case ExprCompareNode::Op::Ge: methodName = "ge"; break;
        default: break;
        }
        return compileCustomTypeBinaryOp(node->left(), node->right(), leftType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto left = compileExpr(node->left());
    auto right = compileExpr(node->right());
    bool isFloat = leftType.startsWith('f');
    bool isUnsigned = leftType.startsWith('u');

    switch (node->op()) {
    case ExprCompareNode::Op::Eq:
        if (isFloat) {
            return _builder.CreateFCmpOEQ(left, right);
        }
        return _builder.CreateICmpEQ(left, right);
    case ExprCompareNode::Op::Ne:
        if (isFloat) {
            return _builder.CreateFCmpONE(left, right);
        }
        return _builder.CreateICmpNE(left, right);
    case ExprCompareNode::Op::Lt:
        if (isFloat) {
            return _builder.CreateFCmpOLT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULT(left, right);
        }
        return _builder.CreateICmpSLT(left, right);
    case ExprCompareNode::Op::Le:
        if (isFloat) {
            return _builder.CreateFCmpOLE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpULE(left, right);
        }
        return _builder.CreateICmpSLE(left, right);
    case ExprCompareNode::Op::Gt:
        if (isFloat) {
            return _builder.CreateFCmpOGT(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGT(left, right);
        }
        return _builder.CreateICmpSGT(left, right);
    case ExprCompareNode::Op::Ge:
        if (isFloat) {
            return _builder.CreateFCmpOGE(left, right);
        }
        if (isUnsigned) {
            return _builder.CreateICmpUGE(left, right);
        }
        return _builder.CreateICmpSGE(left, right);
    default:
        break;
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3076);
}

llvm::Value* Compiler::compileIfElseExpr(p<ExprIfElseNode> node) {
    auto resultType = node->getType();
    bool hasResult = !resultType.empty();

    DEBUG_LOG_VAL(
        "    Expr: IfElse", "hasResult=" << hasResult << ", type=" << (resultType.empty() ? "void" : resultType.name));

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    DEBUG_LOG("      Created basic blocks: if.then, if.else, if.merge");
    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);

    llvm::PHINode* phi = nullptr;
    if (hasResult) {
        phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    }

    DEBUG_LOG("      Compiling then block");
    compileStatementBlockWithResult(node->thenBlock(), mergeBB, phi, resultType);

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);

    auto& elifs = node->elifs();
    auto elseBlock = node->elseBlock();
    DEBUG_LOG_VAL("      elifs count", elifs.size());

    if (!elifs.empty()) {
        for (size_t i = 0; i < elifs.size(); ++i) {
            auto& elif = elifs[i];
            DEBUG_LOG_VAL("        Compiling elif", i);
            auto elifCond = compileExpr(elif->condition());
            auto elifCondBool = _builder.CreateICmpNE(
                elifCond, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "elif.cond");

            llvm::BasicBlock* elifThenBB = llvm::BasicBlock::Create(_context, "elif.then", func);
            llvm::BasicBlock* elifElseBB = llvm::BasicBlock::Create(_context, "elif.else");

            _builder.CreateCondBr(elifCondBool, elifThenBB, elifElseBB);

            _builder.SetInsertPoint(elifThenBB);
            compileStatementBlockWithResult(elif->block(), mergeBB, phi, resultType);

            func->insert(func->end(), elifElseBB);
            _builder.SetInsertPoint(elifElseBB);
        }
    }

    if (elseBlock) {
        DEBUG_LOG("      Compiling else block");
        compileStatementBlockWithResult(elseBlock, mergeBB, phi, resultType);
    } else {
        DEBUG_LOG("      No else block");
        if (hasResult) {
            phi->addIncoming(llvm::UndefValue::get(getLLVMType(resultType)), _builder.GetInsertBlock());
        }
        _builder.CreateBr(mergeBB);
    }

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    if (hasResult) {
        DEBUG_LOG("      Returning phi node");
        // Phase 8d.3: 各分支已归一为 +1，phi 整体作为 fresh 句柄交给外层 statement frame
        if (resultType.isBox() || resultType.isArrayGeneric() || resultType.isWeak()) {
            recordTemp(phi, resultType);
        }
        return phi;
    }
    return nullptr;
}

llvm::Value* Compiler::compileOneLineIfElseExpr(p<ExprOneLineIfElseNode> node) {
    auto resultType = node->getType();

    DEBUG_LOG_VAL("    Expr: OneLineIfElse", "type=" << resultType.name);

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);
    auto trueVal = compileBranchResultNormalized(node->trueValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto thenEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto falseVal = compileBranchResultNormalized(node->falseValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto elseEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    auto phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    phi->addIncoming(trueVal, thenEndBB);
    phi->addIncoming(falseVal, elseEndBB);

    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (resultType.isBox() || resultType.isArrayGeneric() || resultType.isWeak()) {
        recordTemp(phi, resultType);
    }
    return phi;
}

llvm::Value* Compiler::compileIfElsePreValueExpr(p<ExprIfElsePreValueNode> node) {
    auto resultType = node->getType();

    DEBUG_LOG_VAL("    Expr: IfElsePreValue", "type=" << resultType.name);

    auto condVal = compileExpr(node->condition());
    auto condBool = _builder.CreateICmpNE(condVal, llvm::ConstantInt::get(_builder.getInt1Ty(), 0), "if.cond");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(_context, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(_context, "if.else");
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(_context, "if.merge");

    _builder.CreateCondBr(condBool, thenBB, elseBB);

    _builder.SetInsertPoint(thenBB);
    auto trueVal = compileBranchResultNormalized(node->trueValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto thenEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto falseVal = compileBranchResultNormalized(node->falseValue(), resultType);
    _builder.CreateBr(mergeBB);
    auto elseEndBB = _builder.GetInsertBlock();

    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);

    auto phi = llvm::PHINode::Create(getLLVMType(resultType), 2, "if.result", mergeBB);
    phi->addIncoming(trueVal, thenEndBB);
    phi->addIncoming(falseVal, elseEndBB);

    if (resultType.isBox() || resultType.isArrayGeneric() || resultType.isWeak()) {
        recordTemp(phi, resultType);
    }
    return phi;
}

llvm::Value* Compiler::compileArrayGetExpr(p<ExprGetNode> node) {
    auto arrayExpr = node->arrayExpr();
    auto arrayType = arrayExpr->getType();

    auto& indices = node->indices();
    if (indices.empty()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3060);
    }

    DEBUG_LOG_VAL("    Expr: ArrayGet", arrayType.name);

    llvm::Value* currentPtr = nullptr;
    TypeInfo currentType = arrayType;

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(arrayExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(literalNode->literal())) {
            auto varName = objLiteral->getValue().getText();
            auto it = _localVarPtrs.find(varName);
            if (it == _localVarPtrs.end()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3033, varName);
            }
            currentPtr = it->second;
        }
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(arrayExpr)) {
        auto fieldPtr = compileExpr(dotNode);
        auto baseType = dotNode->baseExpr()->getType();
        auto member = dotNode->member();

        auto structDecl = _file->getStructDecl(baseType.name);
        if (structDecl) {
            int fieldIndex = structDecl->fieldIndex(member);
            if (fieldIndex >= 0) {
                auto alloca = _builder.CreateAlloca(getLLVMType(arrayType), nullptr, "array_field_tmp");
                _builder.CreateStore(fieldPtr, alloca);
                currentPtr = alloca;
            }
        }
    }

    if (!currentPtr) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3061);
    }

    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        if (!elemType) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3055);
        }

        auto elemLLVMType = getLLVMType(*elemType);
        auto handle = loadArrayHandle(currentPtr);
        auto dataPtr = _builder.CreateLoad(llvm::PointerType::get(_context, 0), arrayBlockDataFieldPtr(handle), "array.data.ptr");

        auto indexVal = compileExpr(indices[0]);
        auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {indexVal}, "array.elem.ptr");

        return _builder.CreateLoad(elemLLVMType, elemPtr, "array.elem.load");
    }

    if (!arrayType.isArray()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3062, arrayType.name);
    }

    for (auto& indexExpr : indices) {
        auto indexVal = compileExpr(indexExpr);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        llvm::Value* gepIndices[] = {zero, indexVal};

        auto llvmArrayType = getLLVMType(currentType);
        currentPtr = _builder.CreateGEP(llvmArrayType, currentPtr, gepIndices, "array.element");

        if (currentType.elementType) {
            currentType = *currentType.elementType;
        }
    }

    return _builder.CreateLoad(getLLVMType(currentType), currentPtr, "array.load");
}

// 编译元组构造表达式 (e1, e2, ...)
// Phase 3：透明 layout，按声明顺序构造一个匿名 struct 值；元素递归编译
// 实现：从 undef 起，逐个 CreateInsertValue 写入；返回 struct 值（非指针）
llvm::Value* Compiler::compileTupleExpr(p<ExprTupleNode> node) {
    auto tupleType = node->getType();
    auto llvmTy = getLLVMType(tupleType);
    if (!llvmTy) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3098,
                       tupleType.name, string("(tuple)"), string("(tuple)"));
    }
    DEBUG_LOG_VAL("    Expr: Tuple", tupleType.name);

    llvm::Value* aggr = llvm::UndefValue::get(llvmTy);
    auto& elems = node->elements();
    for (size_t i = 0; i < elems.size(); ++i) {
        auto elemVal = compileExpr(elems[i]);
        if (!elemVal) {
            throw YuxError(elems[i]->getLineNumber(), elems[i]->getColumn(), ErrorCode::E3091);
        }
        aggr = _builder.CreateInsertValue(aggr, elemVal, {static_cast<unsigned>(i)}, "tuple.ins");
    }
    return aggr;
}

llvm::Value* Compiler::compileArrayLiteralExpr(p<ExprArrayNode> node) {
    auto& elements = node->elements();
    auto arrayType = node->getType();
    auto llvmArrayType = getLLVMType(arrayType);

    DEBUG_LOG_VAL("    Expr: ArrayLiteral", arrayType.name);

    // Array<T> 字面量（动态数组）：分配 Block，写入元素，返回 { handle } 结构体值
    if (arrayType.isArrayGeneric()) {
        auto elemType = arrayType.arrayGenericElementType();
        auto elemLLVMType = elemType ? getLLVMType(*elemType) : _builder.getInt8Ty();
        auto count = elements.size();
        auto countVal = _builder.getInt64(count);

        auto block = allocArrayBlock(elemLLVMType, countVal, countVal);

        if (count > 0) {
            auto ptrTy = llvm::PointerType::get(_context, 0);
            auto dataPtr = _builder.CreateLoad(ptrTy, arrayBlockDataFieldPtr(block), "lit.data");
            for (size_t i = 0; i < count; ++i) {
                auto elemVal = compileExpr(elements[i]);
                auto idx = _builder.getInt64(i);
                auto elemPtr = _builder.CreateGEP(elemLLVMType, dataPtr, {idx}, "lit.elem.ptr");
                // Phase 3d: RC 元素从已有 var/field 读出再写入新槽位 → 复制语义 retain
                // Phase 8b: fresh 元素表达式（如 [make_box()]）已 +1，跳过 retain
                // Phase 8d.1: fresh 元素从临时帧消费
                if (elemType && typeNeedsDestructor(*elemType)) {
                    if (!isFreshHandleExpr(elements[i])) {
                        retainHandleAtCallSite(elemVal, *elemType);
                    } else {
                        consumeTemp(elemVal);
                    }
                }
                _builder.CreateStore(elemVal, elemPtr);
            }
        }

        auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.literal");
        storeArrayHandle(alloca, block);
        return _builder.CreateLoad(llvmArrayType, alloca, "array.literal.load");
    }

    // 固定大小数组 [N]T 字面量
    if (elements.empty()) {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3063);
    }

    auto alloca = _builder.CreateAlloca(llvmArrayType, nullptr, "array.literal");

    for (size_t i = 0; i < elements.size(); ++i) {
        auto elemVal = compileExpr(elements[i]);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto index = llvm::ConstantInt::get(_builder.getInt32Ty(), i);
        llvm::Value* indices[] = {zero, index};
        auto elemPtr = _builder.CreateGEP(llvmArrayType, alloca, indices, "array.elem.ptr");
        _builder.CreateStore(elemVal, elemPtr);
    }

    return _builder.CreateLoad(llvmArrayType, alloca, "array.load");
}

llvm::Value* Compiler::compileGetRefExpr(p<ExprGetRefNode> node) {
    auto objName = node->obj().getText();
    auto& subs = node->subs();

    DEBUG_LOG_VAL("    Expr: GetRef", objName);

    auto it = _localVarPtrs.find(objName);
    if (it == _localVarPtrs.end()) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode,
            node->getLineNumber(), node->getColumn(), ErrorCode::E3031, objName);
    }

    llvm::Value* currentPtr = it->second;
    auto sym = _currentFnNode->lookupSymbol(objName);
    if (!sym) {
        SymbolSuggest::throwSymbolNotFound(_currentFnNode,
            node->getLineNumber(), node->getColumn(), ErrorCode::E3030, objName);
    }

    TypeInfo currentType = sym->type;
    // Phase 4a: 若源是 T&（参数 / 局部 ref），currentPtr 已经是底层 T 的地址；剥到 T
    if (currentType.isRef()) {
        if (auto inner = currentType.refElementType()) currentType = *inner;
    }

    for (auto& sub : subs) {
        auto memberName = sub.getText();

        // Phase 4c: Box<T>.field —— 自动 deref：load handle，payload = handle + 8
        if (currentType.isBox()) {
            auto boxStructType = getLLVMType(currentType);
            auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto handleField = _builder.CreateGEP(boxStructType, currentPtr, {zero32, zero32}, "box.handle_field");
            auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "box.handle");
            currentPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "box.payload");
            if (auto inner = currentType.boxElementType()) currentType = *inner;
        }

        auto structDecl = _file->getStructDecl(currentType.name);
        if (!structDecl && _yux && _yux->sdkFile()) {
            structDecl = _yux->sdkFile()->getStructDecl(currentType.name);
        }
        if (!structDecl) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3041, currentType.name);
        }

        int fieldIndex = structDecl->fieldIndex(memberName);
        if (fieldIndex < 0) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3040, currentType.name, memberName);
        }

        auto field = structDecl->fields()[fieldIndex];
        if (field->isPrivate()) {
            string currentBase = _currentStructName;
            auto dollarPos = currentBase.find('$');
            if (dollarPos != string::npos) currentBase = currentBase.substr(0, dollarPos);
            if (currentBase != currentType.name) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3042, memberName, currentType.name);
            }
        }

        auto structType = getLLVMType(currentType);
        auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
        auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
        llvm::Value* indices[] = {zero, idx};

        currentPtr = _builder.CreateGEP(structType, currentPtr, indices, "struct.field.ptr");
        TypeInfo fieldType = field->getType();
        if (currentType.isGeneric() && structDecl->isGeneric()
            && currentType.genericArgs.size() == structDecl->typeParams().size()) {
            map<string, TypeInfo> subst;
            for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                subst[structDecl->typeParams()[i]] =
                    currentType.genericArgs[i] ? *currentType.genericArgs[i] : TypeInfo();
            }
            fieldType = fieldType.substitute(subst);
        }
        currentType = fieldType;
    }

    return currentPtr;
}

llvm::Value* Compiler::compileUnaryExpr(p<ExprUnaryNode> node) {
    auto type = node->getType();
    auto rightType = node->right()->getType();

    string opStr;
    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        opStr = "-";
        break;
    case ExprUnaryNode::Op::Rev:
        opStr = "~";
        break;
    case ExprUnaryNode::Op::Not:
        opStr = "!";
        break;
    }
    DEBUG_LOG_VAL("    Expr: Unary", opStr << " : " << type.name);

    // 检查是否为自定义类型
    if (!isBuiltinType(rightType.name)) {
        string methodName;
        switch (node->op()) {
        case ExprUnaryNode::Op::Neg: methodName = "neg"; break;
        case ExprUnaryNode::Op::Rev: methodName = "inv"; break;
        case ExprUnaryNode::Op::Not: methodName = "not"; break;
        }
        return compileCustomTypeUnaryOp(node->right(), rightType, methodName, node->getLineNumber());
    }

    // 内置类型：直接生成 LLVM IR
    auto right = compileExpr(node->right());
    bool isFloat = type.startsWith('f');
    bool isBool = type.name == "bool";

    switch (node->op()) {
    case ExprUnaryNode::Op::Neg:
        if (isFloat) {
            return _builder.CreateFNeg(right, "neg");
        }
        return _builder.CreateNeg(right, "neg");
    case ExprUnaryNode::Op::Rev:
        if (isFloat) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3070, type.name);
        }
        return _builder.CreateNot(right, "not");
    case ExprUnaryNode::Op::Not:
        if (!isBool) {
            throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3071, type.name);
        }
        return _builder.CreateNot(right, "lnot");
    }

    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3072);
}

llvm::Value* Compiler::compileDotExpr(p<ExprDotNode> node) {
    // 安全访问 a?.b：单独走分支
    if (node->isSafe()) {
        return compileSafeDotExpr(node);
    }
    auto baseExpr = node->baseExpr();
    auto member = node->member();

    // 元组成员访问 a.N：member 为纯数字，base 解析后必须是 Tuple
    // 透明 alias 由 applySubst 兜底（顶层 Normal alias、泛型 alias 实例化均能展开）
    if (!member.empty() && std::all_of(member.begin(), member.end(),
                                       [](char c) { return c >= '0' && c <= '9'; })) {
        auto baseTypeRaw = baseExpr->getType();
        auto baseTypeResolved = applySubst(baseTypeRaw);
        if (baseTypeResolved.isTuple()) {
            auto& elems = baseTypeResolved.tupleElements();
            size_t idx = static_cast<size_t>(std::stoul(member));
            if (idx >= elems.size()) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3100,
                               member, baseTypeRaw.getFullName(),
                               std::to_string(elems.size()));
            }
            DEBUG_LOG_VAL("    Expr: TupleMemberAccess", baseTypeResolved.name << "." << member);
            auto baseVal = compileExpr(baseExpr);
            if (!baseVal) {
                throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
            }
            return _builder.CreateExtractValue(baseVal, {static_cast<unsigned>(idx)}, "tuple.elem");
        }
        // 非 Tuple 的 .N 留给后续逻辑（当前会落到 E3090 报"Unsupported dot expression"）
    }

    if (member.starts_with("to_")) {
        string dstType = member.substr(3);
        DEBUG_LOG_VAL("    Expr: DotCast", member);
        auto baseVal = compileExpr(baseExpr);
        auto srcType = baseExpr->getType();

        string castFnName = "__cast_" + to_string(_castCounter++);
        _castFunctions[castFnName] = {baseVal, srcType, TypeInfo(dstType)};

        return llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    }

    auto baseType = baseExpr->getType();
    TypeInfo actualType = baseType;
    llvm::Value* structPtr = nullptr;

    if (baseType.isRef()) {
        auto refElemType = baseType.refElementType();
        if (refElemType) {
            actualType = *refElemType;
        }
    }

    if (baseType.isBox()) {
        auto boxElemType = baseType.boxElementType();
        if (boxElemType) {
            actualType = *boxElemType;
        }
    }

    auto structDecl = _file->getStructDecl(actualType.name);
    if (!structDecl && _yux && _yux->sdkFile()) {
        structDecl = _yux->sdkFile()->getStructDecl(actualType.name);
    }

    if (structDecl) {
        int fieldIndex = structDecl->fieldIndex(member);
        if (fieldIndex >= 0) {
            DEBUG_LOG_VAL("    Expr: StructFieldAccess", actualType.name << "." << member);

            auto field = structDecl->fields()[fieldIndex];
            if (field->isPrivate()) {
                string currentBase = _currentStructName;
                auto dollarPos = currentBase.find('$');
                if (dollarPos != string::npos) currentBase = currentBase.substr(0, dollarPos);
                if (currentBase != actualType.name) {
                    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3042, member, actualType.name);
                }
            }

            if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
                if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
                    auto varName = objLiteral->getValue().getText();
                    auto it = _localVarPtrs.find(varName);
                    if (it != _localVarPtrs.end()) {
                        structPtr = it->second;
                    }
                }
            }

            if (!structPtr) {
                auto baseVal = compileExpr(baseExpr);
                auto tmpAlloca = _builder.CreateAlloca(getLLVMType(baseType), nullptr, "struct_field_tmp");
                _builder.CreateStore(baseVal, tmpAlloca);
                structPtr = tmpAlloca;
            }

            llvm::Value* dataPtr = structPtr;

            if (baseType.isBox()) {
                // Box.field：load handle，payload = handle + 8
                auto boxStructType = getLLVMType(baseType);
                auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
                auto handleField = _builder.CreateGEP(boxStructType, structPtr, {zero, zero}, "box.handle_field");
                auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "box.handle");
                dataPtr = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "box.payload");
            }

            auto structType = getLLVMType(actualType);
            auto zero = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
            auto idx = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIndex);
            llvm::Value* indices[] = {zero, idx};

            auto fieldPtr = _builder.CreateGEP(structType, dataPtr, indices, "struct.field");
            auto fieldType = field->getType();

            if (actualType.isGeneric() && structDecl->isGeneric()
                && actualType.genericArgs.size() == structDecl->typeParams().size()) {
                map<string, TypeInfo> subst;
                for (size_t i = 0; i < structDecl->typeParams().size(); ++i) {
                    subst[structDecl->typeParams()[i]] =
                        actualType.genericArgs[i] ? *actualType.genericArgs[i] : TypeInfo();
                }
                fieldType = fieldType.substitute(subst);
            }

            return _builder.CreateLoad(getLLVMType(fieldType), fieldPtr, "field.load");
        }
    }

    if (auto baseLiteral = dynamic_cast<ExprLiteralNode*>(baseExpr)) {
        if (auto objLiteral = dynamic_cast<LiteralObjNode*>(baseLiteral->literal())) {
            auto objName = objLiteral->getValue().getText();
            auto sym = _currentFnNode->lookupSymbol(objName);
            if (sym && _localVarPtrs.contains(objName)) {
                DEBUG_LOG_VAL("    Expr: DotMemberLoad", objName << "." << member);
                return _builder.CreateLoad(getLLVMType(sym->type), _localVarPtrs[objName]);
            }
        }
    }
    throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3090);
}

// 编译 a?.b 安全成员访问
// 语义：a 是 Nullable<T>，T 是结构体，b 是 T 的字段
//   - a 持值 → Nullable<U>{ has=true, value=a._value.b }
//   - a 不持值 → Nullable<U>{ has=false, value=zero }
// 当前实现仅覆盖字段访问；方法调用形式 a?.foo() 不在本阶段
// 链式 a?.b?.c 自然递归（每层 base 类型为 Nullable<X>，仍走同分支）
llvm::Value* Compiler::compileSafeDotExpr(p<ExprDotNode> node) {
    auto baseExpr = node->baseExpr();
    auto member = node->member();
    auto baseType = baseExpr->getType();

    if (!baseType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3025, baseType.name);
    }
    auto innerType = baseType.nullableInnerType();
    // Phase 5: Box<T>? 自动 deref —— 把 Box<U> 视为 U 进字段查
    bool innerIsBox = innerType->isBox();
    auto rawInnerType = innerType;  // Box<U>（用于 LLVM 类型 = { ptr handle }）
    if (innerIsBox) {
        auto boxInner = innerType->boxElementType();
        if (!boxInner) {
            throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3050);
        }
        innerType = boxInner;
    }
    auto innerStructDecl = _file->getStructDecl(innerType->name);
    if (!innerStructDecl && _yux && _yux->sdkFile()) {
        innerStructDecl = _yux->sdkFile()->getStructDecl(innerType->name);
    }
    if (!innerStructDecl) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3044, innerType->name);
    }
    int fieldIdx = innerStructDecl->fieldIndex(member);
    if (fieldIdx < 0) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3040, innerType->name, member);
    }
    auto fieldType = innerStructDecl->fields()[fieldIdx]->getType();
    if (innerType->isGeneric() && innerStructDecl->isGeneric()
        && innerType->genericArgs.size() == innerStructDecl->typeParams().size()) {
        map<string, TypeInfo> subst;
        for (size_t i = 0; i < innerStructDecl->typeParams().size(); ++i) {
            subst[innerStructDecl->typeParams()[i]] =
                innerType->genericArgs[i] ? *innerType->genericArgs[i] : TypeInfo();
        }
        fieldType = fieldType.substitute(subst);
    }

    // 结果类型 Nullable<U>
    vector<sp<TypeInfo>> nullArgs;
    nullArgs.push_back(make_shared<TypeInfo>(fieldType));
    TypeInfo resultType("Nullable", nullArgs);

    auto baseLLVMType = getLLVMType(baseType);     // Nullable<T> struct
    auto innerLLVMType = getLLVMType(*innerType);  // T struct
    auto fieldLLVMType = getLLVMType(fieldType);   // U
    auto resultLLVMType = getLLVMType(resultType); // Nullable<U> struct

    auto baseVal = compileExpr(baseExpr);
    auto hasVal = _builder.CreateExtractValue(baseVal, {0}, "sd.base.has");
    auto innerVal = _builder.CreateExtractValue(baseVal, {1}, "sd.base.inner");

    auto resultAlloca = _builder.CreateAlloca(resultLLVMType, nullptr, "sd.result");
    auto zero32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 0);
    auto one32 = llvm::ConstantInt::get(_builder.getInt32Ty(), 1);
    auto resHasField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, zero32}, "sd.res.has");
    auto resValueField = _builder.CreateGEP(resultLLVMType, resultAlloca, {zero32, one32}, "sd.res.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "sd.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "sd.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "sd.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    // then: 取出 inner 的字段，包装到 result
    _builder.SetInsertPoint(thenBB);
    llvm::Value* fieldPtr = nullptr;
    auto fieldIdxConst = llvm::ConstantInt::get(_builder.getInt32Ty(), fieldIdx);
    if (innerIsBox) {
        // Box<U>: 提取 handle，payload = handle + 8，GEP 到字段
        auto rawInnerLLVMTy = getLLVMType(*rawInnerType);  // Box struct { ptr handle }
        auto boxAlloca = _builder.CreateAlloca(rawInnerLLVMTy, nullptr, "sd.box.tmp");
        _builder.CreateStore(innerVal, boxAlloca);
        auto handleField = _builder.CreateGEP(rawInnerLLVMTy, boxAlloca, {zero32, zero32}, "sd.box.handle_field");
        auto handle = _builder.CreateLoad(llvm::PointerType::get(_context, 0), handleField, "sd.box.handle");
        auto payload = _builder.CreateGEP(_builder.getInt8Ty(), handle, {_builder.getInt64(8)}, "sd.box.payload");
        fieldPtr = _builder.CreateGEP(innerLLVMType, payload, {zero32, fieldIdxConst}, "sd.field");
    } else {
        auto innerAlloca = _builder.CreateAlloca(innerLLVMType, nullptr, "sd.inner.tmp");
        _builder.CreateStore(innerVal, innerAlloca);
        fieldPtr = _builder.CreateGEP(innerLLVMType, innerAlloca, {zero32, fieldIdxConst}, "sd.field");
    }
    auto fieldVal = _builder.CreateLoad(fieldLLVMType, fieldPtr, "sd.field.load");
    _builder.CreateStore(_builder.getInt1(true), resHasField);
    _builder.CreateStore(fieldVal, resValueField);
    _builder.CreateBr(mergeBB);

    // else: 空 Nullable<U>
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    _builder.CreateStore(_builder.getInt1(false), resHasField);
    _builder.CreateStore(llvm::Constant::getNullValue(fieldLLVMType), resValueField);
    _builder.CreateBr(mergeBB);

    // merge: load result 作为 ssa 值
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    return _builder.CreateLoad(resultLLVMType, resultAlloca, "sd.result.load");
}

// 编译 a ?? b 表达式
// 语义：a 是 Nullable<T>。a 持值则结果取 a._value，否则取 b（b 必须可转 T）
// IR 形态：
//   %has = extractvalue %a, 0
//   %v   = extractvalue %a, 1
//   br %has, then, else
//   then: br merge (carry %v)
//   else: %r = compile(b); br merge (carry %r)
//   merge: phi T [%v, then] [%r, else]
llvm::Value* Compiler::compileNullElseExpr(p<ExprNullElseNode> node) {
    auto leftType = node->left()->getType();
    if (!leftType.isNullable()) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3024, leftType.name);
    }
    auto innerType = leftType.nullableInnerType();
    if (!innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(), ErrorCode::E3051);
    }
    auto innerLLVMType = getLLVMType(*innerType);

    // flexible int 推断：右侧无后缀整数 → 推断为 T
    if (isIntTypeName(innerType->name) && isFlexibleIntExpr(node->right())) {
        tryInferIntType(node->right(), *innerType);
    }

    // 计算左侧（Nullable 结构体值）
    auto leftVal = compileExpr(node->left());
    auto hasVal = _builder.CreateExtractValue(leftVal, {0}, "ne.has");
    auto valueVal = _builder.CreateExtractValue(leftVal, {1}, "ne.value");

    llvm::Function* func = _builder.GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(_context, "ne.then", func);
    auto elseBB = llvm::BasicBlock::Create(_context, "ne.else");
    auto mergeBB = llvm::BasicBlock::Create(_context, "ne.merge");

    _builder.CreateCondBr(hasVal, thenBB, elseBB);

    bool isRcHandle = innerType->isBox() || innerType->isArrayGeneric() || innerType->isWeak();

    // then: 持值，直接用 _value（这是从 Nullable struct 抽出的借用，需 retain 归一为 +1）
    _builder.SetInsertPoint(thenBB);
    if (isRcHandle) {
        emitRetainOnHandleValue(valueVal, *innerType);
    }
    auto thenEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // else: 取右侧默认值（用子帧 + 归一）
    func->insert(func->end(), elseBB);
    _builder.SetInsertPoint(elseBB);
    auto rightType = node->right()->getType();
    auto rightVal = compileBranchResultNormalized(node->right(), *innerType);
    if (rightType != *innerType) {
        throw YuxError(node->resolveLineNumber(), node->resolveColumn(),
            ErrorCode::E3023, rightType.name, innerType->name);
    }
    auto elseEndBB = _builder.GetInsertBlock();
    _builder.CreateBr(mergeBB);

    // merge: phi 合并
    func->insert(func->end(), mergeBB);
    _builder.SetInsertPoint(mergeBB);
    auto phi = _builder.CreatePHI(innerLLVMType, 2, "ne.result");
    phi->addIncoming(valueVal, thenEndBB);
    phi->addIncoming(rightVal, elseEndBB);
    // Phase 8d.3: 两支已归一 +1，phi 作 fresh 句柄登记外层
    if (isRcHandle) {
        recordTemp(phi, *innerType);
    }
    return phi;
}

llvm::Value* Compiler::compileExpr(p<ExprNode> node) {
    auto type = node->getType();
    DEBUG_LOG_VAL("  compileExpr", "type=" << (type.empty() ? "void" : type.name));

    if (auto literalNode = dynamic_cast<ExprLiteralNode*>(node)) {
        return compileLiteralExpr(literalNode);
    } else if (auto addSubNode = dynamic_cast<ExprAddSubNode*>(node)) {
        return compileAddSubExpr(addSubNode);
    } else if (auto mulDivModNode = dynamic_cast<ExprMulDivModNode*>(node)) {
        return compileMulDivModExpr(mulDivModNode);
    } else if (auto binOpNode = dynamic_cast<ExprBinOpNode*>(node)) {
        return compileBinOpExpr(binOpNode);
    } else if (auto parenNode = dynamic_cast<ExprParenNode*>(node)) {
        return compileParenExpr(parenNode);
    } else if (auto callNode = dynamic_cast<ExprCallNode*>(node)) {
        // Phase 8d.1: 调用结果若为 fresh RC 句柄（Box/Array/Weak），登记到当前语句临时帧
        auto val = compileCallExpr(callNode);
        if (val) {
            recordTemp(val, type);
        }
        return val;
    } else if (auto dotNode = dynamic_cast<ExprDotNode*>(node)) {
        return compileDotExpr(dotNode);
    } else if (auto compareNode = dynamic_cast<ExprCompareNode*>(node)) {
        return compileCompareExpr(compareNode);
    } else if (auto ifElseNode = dynamic_cast<ExprIfElseNode*>(node)) {
        return compileIfElseExpr(ifElseNode);
    } else if (auto oneLineIfElseNode = dynamic_cast<ExprOneLineIfElseNode*>(node)) {
        return compileOneLineIfElseExpr(oneLineIfElseNode);
    } else if (auto ifElsePreValueNode = dynamic_cast<ExprIfElsePreValueNode*>(node)) {
        return compileIfElsePreValueExpr(ifElsePreValueNode);
    } else if (auto getNode = dynamic_cast<ExprGetNode*>(node)) {
        return compileArrayGetExpr(getNode);
    } else if (auto arrayNode = dynamic_cast<ExprArrayNode*>(node)) {
        // Phase 8d.1: 数组字面量 _array_alloc 给 strong=1，登记为 fresh 临时
        auto val = compileArrayLiteralExpr(arrayNode);
        if (val) {
            recordTemp(val, type);
        }
        return val;
    } else if (auto tupleNode = dynamic_cast<ExprTupleNode*>(node)) {
        return compileTupleExpr(tupleNode);
    } else if (auto getRefNode = dynamic_cast<ExprGetRefNode*>(node)) {
        return compileGetRefExpr(getRefNode);
    } else if (auto unaryNode = dynamic_cast<ExprUnaryNode*>(node)) {
        return compileUnaryExpr(unaryNode);
    } else if (auto nullElseNode = dynamic_cast<ExprNullElseNode*>(node)) {
        return compileNullElseExpr(nullElseNode);
    } else if (auto arrayInitNode = dynamic_cast<ExprArrayInitNode*>(node)) {
        // 数组填充表达式需要类型注解，这里返回 nullptr
        // 实际处理在 compileDeclareAssignStatement 中
        return nullptr;
    } else {
        throw YuxError(node->getLineNumber(), node->getColumn(), ErrorCode::E3091);
    }
}

void Compiler::compileStatementBlock(p<StatementBlockNode> block) {
    DEBUG_LOG_VAL(
        "  compileStatementBlock", block->statements().size() << " statements, hasResult=" << block->hasResult());
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }
    if (block->hasResult()) {
        DEBUG_LOG("    Compiling result expression");
        compileExpr(block->resultExpr());
    }
}

llvm::Value* Compiler::compileStatementBlockWithResult(
    p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType) {
    for (auto& stmt : block->statements()) {
        compileStatement(stmt);
    }

    if (_builder.GetInsertBlock()->getTerminator()) {
        return nullptr;
    }

    if (block->hasResult()) {
        // Phase 8d.3: RC 句柄分支结果走子帧 + 归一 retain；非 RC 沿用旧行为
        auto resultVal = compileBranchResultNormalized(block->resultExpr(), resultType);
        if (phi && !resultType.empty()) {
            phi->addIncoming(resultVal, _builder.GetInsertBlock());
        }
    }

    _builder.CreateBr(continueBlock);
    return nullptr;
}
