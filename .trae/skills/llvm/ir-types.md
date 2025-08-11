# LLVM IR 类型系统

LLVM IR具有丰富的类型系统，本文档介绍类型系统的核心概念。

## Value（值）

**位置**: `llvm/include/llvm/IR/Value.h`

Value是LLVM中所有值的基类，是LLVM IR的核心。

### 继承关系

```
Value
  ├── Argument (参数)
  ├── BasicBlock (基本块)
  ├── InlineAsm (内联汇编)
  ├── User (使用者)
  │     ├── Constant (常量)
  │     │     ├── ConstantInt
  │     │     ├── ConstantFP
  │     │     ├── ConstantArray
  │     │     ├── ConstantStruct
  │     │     └── GlobalValue
  │     │           ├── GlobalVariable
  │     │           └── Function
  │     └── Instruction (指令)
  │           ├── BinaryOperator
  │           ├── CmpInst
  │           ├── CallInst
  │           └── ...
  └── MetadataAsValue
```

### 核心特性

1. **类型**: 每个Value都有一个Type
2. **名称**: 可选的名称，存储在模块符号表中
3. **使用列表**: 跟踪哪些User使用此Value
4. **值句柄**: 支持ValueHandle监听RAUW和Destroy事件

### 核心API

```cpp
class Value {
  Type *VTy;           // 值的类型
  Use *UseList;        // 使用列表
  
public:
  Type *getType() const;              // 获取类型
  LLVMContext &getContext() const;    // 获取上下文
  
  void setName(const Twine &Name);    // 设置名称
  StringRef getName() const;          // 获取名称
  
  void replaceAllUsesWith(Value *V);  // 替换所有使用
  void deleteValue();                 // 删除值
  
  use_iterator use_begin();           // 使用迭代器
  use_iterator use_end();
  bool use_empty();                   // 是否无使用
  bool hasOneUse();                   // 是否单一使用
};
```

### 使用-定义链（Use-Def Chain）

LLVM维护双向的使用-定义链：

- **Use**: 表示对Value的一次使用
- **User**: 使用其他Value的Value
- **Use List**: Value维护的所有Use的列表

```cpp
for (User *U : Value->users()) {
  // 遍历所有使用者
}
```

## User（使用者）

**位置**: `llvm/include/llvm/IR/User.h`

User是使用其他Value作为操作数的Value。

### 继承关系

```
User : public Value
  ├── Constant
  └── Instruction
```

### 核心API

```cpp
class User : public Value {
public:
  Value *getOperand(unsigned i);         // 获取操作数
  void setOperand(unsigned i, Value *Val); // 设置操作数
  unsigned getNumOperands();             // 操作数数量
  
  op_iterator op_begin();                // 操作数迭代器
  op_iterator op_end();
  
  void dropAllReferences();              // 释放所有引用
};
```

### 操作数存储方式

User有两种操作数存储方式：

1. **内嵌存储（Intrusive）**: 操作数与User对象一起分配
2. **悬挂存储（Hung-off）**: 操作数单独分配

## Type（类型）

**位置**: `llvm/include/llvm/IR/Type.h`

Type表示LLVM IR中的类型，是不可变的且唯一的。

### 类型分类

```cpp
enum TypeID {
  // 原始类型
  HalfTyID,        // 16位浮点
  BFloatTyID,      // 16位浮点（7位尾数）
  FloatTyID,       // 32位浮点
  DoubleTyID,      // 64位浮点
  X86_FP80TyID,    // 80位浮点（X87）
  FP128TyID,       // 128位浮点（112位尾数）
  PPC_FP128TyID,   // 128位浮点（两个64位）
  VoidTyID,        // void类型
  LabelTyID,       // 标签
  MetadataTyID,    // 元数据
  X86_AMXTyID,     // AMX向量
  TokenTyID,       // Token
  
  // 派生类型
  IntegerTyID,        // 整数
  FunctionTyID,       // 函数
  PointerTyID,        // 指针
  StructTyID,         // 结构体
  ArrayTyID,          // 数组
  FixedVectorTyID,    // 定长向量
  ScalableVectorTyID, // 可伸缩向量
  TypedPointerTyID,   // 类型化指针
  TargetExtTyID,      // 目标扩展类型
};
```

### 原始类型

