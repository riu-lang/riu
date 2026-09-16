// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Compiler 类型子系统：TypeInfo → LLVM、Array GEP、Fallible ABI、形参指针判定。
// 仅由 compiler.h 在 class Compiler 体内、定义 RIU_COMPILER_MEMBERS 后 include。

#ifdef RIU_COMPILER_MEMBERS
// clang-format off

    // ==================== 类型系统 ====================
    llvm::Type* getLLVMType(const TypeInfo& type); // 将 TypeInfo 转换为 LLVM 类型
    // 从 struct 名还原完整 TypeInfo。命中 generic 实例表（mangle `Foo<i32>`）时带上
    // args 走 kind 分发；否则 `TypeInfo(name)`（Normal / 标量 / 源码名，唯一名字构造点）。
    [[nodiscard]] TypeInfo typeInfoForNamedStruct(const string& name) const;

    // ==================== usize 辅助 ====================
    // 返回 usize 对应的 LLVM 类型（指针宽度整数，64-bit 上为 i64）
    [[nodiscard]] llvm::Type* getSizeType() const;

    // ==================== Array<T> 内联字段辅助（B-3） ====================
    // Array 实例 layout：{ ptr _data @0, usize _len @8, usize _cap @16 }
    [[nodiscard]] llvm::StructType* getArrayStructTypeForGEP() const; // 获取 Array LLVM struct 类型
    llvm::Value* arrayDataFieldPtr(llvm::Value* arrayStructPtr,
                                   const string& name = ""); // _data 字段指针（ptr*）
    llvm::Value* arrayLenFieldPtr(llvm::Value* arrayStructPtr,
                                  const string& name = ""); // _len 字段指针（usize*）
    llvm::Value* arrayCapFieldPtr(llvm::Value* arrayStructPtr,
                                  const string& name = ""); // _cap 字段指针（usize*）
    // 把 ExprArrayNode 按 Array<elemType> 字面量编译，直接分配数据缓冲并填充元素，
    // 返回 Array<T> struct 值。处理元素 retain / consumeTemp，嵌套 Array 递归。
    llvm::Value* buildArrayLiteralBlock(ExprArrayNode* arrayNode, const TypeInfo& elemType);
    llvm::FunctionType* getLLVMFunctionType(FnHeaderNode* header); // 获取函数的 LLVM 类型
    // DRAFT-错误.md [#10.A]：把 #Fallible(E) 函数的成功返回类型包成
    //   { i1 isErr, T_ok, ErrEnum }（T_ok = void 时退化为 { i1, ErrEnum }）。
    // errTypeName 为空时直接返回 raw return type（成功 / 普通函数同行为）。
    // 10g-7：main 标 #Fallible(E) 时生成 mainStartup wrapper：
    //   riu_main 返回 { i1 isErr, EnumLLVM err }；isErr=1 → 解 tag，按 variant
    //   名往 stderr 写 "error: <module>.<EnumName>::<VariantName>[(...)]\n" 后 ExitProcess(1)；
    //   isErr=0 → ret 0。spec §6.1。
    void emitMainStartupFallible(const string& fallibleErrName);

    llvm::Type* wrapFallibleRetType(const TypeInfo& retType, const string& errTypeName);
    // fallible 函数成功通道 ret：{ false, T_ok, zero(E) }
    llvm::Value* wrapFallibleSuccessRet(llvm::Value* okVal, const TypeInfo& successType, const string& fallibleErr);
    // 同上，但强制返回 StructType* 用于 ret 路径构造 insertvalue。errTypeName 必须非空。
    llvm::StructType* getFallibleRetStructType(const TypeInfo& retType, const string& errTypeName);
    llvm::StructType* getOrCreateStructType(StructDeclNode* structDecl,
                                            FileNode* sourceFile = nullptr); // 获取或创建结构体类型

    // Phase 3c.1/3c.2: 结构体形参 ABI 判定
    // 返回 true 表示该结构体形参按指针传递（保守路径），false 则按 LLVM by-value
    // 规则：内置类型 / Ptr / Ref → false；ArrayGeneric → true；用户 struct（普通或泛型实例）一律
    // by-value（false）；仅 _structTypes 中注册但找不到声明（跨模块未通配导入）→ 保守 true
    // 调用方必须传完整 TypeInfo；禁止从裸名重建（会丢掉 genericArgs，Array/Rc 变成错误 kind）
    bool structParamUsesPointer(const TypeInfo& ti);

    // Phase 3c.2.c: 解析结构体字段类型清单
    // 普通 struct → 直接取 fields().getType()
    // 泛型实例（generic 实例表）→ 取 baseDecl 字段并按实例 args 套替换
    // 找不到则返回空
    vector<TypeInfo> resolveStructFieldTypes(const string& structName);

    // 从 target type 递归推断灵活整数类型（含 tuple/泛型别名展开）
    // 当 target 解析为 tuple 且 expr 为 tuple 字面量时，逐元素递归推断；
    // 否则委托给 AST 层的 tryInferIntType。
    void inferFlexibleInts(ExprNode* expr, const TypeInfo& target);
// clang-format on
#endif
