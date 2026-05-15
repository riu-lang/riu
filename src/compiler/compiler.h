// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 编译器核心头文件
// 负责将 AST 节点编译为 LLVM IR
// 
// 主要职责:
// 1. 管理编译上下文 (LLVMContext, IRBuilder, Module)
// 2. 处理泛型单态化 (结构体实例化、函数实例化)
// 3. 编译全局常量、结构体声明、函数定义
// 4. 表达式和语句的 IR 生成
// 5. 析构函数自动生成和调用

#ifndef YUX_LANG_COMPILER_H
#define YUX_LANG_COMPILER_H


#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/global_const_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/struct_node.h"
#include "ast/yux.h"
#include "compiler_runtime.h"

// 类型转换信息
// 用于延迟处理类型转换 (如 .to_i32() 方法调用)
struct CastInfo {
    llvm::Value* value;     // 待转换的值
    TypeInfo srcType;       // 源类型
    TypeInfo dstType;       // 目标类型
};

class Compiler {
    // ==================== LLVM 核心组件 ====================
    llvm::LLVMContext& _context;    // LLVM 上下文，管理类型和常量
    llvm::IRBuilder<>& _builder;    // IR 构建器，用于生成指令
    llvm::Module* _module;          // LLVM 模块，包含所有函数和全局变量
    p<FileNode> _file;              // 当前编译的源文件 AST
    Yux* _yux = nullptr;            // 编译器主驱动，用于访问 SDK 等全局资源
    bool _isSdk = false;            // 是否正在编译 SDK (core.yux)

    // ==================== 类型映射表 ====================
    map<string, llvm::Type*> _typeMap;              // 基本类型 -> LLVM 类型映射
    map<string, llvm::StructType*> _structTypes;    // 结构体名 -> LLVM 结构体类型
    map<string, llvm::Value*> _localVarPtrs;        // 局部变量名 -> 栈上地址 (alloca)
    map<string, CastInfo> _castFunctions;           // 延迟类型转换缓存
    int _castCounter = 0;                           // 类型转换计数器，用于生成唯一名称

    // ==================== 泛型单态化 ====================
    // 泛型结构体单态化：key = 实例 mangle 名（如 "A$i32"）
    struct StructInstance {
        p<StructDeclNode> baseDecl;     // 泛型结构体声明
        p<StructImplNode> baseImpl;     // 泛型结构体实现 (包含方法)
        p<FileNode> ownerFile;          // 定义该结构体的文件
        vector<TypeInfo> args;          // 类型参数实例化参数
        string mangledName;             // mangle 后的实例名
        // 实例的"消费方"模块名，即触发该实例化的当前编译模块。
        // 每个用到泛型实例的模块各自生成一份 IR，符号名以本字段为前缀，
        // 不再共用 baseDecl owner 的前缀，避免 SDK + 用户模块同时实例化
        // 相同 Nullable<T> 时出现 lld-link duplicate symbol。
        string consumerModule;
        bool methodsEmitted = false;    // 方法是否已生成
        string sourceFile;              // 实例化发生的源文件 (用于错误报告)
        int sourceLine = 0;             // 实例化发生的行号 (用于错误报告)
    };
    map<string, StructInstance> _structInstances;

    // 泛型函数单态化：key = 实例 mangle 名（如 "foo$i32"）
    struct FnInstance {
        p<FnNode> baseFn;           // 泛型函数定义
        p<FileNode> ownerFile;      // 定义该函数的文件
        vector<TypeInfo> typeArgs;  // 类型参数实例化参数
        string mangledName;         // mangle 后的实例名
        bool emitted = false;       // 是否已生成 IR
    };
    map<string, FnInstance> _fnInstances;

    // 类型替换栈帧
    // 用于在泛型实例化过程中跟踪类型参数替换
    struct SubstFrame {
        map<string, TypeInfo> subst;    // 类型参数 -> 实际类型 的映射
        string baseStructName;          // 泛型原名，如 "Foo2"
        string effStructName;           // 实例名，如 "Foo2$i32"
        string sourceFile;              // 实例化发生的源文件
        int sourceLine = 0;             // 实例化发生的行号
    };
    vector<SubstFrame> _substStack;

