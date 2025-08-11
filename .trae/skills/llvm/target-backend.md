# LLVM Target 后端

Target后端是LLVM支持特定目标架构的核心组件，负责指令选择、调度、代码发射等。

## 概述

Target后端架构：
```
LLVM IR → TargetLowering → SelectionDAG/GlobalISel → MachineInstr → MCInst
```

核心目录：`llvm/lib/Target/`

## TargetMachine 目标机器

`TargetMachine` 是目标后端的顶层类，封装了目标架构的所有信息。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `TargetMachine` | 目标机器基类 | `llvm/include/llvm/Target/TargetMachine.h` |
| `LLVMTargetMachine` | LLVM目标机器基类 | `llvm/lib/Target/TargetMachine.cpp` |
| `TargetSubtargetInfo` | 子目标信息 | `llvm/lib/Target/TargetSubtargetInfo.cpp` |
| `TargetOptions` | 目标选项 | `llvm/include/llvm/Target/TargetOptions.h` |

### TargetMachine 结构

```cpp
class TargetMachine {
protected:
  const Target &TheTarget;        // 目标描述
  const DataLayout DL;            // 数据布局
  Triple TargetTriple;            // 目标三元组
  std::string TargetCPU;          // 目标CPU
  std::string TargetFeatureString; // 目标特性字符串
  TargetOptions Options;          // 目标选项
  Reloc::Model RM;                // 重定位模型
  CodeModel::Model CMModel;       // 代码模型
  CodeGenOptLevel OptLevel;       // 优化级别
  
public:
  virtual const TargetSubtargetInfo *getSubtargetImpl(const Function &) const;
  virtual TargetLowering *getTargetLowering() const;
  virtual TargetTransformInfo getTargetTransformInfo(const Function &F);
  
  TargetPassConfig *createPassConfig(PassManagerBase &PM);
  bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                           CodeGenFileType FileType);
};
```

### 创建TargetMachine

```cpp
Target *T = TargetRegistry::lookupTarget(TripleName, Error);
TargetMachine *TM = T->createTargetMachine(
    TripleName,      // 目标三元组
    CPU,             // 目标CPU
    Features,        // 目标特性
    Options,         // 目标选项
    Reloc::Model,    // 重定位模型
    CodeModel::Model,// 代码模型
    OptLevel         // 优化级别
);
```

## TargetRegistration 目标注册

LLVM使用注册机制管理所有目标后端。

### 注册机制

```cpp
namespace llvm {

class Target {
public:
  const char *getName() const;
  const char *getShortDescription() const;
  const char *getBackendName() const;
  
  bool hasJIT() const;
  bool hasTargetMachine() const;
  bool hasMCAsmBackend() const;
  
  TargetMachine *createTargetMachine(
      StringRef TT, StringRef CPU, StringRef Features,
      const TargetOptions &Options, Reloc::Model RM,
      CodeModel::Model CM, CodeGenOptLevel OL) const;
  
  MCAsmBackend *createMCAsmBackend(const MCSubtargetInfo &STI) const;
  MCCodeEmitter *createMCCodeEmitter(const MCInstrInfo &II) const;
};

class TargetRegistry {
public:
  static void RegisterTarget(Target &T, const char *Name, ...);
  static iterator_range<TargetIterator> targets();
  static const Target *lookupTarget(const std::string &Triple, std::string &Error);
  static const Target *lookupTarget(const std::string &ArchName, Triple &Triple, std::string &Error);
};

}

// 目标注册宏
#define LLVM_EXTERNAL_VISIBILITY __attribute__((visibility("default")))

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeX86TargetInfo() {
  RegisterTarget<Triple::x86_64> X(getTheX86_64Target(), "x86-64", "64-bit X86");
}
```

### 初始化目标

```cpp
// 初始化所有目标
InitializeAllTargetInfos();
InitializeAllTargets();
InitializeAllTargetMCs();
InitializeAllAsmPrinters();
InitializeAllAsmParsers();
InitializeAllDisassemblers();

// 初始化特定目标
LLVMInitializeX86TargetInfo();
LLVMInitializeX86Target();
LLVMInitializeX86TargetMC();
LLVMInitializeX86AsmPrinter();
LLVMInitializeX86AsmParser();
LLVMInitializeX86Disassembler();
```

## TargetLowering 目标Lowering

