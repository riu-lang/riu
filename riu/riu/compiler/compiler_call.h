// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 调用子系统：取 LLVM Function、方法/函数分派、extern ABI、测试断言。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // ==================== 函数获取 ====================
    llvm::Function* getFunction(FnHeaderNode* header); // 获取或创建函数
    llvm::Function* getMethodFunction(
        const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes, const TypeInfo& retType,
        const string& fallibleErrType = "", bool isStatic = false,
        string ownerModuleHint = {}); // isStatic=true 走 Mangler::staticMethod；ownerModuleHint 用于路径 LHS
    llvm::Function* getDestructorFunction(const string& structName,
                                          string ownerModuleHint = {}); // ownerModuleHint：有 owner 时不再短名找错模块

    // [#10.A] / [#10.C] 调用侧 `!` 透传：callee 是 #Fallible 时，分流 isErr 位 →
    //   - 错误分支：构外层 fn 错误返回 struct + ret（透传到 caller 的 #Fallible 通道）
    //   - 成功分支：extract T_ok，caller 在 okBB 继续编译；返回 T_ok（void 时 nullptr）
    // calleeFallibleErr 空时直接返回 callResult（普通调用同行为）。
    llvm::Value* handleFallibleCallResult(llvm::Value* callResult, const string& calleeFallibleErr,
                                          const TypeInfo& calleeRetType, ExprCallNode* callNode);

    // extern Win64 C ABI（§8.7.4.2）：bool→i8；小聚合当整数；大聚合 byval/sret
    struct ExternAbiSlot {
        llvm::Type* abiTy = nullptr;   // LLVM 函数签名里的类型
        llvm::Type* valueTy = nullptr; // riu 值类型
        bool boolExt = false;          // i1 ↔ i8
        bool integerAgg = false;       // 1/2/4/8 字节聚合按整数
        bool indirect = false;         // 大聚合：实参 byval / 返回 sret
    };
    ExternAbiSlot externAbiSlot(const TypeInfo& t);
    llvm::Value* coerceToExternArg(llvm::Value* v, const ExternAbiSlot& slot);
    llvm::Value* coerceFromExternRet(llvm::Value* v, const ExternAbiSlot& slot, llvm::Value* sretAlloca);
    llvm::Function* getOrCreateExternFunction(const string& cName, const FnSymbolInfo& fnSymbol);
    void applyExternCallAttrs(llvm::CallInst* ci, const FnSymbolInfo& fnSymbol);

    // ==================== 方法/函数调用编译 ====================
    bool isBuiltinMethod(const string& structName, const string& methodName); // 检查是否为编译器内部方法
    llvm::Value* compileMethodCall(ExprCallNode* callNode, ExprDotNode* dotNode, vector<llvm::Value*>& args,
                                   vector<TypeInfo>& argTypes); // 编译方法调用
    llvm::Value* compileSafeDotMethodCall(ExprCallNode* callNode, ExprDotNode* dotNode,
                                          vector<TypeInfo>& argTypes); // 编译 a?.foo() 安全方法调用
    llvm::Value* compileFunctionCall(ExprCallNode* callNode, const string& fnName, vector<llvm::Value*>& args,
                                     vector<TypeInfo>& argTypes); // 编译函数调用
    llvm::Value* compileKnownFunctionCall(ExprCallNode* callNode, const string& fnName, vector<llvm::Value*>& args,
                                          vector<TypeInfo>& argTypes,
                                          FnSymbolInfo* fnSymbol); // 编译已知函数调用

    // ==================== 特殊类型方法编译 ====================
    llvm::Value* compileArrayMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                        const string& member, vector<llvm::Value*>& args,
                                        vector<TypeInfo>& argTypes); // 查 kBuiltinMethods 后 lowering
    // Array:<T>::with_capacity(n) — 表里 ArrayWithCapacity 的 lowering
    llvm::Value* compileArrayWithCapacity(ExprPathCallNode* node);
    llvm::Value* compileBuiltinTypeMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                              const string& member, vector<llvm::Value*>& args,
                                              vector<TypeInfo>& argTypes); // 编译内置类型方法调用
    llvm::Value* compileStructMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                         const TypeInfo& actualType, const string& member, vector<llvm::Value*>& args,
                                         vector<TypeInfo>& argTypes); // 编译结构体方法调用
    llvm::Value* compileDynMethodCall(ExprCallNode* callNode, ExprNode* baseExpr, const TypeInfo& baseType,
                                      const string& member, vector<llvm::Value*>& args,
                                      vector<TypeInfo>& argTypes); // Phase 2d/3d: Dyn<D> 方法调用
    llvm::Value* compileGenericFunctionCall(ExprCallNode* callNode, const string& fnName, vector<llvm::Value*>& args,
                                            vector<TypeInfo>& argTypes, FnNode* genericFn,
                                            FileNode* fnOwner); // 编译泛型函数调用

    // ==================== 测试断言内建（compiler_test_intrinsics.cpp）====================
    // assert_eq:<T> 失败时合成 stdout 写入 + 调 _riu_test_assert_failed → RaiseException
    // 详见 docs/spec/11-编译期注解.md §11.3.5
    llvm::Value* compileTestAssertEq(ExprCallNode* callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
                                     const TypeInfo& typeArg);
    llvm::Value* compileTestAssertTrue(ExprCallNode* callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileTestAssertFalse(ExprCallNode* callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileTestFail(ExprCallNode* callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
// clang-format on
#endif