    // ==================== 错误报告辅助 ====================
    [[nodiscard]] string formatInstantiationContext() const;                       // 格式化泛型实例化上下文信息
    [[noreturn]] void rethrowWithInstantiationContext(const YuxError& e) const;    // 重新抛出异常并附加实例化上下文

    // ==================== 泛型实例化 ====================
    TypeInfo applySubst(const TypeInfo& t) const;                                  // 应用当前类型替换（含别名透明替换）
    // 顶层透明类型别名解析；递归把 alias 名替换为目标类型，遇环抛 E2016
    TypeInfo resolveAlias(const TypeInfo& t) const;
    // 编译入口处的别名一次性校验：名称冲突 (E2017) + 环检测 (E2016)
    void validateAliases();
    string ensureStructInstance(p<StructDeclNode> baseDecl, const vector<sp<TypeInfo>>& args, p<FileNode> ownerFile, int sourceLine = 0);  // 确保结构体实例存在
    string ensureFnInstance(p<FnNode> baseFn, const vector<TypeInfo>& typeArgs, p<FileNode> ownerFile, int sourceLine);  // 确保函数实例存在
    void emitInstanceMethods();    // 生成所有泛型结构体实例的方法
    void emitFnInstances();        // 生成所有泛型函数实例

    // ==================== 作用域管理 ====================
    vector<string> _scopeVars;     // 当前作用域内的变量名列表 (用于析构函数调用)

    // ==================== 当前编译状态 ====================
    llvm::Function* _currentFn = nullptr;       // 当前正在编译的函数
    p<FnNode> _currentFnNode = nullptr;         // 当前函数的 AST 节点
    string _currentStructName;                  // 当前方法所属的结构体名
    // Phase 2c：当前正在编译的 lambda body 作用域（emitLambdaFunction 期间有效）
    // 非空时 compileLiteralExpr 的 LiteralObj 路径启用 FV 校验 / 捕获识别。
    p<ScopeNode> _currentLambdaBodyScope = nullptr;
    // Phase 4a：当前正在编译的 lambda 节点（emitLambdaFunction 期间有效）
    // 非空时 compileLiteralExpr 命中外层 local 标识符 → addCapture + GEP 读 captures
    // 而非抛 E2028。Phase 4a 仅识别标量；非标量类型仍报 E2028（堆句柄推到 4a-2）。
    LambdaExprNode* _currentLambdaForCapture = nullptr;
    // captures 指针：emit lambda body 时缓存"当前 lambda 函数的第 0 形参（captures Ptr）"，
    // compileLiteralExpr 命中捕获时用作 GEP base
    llvm::Value* _currentLambdaCapturesArg = nullptr;

    // ==================== 控制流 ====================
    vector<llvm::BasicBlock*> _loopExitBlocks;  // 循环退出块栈 (用于 break 语句)

public:
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

private:

    // ==================== 类型系统 ====================
    llvm::Type* getLLVMType(const TypeInfo& type);                              // 将 TypeInfo 转换为 LLVM 类型
    llvm::StructType* getArrayBlockType();                                      // Phase 1b: { u32 strong, u32 weak, i64 len, i64 cap, ptr data } - Array<T> 的 RC Block 布局，与 T 无关