`TargetLowering` 定义了如何将LLVM IR操作降低到目标指令。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `TargetLowering` | 目标Lowering基类 | `llvm/lib/CodeGen/SelectionDAG/TargetLowering.cpp` |
| `TargetLoweringBase` | Lowering基础实现 | `llvm/lib/CodeGen/SelectionDAG/TargetLoweringBase.cpp` |

### 关键接口

```cpp
class TargetLowering {
public:
  // 类型合法化
  MVT getRegisterType(MVT VT) const;
  unsigned getNumRegisters(MVT VT) const;
  
  // 操作合法化
  LegalizeAction getOperationAction(unsigned Op, MVT VT) const;
  bool isOperationLegal(unsigned Op, MVT VT) const;
  
  // DAG节点Lowering
  virtual SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const;
  
  // 调用约定
  virtual bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                              bool isVarArg, const SmallVectorImpl<ISD::OutputArg> &Outs,
                              LLVMContext &Context) const;
  virtual SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                              bool isVarArg, const SmallVectorImpl<ISD::OutputArg> &Outs,
                              const SmallVectorImpl<SDValue> &OutVals,
                              const SDLoc &dl, SelectionDAG &DAG) const;
  virtual SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                                       bool isVarArg, const SmallVectorImpl<ISD::InputArg> &Ins,
                                       const SDLoc &dl, SelectionDAG &DAG,
                                       SmallVectorImpl<SDValue> &InVals) const;
  virtual SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                            SmallVectorImpl<SDValue> &InVals) const;
  
  // 地址模式
  bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM,
                             Type *Ty, unsigned AS) const;
  
  // 内置函数
  virtual SDValue LowerINTRINSIC_WO_CHAIN(SDValue Op, SelectionDAG &DAG) const;
  virtual SDValue LowerINTRINSIC_VOID(SDValue Op, SelectionDAG &DAG) const;
};
```

### 合法化动作

```cpp
enum LegalizeAction {
  Legal,        // 原生支持
  Promote,      // 类型提升
  Expand,       // 展开为其他操作
  Custom,       // 自定义处理
  LibCall,      // 库函数调用
  TypeScalarize, // 标量化
  TypeSplit,    // 拆分
  TypeWidenVector, // 向量加宽
};
```

## TargetInstrInfo 目标指令信息

`TargetInstrInfo` 描述目标架构的指令集。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `TargetInstrInfo` | 目标指令信息基类 | `llvm/lib/CodeGen/TargetInstrInfo.cpp` |
| `MCInstrDesc` | 指令描述符 | `llvm/lib/MC/MCInstrDesc.cpp` |

### 指令描述符

```cpp
struct MCInstrDesc {
  unsigned short Opcode;         // 操作码
  unsigned short NumOperands;    // 操作数数量
  unsigned short NumDefs;        // 定义操作数数量
  unsigned short Size;           // 指令大小
  uint64_t Flags;                // 指令标志
  uint64_t TSFlags;              // 目标特定标志
  const MCPhysReg *ImplicitUses; // 隐式使用寄存器
  const MCPhysReg *ImplicitDefs; // 隐式定义寄存器
  const MCOperandInfo *OpInfo;   // 操作数信息
};

// 指令标志
namespace MCID {
  enum Flag {
    PreISelOpcode,    // 指令选择前的伪指令
    Variadic,         // 可变参数
    UnmodeledSideEffects, // 有未建模的副作用
    Commutable,       // 可交换
    Terminiator,      // 基本块终结符
    Branch,           // 分支指令
    IndirectBranch,   // 间接分支
    Compare,          // 比较指令
    MoveImm,          // 立即数传送
    Bitcast,          // 位转换
    Select,           // 选择指令
    DelaySlot,        // 延迟槽
    FoldableAsLoad,   // 可作为加载折叠
    MayLoad,          // 可能加载
    MayStore,         // 可能存储
    Predicable,       // 可预测
    NotDuplicable,    // 不可复制
    // ...
  };
}
```

### 关键接口

