# LLVM IR 核心概念

LLVM IR（Intermediate Representation）是LLVM编译器基础设施的核心中间表示。本文档介绍IR的核心概念和主要类。

## 概述

LLVM IR是一种低级程序表示，类似于汇编语言但具有更多高级特性。它是类型化的、基于SSA（Static Single Assignment）的表示形式。

## 核心类层次结构

```
Module (模块)
  └── Function (函数)
        └── BasicBlock (基本块)
              └── Instruction (指令)
                    └── Value (值)
```

## Module（模块）

**位置**: `llvm/include/llvm/IR/Module.h`

Module是LLVM IR的顶层容器，包含：

### 主要组成部分

| 组件 | 说明 |
|------|------|
| 全局变量 | GlobalVariable列表 |
| 函数 | Function列表 |
| 别名 | GlobalAlias列表 |
| IFunc | GlobalIFunc列表 |
| 命名元数据 | NamedMDNode列表 |
| 符号表 | ValueSymbolTable |
| 数据布局 | DataLayout |
| 目标三元组 | Target Triple |

### 核心API

```cpp
class Module {
public:
  using FunctionListType = SymbolTableList<Function>;
  using GlobalListType = SymbolTableList<GlobalVariable>;
  using AliasListType = SymbolTableList<GlobalAlias>;
  
  iterator begin();              // 函数迭代器
  iterator end();
  global_iterator global_begin(); // 全局变量迭代器
  global_iterator global_end();
  
  Function *getFunction(StringRef Name);  // 按名称查找函数
  GlobalVariable *getGlobalVariable(StringRef Name);
  
  const DataLayout &getDataLayout() const;
  const std::string &getTargetTriple() const;
};
```

### 模块标志行为

```cpp
enum ModFlagBehavior {
  Error = 1,        // 值不同时报错
  Warning = 2,      // 值不同时警告
  Require = 3,      // 要求另一个标志存在
  Override = 4,     // 使用指定值
  Append = 5,       // 追加两个值
  AppendUnique = 6, // 追加并去重
  Max = 7,          // 取最大值
};
```

## Function（函数）

**位置**: `llvm/include/llvm/IR/Function.h`

Function表示一个函数，包含基本块列表和参数列表。

### 继承关系

```
Function : public GlobalObject, public ilist_node<Function>
```

### 主要组成部分

| 组件 | 说明 |
|------|------|
| 基本块列表 | BasicBlockListType |
| 参数列表 | Argument数组 |
| 符号表 | ValueSymbolTable |
| 属性集 | AttributeList |

### 核心API

```cpp
class Function : public GlobalObject {
public:
  using BasicBlockListType = SymbolTableList<BasicBlock>;
  using iterator = BasicBlockListType::iterator;
  using arg_iterator = Argument *;
  
  iterator begin();              // 基本块迭代器
  iterator end();
  arg_iterator arg_begin();      // 参数迭代器
  arg_iterator arg_end();
  
  BasicBlock &getEntryBlock();   // 获取入口块
  size_t size() const;           // 基本块数量
  
  Argument *getArg(unsigned i);  // 获取第i个参数
  unsigned getNumArgs() const;   // 参数数量
  
  AttributeList getAttributes() const;
  CallingConv::ID getCallingConv() const;
};
```

### 函数属性

- **调用约定**: CallingConv (C, FastCall, ThisCall等)
- **属性**: AttributeList (readonly, nounwind, alwaysinline等)
- **垃圾回收**: GC策略
- **调试信息**: DISubprogram

## BasicBlock（基本块）

**位置**: `llvm/include/llvm/IR/BasicBlock.h`

BasicBlock表示一个基本块，是顺序执行的指令序列。

### 继承关系

```
BasicBlock : public Value, public ilist_node_with_parent<BasicBlock, Function>
```

### 特性

- 基本块是Value，类型为LabelTy
- 包含指令列表
- 以终结指令（terminator）结束
- 可以被分支指令引用

### 核心API