    // ==================== Array<T> 句柄辅助（Phase 1b） ====================
    // 这些辅助统一处理 Array<T> 实例 = { ptr handle } 经由 handle 间接访问 Block 的模式
    llvm::Value* loadArrayHandle(llvm::Value* arrayStructPtr, const string& name = "array.handle");  // 从 Array<T> 实例 alloca 加载句柄（指向 Block）
    llvm::Value* arrayBlockLenPtr(llvm::Value* handle);                         // Block.len 字段地址（i64*）
    llvm::Value* arrayBlockCapPtr(llvm::Value* handle);                         // Block.cap 字段地址（i64*）
    llvm::Value* arrayBlockDataFieldPtr(llvm::Value* handle);                   // Block.data 字段地址（ptr*；存放当前数据缓冲指针）
    llvm::Value* allocArrayBlock(llvm::Type* elemLLVMType, llvm::Value* initCap, llvm::Value* initLen);  // 调用 _array_alloc 返回 Block*
    void storeArrayHandle(llvm::Value* arrayStructPtr, llvm::Value* handle);    // 把句柄写到 Array<T> 实例（field 0）
    // 把 ExprArrayNode 按 Array<elemType> 字面量编译，分配 Block 并写入元素，返回 Block* 句柄（strong=1）。
    // 调用方收到句柄后通常用 storeArrayHandle 写入目标 Array<T> 实例的句柄槽。
    // 处理元素 retain / consumeTemp，并在 elemType 自身是 Array<U> 且元素是嵌套字面量时
    // 递归调用自身（修「嵌套 Array 字面量未按外层元素类型期望泛型形态」：内层若按
    // ExprArrayNode::getType() 自报为 [N x U] 固定数组，会被外层 store 越界踩坏后续槽）。
    llvm::Value* buildArrayLiteralBlock(ExprArrayNode* arrayNode, const TypeInfo& elemType);
    llvm::FunctionType* getLLVMFunctionType(p<FnHeaderNode> header);            // 获取函数的 LLVM 类型
    // DRAFT-错误.md [#10.A]：把 #Fallible(E) 函数的成功返回类型包成
    //   { i1 isErr, T_ok, ErrEnum }（T_ok = void 时退化为 { i1, ErrEnum }）。
    // errTypeName 为空时直接返回 raw return type（成功 / 普通函数同行为）。
    // 10g-7：main 标 #Fallible(E) 时生成 mainStartup wrapper：
    //   yux_main 返回 { i1 isErr, EnumLLVM err }；isErr=1 → 解 tag，按 variant
    //   名往 stderr 写 "error: <module>.<EnumName>::<VariantName>[(...)]\n" 后 ExitProcess(1)；
    //   isErr=0 → ret 0。spec §6.1。
    void emitMainStartupFallible(const string& fallibleErrName);

    llvm::Type* wrapFallibleRetType(const TypeInfo& retType, const string& errTypeName);
    // 同上，但强制返回 StructType* 用于 ret 路径构造 insertvalue。errTypeName 必须非空。
    llvm::StructType* getFallibleRetStructType(const TypeInfo& retType, const string& errTypeName);
    // [#10.A] / [#10.C] 调用侧 `!` 透传：callee 是 #Fallible 时，分流 isErr 位 →
    //   - 错误分支：构外层 fn 错误返回 struct + ret（透传到 caller 的 #Fallible 通道）
    //   - 成功分支：extract T_ok，caller 在 okBB 继续编译；返回 T_ok（void 时 nullptr）
    // calleeFallibleErr 空时直接返回 callResult（普通调用同行为）。
    // 仅 ID-callee 路径调用本 helper（10g-4 范围）；方法 / 泛型 / fn-value 推后续子项。
    llvm::Value* handleFallibleCallResult(
        llvm::Value* callResult, const string& calleeFallibleErr,
        const TypeInfo& calleeRetType, p<ExprCallNode> callNode);
    llvm::StructType* getOrCreateStructType(p<StructDeclNode> structDecl, p<FileNode> sourceFile = nullptr);  // 获取或创建结构体类型

    // ==================== 函数获取 ====================
    llvm::Function* getFunction(p<FnHeaderNode> header);                        // 获取或创建函数
    llvm::Function* getMethodFunction(
        const string& structName, const string& methodName, const vector<TypeInfo>& paramTypes,
        const TypeInfo& retType, const string& fallibleErrType = "");           // 获取或创建方法函数
    llvm::Function* getDestructorFunction(const string& structName);            // 获取或创建析构函数

    // ==================== 表达式编译 ====================
    llvm::Value* compileExpr(p<ExprNode> node);                                 // 编译表达式 (主入口)
    // Phase 2.4 Sema/Codegen 拆分：codegen 读类型的统一入口。
    // 优先返回 compile<Foo>Expr 已写入的 resolvedType；尚未走过该路径的节点
    // 回退到节点自身的 getType()（与 2.2 之前的行为一致）。debug 构建下若
    // 两者均可得，校验其一致，拦截写入污染或被遗漏的入口。
    [[nodiscard]] TypeInfo resolvedOrInferredType(p<ExprNode> node) const;
    llvm::Value* compileArrayInitExpr(p<ExprArrayInitNode> node, const TypeInfo& targetType, llvm::Value* destPtr = nullptr);  // 编译数组初始化表达式
    llvm::Value* createCast(llvm::Value* val, const TypeInfo& srcType, const TypeInfo& dstType);  // 创建类型转换

