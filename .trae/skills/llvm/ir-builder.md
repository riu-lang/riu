# LLVM IR 构建与操作

本文档介绍如何构建和操作LLVM IR，包括IRBuilder、Pass管理等。

## IRBuilder（IR构建器）

**位置**: [llvm/include/llvm/IR/IRBuilder.h](llvm/IR/IRBuilder.h)

IRBuilder是创建LLVM指令的便捷工具类。

### 核心特性

- 提供一致的简化接口创建指令
- 自动插入指令到指定位置
- 支持常量折叠
- 支持快速数学标志
- 支持调试信息

### 基本用法

```cpp
#include "llvm/IR/IRBuilder.h"

LLVMContext Context;
Module M("test", Context);
IRBuilder<> Builder(Context);

// 设置插入点
BasicBlock *BB = BasicBlock::Create(Context, "entry", F);
Builder.SetInsertPoint(BB);

// 创建指令
Value *A = Builder.CreateAlloca(Type::getInt32Ty(Context), nullptr, "a");
Value *LoadA = Builder.CreateLoad(Type::getInt32Ty(Context), A, "load_a");
Value *One = ConstantInt::get(Type::getInt32Ty(Context), 1);
Value *Add = Builder.CreateAdd(LoadA, One, "add");
Builder.CreateRet(Add);
```

### IRBuilderBase API

```cpp
class IRBuilderBase {
protected:
  BasicBlock *BB;
  BasicBlock::iterator InsertPt;
  LLVMContext &Context;
  const IRBuilderFolder &Folder;
  const IRBuilderDefaultInserter &Inserter;
  
public:
  // 插入点管理
  void SetInsertPoint(BasicBlock *BB);
  void SetInsertPoint(Instruction *I);
  void SetInsertPoint(BasicBlock *BB, BasicBlock::iterator IP);
  
  BasicBlock *GetInsertBlock();
  BasicBlock::iterator GetInsertPoint();
  
  // 调试信息
  void SetCurrentDebugLocation(DebugLoc L);
  DebugLoc getCurrentDebugLocation();
  void SetInstDebugLocation(Instruction *I);
};
```

### 类型创建方法

```cpp
// 整数类型
IntegerType *getInt1Ty();
IntegerType *getInt8Ty();
IntegerType *getInt16Ty();
IntegerType *getInt32Ty();
IntegerType *getInt64Ty();
IntegerType *getInt128Ty();
IntegerType *getIntNTy(unsigned N);

// 浮点类型
Type *getFloatTy();
Type *getDoubleTy();
Type *getVoidTy();

// 指针类型
PointerType *getInt8PtrTy(unsigned AS = 0);
PointerType *getInt32PtrTy(unsigned AS = 0);

// 函数类型
FunctionType *getFunctionType(Type *Result, ArrayRef<Type*> Params, bool isVarArg);
```

### 常量创建方法

```cpp
// 整数常量
ConstantInt *getInt1(bool V);
ConstantInt *getInt8(uint8_t V);
ConstantInt *getInt16(uint16_t V);
ConstantInt *getInt32(uint32_t V);
ConstantInt *getInt64(uint64_t V);
ConstantInt *getIntN(unsigned N, uint64_t V);

// 浮点常量
ConstantFP *getFloat(float V);
ConstantFP *getDouble(double V);

// 特殊常量
ConstantPointerNull *getNullPtr(Type *Ty);
UndefValue *getUndef(Type *Ty);
PoisonValue *getPoison(Type *Ty);
```

### 指令创建方法

#### 终结指令

```cpp
// 返回
ReturnInst *CreateRet(Value *RetVal);
ReturnInst *CreateRetVoid();

// 分支
BranchInst *CreateBr(BasicBlock *Dest);
BranchInst *CreateCondBr(Value *Cond, BasicBlock *True, BasicBlock *False);

// Switch
SwitchInst *CreateSwitch(Value *V, BasicBlock *Dest, unsigned NumCases);

// 不可达
UnreachableInst *CreateUnreachable();
```

