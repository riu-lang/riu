# LLVM 代码生成（CodeGen）

代码生成是LLVM编译器将IR转换为机器码的关键阶段。本文档介绍CodeGen的核心组件。

## 概述

代码生成流程：
```
LLVM IR → SelectionDAG/GlobalISel → MachineInstr → MCInst → 目标文件
```

核心目录：`llvm/lib/CodeGen/`

## MachineFunction 机器函数

`MachineFunction` 是目标机器级别的函数表示，对应一个LLVM `Function`。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `MachineFunction` | 机器函数，包含多个MachineBasicBlock | `llvm/lib/CodeGen/MachineFunction.cpp` |
| `MachineBasicBlock` | 机器基本块，包含MachineInstr序列 | `llvm/lib/CodeGen/MachineBasicBlock.cpp` |
| `MachineInstr` | 机器指令 | `llvm/lib/CodeGen/MachineInstr.cpp` |
| `MachineOperand` | 机器操作数 | `llvm/lib/CodeGen/MachineOperand.cpp` |
| `MachineRegisterInfo` | 虚拟/物理寄存器信息 | `llvm/lib/CodeGen/MachineRegisterInfo.cpp` |

### MachineInstr 结构

```cpp
class MachineInstr {
  const MCInstrDesc *MCID;      // 指令描述符
  MachineBasicBlock *Parent;    // 所属基本块
  MachineOperand Operands[];    // 操作数列表
  DebugLoc debugLoc;            // 调试信息
  // ...
};
```

### MachineOperand 类型

```cpp
enum OperandType {
  MO_Register,          // 寄存器
  MO_Immediate,         // 立即数
  MO_FPImmediate,       // 浮点立即数
  MO_MachineBasicBlock, // 基本块地址
  MO_FrameIndex,        // 栈帧索引
  MO_ConstantPoolIndex, // 常量池索引
  MO_JumpTableIndex,    // 跳转表索引
  MO_ExternalSymbol,    // 外部符号
  MO_GlobalAddress,     // 全局地址
  MO_BlockAddress,      // 块地址
  MO_RegisterMask,      // 寄存器掩码
  MO_Metadata,          // 元数据
  MO_MCSymbol,          // MC符号
  MO_CFIIndex,          // CFI索引
  MO_IntrinsicID,       // 内置函数ID
  MO_Predicate,         // 谓词
  MO_ShuffleMask,       // Shuffle掩码
};
```

### 创建MachineInstr示例

```cpp
MachineFunction &MF = ...;
const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();

MachineInstr *MI = BuildMI(MF, DL, TII->get(X86::ADD32rr))
    .addReg(DestReg, RegState::Define)
    .addReg(SrcReg1)
    .addReg(SrcReg2);
```

## SelectionDAG 选择DAG

SelectionDAG是一种有向无环图（DAG），用于指令选择阶段的中间表示。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `SelectionDAG` | 选择DAG主类 | `llvm/lib/CodeGen/SelectionDAG/SelectionDAG.cpp` |
| `SDNode` | DAG节点 | `llvm/lib/CodeGen/SelectionDAG/SelectionDAGNodes.h` |
| `SDValue` | DAG值（节点+结果索引） | `llvm/lib/CodeGen/SelectionDAG/SelectionDAGNodes.h` |
| `TargetLowering` | 目标 lowering 接口 | `llvm/lib/CodeGen/SelectionDAG/TargetLowering.cpp` |
| `DAGCombiner` | DAG优化合并器 | `llvm/lib/CodeGen/SelectionDAG/DAGCombiner.cpp` |

### SDNode 类型

```cpp
enum NodeType {
  // 叶子节点
  Constant,           // 常量
  Register,           // 寄存器
  FrameIndex,         // 栈帧索引
  GlobalAddress,      // 全局地址
  BasicBlock,         // 基本块
  
  // 算术运算
  ADD, SUB, MUL, DIV, REM,
  FADD, FSUB, FMUL, FDIV, FREM,
  
  // 位运算
  AND, OR, XOR, SHL, SRA, SRL,
  
  // 内存操作
  LOAD, STORE,
  
  // 控制流
  BR, BRCOND, BR_CC,
  CALL,
  
  // 类型转换
  SIGN_EXTEND, ZERO_EXTEND, TRUNCATE,
  FP_TO_INT, INT_TO_FP,
  
  // 向量操作
  BUILD_VECTOR, VECTOR_SHUFFLE,
  EXTRACT_VECTOR_ELT, INSERT_VECTOR_ELT,
};
```

