// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// Codegen driver：LLVMContext + Module + 跑 codegen Pass。
// ownership / destructor / dyn-vtable / types / call / expr / stmt 声明在窄头
// （class 体内 include）。新子系统状态写对应窄头，不必改本文件成员表。

#ifndef RIU_LANG_COMPILER_H
#define RIU_LANG_COMPILER_H

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "ast/node/ast_visitor.h"
#include "ast/node/enum_node.h"
#include "ast/node/expr_node.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/global_const_node.h"
#include "ast/node/statement_node.h"
#include "ast/node/struct_node.h"
#include "ast/riu.h"
#include "compiler_runtime.h"
#include "generic/generic.h"
#include "sema/const_eval.h"
#include "sema/name_resolver.h"
#include "types.h"

// SemaPass 已覆盖的语义错若仍走到 codegen：内部缺口，抛 E3091，不再抛原码。
[[noreturn]] inline void throwSemaGap(size_t line, int col = 0) {
    if (col != 0) throw RiuError(line, col, ErrorCode::E3091);
    throw RiuError(line, ErrorCode::E3091);
}

// 类型转换信息
// 用于延迟处理类型转换 (如 .to_i32() 方法调用)
struct CastInfo {
    llvm::Value* value; // 待转换的值
    TypeInfo srcType;   // 源类型
    TypeInfo dstType;   // 目标类型
};

// 4.3：AstVisitor。compileExpr / compileStatement 经 accept 分派；漏 override 编不过。
class Compiler : public AstVisitor {
    // ==================== LLVM 核心组件 ====================
    llvm::LLVMContext& _context; // LLVM 上下文，管理类型和常量
    llvm::IRBuilder<>& _builder; // IR 构建器，用于生成指令
    llvm::Module* _module;       // LLVM 模块，包含所有函数和全局变量
    FileNode* _file;             // 当前编译的源文件 AST
    Riu* _riu = nullptr;         // 编译器主驱动，用于访问 SDK 等全局资源
    bool _isSdk = false;         // 是否正在编译 SDK (core.ut)
    bool _isTestDll = false;     // 是否为 test DLL 模式（影响断言内建 + 导出）

    // ==================== 类型映射表 ====================
    map<string, llvm::Type*> _typeMap;           // 基本类型 -> LLVM 类型映射
    map<string, llvm::StructType*> _structTypes; // 结构体名 -> LLVM 结构体类型
    // riu 字段下标 → LLVM 字段下标（#Packed / 字段 #Align 插入 pad 时）
    map<llvm::StructType*, vector<unsigned>> _llvmFieldOfRiu;
    map<llvm::StructType*, uint64_t> _llvmAbiAlign; // packed LLVM 类型的 C ABI 对齐
    map<string, llvm::Value*> _localVarPtrs;        // 局部变量名 -> 栈上地址 (alloca)
    map<string, CastInfo> _castFunctions;           // 延迟类型转换缓存
    int _castCounter = 0;                           // 类型转换计数器，用于生成唯一名称
    int _forInSerial = 0;                           // for-in 临时集合名
    int _discardSerial = 0;                         // `_` 丢弃槽 alloca 名

    // ==================== 泛型单态化 ====================
    // 实例表在 generic::Registry；Compiler 问已具体实例再发 LLVM 类型 / IR。
    generic::Registry _generic;
    using SubstFrame = generic::SubstFrame;
    generic::SubstStack _substStack; // 发射泛型体 IR 时压帧，套 applySubst

    // ==================== 错误报告辅助 ====================
    [[nodiscard]] string formatInstantiationContext() const;                    // 格式化泛型实例化上下文信息
    [[noreturn]] void rethrowWithInstantiationContext(const RiuError& e) const; // 重新抛出异常并附加实例化上下文

