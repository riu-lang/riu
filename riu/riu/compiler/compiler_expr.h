// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 表达式发射：compileExpr / 各节点 IR / lambda / 块结果。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // ==================== 表达式编译 ====================
    llvm::Value* compileExpr(ExprNode* node);                   // 编译表达式 (主入口，accept 分派)
    llvm::Value* compileStructLitExpr(ExprStructLitNode* node); // Self / TypeName { ... } 字面量
    // Phase 2.4 / 1.7：codegen 读类型的统一入口。
    // 无 subst 时优先读 Sema 槽，否则 getType()。泛型 subst 帧用 structuralType()
    // 再 applySubst，避免复用模板 AST 时槽停留在上一实例。
    [[nodiscard]] TypeInfo resolvedOrInferredType(ExprNode* node) const;
    llvm::Value* compileArrayInitExpr(ExprArrayInitNode* node, const TypeInfo& targetType,
                                      llvm::Value* destPtr = nullptr);                           // 编译数组初始化表达式
    llvm::Value* createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType); // 创建类型转换

    // ==================== 语句块编译 ====================
    void compileStatementBlock(StatementBlockNode* block); // 编译语句块 (无返回值)
    llvm::Value* compileStatementBlockWithResult(StatementBlockNode* block, llvm::BasicBlock* continueBlock,
                                                 llvm::PHINode* phi,
                                                 const TypeInfo& resultType); // 编译语句块 (有返回值)

    // ==================== 表达式编译 (具体类型) ====================
    llvm::Value* compileLiteralExpr(ExprLiteralNode* node); // 编译字面量表达式
    llvm::Value* emitStringLiteralValue(
        const vector<u32>& codePoints); // B-4: 由码点向量发射 sentinel Rc<Array<u32>> 包装的 String 值
    // B-4: 由码点向量发射 sentinel RC Block 全局常量。
    // Block layout: { u32 strong(0xFFFFFFFF), u32 weak(0), ptr _data, usize _len, usize _cap }
    // 不需 _builder, 可在 builder 未设当前 BB 时调用 (供 reflect rodata 节点 emit 复用).
    llvm::GlobalVariable* emitStringRcBlockConst(const vector<u32>& codePoints);
    // DRAFT-spec-reflect Phase 3a: lazy emit reflect rodata globals.
    // Returns the Type global; optionally returns the [N x ptr] fields ref array via outFieldsRefs.
    // 由 `__riu_reflect_type:<T>()` intrinsic 与 `<Struct>::fields` 调用站调用.
    // 仅对 Normal 用户 / SDK struct 类型 emit; 找不到 owner 返回 nullptr.
    llvm::GlobalVariable* ensureReflectTypeGlobal(const TypeInfo& t, llvm::GlobalVariable** outFieldsRefs = nullptr);
    llvm::Value*
    compileStringTemplate(StringTemplateNode* node); // v0.6 Phase 2a：StringTemplateNode → StringBuilder lower
    llvm::Value* compileStringPlusChain(
        ExprAddSubNode* node); // v0.6 Phase 2c：连续 String + ... 整链 lower 为单条 StringBuilder 累加
    llvm::Value* compileAddSubExpr(ExprAddSubNode* node);               // 编译加减表达式
    llvm::Value* compileMulDivModExpr(ExprMulDivModNode* node);         // 编译乘除取模表达式
    llvm::Value* compileBinOpExpr(ExprBinOpNode* node);                 // 编译位运算表达式
    llvm::Value* compileParenExpr(ExprParenNode* node);                 // 编译括号表达式
    llvm::Value* compileCallExpr(ExprCallNode* node);                   // 编译函数调用表达式
    llvm::Value* compileDotExpr(ExprDotNode* node);                     // 编译成员访问表达式
    llvm::Value* compileCompareExpr(ExprCompareNode* node);             // 编译比较表达式
    llvm::Value* compileIfElseExpr(ExprIfElseNode* node);               // 编译 if-else 表达式
    llvm::Value* compileOneLineIfElseExpr(ExprOneLineIfElseNode* node); // 编译单行 if-else 表达式
    llvm::Value* compileArrayGetExpr(ExprGetNode* node);                // 编译数组索引表达式
    llvm::Value* compileArrayLiteralExpr(ExprArrayNode* node);          // 编译数组字面量表达式
    llvm::Value* compileTupleExpr(ExprTupleNode* node);                 // 编译元组构造表达式 (e1, e2, ...)
    llvm::Value* compileEnumCtorExpr(ExprPathCallNode* node);           // 编译枚举构造表达式 E::V / E::V(args)
    // Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
    // Phase 1c：仅 emit { vtable=null, data=src.handle } 占位 fat ptr，
    // 真 vtable 与对象安全检查留 Phase 2/3
    llvm::Value* compileDynCtorExpr(class ExprDynCtorNode* node);
    llvm::Value* compileMatchExpr(ExprMatchNode* node); // Phase 6: 编译 match 表达式（switch on tag + 绑定 + arm 体）
    llvm::Value* compileTryCatchExpr(
        ExprTryCatchNode* node); // Phase 10f: 编译 try-catch 表达式（10f 仅占位 + 语义校验，IR 路由推 10g）

    // ==================== Lambda（spec §4 / Phase 2b：零捕获） ====================
    // compileLambdaExpr：把 LambdaExprNode 编译为 16 字节 fat-ptr 值 { fn_ptr, captures }；
    // captures 永远为 null（捕获留给 Phase 4）。底层 Function 由 emitLambdaFunction 生成。
    llvm::Value* compileLambdaExpr(class LambdaExprNode* node);
    // emitLambdaFunction：取 LambdaExprNode + 期望 Fn 类型，按 captures-leading ABI
    // (Ptr captures, P1, ..., Pn) → R 生成顶层 LLVM Function。
    // expectedFnType 用于回填实例化后的形参类型（lambda 形参可省类型；调用者必须先反推）。
    // 同 (node, mangledName) 已生成则直接返回缓存。
    llvm::Function* emitLambdaFunction(class LambdaExprNode* node, const TypeInfo& expectedFnType);
    // Phase 4a-2：为含堆句柄 captures 的 lambda 合成析构函数。
    // 签名 void __captures_dtor_<mangle>(ptr fields_base)，逐 capture 字段调 releaseAtPtr。
    // 全标量 captures（无字段需 release）→ 返回 nullptr，调用方在 dtor 槽存 null。
    llvm::Function* emitCapturesDtorFunction(class LambdaExprNode* node, const string& lambdaMangled);
    // 调用 fn-typed 值：从 fat-ptr 提取 fn_ptr / captures，按 ABI 调用
    llvm::Value* compileFnValueCall(ExprCallNode* node);
    // Phase 3c：callee 为 Rc<fn(...)R>，从 Rc payload load fat-ptr 后按同款 ABI 调用
    // innerFnType 为 Rc 元素类型（Fn TypeInfo），用于实参反推 / 形参类型 / 返回类型
    llvm::Value* compileRcFnValueCall(ExprCallNode* node, const TypeInfo& innerFnType);
    // v0.16：callee 为 Ref<fn(...)R>（如 arr[i] 返回 fn&），Load 引用得 fat-ptr 后按同款 ABI 调用
    // innerFnType 为 Ref 元素类型（Fn TypeInfo），用于实参反推 / 形参类型 / 返回类型
    llvm::Value* compileRefFnValueCall(ExprCallNode* node, const TypeInfo& innerFnType);
    // fn-value 实参：形参是 T& 时与 named-fn 同款（本帧指针 / 已是 ptr / alloca 临时），
    // 避免 compileExpr 把 T& 自动解引用成 T 后与 fat-ptr ABI 对不上。
    llvm::Value* compileFnValueArg(ExprNode* arg, const TypeInfo* expected);
    // 实参位置 lambda：把期望 Fn 类型写到 inferredFnType，并回填 bodyScope 未标注形参。
    // 泛型调用须在 typeArgs 替换之后再调，避免 Function<T,...> 进入 emitLambdaFunction。
    void inferLambdaParamsFromFnType(class LambdaExprNode* lambda, const TypeInfo& expectedFnType);
    // fn-value 调用：Fn TypeInfo → LLVM 返回类型（含 T ! E ABI 包装）
    llvm::Type* llvmRetTypeForFnValue(const TypeInfo& fnType);
    // fn-value 调用完成后走 handleFallibleCallResult
    llvm::Value* finishFnValueFallibleCall(llvm::Value* callResult, const TypeInfo& fnType, ExprCallNode* callNode);
    llvm::Value* compileGetRefExpr(ExprGetRefNode* node);         // 编译取引用表达式
    llvm::Value* compileUnaryExpr(ExprUnaryNode* node);           // 编译一元表达式
    llvm::Value* compileNullElseExpr(ExprNullElseNode* node);     // 编译 a ?? b：a 持值则取 a.get()，否则取 b
    llvm::Value* compileMoveAssignExpr(ExprMoveAssignNode* node); // 编译 a <- b：移出旧值、替换新值、返回旧值
    llvm::Value* compileLvalueAddr(ExprNode* node);               // 取 lvalue 表达式的地址 (alloca/GEP)
    llvm::Value* compileSafeDotExpr(ExprDotNode* node); // 编译 a?.b：a 持值则包一层 Nullable<a.get().b>，否则空

    // ==================== 自定义类型运算符方法调用 ====================
    llvm::Value* compileCustomTypeBinaryOp(ExprNode* leftExpr, ExprNode* rightExpr, const TypeInfo& leftType,
                                           const string& methodName, int lineNum); // 编译自定义类型二元运算符
    llvm::Value* compileCustomTypeUnaryOp(ExprNode* expr, const TypeInfo& type, const string& methodName,
                                          int lineNum); // 编译自定义类型一元运算符
// clang-format on
#endif
