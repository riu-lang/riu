// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 所有权子系统：三个入口 + 临时帧 + move 集合。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // Phase B-1: move 语义 — 已被 move 的变量名集合 (不可再访问，析构时跳过)
    set<string> _movedVars;

    // ==================== OwnershipOps（三个入口）====================
    // 复制语义：fresh → consumeTemp；否则 typeNeedsDestructor 则 retain。
    // 调用点走下面三个；retainHandleAtCallSite / consumeTemp / isFreshHandleExpr
    // 是本层实现细节（深拷循环 / 内建 / 临时帧仍可直接用）。
    enum class SlotStore : u8 { Init, Replace };
    void takeOwnership(llvm::Value* val, const TypeInfo& type, ExprNode* expr);
    void storeIntoSlot(llvm::Value* slotPtr, llvm::Value* val, const TypeInfo& type, ExprNode* expr, SlotStore kind);
    void passAsArg(llvm::Value* val, const TypeInfo& type, ExprNode* expr);
    // T& 形参要底层 T*。compileExpr 对 T& 标识符会自解成 T 值，不能再把该值当指针传入。
    llvm::Value* pointerForRefParam(ExprNode* argExpr, llvm::Value* compiledVal);
    // 按形参 ABI：T& → pointerForRefParam；struct 指针 ABI → alloca 暂存；其余原值。
    llvm::Value* abiValueForParam(ExprNode* argExpr, llvm::Value* compiledVal, const TypeInfo& formal);
    // retainHandle=true：Rc/Weak（及非 Fallible 的 Fn）走 takeOwnership，返回 true。
    // 否则需析构时：fresh → consumeTemp；非 fresh 可拷 struct → retain 并返回 true
    // （跳过 peephole；#NoCopy / Array 仍 false，靠 peephole 移出局部）。
    bool returnValue(llvm::Value* val, const TypeInfo& type, ExprNode* expr, bool retainHandle);

    // Phase 3a: callee-clean 调用约定
    // 给 Rc/Array/Weak 实参在传入前 retain；callee 末尾析构 release 抵消
    // 非堆句柄类型 no-op；返回 true 表示已发出 retain
    bool retainHandleAtCallSite(llvm::Value* argVal, const TypeInfo& argType);

    // Phase 3c.2.a: struct value 内逐 RC 字段（嵌套 struct 递归）retain
    void retainStructFieldsAtCallSite(llvm::Value* argVal, const string& structName);

    // Phase 3c.2.b: copy_of 专用 — 深拷 struct 所有字段，含 Heap 新分配 + Dyn retain
    // 返回可能被 InsertValue 替换了 Heap 指针的新 struct value
    llvm::Value* copyOfStructFields(llvm::Value* structVal, const string& structName);
    // 拥有型拷贝：Array 走 clone（递归元素）；其余 retain + 结构体 Heap/Array 字段深拷。
    llvm::Value* copyOwnedValue(llvm::Value* val, const TypeInfo& type);
    llvm::Value* cloneArrayAtPtr(llvm::Value* arrayPtr, const TypeInfo& arrayType);
    llvm::Value* cloneArrayValue(llvm::Value* arrayVal, const TypeInfo& arrayType);

    // Phase 3d.3: 若 expr 是 `Heap<T>?` 的 lvalue (局部 ID 或 局部 struct 的字段访问),
    // 返回其 slot ptr + slot llvm 类型. 供 B 档 nullable move 在调用点 / struct-lit
    // 把源槽写回 {has=false, value=null} 用. 不支持 Rc / ref base 的字段 (后续切片).
    bool tryHeapNullableLvalueSlot(ExprNode* expr, llvm::Value*& outSlot, llvm::Type*& outTy);

    // Phase 8d.1: per-statement 临时清单
    // 栈帧式追踪 fresh RC 句柄（Rc/Array/Weak）；语句开始 pushTempFrame，
    // 结束 popAndReleaseTempFrame 对未消费项发出 release 调用。
    // 仅覆盖线性控制流；分支汇合（if-else 表达式作为语句）见 8d.3。
    struct PendingTemp {
        llvm::Value* val;
        TypeInfo type;
        // Phase 8d.4: 含 RC 字段 struct value 临时落 entry 块 alloca；
        // 帧弹出时调 releaseAtPtr 释放。Rc/Array/Weak 不用此字段（直接 extractValue 拿 handle）。
        llvm::Value* spillSlot = nullptr;
    };
    vector<vector<PendingTemp>> _tempStack;
    void pushTempFrame();
    void popAndReleaseTempFrame();
    void recordTemp(llvm::Value* val, const TypeInfo& type);
    // 返回 true 表示在顶帧中找到并移除了该 Value（即调用方能由此推断 val 是 fresh）
    bool consumeTemp(llvm::Value* val);
    // Phase 8d.3: 在指定 BB 末尾对一个 RC 句柄 value 发 retain（用于分支汇合归一为 fresh）
    void emitRetainOnHandleValue(llvm::Value* val, const TypeInfo& type);
    // Phase 8d.3: 编译"分支结果表达式"——始终 push 子帧、compile、pop 释放中间临时
    // （void 尾调用里的模板插值 String 也要在本支释放，不能留到 merge）。
    // 若 expectedType 需析构：fresh → consumeTemp；非 fresh → retain 归一为 +1。
    // 调用方在 phi 汇合后应 recordTemp(phi, expectedType) 把统一 +1 句柄交给外层 statement frame
    llvm::Value* compileBranchResultNormalized(ExprNode* expr, const TypeInfo& expectedType);

    // Phase 8b: 识别 +1 所有权（"fresh"）表达式
    // - 函数 / 方法 / 构造器调用：callee 已在 ret 处 move-return retain，结果是 +1 所有权句柄
    // - 数组字面量：_array_alloc 给 strong=1
    // - 其他（变量引用 / 字段访问 / if-else / ?? 等）：视作借用，复制语义需 retain
    // 用于在 declare-assign / assign / 字面量元素写入等"复制语义"路径上跳过多余 retain
    bool isFreshHandleExpr(ExprNode* expr);
// clang-format on
#endif