    // ==================== 泛型实例化 ====================
    [[nodiscard]] TypeInfo applySubst(const TypeInfo& t) const; // generic::applySubst（含别名透明替换）
    // `Self` / 泛型原名 → 当前单态 TypeInfo（如 `Slot<i32>`）
    [[nodiscard]] TypeInfo bindStructSelfType(const TypeInfo& t, const string& baseName, const string& effName) const;
    // 顶层透明类型别名解析；递归把 alias 名替换为目标类型，遇环抛 E2016
    [[nodiscard]] TypeInfo resolveAlias(const TypeInfo& t) const;
    // 本文件 → SDK → wildcard（0 LLVM，与 SemaPass 共用）
    [[nodiscard]] sema::NameResolver names() const { return {_file, _riu ? _riu->sdkFile() : nullptr}; }
    // LLVM 符号里的类型用定义模块视角补 owner，避免 `Ref<String>` 与
    // `Ref<riu.core.string.String>` 裂成两个链接名。查找不要走调用方文件
    // （用户本地同名 struct 会抢 SDK 类型）。
    [[nodiscard]] TypeInfo withMangleOwners(const TypeInfo& t, FileNode* fromFile) const;
    [[nodiscard]] vector<TypeInfo> withMangleOwners(const vector<TypeInfo>& ts, FileNode* fromFile) const;
    [[nodiscard]] FileNode* fileForMangleModule(const string& module) const;
    [[nodiscard]] string mangleFallibleErr(const string& err, FileNode* fromFile) const;
    [[nodiscard]] string mangleFunction(const string& module, const string& name, const vector<TypeInfo>& params,
                                        bool isPrivate, const TypeInfo& retType = TypeInfo(),
                                        const string& fallibleErrType = "") const;
    [[nodiscard]] string mangleMethod(const string& module, const string& structName, const string& methodName,
                                      const vector<TypeInfo>& params, bool isPrivate,
                                      const TypeInfo& retType = TypeInfo(), const string& fallibleErrType = "") const;
    [[nodiscard]] string mangleStaticMethod(const string& module, const string& structName, const string& methodName,
                                            const vector<TypeInfo>& params, const TypeInfo& retType = TypeInfo(),
                                            const string& fallibleErrType = "") const;
    string genericStruct(StructDeclNode* baseDecl, const vector<sp<TypeInfo>>& args, FileNode* ownerFile,
                         int sourceLine = 0); // 问 generic 登记 struct，缺则发 LLVM layout
    string genericEnum(EnumDeclNode* baseDecl, const vector<sp<TypeInfo>>& args, FileNode* ownerFile,
                       int sourceLine = 0); // 问 generic 登记 enum，缺则发 LLVM layout
    string internGenericFn(FnNode* baseFn, const vector<TypeInfo>& typeArgs, FileNode* ownerFile,
                           int sourceLine); // 问 generic 登记 fn 实例
    string internGenericMethod(FnNode* baseMethod, const string& structName, const vector<TypeInfo>& typeArgs,
                               FileNode* ownerFile,
                               int sourceLine);                      // 问 generic 登记 method 实例
    void emitGenericStructLlvm(const generic::StructInstance& inst); // 按 subst 发 LLVM struct 类型
    void emitGenericEnumLlvm(const generic::EnumInstance& inst);     // 按 subst 发 LLVM enum 类型
    void emitGenericEnumDtor(generic::EnumInstance& inst);           // 该单态若需析构则合成 dtor
    void emitInstanceMethods();                                      // 生成所有泛型结构体实例的方法
    void emitFnInstances();                                          // 生成所有泛型函数实例

