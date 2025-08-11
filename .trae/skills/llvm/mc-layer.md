# LLVM MC层（机器码层）

MC层是LLVM代码生成的最后阶段，负责将MachineInstr转换为目标文件或汇编代码。

## 概述

MC层流程：
```
MachineInstr → MCInst → MCStreamer → 目标文件(.o) / 汇编(.s)
```

核心目录：`llvm/lib/MC/`

## MCInst 机器码指令

`MCInst` 是目标无关的机器指令表示，比 `MachineInstr` 更底层。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `MCInst` | MC指令 | `llvm/lib/MC/MCInst.cpp` |
| `MCOperand` | MC操作数 | `llvm/lib/MC/MCInst.cpp` |
| `MCInstrDesc` | 指令描述符 | `llvm/lib/MC/MCInstrDesc.cpp` |
| `MCInstrInfo` | 指令信息 | `llvm/lib/MC/MCInstrInfo.cpp` |

### MCInst 结构

```cpp
class MCInst {
  unsigned Opcode;              // 操作码
  SmallVector<MCOperand, 8> Operands;  // 操作数列表
  // ...
};

class MCOperand {
  enum OperandType {
    kInvalid,       // 无效
    kRegister,      // 寄存器
    kImmediate,     // 立即数
    kSFPImmediate,  // 单精度浮点立即数
    kDFPImmediate,  // 双精度浮点立即数
    kExpr,          // MC表达式
    kInst,          // 嵌套MCInst
  };
};
```

### MachineInstr 到 MCInst 转换

```cpp
// 在目标后端实现
void X86MCInstLower::Lower(const MachineInstr *MI, MCInst &OutMI) {
  OutMI.setOpcode(MI->getOpcode());
  
  for (const MachineOperand &MO : MI->operands()) {
    MCOperand MCOp;
    switch (MO.getType()) {
    case MachineOperand::MO_Register:
      MCOp = MCOperand::createReg(MO.getReg());
      break;
    case MachineOperand::MO_Immediate:
      MCOp = MCOperand::createImm(MO.getImm());
      break;
    case MachineOperand::MO_GlobalAddress:
      MCOp = LowerSymbolOperand(MO, GetSymbolFromOperand(MO));
      break;
    // ...
    }
    OutMI.addOperand(MCOp);
  }
}
```

## MCStreamer 输出流

`MCStreamer` 是MC层的核心输出接口，支持多种输出格式。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `MCStreamer` | 输出流基类 | `llvm/lib/MC/MCStreamer.cpp` |
| `MCObjectStreamer` | 对象文件输出基类 | `llvm/lib/MC/MCObjectStreamer.cpp` |
| `MCELFStreamer` | ELF格式输出 | `llvm/lib/MC/MCELFStreamer.cpp` |
| `MCWinCOFFStreamer` | COFF格式输出 | `llvm/lib/MC/MCWinCOFFStreamer.cpp` |
| `MCMachOStreamer` | Mach-O格式输出 | `llvm/lib/MC/MCMachOStreamer.cpp` |
| `MCAsmStreamer` | 汇编文本输出 | `llvm/lib/MC/MCAsmStreamer.cpp` |

### MCStreamer 接口

```cpp
class MCStreamer {
public:
  // 段管理
  virtual void switchSection(MCSection *Section);
  virtual void pushSection();
  virtual void popSection();
  
  // 指令发射
  virtual void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI);
  
  // 数据发射
  virtual void emitBytes(StringRef Data);
  virtual void emitValue(const MCExpr *Value, unsigned Size, SMLoc Loc);
  virtual void emitIntValue(uint64_t Value, unsigned Size);
  virtual void emitFill(uint64_t Count, uint8_t Value);
  
  // 符号管理
  virtual void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc());
  virtual void emitELFSize(MCSymbol *Symbol, const MCExpr *Size);
  virtual void emitELFSymverDirective(const MCSymbol *Aliasee, StringRef Name);
  
  // 对齐
  virtual void emitCodeAlignment(unsigned ByteAlignment, const MCSubtargetInfo *STI, unsigned MaxBytesToEmit = 0);
  virtual void emitValueToAlignment(unsigned ByteAlignment, int64_t Value = 0, unsigned ValueSize = 1, unsigned MaxBytesToEmit = 0);
  
  // 调试信息
  virtual void emitCFIStartProc(bool IsSimple, SMLoc Loc);
  virtual void emitCFIEndProc();
  virtual void emitCFIDefCfa(int64_t Register, int64_t Offset);
  virtual void emitCFIDefCfaOffset(int64_t Offset);
  virtual void emitCFIOffset(int64_t Register, int64_t Offset);
};
```