```cpp
class TargetInstrInfo {
public:
  // 指令分析
  virtual bool isBranch(const MachineInstr &MI) const;
  virtual bool isCall(const MachineInstr &MI) const;
  virtual bool isReturn(const MachineInstr &MI) const;
  virtual bool isLoad(const MachineInstr &MI) const;
  virtual bool isStore(const MachineInstr &MI) const;
  
  // 指令复制
  virtual void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                          const DebugLoc &DL, MCRegister DestReg, MCRegister SrcReg,
                          bool KillSrc) const;
  
  // 立即数加载
  virtual void storeRegToStackSlot(MachineBasicBlock &MBB,
                                   MachineBasicBlock::iterator MI,
                                   Register SrcReg, bool isKill, int FrameIndex,
                                   const TargetRegisterClass *RC,
                                   const TargetRegisterInfo *TRI,
                                   Register VReg) const;
  virtual void loadRegFromStackSlot(MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator MI,
                                    Register DestReg, int FrameIndex,
                                    const TargetRegisterClass *RC,
                                    const TargetRegisterInfo *TRI,
                                    Register VReg) const;
  
  // 指令优化
  virtual bool optimizeCompareInstr(MachineInstr &CmpInstr, Register SrcReg,
                                   unsigned SrcMask, int CmpValue,
                                   const MachineInstr *PrevInstr,
                                   const MachineInstr *NextInstr,
                                   const MachineRegisterInfo *MRI,
                                   const TargetRegisterInfo *TRI) const;
  
  // 分支指令处理
  virtual bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                            MachineBasicBlock *&FBB,
                            SmallVectorImpl<MachineOperand> &Cond,
                            bool AllowModify = false) const;
  virtual unsigned removeBranch(MachineBasicBlock &MBB, int *BytesRemoved = nullptr) const;
  virtual unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                               MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                               const DebugLoc &DL, int *BytesAdded = nullptr) const;
};
```

## TargetRegisterInfo 目标寄存器信息

`TargetRegisterInfo` 描述目标架构的寄存器集。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `TargetRegisterInfo` | 目标寄存器信息基类 | `llvm/lib/CodeGen/TargetRegisterInfo.cpp` |
| `TargetRegisterClass` | 寄存器类 | `llvm/lib/CodeGen/TargetRegisterInfo.cpp` |

### 寄存器类

```cpp
struct TargetRegisterClass {
  unsigned ID;                    // 寄存器类ID
  const char *Name;               // 名称
  unsigned SpillSize;             // 溢出大小
  unsigned SpillAlignment;        // 溢出对齐
  const MCPhysReg *Regs;          // 寄存器列表
  const uint32_t *SubClassMask;   // 子类掩码
  const uint16_t *SuperRegIndices;// 超级寄存器索引
  LaneBitmask LaneMask;           // 通道掩码
  bool HasDisjunctSubRegs;        // 有不相交子寄存器
  bool CoveredBySubRegs;          // 被子寄存器覆盖
  const TargetRegisterClass *SuperClasses; // 超类
  
  iterator begin() const { return Regs; }
  iterator end() const { return Regs + NumRegs; }
  unsigned getSize() const { return SpillSize / 8; }
};
```

### 关键接口

```cpp
class TargetRegisterInfo {
public:
  // 寄存器信息
  virtual const char *getRegName(MCRegister Reg) const = 0;
  virtual unsigned getRegSizeInBits(MCRegister Reg, const MachineRegisterInfo &MRI) const;
  virtual bool isPhysicalRegister(MCRegister Reg) const;
  
  // 寄存器类
  virtual const TargetRegisterClass *getLargestLegalSuperClass(const TargetRegisterClass *RC,
                                                               const MachineFunction &MF) const;
  virtual const TargetRegisterClass *getPointerRegClass(const MachineFunction &MF,
                                                        unsigned Kind = 0) const;
  
  // 调用约定
  virtual BitVector getReservedRegs(const MachineFunction &MF) const = 0;
  virtual const MCPhysReg *getCalleeSavedRegs(const MachineFunction *MF) const = 0;
  virtual const uint32_t *getCallPreservedMask(const MachineFunction &MF,
                                               CallingConv::ID) const;
  
  // 栈帧
  virtual void eliminateFrameIndex(MachineBasicBlock::iterator MI, int SPAdj,
                                   unsigned FIOperandNum, RegScavenger *RS = nullptr) const = 0;
  
  // 调试
  virtual void print(raw_ostream &OS, const MachineRegisterInfo &MRI) const;
};
```

## TargetFrameLowering 目标栈帧

`TargetFrameLowering` 定义目标架构的栈帧布局。

### 核心类

```cpp
class TargetFrameLowering {
public:
  enum StackDirection {
    StackGrowsUp,    // 栈向上增长
    StackGrowsDown,  // 栈向下增长
  };
  
  virtual void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const = 0;
  virtual void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const = 0;
  
  virtual bool hasFP(const MachineFunction &MF) const = 0;
  virtual bool hasReservedCallFrame(const MachineFunction &MF) const;
  
  virtual int getFrameIndexReference(const MachineFunction &MF, int FI,
                                     unsigned &FrameReg) const;
  virtual uint64_t getStackSize(const MachineFunction &MF) const;
  virtual unsigned estimateStackSize(const MachineFunction &MF) const;
  
  virtual void determineFrameLayout(MachineFunction &MF) const;
};
```