    // ==================== 当前编译状态 ====================
    llvm::Function* _currentFn = nullptr; // 当前正在编译的函数
    FnNode* _currentFnNode = nullptr;     // 当前函数的 AST 节点
    string _currentStructName;            // 当前方法所属的结构体名
    // String 字面量发射缓存（per-module，避免跨测试状态泄漏）
    llvm::GlobalVariable* _strEmptyBlock = nullptr; // 空 String sentinel block 复用
    int _strDataCounter = 0;                        // .str.data 命名计数器
    int _strBlockCounter = 0;                       // .str.rc 命名计数器
    // #Inline #Cval 内联常量值缓存（mangledName → llvm::Constant*）。
    // compileGlobalConsts 在遇到 #Inline 标注的 #Cval 时不创建 GlobalVariable，
    // 而是把 llvm::Constant* 存入此表；compileLiteralExpr 在引用全局常量时优先查此表，
    // 命中则直接返回常量（无 load），实现 C #define 风格的内联替换。
    std::map<std::string, llvm::Constant*> _inlineConstantValues;
    // Phase 2c：当前正在编译的 lambda body 作用域（emitLambdaFunction 期间有效）
    // 非空时 compileLiteralExpr 的 LiteralObj 路径启用 FV 校验 / 捕获识别。
    ScopeNode* _currentLambdaBodyScope = nullptr;
    // Phase 4a：当前正在编译的 lambda 节点（emitLambdaFunction 期间有效）
    // 非空时 compileLiteralExpr 命中外层 local 标识符 → addCapture + GEP 读 captures。
    // 捕获通路未启走 throwSemaGap；不支持的捕获类型由 SemaPass 报 E2029。
    LambdaExprNode* _currentLambdaForCapture = nullptr;
    // captures 指针：emit lambda body 时缓存"当前 lambda 函数的第 0 形参（captures Ptr）"，
    // compileLiteralExpr 命中捕获时用作 GEP base
    llvm::Value* _currentLambdaCapturesArg = nullptr;
    // 4.3：compileExpr 经 accept 写回；嵌套 compileExpr 用栈帧保存恢复。
    llvm::Value* _compileExprResult = nullptr;

    // 5.2：子系统声明。新成员写对应窄头，不必改本文件。
    // clang-format off
#define RIU_COMPILER_MEMBERS
#include "compiler_ownership.h"
#include "compiler_destructor.h"
#include "compiler_dyn_vtable.h"
#include "compiler_types.h"
#include "compiler_call.h"
#include "compiler_expr.h"
#include "compiler_stmt.h"
#undef RIU_COMPILER_MEMBERS
    // clang-format on

    // codegen Pass：分析表跑完后发 LLVM IR（原 Compiler::compile 后半）。
    class CodegenPass;
    void emitIr(FileNode* file);
    // 测试 DLL 模式：生成 _riu_register_tests()（InternalLinkage），为每个 #Test fn 调 _riu_test_register
    // 由 riu_test_init（dllexport）在全局 init 之后调用
    void emitTestRegistrations();

    // 4.3：AstVisitor。表达式写 _compileExprResult；语句转发 compile*Statement。
public:
    void visitCall(ExprCallNode&) override;
    void visitLiteral(ExprLiteralNode&) override;
    void visitAddSub(ExprAddSubNode&) override;
    void visitMulDivMod(ExprMulDivModNode&) override;
    void visitBinOp(ExprBinOpNode&) override;
    void visitParen(ExprParenNode&) override;
    void visitDot(ExprDotNode&) override;
    void visitCompare(ExprCompareNode&) override;
    void visitIfElse(ExprIfElseNode&) override;
    void visitOneLineIfElse(ExprOneLineIfElseNode&) override;
    void visitGet(ExprGetNode&) override;
    void visitArray(ExprArrayNode&) override;
    void visitArrayInit(ExprArrayInitNode&) override;
    void visitGetRef(ExprGetRefNode&) override;
    void visitUnary(ExprUnaryNode&) override;
    void visitLambda(LambdaExprNode&) override;
    void visitTuple(ExprTupleNode&) override;
    void visitPathCall(ExprPathCallNode&) override;
    void visitStructLit(ExprStructLitNode&) override;
    void visitMatch(ExprMatchNode&) override;
    void visitTryCatch(ExprTryCatchNode&) override;
    void visitDynCtor(ExprDynCtorNode&) override;
    void visitMoveAssign(ExprMoveAssignNode&) override;
    void visitNullElse(ExprNullElseNode&) override;