### 输出格式选择

```cpp
// 创建MCStreamer
std::unique_ptr<MCStreamer> createStreamer(
    raw_ostream &OS,
    MCContext &Ctx,
    const TargetOptions &Options) {
  
  if (Options.OutputType == OutputFileType::Assembly) {
    return createAsmStreamer(Ctx, OS);  // 汇编输出
  } else if (Options.ObjectFormat == Triple::ELF) {
    return createELFStreamer(Ctx);      // ELF对象文件
  } else if (Options.ObjectFormat == Triple::COFF) {
    return createWinCOFFStreamer(Ctx);  // COFF对象文件
  }
  // ...
}
```

## MCContext 上下文

`MCContext` 管理MC层的全局状态和资源。

### 核心功能

```cpp
class MCContext {
public:
  // 符号管理
  MCSymbol *getOrCreateSymbol(StringRef Name);
  MCSymbol *createTempSymbol();
  MCSymbol *createLinkerPrivateTempSymbol();
  
  // 节管理
  MCSectionELF *getELFSection(StringRef Section, unsigned Type, unsigned Flags);
  MCSectionCOFF *getCOFFSection(StringRef Section, unsigned Characteristics);
  
  // 表达式管理
  const MCExpr *createConstantExpr(int64_t Value);
  const MCExpr *createSymbolRefExpr(const MCSymbol *Symbol);
  const MCExpr *createBinaryExpr(const MCExpr *LHS, Opcode, const MCExpr *RHS);
  
  // 数据布局
  const DataLayout &getDataLayout() const;
  
  // 目标信息
  const MCAsmInfo *getAsmInfo() const;
  const MCRegisterInfo *getRegisterInfo() const;
  const MCInstrInfo *getInstrInfo() const;
};
```

## MCSymbol 符号

`MCSymbol` 表示目标文件中的符号。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `MCSymbol` | 符号基类 | `llvm/lib/MC/MCSymbol.cpp` |
| `MCSymbolELF` | ELF符号 | `llvm/lib/MC/MCSymbolELF.cpp` |
| `MCSymbolCOFF` | COFF符号 | - |
| `MCSymbolMachO` | Mach-O符号 | - |

### 符号属性

```cpp
class MCSymbol {
public:
  enum SymbolKind {
    SymbolKindTemporary,    // 临时符号（.L开头）
    SymbolKindLinkerPrivate, // 链接器私有符号
    SymbolKindGlobal,       // 全局符号
  };
  
  // 符号状态
  bool isDefined() const;   // 是否已定义
  bool isInSection() const; // 是否在节中
  bool isExternal() const;  // 是否外部可见
  bool isPrivateExtern() const;
  
  // ELF特定属性
  void setBinding(unsigned Binding);  // STB_LOCAL, STB_GLOBAL, STB_WEAK
  void setType(unsigned Type);        // STT_NOTYPE, STT_OBJECT, STT_FUNC, ...
  void setVisibility(unsigned Visibility); // STV_DEFAULT, STV_HIDDEN, ...
};
```

## MCSection 节

`MCSection` 表示目标文件中的节。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `MCSection` | 节基类 | `llvm/lib/MC/MCSection.cpp` |
| `MCSectionELF` | ELF节 | `llvm/lib/MC/MCSectionELF.cpp` |
| `MCSectionCOFF` | COFF节 | - |
| `MCSectionMachO` | Mach-O节 | `llvm/lib/MC/MCSectionMachO.cpp` |

### ELF节类型和标志