### SelectionDAG 流程

```
1. 构建阶段（ISel）
   LLVM IR → SelectionDAG
   
2. 类型合法化
   不支持的类型 → 支持的类型
   
3. 操作合法化
   不支持的操作 → 支持的操作
   
4. DAG优化
   合并、简化、常量折叠
   
5. 指令选择
   SelectionDAG → MachineInstr
   
6. 调度与发射
   MachineInstr序列化
```

### DAG构建示例

```cpp
SelectionDAG &DAG = ...;
SDLoc DL(...);

SDValue N1 = DAG.getConstant(10, DL, MVT::i32);
SDValue N2 = DAG.getConstant(20, DL, MVT::i32);
SDValue Add = DAG.getNode(ISD::ADD, DL, MVT::i32, N1, N2);
```

## GlobalISel 全局指令选择

GlobalISel是LLVM的新一代指令选择框架，相比SelectionDAG更模块化、更适合调试。

### 核心组件

| 组件 | 说明 | 源码位置 |
|---|---|---|
| `IRTranslator` | IR到GISel MIR转换 | `llvm/lib/CodeGen/GlobalISel/IRTranslator.cpp` |
| `Legalizer` | 操作合法化 | `llvm/lib/CodeGen/GlobalISel/Legalizer.cpp` |
| `RegBankSelect` | 寄存器组选择 | `llvm/lib/CodeGen/GlobalISel/RegBankSelect.cpp` |
| `InstructionSelect` | 指令选择 | `llvm/lib/CodeGen/GlobalISel/InstructionSelect.cpp` |

### GlobalISel 流程

```
LLVM IR
   ↓ IRTranslator
Generic Machine IR (GMI)
   ↓ Legalizer
合法化的GMI
   ↓ RegBankSelect
寄存器组分配
   ↓ InstructionSelect
目标MachineInstr
```

### 通用机器指令（Generic MachineInstr）

```cpp
// 通用操作码
enum GenericOpcode {
  G_ADD,       // 整数加法
  G_SUB,       // 整数减法
  G_MUL,       // 整数乘法
  G_LOAD,      // 加载
  G_STORE,     // 存储
  G_BR,        // 无条件跳转
  G_BRCOND,    // 条件跳转
  G_ICMP,      // 整数比较
  G_FCMP,      // 浮点比较
  G_SELECT,    // 选择
  G_PHI,       // PHI节点
  G_PTR_ADD,   // 指针加法
  G_ZEXT,      // 零扩展
  G_SEXT,      // 符号扩展
  G_TRUNC,     // 截断
  G_FPTOUI,    // 浮点转无符号整数
  G_FPTOSI,    // 浮点转有符号整数
  G_UITOFP,    // 无符号整数转浮点
  G_SITOFP,    // 有符号整数转浮点
};
```

### LegalizerInfo 合法化信息

```cpp
// 定义操作的合法化策略
class LegalizerInfo {
  enum LegalizeAction {
    Legal,      // 原生支持
    NarrowScalar, // 标量变窄
    WidenScalar,  // 标量变宽
    FewerElements, // 向量元素减少
    MoreElements,  // 向量元素增加
    Bitcast,    // 位转换
    Lower,      // 降低为其他操作
    Libcall,    // 库函数调用
    Custom,     // 自定义处理
    Unsupported, // 不支持
  };
};
```

## 寄存器分配

寄存器分配是将虚拟寄存器映射到物理寄存器的过程。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `LiveIntervals` | 活跃区间分析 | `llvm/lib/CodeGen/LiveIntervals.cpp` |
| `LiveInterval` | 单个虚拟寄存器的活跃区间 | `llvm/lib/CodeGen/LiveInterval.cpp` |
| `VirtRegMap` | 虚拟寄存器映射 | `llvm/lib/CodeGen/VirtRegMap.cpp` |
| `RegAllocBase` | 寄存器分配基类 | `llvm/lib/CodeGen/RegAllocBase.cpp` |

### 寄存器分配器