```cpp
class BasicBlock : public Value {
public:
  using InstListType = SymbolTableList<Instruction>;
  
  InstListType::iterator begin();    // 指令迭代器
  InstListType::iterator end();
  
  Instruction *getTerminator();      // 获取终结指令
  Instruction &front();              // 第一条指令
  Instruction &back();               // 最后一条指令
  
  Function *getParent();             // 所属函数
  Module *getModule();               // 所属模块
  
  BasicBlock *getSinglePredecessor();  // 单一前驱
  BasicBlock *getSingleSuccessor();    // 单一后继
};
```

### 基本块特性

1. **良好形式的基本块**:
   - 非终结指令序列 + 单个终结指令
   - 终结指令不能出现在中间
   - 必须以终结指令结束

2. **终结指令类型**:
   - `ret` - 返回
   - `br` - 分支
   - `switch` - 开关
   - `unreachable` - 不可达

## LLVMContext（上下文）

**位置**: `llvm/include/llvm/IR/LLVMContext.h`

LLVMContext是LLVM核心基础设施的上下文，管理全局状态。

### 功能

- 类型唯一化
- 常量唯一化
- 元数据管理
- 诊断处理

### 使用示例

```cpp
LLVMContext Context;
Module *M = new Module("test", Context);
```

## 数据布局（DataLayout）

**位置**: `llvm/include/llvm/IR/DataLayout.h`

DataLayout描述目标平台的数据布局特性。

### 主要信息

- 指针大小和对齐
- 整数类型大小
- 浮点类型大小
- 字节序（大端/小端）
- 结构体布局规则

### 使用示例

```cpp
DataLayout DL("e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128");
unsigned PointerSize = DL.getPointerSize();
```

## 全局值（GlobalValue）

**位置**: `llvm/include/llvm/IR/GlobalValue.h`

GlobalValue是全局对象的基类。

### 继承关系

```
GlobalValue : public GlobalObject
  ├── GlobalVariable
  ├── Function
  └── GlobalAlias
```

### 链接类型

```cpp
enum LinkageTypes {
  ExternalLinkage,      // 外部可见
  AvailableExternallyLinkage,
  LinkOnceAnyLinkage,
  LinkOnceODRLinkage,
  WeakAnyLinkage,
  WeakODRLinkage,
  AppendingLinkage,
  InternalLinkage,      // 内部可见
  PrivateLinkage,       // 私有
  ExternalWeakLinkage,
  CommonLinkage
};
```

## 常见使用模式

### 创建模块和函数

```cpp
LLVMContext Context;
Module *M = new Module("test", Context);

FunctionType *FT = FunctionType::get(Type::getVoidTy(Context), false);
Function *F = Function::Create(FT, Function::ExternalLinkage, "main", M);
```

### 创建基本块和指令

```cpp
BasicBlock *BB = BasicBlock::Create(Context, "entry", F);
IRBuilder<> Builder(BB);
Builder.CreateRetVoid();
```

### 遍历模块

```cpp
for (Function &F : *M) {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      // 处理指令
    }
  }
}
```

## 相关文件

### 头文件

- `llvm/include/llvm/IR/Module.h` - 模块定义
- `llvm/include/llvm/IR/Function.h` - 函数定义
- `llvm/include/llvm/IR/BasicBlock.h` - 基本块定义
- `llvm/include/llvm/IR/LLVMContext.h` - 上下文定义
- `llvm/include/llvm/IR/DataLayout.h` - 数据布局
- `llvm/include/llvm/IR/GlobalValue.h` - 全局值

### 源文件

- `llvm/lib/IR/Module.cpp` - 模块实现
- `llvm/lib/IR/Function.cpp` - 函数实现
- `llvm/lib/IR/BasicBlock.cpp` - 基本块实现
- `llvm/lib/IR/LLVMContext.cpp` - 上下文实现

## 参考链接

- [LLVM Language Reference Manual](https://llvm.org/docs/LangRef.html)
- [LLVM Programmer's Manual](https://llvm.org/docs/ProgrammersManual.html)