    // ==================== 语句块编译 ====================
    void compileStatementBlock(p<StatementBlockNode> block);                    // 编译语句块 (无返回值)
    llvm::Value* compileStatementBlockWithResult(
        p<StatementBlockNode> block, llvm::BasicBlock* continueBlock, llvm::PHINode* phi, const TypeInfo& resultType);  // 编译语句块 (有返回值)

    // ==================== 析构函数 ====================
    void callDestructor(const string& varName, const TypeInfo& varType);         // 调用单个变量的析构函数
    void callDestructorsForScope();                                             // 调用当前作用域所有变量的析构函数
    void callFieldDestructor(llvm::Value* structPtr, const string& structName); // 调用结构体字段的析构函数
    void generateDefaultDestructor(const string& structName);                   // 生成默认析构函数
    bool typeNeedsDestructor(const TypeInfo& type);                             // 检查类型是否需要析构
    bool structNeedsDestructor(const string& structName);                       // 检查结构体是否需要析构
    bool enumNeedsDestructor(const string& enumName);                           // Phase 5: 任一 variant payload 需析构则枚举需析构
    bool enumDeclNeedsDestructor(p<EnumDeclNode> decl);                         // Phase 5: 同上，按声明节点
    llvm::Function* getEnumDestructorFunction(const string& enumName);          // Phase 5: 获取或创建 __enum_drop_<E>
    void generateEnumDestructor(p<EnumDeclNode> decl, p<FileNode> owner);       // Phase 5: 合成 __enum_drop_<E>(p*) 实现
    void compileEnumDtors();                                                    // Phase 5: 在主流水线中为本文件 enum 生成 dtor 定义

    // Phase 3a: callee-clean 调用约定
    // 给 Rc/Array/Weak 实参在传入前 retain；callee 末尾析构 release 抵消
    // 非堆句柄类型 no-op；返回 true 表示已发出 retain
    bool retainHandleAtCallSite(llvm::Value* argVal, const TypeInfo& argType);

    // Phase 3c.2.a: struct value 内逐 RC 字段（嵌套 struct 递归）retain
    void retainStructFieldsAtCallSite(llvm::Value* argVal, const string& structName);

    // Phase 3d: 释放槽位（变量 / 字段 / 元素地址）当前持有的 RC 值
    // Rc/Array/Weak: load handle 后调对应 release；含 RC 字段 struct: 调其析构（字段逆序 release）
    // 内置 / 引用 / 指针 / 平凡 struct: no-op
    void releaseAtPtr(llvm::Value* slotPtr, const TypeInfo& type);

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
    // Phase 8d.3: 编译"分支结果表达式"——push 子帧、compile、consume 结果、pop 释放中间临时；
    // 若 expectedType 是 RC 句柄（Rc/Array/Weak）且结果非 fresh，发 retain 归一为 +1。
    // 调用方在 phi 汇合后应 recordTemp(phi, expectedType) 把统一 +1 句柄交给外层 statement frame
    llvm::Value* compileBranchResultNormalized(p<ExprNode> expr, const TypeInfo& expectedType);

    // Phase 8b: 识别 +1 所有权（"fresh"）表达式
    // - 函数 / 方法 / 构造器调用：callee 已在 ret 处 move-return retain，结果是 +1 所有权句柄
    // - 数组字面量：_array_alloc 给 strong=1
    // - 其他（变量引用 / 字段访问 / if-else / ?? 等）：视作借用，复制语义需 retain
    // 用于在 declare-assign / assign / 字面量元素写入等"复制语义"路径上跳过多余 retain
    bool isFreshHandleExpr(p<ExprNode> expr);

    // Phase 3c.1/3c.2: 结构体形参 ABI 判定
    // 返回 true 表示该结构体形参按指针传递（保守路径），false 则按 LLVM by-value
    // 规则：内置类型 / Ptr / Ref → false；用户 struct（普通或泛型实例）一律 by-value（false）；
    // 仅 _structTypes 中注册但找不到声明（跨模块未通配导入）→ 保守 true
    bool structParamUsesPointer(const string& typeName);