#### 算术指令

```cpp
// 整数算术
Value *CreateAdd(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateSub(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateMul(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateUDiv(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateSDiv(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateURem(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateSRem(Value *LHS, Value *RHS, const Twine &Name = "");

// 浮点算术
Value *CreateFAdd(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFSub(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFMul(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFDiv(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFRem(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFNeg(Value *V, const Twine &Name = "");
```

#### 位运算指令

```cpp
Value *CreateShl(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateLShr(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateAShr(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateAnd(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateOr(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateXor(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateNot(Value *V, const Twine &Name = "");
```

#### 内存指令

```cpp
// Alloca
AllocaInst *CreateAlloca(Type *Ty, Value *ArraySize = nullptr, const Twine &Name = "");

// Load/Store
LoadInst *CreateLoad(Type *Ty, Value *Ptr, const Twine &Name = "");
LoadInst *CreateAlignedLoad(Type *Ty, Value *Ptr, Align Align, const Twine &Name = "");
StoreInst *CreateStore(Value *Val, Value *Ptr);
StoreInst *CreateAlignedStore(Value *Val, Value *Ptr, Align Align);

// GEP
Value *CreateGEP(Type *SourceElementType, Value *Ptr, ArrayRef<Value *> IdxList,
                const Twine &Name = "");
Value *CreateInBoundsGEP(Type *SourceElementType, Value *Ptr, ArrayRef<Value *> IdxList,
                        const Twine &Name = "");

// 结构体访问
Value *CreateStructGEP(Type *StructTy, Value *Ptr, unsigned Idx, const Twine &Name = "");

// 数组访问
Value *CreateConstGEP1_32(Type *Ty, Value *Ptr, unsigned Idx0, const Twine &Name = "");
```

#### 比较指令

```cpp
// 整数比较
Value *CreateICmpEQ(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpNE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpUGT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpUGE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpULT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpULE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpSGT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpSGE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpSLT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateICmpSLE(Value *LHS, Value *RHS, const Twine &Name = "");

// 浮点比较
Value *CreateFCmpOEQ(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFCmpONE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFCmpOGT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFCmpOGE(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFCmpOLT(Value *LHS, Value *RHS, const Twine &Name = "");
Value *CreateFCmpOLE(Value *LHS, Value *RHS, const Twine &Name = "");
```

#### 类型转换指令

```cpp
Value *CreateTrunc(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateZExt(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateSExt(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateFPToUI(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateFPToSI(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateUIToFP(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateSIToFP(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateFPTrunc(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateFPExt(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreatePtrToInt(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateIntToPtr(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateBitCast(Value *V, Type *DestTy, const Twine &Name = "");
Value *CreateAddrSpaceCast(Value *V, Type *DestTy, const Twine &Name = "");
```

#### 其他指令

```cpp
// 函数调用
CallInst *CreateCall(FunctionType *FTy, Value *Callee, ArrayRef<Value *> Args,
                    const Twine &Name = "");

// Select
Value *CreateSelect(Value *C, Value *True, Value *False, const Twine &Name = "");

// PHI
PHINode *CreatePHI(Type *Ty, unsigned NumReservedValues, const Twine &Name = "");

// ExtractValue/InsertValue
Value *CreateExtractValue(Value *Agg, ArrayRef<unsigned> Idxs, const Twine &Name = "");
Value *CreateInsertValue(Value *Agg, Value *Val, ArrayRef<unsigned> Idxs, const Twine &Name = "");

// 向量操作
Value *CreateExtractElement(Value *Vec, Value *Idx, const Twine &Name = "");
Value *CreateInsertElement(Value *Vec, Value *NewElt, Value *Idx, const Twine &Name = "");
Value *CreateShuffleVector(Value *V1, Value *V2, Value *Mask, const Twine &Name = "");
```

### 快速数学标志