| 类型 | 大小 | 说明 |
|------|------|------|
| `void` | 0 | 无类型 |
| `i1` | 1位 | 布尔类型 |
| `i8` | 8位 | 字节 |
| `i16` | 16位 | 字 |
| `i32` | 32位 | 双字 |
| `i64` | 64位 | 四字 |
| `half` | 16位 | IEEE半精度浮点 |
| `float` | 32位 | IEEE单精度浮点 |
| `double` | 64位 | IEEE双精度浮点 |

### 核心API

```cpp
class Type {
  LLVMContext &Context;
  TypeID ID;
  
public:
  TypeID getTypeID() const;
  LLVMContext &getContext() const;
  
  bool isVoidTy() const;
  bool isIntegerTy() const;
  bool isFloatTy() const;
  bool isDoubleTy() const;
  bool isPointerTy() const;
  
  static Type *getVoidTy(LLVMContext &C);
  static Type *getInt1Ty(LLVMContext &C);
  static Type *getInt32Ty(LLVMContext &C);
  static Type *getFloatTy(LLVMContext &C);
  static Type *getDoubleTy(LLVMContext &C);
};
```

## IntegerType（整数类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

IntegerType表示任意位宽的整数类型。

### 核心API

```cpp
class IntegerType : public Type {
public:
  static IntegerType *get(LLVMContext &C, unsigned NumBits);
  unsigned getBitWidth() const;
  
  static constexpr unsigned MAX_INT_BITS = 16777215; // 最大位数
};

// 便捷方法
Type *Type::getInt1Ty(LLVMContext &C);   // i1
Type *Type::getInt8Ty(LLVMContext &C);   // i8
Type *Type::getInt16Ty(LLVMContext &C);  // i16
Type *Type::getInt32Ty(LLVMContext &C);  // i32
Type *Type::getInt64Ty(LLVMContext &C);  // i64
Type *Type::getInt128Ty(LLVMContext &C); // i128
```

## FunctionType（函数类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

FunctionType表示函数类型。

### 核心API

```cpp
class FunctionType : public Type {
public:
  static FunctionType *get(Type *Result,
                           ArrayRef<Type*> Params,
                           bool isVarArg);
  
  Type *getReturnType() const;
  unsigned getNumParams() const;
  Type *getParamType(unsigned i) const;
  bool isVarArg() const;
};
```

### 使用示例

```cpp
// void ()
FunctionType *FT1 = FunctionType::get(Type::getVoidTy(Context), false);

// i32 (i32, i32)
FunctionType *FT2 = FunctionType::get(
  Type::getInt32Ty(Context),
  {Type::getInt32Ty(Context), Type::getInt32Ty(Context)},
  false
);

// i32 (i32, ...) - 可变参数
FunctionType *FT3 = FunctionType::get(
  Type::getInt32Ty(Context),
  {Type::getInt32Ty(Context)},
  true
);
```

## PointerType（指针类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

PointerType表示指针类型（LLVM 15+使用不透明指针）。

### 核心API

```cpp
class PointerType : public Type {
public:
  static PointerType *get(Type *ElementType, unsigned AddressSpace);
  static PointerType *get(LLVMContext &C, unsigned AddressSpace);
  
  Type *getElementType() const;  // LLVM 15+已废弃
  unsigned getAddressSpace() const;
};

// 便捷方法
Type *Type::getInt8PtrTy(LLVMContext &C, unsigned AS = 0);
Type *Type::getInt32PtrTy(LLVMContext &C, unsigned AS = 0);
```

### 地址空间

LLVM支持多个地址空间：

- `0`: 默认地址空间
- `1-223`: 目标特定地址空间
- 示例：GPU的全局内存、共享内存等

## StructType（结构体类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

StructType表示结构体类型。

### 结构体类型

1. **字面结构体（Literal）**: 匿名，按结构等价
2. **命名结构体（Identified）**: 有名称，按名称等价

### 核心API

```cpp
class StructType : public Type {
public:
  static StructType *create(LLVMContext &C, 
                            StringRef Name);
  static StructType *get(LLVMContext &C,
                        ArrayRef<Type*> Elements);
  
  void setBody(ArrayRef<Type*> Elements, bool isPacked = false);
  
  unsigned getNumElements() const;
  Type *getElementType(unsigned N) const;
  StringRef getName() const;
  bool isLiteral() const;
  bool isPacked() const;
};
```