## TableGen 目标描述

LLVM使用TableGen描述目标架构，生成C++代码。

### 目标描述文件

| 文件类型 | 说明 |
|---|---|
| `*.td` | TableGen描述文件 |
| `*InstrInfo.td` | 指令集描述 |
| `*RegisterInfo.td` | 寄存器集描述 |
| `*CallingConv.td` | 调用约定描述 |
| `*Sched.td` | 调度模型描述 |

### 指令定义示例

```tablegen
// X86指令定义示例
def ADD32rr : I<0x01, MRMDestReg, (outs GR32:$dst), (ins GR32:$src1, GR32:$src2),
                "add{l}\t{$src2, $dst|$dst, $src2}",
                [(set GR32:$dst, (add GR32:$src1, GR32:$src2))]>,
            IIC_ALU_NONMEM;

def MOV32ri : Ii32<0xB8, AddRegFrm, (outs GR32:$dst), (ins i32imm:$src),
                   "mov{l}\t{$src, $dst|$dst, $src}",
                   [(set GR32:$dst, imm:$src)]>;
```

### 寄存器定义示例

```tablegen
// X86寄存器定义示例
def EAX : Register<"eax">;
def ECX : Register<"ecx">;
def EDX : Register<"edx">;
def EBX : Register<"ebx">;

def GR32 : RegisterClass<"X86", [i32], 32, (add EAX, ECX, EDX, EBX, ESI, EDI, EBP, ESP,
                                          R8D, R9D, R10D, R11D, R12D, R13D, R14D, R15D)>;
```

### 调用约定定义示例

```tablegen
// X86-64调用约定
def X86_64_SysV_CC : CallingConv<[
  CCIfType<[i64], CCAssignToReg<[RDI, RSI, RDX, RCX, R8, R9]>>,
  CCIfType<[f64], CCAssignToReg<[XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7]>>,
  CCIfType<[i32], CCAssignToReg<[EDI, ESI, EDX, ECX, R8D, R9D]>>,
  CCIfType<[f32], CCAssignToReg<[XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7]>>,
  CCAssignToStack<8, 8>
]>;
```

## 指令选择

指令选择将LLVM IR转换为MachineInstr。

### SelectionDAG指令选择

```cpp
class SelectionDAGISel {
public:
  virtual void Select(SDNode *N);
  virtual void SelectInlineAsmMemoryOperands(std::vector<SDValue> &Ops);
  
protected:
  virtual SDNode *SelectCode(SDNode *N);
  bool SelectInlineAsmMemoryOperand(const SDValue &Op, unsigned ConstraintCode,
                                    std::vector<SDValue> &OutOps);
};

// 目标特定选择
class X86DAGToDAGISel : public SelectionDAGISel {
  void Select(SDNode *Node) override;
  bool tryFoldLoad(SDNode *P, SDValue N, SDValue &Base, SDValue &Offset,
                   SDValue &Scale, SDValue &Index);
  bool matchAddressRecursively(SDValue N, X86ISelAddressMode &AM, unsigned Depth);
};
```

### GlobalISel指令选择

```cpp
class InstructionSelector {
public:
  virtual bool select(MachineInstr &I) = 0;
};

// 目标特定选择器
class X86InstructionSelector : public InstructionSelector {
  bool select(MachineInstr &I) override;
  bool selectLoadStoreOp(MachineInstr &I, unsigned NewOpc);
  bool selectArithOp(MachineInstr &I, unsigned NewOpc);
};
```

## 子目标信息

`TargetSubtargetInfo` 描述目标架构的特定变体。

### 核心功能

```cpp
class TargetSubtargetInfo {
public:
  virtual bool enableMachineScheduler() const;
  virtual bool enableAtomicExpand() const;
  virtual bool enableIndirectBrExpand() const;
  
  virtual const TargetInstrInfo *getInstrInfo() const = 0;
  virtual const TargetFrameLowering *getFrameLowering() const = 0;
  virtual const TargetLowering *getTargetLowering() const = 0;
  virtual const TargetRegisterInfo *getRegisterInfo() const = 0;
  virtual const CallLowering *getCallLowering() const;
  virtual const LegalizerInfo *getLegalizerInfo() const;
  virtual const RegisterBankInfo *getRegBankInfo() const;
  
  virtual unsigned getHwMode() const;
  virtual bool isCPUStringValid(StringRef CPU) const;
};
```