    // Phase 3c.2.c: 解析结构体字段类型清单
    // 普通 struct → 直接取 fields().getType()
    // 泛型实例 (`_structInstances`) → 取 baseDecl 字段并按实例 args 套替换
    // 找不到则返回空
    vector<TypeInfo> resolveStructFieldTypes(const string& structName);

    // ==================== 语句编译 ====================
    void compileRetStatement(p<StatementRetNode> node);                         // 编译 return 语句
    void compileRetVoidStatement(p<StatementRetVoidNode> node);                 // 编译 return; 语句
    void compileDeclareStatement(p<StatementDeclareNode> node);                 // 编译变量声明语句（无初始化）
    void compileDeclareAssignStatement(p<StatementDeclareAssignNode> node);     // 编译变量声明语句
    void compileDeclareAssignTupleStatement(p<StatementDeclareAssignTupleNode> node); // 编译元组解构声明语句
    void compileAssignStatement(p<StatementAssignNode> node);                   // 编译赋值语句
    void compileLoopStatement(p<StatementLoopNode> node);                       // 编译 loop 语句
    void compileBreakStatement(p<StatementBreakNode> node);                     // 编译 break 语句
    void compileArraySetStatement(p<StatementSetNode> node);                    // 编译数组元素赋值语句

    // ==================== 表达式编译 (具体类型) ====================
    llvm::Value* compileLiteralExpr(p<ExprLiteralNode> node);                   // 编译字面量表达式
    llvm::Value* emitStringLiteralValue(const vector<u32>& codePoints);         // 由码点向量发射 .rodata 哨兵 String 值（StringLiteral / StringTemplate 共用）
    llvm::Value* compileStringTemplate(StringTemplateNode* node);               // v0.6 Phase 2a：StringTemplateNode → StringBuilder lower
    llvm::Value* compileStringPlusChain(ExprAddSubNode* node);                  // v0.6 Phase 2c：连续 String + ... 整链 lower 为单条 StringBuilder 累加
    llvm::Value* compileAddSubExpr(p<ExprAddSubNode> node);                     // 编译加减表达式
    llvm::Value* compileMulDivModExpr(p<ExprMulDivModNode> node);               // 编译乘除取模表达式
    llvm::Value* compileBinOpExpr(p<ExprBinOpNode> node);                       // 编译位运算表达式
    llvm::Value* compileParenExpr(p<ExprParenNode> node);                       // 编译括号表达式
    llvm::Value* compileCallExpr(p<ExprCallNode> node);                         // 编译函数调用表达式
    llvm::Value* compileDotExpr(p<ExprDotNode> node);                           // 编译成员访问表达式
    llvm::Value* compileCompareExpr(p<ExprCompareNode> node);                   // 编译比较表达式
    llvm::Value* compileIfElseExpr(p<ExprIfElseNode> node);                     // 编译 if-else 表达式
    llvm::Value* compileOneLineIfElseExpr(p<ExprOneLineIfElseNode> node);       // 编译单行 if-else 表达式
    llvm::Value* compileIfElsePreValueExpr(p<ExprIfElsePreValueNode> node);     // 编译前置值 if-else 表达式
    llvm::Value* compileArrayGetExpr(p<ExprGetNode> node);                      // 编译数组索引表达式
    llvm::Value* compileArrayLiteralExpr(p<ExprArrayNode> node);                // 编译数组字面量表达式
    llvm::Value* compileTupleExpr(p<ExprTupleNode> node);                       // 编译元组构造表达式 (e1, e2, ...)
    llvm::Value* compileEnumCtorExpr(p<ExprEnumCtorNode> node);                 // 编译枚举构造表达式 E::V / E::V(args)
    // Dyn<D>(x) 构造表达式（DRAFT-dyn-draft / 拟 §12.9）
    // Phase 1c：仅 emit { vtable=null, data=src.handle } 占位 fat ptr，
    // 真 vtable 与对象安全检查留 Phase 2/3
    llvm::Value* compileDynCtorExpr(p<class ExprDynCtorNode> node);

