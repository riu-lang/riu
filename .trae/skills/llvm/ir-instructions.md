# LLVM IR 指令系统

LLVM IR指令是程序执行的基本单元。本文档介绍指令系统的核心概念。

## Instruction（指令）

**位置**: [llvm/include/llvm/IR/Instruction.h](llvm/IR/Instruction.h)

Instruction是所有LLVM指令的基类。

### 继承关系

```
Instruction : public User, public ilist_node_with_parent<Instruction, BasicBlock>
  ├── UnaryInstruction (一元指令)
  │     ├── AllocaInst
  │     ├── LoadInst
  │     ├── UnaryOperator (fneg)
  │     └── CastInst (类型转换)
  ├── BinaryOperator (二元指令)
  ├── CmpInst (比较指令)
  ├── TerminatorInst (终结指令)
  │     ├── RetInst
  │     ├── BranchInst
  │     └── SwitchInst
  └── ...
```

### 核心API

```cpp
class Instruction : public User {
  DebugLoc DbgLoc;        // 调试位置
  unsigned Order;         // 在基本块中的顺序
  
public:
  BasicBlock *getParent();           // 所属基本块
  Function *getFunction();           // 所属函数
  Module *getModule();               // 所属模块
  
  unsigned getOpcode();              // 操作码
  const char *getOpcodeName();       // 操作码名称
  
  void removeFromParent();           // 从基本块移除
  void eraseFromParent();            // 从基本块移除并删除
  
  void moveBefore(Instruction *I);   // 移动到指令前
  void moveAfter(Instruction *I);    // 移动到指令后
  
  DebugLoc getDebugLoc();            // 获取调试位置
  void setDebugLoc(DebugLoc Loc);    // 设置调试位置
};
```

### 指令分类

| 类别 | 说明 | 示例 |
|------|------|------|
| 终结指令 | 基本块结束 | ret, br, switch |
| 一元指令 | 单操作数 | alloca, load, fneg |
| 二元指令 | 双操作数 | add, sub, mul |
| 内存指令 | 内存访问 | load, store, getelementptr |
| 比较指令 | 比较操作 | icmp, fcmp |
| 类型转换 | 类型转换 | bitcast, trunc, zext |
| 其他指令 | 特殊操作 | call, phi, select |

## 操作码定义

**位置**: [llvm/include/llvm/IR/Instruction.def](llvm/IR/Instruction.def)

### 终结指令（Terminator）

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `Ret` | 返回 | `ret void` / `ret i32 %x` |
| `Br` | 分支 | `br label %dest` / `br i1 %cond, label %t, label %f` |
| `Switch` | 开关 | `switch i32 %val, label %default [i32 1, label %case1]` |
| `IndirectBr` | 间接分支 | `indirectbr i8* %addr, [label %l1, label %l2]` |
| `Invoke` | 调用（带异常） | `invoke ... to label %normal unwind label %exception` |
| `Resume` | 恢复异常 | `resume {i8*, i32} %exn` |
| `Unreachable` | 不可达 | `unreachable` |
| `CallBr` | 分支调用 | `callbr ... to label %normal [label %fallthrough]` |

### 一元指令（Unary）

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `FNeg` | 浮点取负 | `%r = fneg float %x` |

### 二元指令（Binary）

#### 整数二元指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `Add` | 加法 | `%r = add i32 %a, %b` |
| `Sub` | 减法 | `%r = sub i32 %a, %b` |
| `Mul` | 乘法 | `%r = mul i32 %a, %b` |
| `UDiv` | 无符号除法 | `%r = udiv i32 %a, %b` |
| `SDiv` | 有符号除法 | `%r = sdiv i32 %a, %b` |
| `URem` | 无符号余数 | `%r = urem i32 %a, %b` |
| `SRem` | 有符号余数 | `%r = srem i32 %a, %b` |

#### 浮点二元指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `FAdd` | 浮点加法 | `%r = fadd float %a, %b` |
| `FSub` | 浮点减法 | `%r = fsub float %a, %b` |
| `FMul` | 浮点乘法 | `%r = fmul float %a, %b` |
| `FDiv` | 浮点除法 | `%r = fdiv float %a, %b` |
| `FRem` | 浮点余数 | `%r = frem float %a, %b` |