    void visitBlock(StatementBlockNode&) override;
    void visitExprStmt(StatementExprNode&) override;
    void visitRet(StatementRetNode&) override;
    void visitRetVoid(StatementRetVoidNode&) override;
    void visitDeclare(StatementDeclareNode&) override;
    void visitDeclareAssign(StatementDeclareAssignNode&) override;
    void visitDeclareAssignTuple(StatementDeclareAssignTupleNode&) override;
    void visitAssign(StatementAssignNode&) override;
    void visitLoop(StatementLoopNode&) override;
    void visitBreak(StatementBreakNode&) override;
    void visitContinue(StatementContinueNode&) override;
    void visitForIn(StatementForInNode&) override;
    void visitStaticFieldSet(StatementStaticFieldSetNode&) override;
    void visitSet(StatementSetNode&) override;
    void visitAlias(AliasDeclNode&) override;

    // ==================== 构造函数 ====================
    // @param context   LLVM 上下文
    // @param builder   IR 构建器
    // @param mod       LLVM 模块
    // @param file       要编译的源文件 AST
    // @param riu        编译器主驱动 (可选)
    // @param isSdk      是否为 SDK 编译
    // @param isTestDll  是否为 test DLL 模式（断言走 _riu_test_throw_failure + dllexport）
    Compiler(llvm::LLVMContext& context, llvm::IRBuilder<>& builder, llvm::Module* mod, FileNode* file,
             Riu* riu = nullptr, bool isSdk = false, bool isTestDll = false);

    // ==================== 编译入口 ====================
    void compile(FileNode* file); // 编译文件 (主入口)
    void compileGlobalConsts();   // 编译全局常量
    void compileGlobalVars();     // 编译全局变量（DRAFT-static-vars Phase 1）
    // 跨模块静态字段：本模块无定义时声明 ExternalLinkage GV（定义在 owner 模块）。
    llvm::GlobalVariable* getOrDeclareStaticFieldGV(const string& mangledName, llvm::Type* llvmType, bool isConstant);
    // DRAFT-const-eval Phase 5: ConstantValue -> llvm::Constant 翻译 (递归; 支持 Struct 嵌套).
    // 失败 (含未支持的 kind / 字段类型不匹配) 返回 nullptr, 调用方报错.
    llvm::Constant* buildLLVMConstantFromValue(const ConstantValue& v, llvm::Type* expectedTy);
    // #Cval #Inline 字段 init 直接求值为 llvm::Constant*（按字段类型解析，越界 E3103）
    llvm::Constant* evalInlineFieldInit(ExprNode* init, const TypeInfo& fieldType, llvm::Type* llvmType);
    void compileStructDecls();                          // 编译结构体声明
    void compileStructImpls();                          // 编译结构体实现 (方法、析构函数)
    void compileFn(FnNode* node, llvm::Function* func); // 编译函数
    void compileMethod(FnNode* node, llvm::Function* func, const string& structName, bool isDestructor = false,
                       bool isStatic = false); // 编译方法 (isStatic=true 跳过 $ 注入与 ctor 零初始化)
    // compileMethod 本体；外层 compileMethod 捕获 RiuError 后按方法节点所属文件补路径
    void compileMethodImpl(FnNode* node, llvm::Function* func, const string& structName, bool isDestructor,
                           bool isStatic);
    // DRAFT-spec-default-body Phase 3: 把该 impl 登记的 InheritedDefault 按 spec 默认体编出来.
    // 用 spec 默认体 FnNode 直接走 compileMethod, 期间临时 patch TypeSelfNode 与 $/形参符号表
    // 到 structName, 编完原样还原. 同步处理 DRAFT-spec-disambig-at 的 @-tagged 发射点.
    void compileInheritedDefaults(StructImplNode* impl, const string& structName);
    // 单条 spec 默认体 emit (compileInheritedDefaults / disambig 共用): emitMethodName 决定
    // LLVM 函数符号 + fnSymbol 表 key.
    void emitSpecDefaultBodyMethod(SpecDeclNode* spec, size_t sigIdx, const string& structName,
                                   const string& emitMethodName, const std::map<std::string, TypeInfo>& subst);
    void compileStatement(StatementNode* node); // 编译语句 (分发函数)
};

#endif // RIU_LANG_COMPILER_H