    // Phase 3a：为 (concreteType U, draftQualified D) 获取或合成 vtable 全局
    // 符号：__yux_vtable_<U_mod>_<U_struct>__<D_qualified>，linkonce_odr。
    // 布局：i8* 数组，长度 = 1 + D.signatures().size()
    //   - 槽 0：U 的析构函数指针；U 无需析构 → null
    //   - 槽 1..N：U 实现 D 各方法的 fn ptr（按 D 声明序），跨文件 impl 块定位
    // 调用方负责 U 满足 D（boundSatisfied 已在构造 / 边界匹配时校验）。
    llvm::GlobalVariable* getOrEmitDynVTable(
        const TypeInfo& concreteType,
        const std::string& draftQualified,
        class DraftDeclNode* draft);

    // Phase 3d 配套: 为「内置类型 U 在 Dyn<D> 下的方法 m」生成 (ptr) → (by-value) 适配 thunk.
    // 用于解决 Dyn 调用约定统一为 (ptr receiver, ...) 但 SDK 内置类型方法实际签名是
    // (<U> by-value, ...) 的 ABI 不匹配。thunk 仅 load 一次 receiver, tail-call 真实 SDK 函数。
    // 仅在 vtable 槽内调用; 普通直接方法调用走 compileStructMethodCall 的 by-value 分支.
    llvm::Function* getOrEmitDynPrimitiveThunk(
        const TypeInfo& concreteType,
        const std::string& draftQualified,
        class FnHeaderNode* sig,
        const std::string& sdkMangled);
    llvm::Value* compileMatchExpr(p<ExprMatchNode> node);                       // Phase 6: 编译 match 表达式（switch on tag + 绑定 + arm 体）
    llvm::Value* compileTryCatchExpr(p<ExprTryCatchNode> node);                 // Phase 10f: 编译 try-catch 表达式（10f 仅占位 + 语义校验，IR 路由推 10g）

    // ==================== Lambda（spec §4 / Phase 2b：零捕获） ====================
    // compileLambdaExpr：把 LambdaExprNode 编译为 16 字节 fat-ptr 值 { fn_ptr, captures }；
    // captures 永远为 null（捕获留给 Phase 4）。底层 Function 由 emitLambdaFunction 生成。
    llvm::Value* compileLambdaExpr(p<class LambdaExprNode> node);
    // emitLambdaFunction：取 LambdaExprNode + 期望 Fn 类型，按 captures-leading ABI
    // (Ptr captures, P1, ..., Pn) → R 生成顶层 LLVM Function。
    // expectedFnType 用于回填实例化后的形参类型（lambda 形参可省类型；调用者必须先反推）。
    // 同 (node, mangledName) 已生成则直接返回缓存。
    llvm::Function* emitLambdaFunction(p<class LambdaExprNode> node, const TypeInfo& expectedFnType);
    // Phase 4a-2：为含堆句柄 captures 的 lambda 合成析构函数。
    // 签名 void __captures_dtor_<mangle>(ptr fields_base)，逐 capture 字段调 releaseAtPtr。
    // 全标量 captures（无字段需 release）→ 返回 nullptr，调用方在 dtor 槽存 null。
    llvm::Function* emitCapturesDtorFunction(p<class LambdaExprNode> node, const string& lambdaMangled);
    // 调用 fn-typed 值：从 fat-ptr 提取 fn_ptr / captures，按 ABI 调用
    llvm::Value* compileFnValueCall(p<ExprCallNode> node);
    // Phase 3c：callee 为 Rc<fn(...)R>，从 Rc payload load fat-ptr 后按同款 ABI 调用
    // innerFnType 为 Rc 元素类型（Fn TypeInfo），用于实参反推 / 形参类型 / 返回类型
    llvm::Value* compileRcFnValueCall(p<ExprCallNode> node, const TypeInfo& innerFnType);
    // 实参位置 lambda 类型反推：用 fnParamTypes()[i] 回填 LambdaExprNode 形参缺失类型
    // 在调用点正式 compileExpr(args) 之前调用
    void inferLambdaParamsFromFnType(p<class LambdaExprNode> lambda, const TypeInfo& expectedFnType);
    // 查找 enum 声明（本文件 + SDK 回退 + wildcard 导入），未找到返回 nullptr / 空 owner
    p<EnumDeclNode> lookupEnumDecl(const string& name, p<FileNode>& outOwner);
    llvm::Value* compileGetRefExpr(p<ExprGetRefNode> node);                     // 编译取引用表达式
    llvm::Value* compileUnaryExpr(p<ExprUnaryNode> node);                       // 编译一元表达式
    llvm::Value* compileNullElseExpr(p<ExprNullElseNode> node);                 // 编译 a ?? b：a 持值则取 a.get()，否则取 b
    llvm::Value* compileSafeDotExpr(p<ExprDotNode> node);                       // 编译 a?.b：a 持值则包一层 Nullable<a.get().b>，否则空

