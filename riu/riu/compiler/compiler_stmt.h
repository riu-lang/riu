// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 语句发射：loop / break / 声明赋值 / try-catch 栈。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // ==================== 控制流 ====================
    // labeled break：每条记录 = (label 文本, 退出 BB, 进入 loop 前的帧深度)；空 label = 无标签 loop
    struct LoopExitInfo {
        string label;
        llvm::BasicBlock* exitBB;
        llvm::BasicBlock* continueBB = nullptr; // 下一轮入口（loop=condBB；for-in=incBB）
        size_t frameDepthBeforeLoop = 0;        // break 时 unwind 到此深度（含销毁 loop-init 帧）
        size_t frameDepthBeforeBody = 0;        // continue 时 unwind 到此深度（保留 init）
    };
    vector<LoopExitInfo> _loopExitBlocks; // 循环退出块栈 (用于 break / continue / @label)

    // DRAFT-错误.md [#4.H]：try-catch 块栈
    // 进入 try block 编译时压栈（入栈对象 = 当前 try 块的所有 catch 类型集合 + 收集到的
    // 可失败调用的错误类型集合）；退出 try block（catch 子句进入前）出栈。
    // compileCallExpr 检测到 #Fallible callee 时：栈非空 → 路由到匹配 catch（10g 实施 IR）；
    // 同时抑制 E7001 / E7006（裸调用合法），但 ! 仍触发 E7016 警告。
    struct TryCatchCtx {
        // catch 子句声明的错误类型集合（按 enum 名字符串去重；此处不查重，重复在
        // visitExprTryCatch 后期校验时报 E7005）
        vector<string> catchTypes;
        // try block 内实际遇到的可失败调用的错误类型集合（compileCallExpr 在编译每个
        // 调用时按 callee 的 fallibleErrType 追加；穷尽性 E7002 / 多余 E7015 据此判定）
        vector<string> seenErrTypes;
        // 10g-5: 路由所需的 per-arm 信息（与 catchTypes 一一对应）
        // armEntryBBs[i] = 第 i 个 catch 子句的入口 BB（错误命中时跳到此处）
        // armEAllocas[i] = 第 i 个 catch 子句 e 绑定的 alloca（错误时先 store ErrEnum）
        vector<llvm::BasicBlock*> armEntryBBs;
        vector<llvm::Value*> armEAllocas;
    };
    vector<TryCatchCtx> _tryCatchStack;

    // ==================== 语句编译 ====================
    void compileRetStatement(StatementRetNode* node);                               // 编译 return 语句
    void compileRetVoidStatement(StatementRetVoidNode* node);                       // 编译 return; 语句
    void compileDeclareStatement(StatementDeclareNode* node);                       // 编译变量声明语句（无初始化）
    void compileDeclareAssignStatement(StatementDeclareAssignNode* node);           // 编译变量声明语句
    void compileDeclareAssignTupleStatement(StatementDeclareAssignTupleNode* node); // 编译元组解构声明语句
    void compileAssignStatement(StatementAssignNode* node);                         // 编译赋值语句
    void compileLoopStatement(StatementLoopNode* node);                             // 编译 loop 语句
    void compileForInStatement(StatementForInNode* node);                           // 编译 for-in 语句
    // Indexed for-in：对已物化的 receiver 调 `len` / `at`（无 AST）
    llvm::Value* compileIndexedMethodCall(llvm::Value* recvPtr, const TypeInfo& recvType, const string& method,
                                          const vector<llvm::Value*>& args, const vector<TypeInfo>& argTypes, int line,
                                          int col);
    void compileBreakStatement(StatementBreakNode* node);                           // 编译 break 语句
    void compileContinueStatement(StatementContinueNode* node);                     // 编译 continue 语句
    void compileArraySetStatement(StatementSetNode* node);                          // 编译数组元素赋值语句
    void compileStaticFieldSetStatement(StatementStaticFieldSetNode* node);         // 编译静态字段写语句 (Phase 5)
// clang-format on
#endif