```cpp
// 设置快速数学标志
FastMathFlags FMF;
FMF.setNoNaNs();
FMF.setNoInfs();
FMF.setUnsafeAlgebra();
Builder.setFastMathFlags(FMF);

// 或从指令复制
Instruction *SrcInst = ...;
Builder.copyFastMathFlags(SrcInst);
```

### 常量折叠

IRBuilder支持自动常量折叠：

```cpp
// 如果操作数都是常量，会自动折叠
Value *Sum = Builder.CreateAdd(
  ConstantInt::get(Type::getInt32Ty(Context), 1),
  ConstantInt::get(Type::getInt32Ty(Context), 2)
);  // 结果是常量3，不会创建指令
```

## Pass管理器

**位置**: [llvm/include/llvm/IR/PassManager.h](llvm/IR/PassManager.h)

### 新Pass管理器（New Pass Manager）

LLVM使用新的Pass管理器架构，基于概念多态。

#### PassInfoMixin

```cpp
template <typename DerivedT>
struct PassInfoMixin {
  static StringRef name();  // 获取Pass名称
};
```

#### AnalysisInfoMixin

```cpp
template <typename DerivedT>
struct AnalysisInfoMixin : PassInfoMixin<DerivedT> {
  static AnalysisKey *ID();  // 获取分析ID
};
```

### ModulePassManager

```cpp
class ModulePassManager {
public:
  // 添加Pass
  void addPass(PassT Pass);
  
  // 运行Pass
  PreservedAnalyses run(Module &M, AnalysisManager<Module> &AM);
};

// 使用示例
ModulePassManager MPM;
MPM.addPass(MyModulePass());
MPM.addPass(AnotherModulePass());
PreservedAnalyses PA = MPM.run(M, MAM);
```

### FunctionPassManager

```cpp
class FunctionPassManager {
public:
  void addPass(PassT Pass);
  PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM);
};

// 使用示例
FunctionPassManager FPM;
FPM.addPass(MyFunctionPass());
PreservedAnalyses PA = FPM.run(F, FAM);
```

### AnalysisManager

```cpp
template <typename IRUnitT, typename... ExtraArgTs>
class AnalysisManager {
public:
  // 获取分析结果
  ResultT getResult(IRUnitT &IR, ExtraArgTs... ExtraArgs);
  
  // 缓存分析结果
  ResultT getCachedResult(IRUnitT &IR);
  
  // 使分析失效
  void invalidate(IRUnitT &IR, const PreservedAnalyses &PA);
};

// 使用示例
AnalysisManager<Module> MAM;
AnalysisManager<Function> FAM;

MyAnalysis::Result &Result = MAM.getResult<MyAnalysis>(M);
```

### 编写自定义Pass

#### 模块Pass

```cpp
struct MyModulePass : public PassInfoMixin<MyModulePass> {
  PreservedAnalyses run(Module &M, AnalysisManager<Module> &AM) {
    // 处理模块
    for (Function &F : M) {
      // 处理函数
    }
    
    // 返回保留的分析
    return PreservedAnalyses::all();
  }
  
  static bool isRequired() { return true; }  // 是否必需
};
```

#### 函数Pass

```cpp
struct MyFunctionPass : public PassInfoMixin<MyFunctionPass> {
  PreservedAnalyses run(Function &F, AnalysisManager<Function> &AM) {
    // 处理函数
    for (BasicBlock &BB : F) {
      // 处理基本块
    }
    
    return PreservedAnalyses::all();
  }
};
```

#### 分析Pass

```cpp
struct MyAnalysis : public AnalysisInfoMixin<MyAnalysis> {
  struct Result {
    // 分析结果数据
  };
  
  Result run(Function &F, AnalysisManager<Function> &AM) {
    Result R;
    // 执行分析
    return R;
  }
  
  static AnalysisKey Key;  // 唯一标识
};

AnalysisKey MyAnalysis::Key;
```

### PreservedAnalyses