    // ==================== 自定义类型运算符方法调用 ====================
    llvm::Value* compileCustomTypeBinaryOp(
        p<ExprNode> leftExpr, p<ExprNode> rightExpr, const TypeInfo& leftType,
        const string& methodName, int lineNum);                                 // 编译自定义类型二元运算符
    llvm::Value* compileCustomTypeUnaryOp(
        p<ExprNode> expr, const TypeInfo& type, const string& methodName, int lineNum);  // 编译自定义类型一元运算符

    // ==================== 方法/函数调用编译 ====================
    bool isCompilerInnerMethod(const string& structName, const string& methodName);  // 检查是否为编译器内部方法
    llvm::Value* compileMethodCall(
        p<ExprCallNode> callNode, p<ExprDotNode> dotNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);  // 编译方法调用
    llvm::Value* compileFunctionCall(
        p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);    // 编译函数调用
    llvm::Value* compileConstructorCall(
        const string& baseName, const string& effName,
        vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
        const vector<bool>& argFresh = {});                                     // 编译构造函数调用
    llvm::Value* compileKnownFunctionCall(
        p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
        FnSymbolInfo* fnSymbol);                                                // 编译已知函数调用

    // ==================== 特殊类型方法编译 ====================
    llvm::Value* compileArrayMethodCall(
        p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
        const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);  // 编译数组方法调用
    llvm::Value* compileBuiltinTypeMethodCall(
        p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
        const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);  // 编译内置类型方法调用
    llvm::Value* compileStructMethodCall(
        p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType, const TypeInfo& actualType,
        const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);  // 编译结构体方法调用
    llvm::Value* compileDynMethodCall(
        p<ExprCallNode> callNode, p<ExprNode> baseExpr, const TypeInfo& baseType,
        const string& member, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);  // Phase 2d/3d: Dyn<D> 方法调用
    llvm::Value* compileGenericFunctionCall(
        p<ExprCallNode> callNode, const string& fnName, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
        p<FnNode> genericFn, p<FileNode> fnOwner);                              // 编译泛型函数调用

    // ==================== 测试断言内建（compiler_test_intrinsics.cpp）====================
    // assert_eq:<T> 失败时合成 stdout 写入 + 调 _yux_test_assert_failed → RaiseException
    // 详见 docs/spec/11-编译期注解.md §11.3.5
    llvm::Value* compileTestAssertEq(
        p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes,
        const TypeInfo& typeArg);
    llvm::Value* compileTestAssertTrue(
        p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileTestAssertFalse(
        p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);
    llvm::Value* compileTestFail(
        p<ExprCallNode> callNode, vector<llvm::Value*>& args, vector<TypeInfo>& argTypes);

public:
    // ==================== 构造函数 ====================
    // @param context   LLVM 上下文
    // @param builder   IR 构建器
    // @param mod       LLVM 模块
    // @param file      要编译的源文件 AST
    // @param yux       编译器主驱动 (可选)
    // @param isSdk     是否为 SDK 编译
    Compiler(
        llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, p<FileNode> file, Yux* yux = nullptr,
        bool isSdk = false);

    // ==================== 编译入口 ====================
    void compile(p<FileNode> file);                     // 编译文件 (主入口)
    void compileGlobalConsts();                         // 编译全局常量
    void compileStructDecls();                          // 编译结构体声明
    void compileStructImpls();                          // 编译结构体实现 (方法、析构函数)
    void compileFn(p<FnNode> node, llvm::Function* func);   // 编译函数
    void compileMethod(p<FnNode> node, llvm::Function* func, const string& structName, bool isDestructor = false);  // 编译方法
    void compileStatement(p<StatementNode> node);       // 编译语句 (分发函数)
};

#endif //YUX_LANG_COMPILER_H