| 分配器 | 说明 | 源码位置 |
|---|---|---|
| `RegAllocFast` | 快速分配器（调试用） | `llvm/lib/CodeGen/RegAllocFast.cpp` |
| `RegAllocBasic` | 基本分配器 | `llvm/lib/CodeGen/RegAllocBasic.cpp` |
| `RegAllocGreedy` | 贪心分配器（默认，高质量） | `llvm/lib/CodeGen/RegAllocGreedy.cpp` |
| `RegAllocPBQP` | PBQP分配器 | `llvm/lib/CodeGen/RegAllocPBQP.cpp` |

### 活跃区间（Live Interval）

```cpp
class LiveInterval {
  unsigned Reg;           // 虚拟寄存器号
  float Weight;           // 区间权重
  Segments segments;      // 活跃段列表 [start, end)
  VNInfoList vnis;        // 值编号信息
  
  struct Segment {
    SlotIndex start;      // 起始位置
    SlotIndex end;        // 结束位置
    VNInfo *valno;        // 值编号
  };
};
```

### 寄存器分配流程

```
1. 活跃分析
   计算每个虚拟寄存器的活跃区间
   
2. 区间排序
   按权重/起始位置排序
   
3. 分配循环
   for each interval:
     a. 尝试分配物理寄存器
     b. 如果失败，选择溢出候选
     c. 必要时溢出到栈
   
4. 溢出代码插入
   插入加载/存储指令
```

## 指令调度

指令调度重排指令顺序以提高性能。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `ScheduleDAG` | 调度DAG基类 | `llvm/lib/CodeGen/ScheduleDAG.cpp` |
| `MachineScheduler` | 机器调度器 | `llvm/lib/CodeGen/MachineScheduler.cpp` |
| `SUnit` | 调度单元 | `llvm/lib/CodeGen/ScheduleDAG.cpp` |

### 调度策略

| 策略 | 说明 |
|---|---|
| `ilp` | 指令级并行优化 |
| `latency` | 延迟优化 |
| `source` | 源码顺序 |
| `list-burr` | 列表调度（寄存器压力感知） |

### 调度阶段

```
1. 构建调度DAG
   MachineInstr → SUnit DAG
   
2. 拓扑排序
   计算优先级
   
3. 列表调度
   按优先级发射指令
   
4. 发射
   生成最终的MachineInstr序列
```

## 代码生成Pass流程

典型的代码生成Pass序列：

```cpp
// Pass顺序示例
TargetPassConfig::addIRPasses()
  → addISelPasses()           // 指令选择
  → addPreRegAlloc()          // 寄存器分配前优化
  → addPostRegAlloc()         // 寄存器分配后优化
  → addPreSched2()            // 第二次调度前
  → addPreEmitPass()          // 发射前优化
  → addPreEmitPass2()         // 最终优化
  → addAsmPrinter()           // 汇编输出
```

### 关键Pass

| Pass | 说明 |
|---|---|
| `ISel` | 指令选择（SelectionDAG或GlobalISel） |
| `FinalizeISel` | 完成指令选择 |
| `LocalStackSlotAllocation` | 局部栈槽分配 |
| `RegisterCoalescer` | 寄存器合并 |
| `MachineLICM` | 机器级循环不变量外提 |
| `MachineCSE` | 机器级公共子表达式消除 |
| `MachineScheduler` | 机器指令调度 |
| `RegAlloc` | 寄存器分配 |
| `VirtRegMap` | 虚拟寄存器映射处理 |
| `PrologueEpilogueInserter` | 插入函数序言/尾声 |
| `BranchRelaxation` | 分支松弛 |
| `AsmPrinter` | 汇编输出 |

## 源码导航

| 功能 | 目录 |
|---|---|
| SelectionDAG | `llvm/lib/CodeGen/SelectionDAG/` |
| GlobalISel | `llvm/lib/CodeGen/GlobalISel/` |
| 寄存器分配 | `llvm/lib/CodeGen/RegAlloc*.cpp` |
| 指令调度 | `llvm/lib/CodeGen/MachineScheduler.cpp` |
| 活跃分析 | `llvm/lib/CodeGen/Live*.cpp` |
| 汇编输出 | `llvm/lib/CodeGen/AsmPrinter/` |
| MIR解析/打印 | `llvm/lib/CodeGen/MIRParser/`, `llvm/lib/CodeGen/MIRPrinter.cpp` |

## 相关文档

- [MC层](mc-layer.md) - 机器码层和对象文件生成
- [Target后端](target-backend.md) - 目标后端实现
- [LLD链接器](lld-linker.md) - 链接器使用