```cpp
class PreservedAnalyses {
public:
  static PreservedAnalyses all();      // 保留所有分析
  static PreservedAnalyses none();     // 不保留任何分析
  
  void preserve();                     // 保留所有
  void preserveSet();   // 保留某类分析
  void abandon();                      // 放弃所有
  
  bool preserved() const;              // 是否保留所有
  bool preserved(AnalysisKey *ID) const;  // 是否保留特定分析
};

// 使用示例
PreservedAnalyses PA;
PA.preserve<DominatorTreeAnalysis>();
PA.preserve<AssumptionAnalysis>();
return PA;
```

## 遗留Pass管理器（Legacy Pass Manager）

**位置**: [llvm/include/llvm/IR/LegacyPassManager.h](llvm/IR/LegacyPassManager.h)

遗留Pass管理器已被弃用，但仍可用于兼容旧代码。

### 基本用法

```cpp
#include "llvm/IR/LegacyPassManager.h"

legacy::PassManager PM;
PM.add(new MyPass());
PM.run(M);
```

## IR验证器

**位置**: [llvm/include/llvm/IR/Verifier.h](llvm/IR/Verifier.h)

### 使用验证器

```cpp
#include "llvm/IR/Verifier.h"

// 验证模块
bool Broken = verifyModule(M, &errs());
if (Broken) {
  // 模块有错误
}

// 验证函数
bool Broken = verifyFunction(F, &errs());
if (Broken) {
  // 函数有错误
}
```

### 验证Pass

```cpp
// 在Pass管理器中添加验证
MPM.addPass(VerifierPass());
```

## IR打印

### 打印模块

```cpp
#include "llvm/IR/Module.h"

// 打印到标准输出
M.print(outs(), nullptr);

// 打印到文件
std::error_code EC;
raw_fd_ostream OS("output.ll", EC);
M.print(OS, nullptr);
```

### 打印函数

```cpp
F.print(outs());
```

### 打印指令

```cpp
I.print(outs());
outs() << I;  // 使用流操作符
```

## IR遍历

### 遍历模块

```cpp
// 遍历函数
for (Function &F : M) {
  // 处理函数
}

// 遍历全局变量
for (GlobalVariable &GV : M.globals()) {
  // 处理全局变量
}

// 遍历别名
for (GlobalAlias &GA : M.aliases()) {
  // 处理别名
}
```

### 遍历函数

```cpp
// 遍历基本块
for (BasicBlock &BB : F) {
  // 处理基本块
}

// 遍历参数
for (Argument &Arg : F.args()) {
  // 处理参数
}

// 遍历所有指令
for (Instruction &I : instructions(F)) {
  // 处理指令
}
```

### 遍历基本块

```cpp
// 遍历指令
for (Instruction &I : BB) {
  // 处理指令
}

// 遍历PHI节点
for (PHINode &PN : BB.phis()) {
  // 处理PHI节点
}
```

### 遍历指令操作数

```cpp
// 遍历操作数
for (Value *Op : I.operands()) {
  // 处理操作数
}

// 遍历使用者
for (User *U : I.users()) {
  // 处理使用者
}
```

## 相关文件

### 头文件

- [IRBuilder.h](llvm/IR/IRBuilder.h) - IR构建器
- [PassManager.h](llvm/IR/PassManager.h) - 新Pass管理器
- [LegacyPassManager.h](llvm/IR/LegacyPassManager.h) - 遗留Pass管理器
- [Verifier.h](llvm/IR/Verifier.h) - IR验证器
- [InstIterator.h](llvm/IR/InstIterator.h) - 指令迭代器

### 源文件

- [IRBuilder.cpp](llvm/lib/IR/IRBuilder.cpp) - IR构建器实现
- [PassManager.cpp](llvm/lib/IR/PassManager.cpp) - Pass管理器实现
- [Verifier.cpp](llvm/lib/IR/Verifier.cpp) - 验证器实现

## 参考链接

- [LLVM IRBuilder](https://llvm.org/doxygen/classllvm_1_1IRBuilder.html)
- [Writing an LLVM Pass](https://llvm.org/docs/WritingAnLLVMPass.html)
- [New Pass Manager](https://llvm.org/docs/NewPassManager.html)