## 目标后端示例：X86

### 文件结构

```
llvm/lib/Target/X86/
├── X86.td                 # 主TableGen文件
├── X86InstrInfo.td        # 指令定义
├── X86RegisterInfo.td     # 寄存器定义
├── X86CallingConv.td      # 调用约定
├── X86Schedule.td         # 调度模型
├── X86TargetMachine.cpp   # TargetMachine实现
├── X86TargetMachine.h
├── X86Subtarget.cpp       # 子目标信息
├── X86Subtarget.h
├── X86ISelLowering.cpp    # ISel Lowering
├── X86ISelLowering.h
├── X86InstrInfo.cpp       # 指令信息
├── X86InstrInfo.h
├── X86RegisterInfo.cpp    # 寄存器信息
├── X86RegisterInfo.h
├── X86FrameLowering.cpp   # 栈帧
├── X86FrameLowering.h
├── X86AsmPrinter.cpp      # 汇编打印
├── X86AsmPrinter.h
├── X86MCInstLower.cpp     # MCInst Lowering
├── X86FastISel.cpp        # 快速指令选择
├── X86ISelDAGToDAG.cpp    # DAG指令选择
├── MCTargetDesc/          # MC层描述
│   ├── X86MCTargetDesc.cpp
│   ├── X86MCCodeEmitter.cpp
│   └── X86AsmBackend.cpp
└── TargetInfo/            # 目标信息
    └── X86TargetInfo.cpp
```

### X86TargetMachine

```cpp
class X86TargetMachine : public LLVMTargetMachine {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  
public:
  X86TargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                   StringRef FS, const TargetOptions &Options,
                   Reloc::Model RM, CodeModel::Model CM,
                   CodeGenOptLevel OL);
  
  const X86Subtarget *getSubtargetImpl(const Function &F) const override;
  TargetTransformInfo getTargetTransformInfo(const Function &F) const override;
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;
};
```

## 添加新目标后端

### 步骤概览

1. **创建目录结构**
   ```
   llvm/lib/Target/MyTarget/
   ├── MyTarget.td
   ├── MyTargetInstrInfo.td
   ├── MyTargetRegisterInfo.td
   ├── MyTargetTargetMachine.cpp
   ├── MyTargetSubtarget.cpp
   ├── MyTargetISelLowering.cpp
   └── ...
   ```

2. **定义寄存器** (`MyTargetRegisterInfo.td`)
   ```tablegen
   def R0 : Register<"r0">;
   def R1 : Register<"r1">;
   
   def GPR : RegisterClass<"MyTarget", [i32], 32, (add R0, R1, R2, R3)>;
   ```

3. **定义指令** (`MyTargetInstrInfo.td`)
   ```tablegen
   def ADD : Instruction {
     let OutOperandList = (outs GPR:$dst);
     let InOperandList = (ins GPR:$src1, GPR:$src2);
     let AsmString = "add $dst, $src1, $src2";
   }
   ```

4. **实现TargetMachine**
   ```cpp
   class MyTargetTargetMachine : public LLVMTargetMachine {
     // ...
   };
   ```

5. **注册目标**
   ```cpp
   extern "C" void LLVMInitializeMyTargetTarget() {
     RegisterTarget<> X(getTheMyTargetTarget(), "mytarget", "My Target");
   }
   ```

6. **添加到CMake**
   ```cmake
   add_llvm_target(MyTarget
     MyTargetTargetMachine.cpp
     MyTargetSubtarget.cpp
     ...
   )
   ```

## 源码导航

| 功能 | 目录 |
|---|---|
| X86后端 | `llvm/lib/Target/X86/` |
| ARM后端 | `llvm/lib/Target/ARM/` |
| AArch64后端 | `llvm/lib/Target/AArch64/` |
| RISC-V后端 | `llvm/lib/Target/RISCV/` |
| Target基类 | `llvm/lib/Target/` |
| SelectionDAG | `llvm/lib/CodeGen/SelectionDAG/` |
| GlobalISel | `llvm/lib/CodeGen/GlobalISel/` |
| TableGen | `llvm/utils/TableGen/` |

## 相关文档

- [代码生成](codegen.md) - MachineInstr和指令选择
- [MC层](mc-layer.md) - 目标文件生成
- [LLD链接器](lld-linker.md) - 链接目标文件