#### 位运算指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `Shl` | 左移 | `%r = shl i32 %a, %b` |
| `LShr` | 逻辑右移 | `%r = lshr i32 %a, %b` |
| `AShr` | 算术右移 | `%r = ashr i32 %a, %b` |
| `And` | 与 | `%r = and i32 %a, %b` |
| `Or` | 或 | `%r = or i32 %a, %b` |
| `Xor` | 异或 | `%r = xor i32 %a, %b` |

### 内存指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `Alloca` | 栈分配 | `%p = alloca i32` |
| `Load` | 加载 | `%v = load i32, i32* %p` |
| `Store` | 存储 | `store i32 %v, i32* %p` |
| `GetElementPtr` | 获取元素指针 | `%p = getelementptr i32, i32* %arr, i64 %idx` |
| `AtomicCmpXchg` | 原子比较交换 | `%r = cmpxchg i32* %p, i32 %old, i32 %new seq_cst seq_cst` |
| `AtomicRMW` | 原子读改写 | `%r = atomicrmw add i32* %p, i32 %v seq_cst` |
| `Fence` | 内存屏障 | `fence seq_cst` |

### 类型转换指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `Trunc` | 截断 | `%r = trunc i32 %x to i16` |
| `ZExt` | 零扩展 | `%r = zext i16 %x to i32` |
| `SExt` | 符号扩展 | `%r = sext i16 %x to i32` |
| `FPTrunc` | 浮点截断 | `%r = fptrunc double %x to float` |
| `FPExt` | 浮点扩展 | `%r = fpext float %x to double` |
| `FPToUI` | 浮点转无符号整数 | `%r = fptoui float %x to i32` |
| `FPToSI` | 浮点转有符号整数 | `%r = fptosi float %x to i32` |
| `UIToFP` | 无符号整数转浮点 | `%r = uitofp i32 %x to float` |
| `SIToFP` | 有符号整数转浮点 | `%r = sitofp i32 %x to float` |
| `PtrToInt` | 指针转整数 | `%r = ptrtoint i8* %p to i64` |
| `IntToPtr` | 整数转指针 | `%r = inttoptr i64 %x to i8*` |
| `BitCast` | 位转换 | `%r = bitcast i32 %x to float` |
| `AddrSpaceCast` | 地址空间转换 | `%r = addrspacecast i32* %p to i32 addrspace(1)*` |

### 比较指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `ICmp` | 整数比较 | `%r = icmp eq i32 %a, %b` |
| `FCmp` | 浮点比较 | `%r = fcmp oeq float %a, %b` |

### 其他指令

| 操作码 | 说明 | IR语法 |
|--------|------|--------|
| `PHI` | PHI节点 | `%r = phi i32 [%a, %bb1], [%b, %bb2]` |
| `Call` | 函数调用 | `%r = call i32 @func(i32 %x)` |
| `Select` | 选择 | `%r = select i1 %cond, i32 %a, i32 %b` |
| `ExtractValue` | 提取聚合值 | `%r = extractvalue {i32, float} %agg, 0` |
| `InsertValue` | 插入聚合值 | `%r = insertvalue {i32, float} %agg, i32 %v, 0` |
| `ExtractElement` | 提取向量元素 | `%r = extractelement <4 x i32> %vec, i32 %idx` |
| `InsertElement` | 插入向量元素 | `%r = insertelement <4 x i32> %vec, i32 %v, i32 %idx` |
| `ShuffleVector` | 向量洗牌 | `%r = shufflevector <4 x i32> %v1, <4 x i32> %v2, <4 x i32> <i32 0, i32 5, i32 2, i32 7>` |
| `Freeze` | 冻结值 | `%r = freeze i32 %x` |

## 主要指令类

### AllocaInst（栈分配）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h#L65)

```cpp
class AllocaInst : public UnaryInstruction {
  Type *AllocatedType;  // 分配的类型
  
public:
  Type *getAllocatedType() const;     // 获取分配类型
  Value *getArraySize();              // 获取数组大小
  Align getAlign();                   // 获取对齐
  unsigned getAddressSpace();         // 获取地址空间
  
  bool isStaticAlloca();              // 是否静态分配
  bool isArrayAllocation();           // 是否数组分配
};

// 创建示例
AllocaInst *AI = new AllocaInst(Type::getInt32Ty(Context), 0, "x", BB);
```