### 使用示例

```cpp
// 字面结构体 { i32, float }
StructType *ST1 = StructType::get(
  Context,
  {Type::getInt32Ty(Context), Type::getFloatTy(Context)}
);

// 命名结构体
StructType *ST2 = StructType::create(Context, "MyStruct");
ST2->setBody({Type::getInt32Ty(Context), Type::getFloatTy(Context)});
```

## ArrayType（数组类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

ArrayType表示数组类型。

### 核心API

```cpp
class ArrayType : public Type {
public:
  static ArrayType *get(Type *ElementType, uint64_t NumElements);
  
  uint64_t getNumElements() const;
  Type *getElementType() const;
};
```

### 使用示例

```cpp
// [10 x i32]
ArrayType *AT = ArrayType::get(Type::getInt32Ty(Context), 10);
```

## VectorType（向量类型）

**位置**: `llvm/include/llvm/IR/DerivedTypes.h`

VectorType表示SIMD向量类型。

### 向量类型

1. **FixedVectorType**: 定长向量
2. **ScalableVectorType**: 可伸缩向量（SVE等）

### 核心API

```cpp
class FixedVectorType : public Type {
public:
  static FixedVectorType *get(Type *ElementType, unsigned NumElts);
  unsigned getNumElements() const;
  Type *getElementType() const;
};

class ScalableVectorType : public Type {
public:
  static ScalableVectorType *get(Type *ElementType, unsigned MinNumElts);
  unsigned getMinNumElements() const;
  Type *getElementType() const;
};
```

### 使用示例

```cpp
// <4 x float> - 定长向量
FixedVectorType *VT1 = FixedVectorType::get(Type::getFloatTy(Context), 4);

// <vscale x 4 x i32> - 可伸缩向量
ScalableVectorType *VT2 = ScalableVectorType::get(Type::getInt32Ty(Context), 4);
```

## Constant（常量）

**位置**: `llvm/include/llvm/IR/Constants.h`

Constant表示常量值。

### 主要常量类型

| 类型 | 说明 |
|------|------|
| `ConstantInt` | 整数常量 |
| `ConstantFP` | 浮点常量 |
| `ConstantArray` | 数组常量 |
| `ConstantStruct` | 结构体常量 |
| `ConstantVector` | 向量常量 |
| `ConstantPointerNull` | 空指针 |
| `UndefValue` | 未定义值 |
| `PoisonValue` | 毒值 |

### 核心API

```cpp
// 整数常量
ConstantInt *CI = ConstantInt::get(Type::getInt32Ty(Context), 42);
ConstantInt *CI = ConstantInt::getSigned(Type::getInt32Ty(Context), -1);

// 浮点常量
ConstantFP *CF = ConstantFP::get(Type::getFloatTy(Context), 3.14);

// 空指针
ConstantPointerNull *Null = ConstantPointerNull::get(PointerTy);

// 未定义值
UndefValue *Undef = UndefValue::get(Type::getInt32Ty(Context));
```

## 类型系统特性

### 类型唯一化

- 每种类型在LLVMContext中唯一
- 类型比较只需指针比较
- 类型通过工厂方法创建

### 类型推断

LLVM提供类型推断辅助：

```cpp
Type *getCommonType(Type *Ty1, Type *Ty2);
bool typesMatch(Type *Ty1, Type *Ty2);
```

## 相关文件

### 头文件

- `llvm/include/llvm/IR/Value.h` - 值定义
- `llvm/include/llvm/IR/User.h` - 使用者定义
- `llvm/include/llvm/IR/Type.h` - 类型定义
- `llvm/include/llvm/IR/DerivedTypes.h` - 派生类型
- `llvm/include/llvm/IR/Constants.h` - 常量定义

### 源文件

- `llvm/lib/IR/Value.cpp` - 值实现
- `llvm/lib/IR/User.cpp` - 使用者实现
- `llvm/lib/IR/Type.cpp` - 类型实现
- `llvm/lib/IR/Constants.cpp` - 常量实现

## 参考链接

- [LLVM Type System](https://llvm.org/docs/LangRef.html#type-system)
- [LLVM Constants](https://llvm.org/docs/LangRef.html#constants)