```cpp
// 节类型 (sh_type)
enum SectionType {
  SHT_NULL,      // 无效节
  SHT_PROGBITS,  // 程序数据
  SHT_SYMTAB,    // 符号表
  SHT_STRTAB,    // 字符串表
  SHT_RELA,      // 重定位表（带加数）
  SHT_HASH,      // 符号哈希表
  SHT_DYNAMIC,   // 动态链接信息
  SHT_NOTE,      // 注释信息
  SHT_NOBITS,    // 未初始化数据（.bss）
  SHT_REL,       // 重定位表（不带加数）
  // ...
};

// 节标志 (sh_flags)
enum SectionFlags {
  SHF_WRITE,     // 可写
  SHF_ALLOC,     // 需要分配内存
  SHF_EXECINSTR, // 可执行
  SHF_MERGE,     // 可合并
  SHF_STRINGS,   // 包含以null结尾的字符串
  SHF_TLS,       // TLS数据
  SHF_GROUP,     // 节组成员
  // ...
};
```

## MCExpr 表达式

`MCExpr` 表示汇编级表达式，用于重定位和符号计算。

### 核心类

```cpp
class MCExpr {
public:
  enum ExprKind {
    Constant,      // 常量
    SymbolRef,     // 符号引用
    Unary,         // 一元操作
    Binary,        // 二元操作
    Target,        // 目标特定表达式
  };
  
  virtual void print(raw_ostream &OS, const MCAsmInfo *MAI) const;
  virtual bool evaluateAsRelocatable(MCValue &Res, const MCLayout *Layout) const;
};

class MCBinaryExpr : public MCExpr {
public:
  enum Opcode {
    Add, Sub, Mul, Div, Mod,  // 算术运算
    Shl, Shr,                 // 移位
    And, Or, Xor,             // 位运算
    EQ, NE, LT, GT, LE, GE,   // 比较
  };
};

class MCSymbolRefExpr : public MCExpr {
public:
  enum VariantKind {
    VK_None,
    VK_GOT,         // @GOT
    VK_GOTOFF,      // @GOTOFF
    VK_PLT,         // @PLT
    VK_TLSGD,       // @TLSGD
    VK_TLSLD,       // @TLSLD
    VK_TPOFF,       // @TPOFF
    VK_DTPOFF,      // @DTPOFF
    VK_GOTPCREL,    // @GOTPCREL (x86-64)
    VK_LO, VK_HI,   // 低/高位
    // ...
  };
};
```

## MCCodeEmitter 代码发射器

`MCCodeEmitter` 负责将MCInst编码为机器码字节。

### 核心接口

```cpp
class MCCodeEmitter {
public:
  virtual void encodeInstruction(
      const MCInst &Inst,
      SmallVectorImpl<char> &CB,
      SmallVectorImpl<MCFixup> &Fixups,
      const MCSubtargetInfo &STI) const = 0;
};
```

### 编码过程

```cpp
// X86示例
void X86MCCodeEmitter::encodeInstruction(
    const MCInst &MI, 
    SmallVectorImpl<char> &CB,
    SmallVectorImpl<MCFixup> &Fixups,
    const MCSubtargetInfo &STI) const {
  
  // 1. 获取指令描述
  const MCInstrDesc &Desc = MII.get(MI.getOpcode());
  
  // 2. 编码前缀
  emitPrefix(MI, CB, STI);
  
  // 3. 编码操作码
  emitOpcode(MI, CB, STI);
  
  // 4. 编码ModR/M和SIB
  emitModRM(MI, CB, Fixups, STI);
  
  // 5. 编码立即数/偏移
  emitImmediate(MI, CB, Fixups, STI);
}
```

## MCFixup 重定位

`MCFixup` 表示需要链接器填充的重定位位置。

### 核心类

```cpp
struct MCFixup {
  uint32_t Offset;      // 在节中的偏移
  const MCExpr *Value;  // 需要修复的值
  MCFixupKind Kind;     // 重定位类型
  SMLoc Loc;            // 源码位置
};

enum MCFixupKind {
  FK_NONE,
  FK_Data_1,            // 1字节数据
  FK_Data_2,            // 2字节数据
  FK_Data_4,            // 4字节数据
  FK_Data_8,            // 8字节数据
  FK_PCRel_1,           // 1字节PC相对
  FK_PCRel_2,           // 2字节PC相对
  FK_PCRel_4,           // 4字节PC相对
  FK_GPRel_4,           // GP相对
  FK_SecRel_2,          // 节相对
  FK_SecRel_4,          // 节相对
  // 目标特定类型...
};
```