### LoadInst / StoreInst（加载/存储）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h#L181)

```cpp
class LoadInst : public UnaryInstruction {
public:
  Value *getPointerOperand();         // 获取指针操作数
  Align getAlign();                   // 获取对齐
  bool isVolatile();                  // 是否volatile
  AtomicOrdering getOrdering();       // 内存序
};

class StoreInst : public Instruction {
public:
  Value *getValueOperand();           // 获取值操作数
  Value *getPointerOperand();         // 获取指针操作数
  Align getAlign();                   // 获取对齐
  bool isVolatile();                  // 是否volatile
  AtomicOrdering getOrdering();       // 内存序
};

// 创建示例
LoadInst *LI = new LoadInst(Type::getInt32Ty(Context), Ptr, "val", BB);
StoreInst *SI = new StoreInst(Val, Ptr, BB);
```

### BinaryOperator（二元运算）

**位置**: [llvm/include/llvm/IR/InstrTypes.h](llvm/IR/InstrTypes.h#L101)

```cpp
class BinaryOperator : public Instruction {
public:
  static BinaryOperator *Create(BinaryOps Op, Value *S1, Value *S2,
                                const Twine &Name = "",
                                InsertPosition InsertBefore = nullptr);
  
  Value *getOperand(unsigned i);
  bool isCommutative();               // 是否可交换
  bool isAssociative();               // 是否可结合
  
  // 便捷方法
  static BinaryOperator *CreateAdd(Value *S1, Value *S2, const Twine &Name = "");
  static BinaryOperator *CreateSub(Value *S1, Value *S2, const Twine &Name = "");
  static BinaryOperator *CreateMul(Value *S1, Value *S2, const Twine &Name = "");
};

// 创建示例
BinaryOperator *BO = BinaryOperator::CreateAdd(A, B, "sum", BB);
```

### CmpInst（比较指令）

**位置**: [llvm/include/llvm/IR/InstrTypes.h](llvm/IR/InstrTypes.h)

```cpp
class CmpInst : public Instruction {
public:
  enum Predicate {
    // 整数比较谓词
    ICMP_EQ = 32,  ICMP_NE = 33,
    ICMP_UGT = 34, ICMP_UGE = 35, ICMP_ULT = 36, ICMP_ULE = 37,
    ICMP_SGT = 38, ICMP_SGE = 39, ICMP_SLT = 40, ICMP_SLE = 41,
    
    // 浮点比较谓词
    FCMP_FALSE = 0, FCMP_OEQ = 1, FCMP_OGT = 2, FCMP_OGE = 3,
    FCMP_OLT = 4, FCMP_OLE = 5, FCMP_ONE = 6, FCMP_ORD = 7,
    FCMP_UNO = 8, FCMP_UEQ = 9, FCMP_UGT = 10, FCMP_UGE = 11,
    FCMP_ULT = 12, FCMP_ULE = 13, FCMP_UNE = 14, FCMP_TRUE = 15
  };
  
  Predicate getPredicate() const;
  Value *getOperand(unsigned i);
  
  static CmpInst *Create(OtherOps Op, Predicate Pred, Value *S1, Value *S2,
                        const Twine &Name = "", InsertPosition InsertBefore = nullptr);
};

// 创建示例
CmpInst *CI = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, A, B, "cmp", BB);
```

### BranchInst（分支指令）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h)

```cpp
class BranchInst : public TerminatorInst {
public:
  bool isConditional();               // 是否条件分支
  bool isUnconditional();             // 是否无条件分支
  
  Value *getCondition();              // 获取条件
  BasicBlock *getSuccessor(unsigned i); // 获取后继块
  
  static BranchInst *Create(BasicBlock *IfTrue, InsertPosition InsertBefore = nullptr);
  static BranchInst *Create(BasicBlock *IfTrue, BasicBlock *IfFalse,
                           Value *Cond, InsertPosition InsertBefore = nullptr);
};

// 创建示例
BranchInst *BI1 = BranchInst::Create(DestBB, BB);  // 无条件跳转
BranchInst *BI2 = BranchInst::Create(TrueBB, FalseBB, Cond, BB);  // 条件分支
```

### CallInst（函数调用）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h)

```cpp
class CallInst : public Instruction {
public:
  Function *getCalledFunction();      // 获取被调用函数
  Value *getCalledOperand();          // 获取被调用操作数
  
  unsigned arg_size();                // 参数数量
  Value *getArgOperand(unsigned i);   // 获取参数
  
  CallingConv::ID getCallingConv();   // 获取调用约定
  AttributeList getAttributes();      // 获取属性
  
  static CallInst *Create(FunctionType *Ty, Value *F,
                         ArrayRef<Value *> Args,
                         const Twine &NameStr = "",
                         InsertPosition InsertBefore = nullptr);
};

// 创建示例
CallInst *CI = CallInst::Create(FT, Func, {Arg1, Arg2}, "result", BB);
```

### GetElementPtrInst（GEP）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h)

```cpp
class GetElementPtrInst : public Instruction {
public:
  Value *getPointerOperand();         // 获取指针操作数
  Type *getSourceElementType();       // 获取源元素类型
  Type *getResultElementType();       // 获取结果元素类型
  
  unsigned getNumIndices();           // 获取索引数量
  Value *getOperand(unsigned i);      // 获取索引
  
  static GetElementPtrInst *Create(Type *SourceElementType, Value *Ptr,
                                  ArrayRef<Value *> IdxList,
                                  const Twine &NameStr = "",
                                  InsertPosition InsertBefore = nullptr);
};

// 创建示例
// 获取数组第i个元素的指针
GetElementPtrInst *GEP = GetElementPtrInst::Create(
  ArrayType::get(Type::getInt32Ty(Context), 10),
  ArrayPtr,
  {ConstantInt::get(Type::getInt64Ty(Context), 0),
   Index},
  "elem_ptr",
  BB
);
```

### PHINode（PHI节点）

**位置**: [llvm/include/llvm/IR/Instructions.h](llvm/IR/Instructions.h)

```cpp
class PHINode : public Instruction {
public:
  void addIncoming(Value *V, BasicBlock *BB);  // 添加入边
  Value *getIncomingValue(unsigned i);         // 获取入边值
  BasicBlock *getIncomingBlock(unsigned i);    // 获取入边块
  unsigned getNumIncomingValues();             // 入边数量
  
  static PHINode *Create(Type *Ty, unsigned NumReservedValues,
                        const Twine &NameStr = "",
                        InsertPosition InsertBefore = nullptr);
};

// 创建示例
PHINode *PN = PHINode::Create(Type::getInt32Ty(Context), 2, "phi", BB);
PN->addIncoming(Val1, BB1);
PN->addIncoming(Val2, BB2);
```

## 指令属性

### FastMathFlags（快速数学标志）

```cpp
class FastMathFlags {
public:
  bool noNaNs() const;        // 允许假设无NaN
  bool noInfs() const;        // 允许假设无无穷
  bool noSignedZeros() const; // 允许忽略符号零
  bool allowReciprocal() const; // 允许倒数优化
  bool allowContract() const; // 允许融合
  bool approxFunc() const;    // 允许近似函数
  bool unsafeAlgebra() const; // 允许不安全代数
};

// 设置快速数学标志
Instruction *I = ...;
I->setFastMathFlags(FMF);
```

### 原子序（AtomicOrdering）

```cpp
enum class AtomicOrdering {
  NotAtomic = 0,
  Unordered = 1,
  Monotonic = 2,
  Acquire = 3,
  Release = 4,
  AcquireRelease = 5,
  SequentiallyConsistent = 6
};

// 使用示例
LoadInst *LI = new LoadInst(Ty, Ptr, "val", false, Align(4), AtomicOrdering::SequentiallyConsistent, BB);
```

## 相关文件

### 头文件

- [Instruction.h](llvm/IR/Instruction.h) - 指令基类
- [Instructions.h](llvm/IR/Instructions.h) - 具体指令类
- [InstrTypes.h](llvm/IR/InstrTypes.h) - 指令类型
- [Instruction.def](llvm/IR/Instruction.def) - 指令定义

### 源文件

- [Instruction.cpp](llvm/lib/IR/Instruction.cpp) - 指令实现
- [Instructions.cpp](llvm/lib/IR/Instructions.cpp) - 具体指令实现

## 参考链接

- [LLVM Instructions](https://llvm.org/docs/LangRef.html#instruction-reference)
- [LLVM Atomic Instructions](https://llvm.org/docs/Atomics.html)