## AsmPrinter 汇编打印器

`AsmPrinter` 是CodeGen和MC层的桥梁，将MachineInstr转换为MCInst并输出。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `AsmPrinter` | 汇编打印器基类 | `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` |
| `TargetLoweringObjectFile` | 目标对象文件信息 | `llvm/lib/CodeGen/TargetLoweringObjectFileImpl.cpp` |

### AsmPrinter 工作流程

```cpp
void AsmPrinter::emitFunctionBody() {
  // 1. 发射函数标签
  emitFunctionEntryLabel();
  
  // 2. 发射CFI起始
  emitCFIStartProc();
  
  // 3. 遍历基本块
  for (const MachineBasicBlock &MBB : *MF) {
    // 发射基本块标签
    emitBasicBlockStart(MBB);
    
    // 4. 遍历指令
    for (const MachineInstr &MI : MBB) {
      emitInstruction(&MI);
    }
    
    emitBasicBlockEnd(MBB);
  }
  
  // 5. 发射CFI结束
  emitCFIEndProc();
  
  // 6. 发射常量池
  emitConstantPool();
}
```

### 目标特定AsmPrinter

```cpp
// X86示例
class X86AsmPrinter : public AsmPrinter {
  void emitInstruction(const MachineInstr *MI) override {
    // 1. 特殊指令处理
    if (emitSpecialInstruction(MI))
      return;
    
    // 2. 转换为MCInst
    MCInst TmpInst;
    Lower->Lower(MI, TmpInst);
    
    // 3. 发射到MCStreamer
    EmitToStreamer(*OutStreamer, TmpInst);
  }
};
```

## 对象文件生成流程

完整的对象文件生成流程：

```
MachineFunction
      ↓
   AsmPrinter
      ↓
   MCInst (通过目标MCInstLower)
      ↓
   MCStreamer
   ├── MCAsmStreamer → 汇编文本 (.s)
   └── MCObjectStreamer
           ├── MCCodeEmitter → 机器码
           ├── MCAssembler → 组装
           └── MCObjectWriter → 目标文件 (.o)
                   ├── ELFObjectWriter
                   ├── WinCOFFObjectWriter
                   └── MachObjectWriter
```

## 调试信息

MC层支持多种调试信息格式：

### DWARF调试信息

```cpp
// 发射DWARF调试信息
class DwarfDebug {
  void beginModule();
  void endModule();
  void beginFunction(const MachineFunction *MF);
  void endFunction(const MachineFunction *MF);
  
  // 发射.debug_info节
  void emitDebugInfo();
  
  // 发射.debug_line节
  void emitDebugLine();
  
  // 发射.debug_abbrev节
  void emitDebugAbbrev();
};
```

### CodeView调试信息（Windows）

```cpp
class CodeViewDebug {
  void emitDebugInfoForFunction(const Function *F);
  void emitDebugInfoForGlobals();
  
  // 发射.pdb节
  void emitPDB();
};
```

## 源码导航

| 功能 | 目录/文件 |
|---|---|
| MCInst | `llvm/lib/MC/MCInst.cpp` |
| MCStreamer | `llvm/lib/MC/MCStreamer.cpp` |
| MCContext | `llvm/lib/MC/MCContext.cpp` |
| MCSymbol | `llvm/lib/MC/MCSymbol.cpp` |
| MCSection | `llvm/lib/MC/MCSection.cpp` |
| MCExpr | `llvm/lib/MC/MCExpr.cpp` |
| MCCodeEmitter | `llvm/lib/MC/MCCodeEmitter.cpp` |
| ELF输出 | `llvm/lib/MC/ELFObjectWriter.cpp` |
| COFF输出 | `llvm/lib/MC/WinCOFFObjectWriter.cpp` |
| AsmPrinter | `llvm/lib/CodeGen/AsmPrinter/` |

## 相关文档

- [代码生成](codegen.md) - MachineInstr和指令选择
- [Target后端](target-backend.md) - 目标后端实现
- [LLD链接器](lld-linker.md) - 链接目标文件
