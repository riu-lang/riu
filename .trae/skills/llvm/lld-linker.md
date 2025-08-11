# LLD 链接器

LLD是LLVM项目的模块化链接器，支持ELF、COFF、Mach-O和WebAssembly格式。

## 概述

LLD链接流程：
```
目标文件(.o) → 符号解析 → 节合并 → 重定位 → 可执行文件(.exe/.out)
```

核心目录：
- `lld/ELF/` - ELF链接器（Linux/Unix）
- `lld/COFF/` - COFF链接器（Windows）
- `lld/MachO/` - Mach-O链接器（macOS/iOS）
- `lld/wasm/` - WebAssembly链接器

## ELF链接器架构

ELF链接器是LLD最成熟的实现，以下详细介绍其架构。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `LinkerDriver` | 链接器驱动，解析命令行参数 | `lld/ELF/Driver.cpp` |
| `SymbolTable` | 符号表管理 | `lld/ELF/SymbolTable.cpp` |
| `InputFile` | 输入文件基类 | `lld/ELF/InputFiles.cpp` |
| `InputSection` | 输入节 | `lld/ELF/InputSection.cpp` |
| `OutputSection` | 输出节 | `lld/ELF/OutputSections.cpp` |
| `Writer` | 输出文件写入器 | `lld/ELF/Writer.cpp` |

### 链接流程

```
1. 解析命令行参数
   Driver::link()
      ↓
2. 读取输入文件
   InputFile::create()
      ↓
3. 符号解析
   SymbolTable::addFile()
   SymbolTable::resolveSymbols()
      ↓
4. 处理弱符号和重复定义
   SymbolTable::scanShlibUndefined()
      ↓
5. 垃圾回收（--gc-sections）
   MarkLive::run()
      ↓
6. 相同节合并（ICF）
   ICF::run()
      ↓
7. 计算节大小和布局
   Writer::run()
      ↓
8. 应用重定位
   InputSection::relocate()
      ↓
9. 写入输出文件
   Writer::writeHeader()
   Writer::writeSections()
```

## Driver 驱动器

`Driver` 负责解析命令行参数并协调链接过程。

### 核心功能

```cpp
namespace lld::elf {

class LinkerDriver {
public:
  void link(ArrayRef<const char *> Args);
  
private:
  void createFiles();
  void inferMachineType();
  void link();
  void compileBitcodeFiles();
  
  SmallVector<InputFile *, 0> Files;
};

void link(ArrayRef<const char *> Args) {
  LinkerDriver().link(Args);
}

}
```

### 常用链接选项

| 选项 | 说明 |
|---|---|
| `-o <file>` | 输出文件名 |
| `-e <symbol>` | 入口点符号 |
| `-l<lib>` | 链接库 |
| `-L<path>` | 库搜索路径 |
| `-rpath <path>` | 运行时库搜索路径 |
| `-soname <name>` | 共享库名称 |
| `--gc-sections` | 垃圾回收未使用节 |
| `--icf=all` | 相同代码折叠 |
| `--strip-all` | 移除符号表 |
| `--export-dynamic` | 导出所有符号 |
| `-Bstatic/-Bdynamic` | 静态/动态链接 |
| `--as-needed` | 按需链接库 |
| `-T <script>` | 链接脚本 |

## Symbol 符号

### 符号类型

```cpp
namespace lld::elf {

class Symbol {
public:
  enum Kind {
    DefinedKind,      // 定义符号
    UndefinedKind,    // 未定义符号
    CommonKind,       // 公共符号
    LazyKind,         // 惰性符号（归档库）
  };
  
  StringRef getName() const;
  uint64_t getValue() const;
  uint64_t getSize() const;
  uint8_t getBinding() const;   // STB_LOCAL/GLOBAL/WEAK
  uint8_t getType() const;      // STT_NOTYPE/OBJECT/FUNC/...
  uint8_t getVisibility() const;
  
  InputFile *getFile() const;
  InputSectionBase *getSection() const;
};

class Defined : public Symbol {
  uint64_t Value;           // 符号值（地址）
  uint64_t Size;            // 大小
  InputSectionBase *Section; // 所属节
};

class Undefined : public Symbol {
  // 需要从其他文件解析的符号
};

class Lazy : public Symbol {
  LazyArchive *Archive;     // 所属归档文件
};

}
```

### 符号解析规则

```cpp
void SymbolTable::resolve(Symbol *Old, Symbol *New) {
  // 解析优先级（从高到低）：
  // 1. 定义符号 > 未定义符号 > 惰性符号
  // 2. 强符号 > 弱符号
  // 3. 普通定义 > 公共符号
  
  if (Old->isDefined() && New->isUndefined())
    return;  // 保留旧符号
    
  if (Old->isWeak() && New->isStrong())
    replace(Old, New);  // 强符号覆盖弱符号
    
  if (Old->isCommon() && New->isDefined())
    replace(Old, New);  // 定义覆盖公共符号
    
  // 检查重复定义
  if (Old->isDefined() && New->isDefined()) {
    if (!Old->isWeak() && !New->isWeak())
      error("duplicate symbol: " + Old->getName());
  }
}
```

## InputFile 输入文件

### 文件类型

```cpp
namespace lld::elf {

class InputFile {
public:
  enum Kind {
    ObjKind,        // 目标文件 (.o)
    SharedKind,     // 共享库 (.so)
    ArchiveKind,    // 静态库 (.a)
    BitcodeKind,    // LLVM位码 (.bc)
    BinaryKind,     // 二进制文件
  };
  
  MemoryBufferRef MB;
  StringRef getName() const;
  ELFKind getELFKind() const;
  uint16_t getEMachine() const;
};

class ObjFile : public InputFile {
  ArrayRef<Symbol *> getSymbols();
  ArrayRef<InputSectionBase *> getSections();
  
  void parse();
};

class SharedFile : public InputFile {
  ArrayRef<Symbol *> getSharedSymbols();
  bool isNeeded() const;
};

class ArchiveFile : public InputFile {
  void parse();
  void fetch(Member);
  ArrayRef<MemoryBufferRef> getMembers();
};

class BitcodeFile : public InputFile {
  std::unique_ptr<lto::InputFile> Obj;
  void parse();
};

}
```

## InputSection 输入节

### 节类型

```cpp
namespace lld::elf {

class InputSectionBase {
public:
  enum Kind {
    Regular,        // 普通节
    EHFrame,        // .eh_frame节
    Merge,          // 可合并节
    Synthetic,      // 合成节
  };
  
  ObjFile *File;            // 所属文件
  uint32_t Index;           // 节索引
  uint64_t Flags;           // 节标志
  uint32_t Alignment;       // 对齐
  ArrayRef<uint8_t> Content; // 节内容
  
  OutputSection *getOutputSection();
  uint64_t getOffset();     // 在输出节中的偏移
};

class InputSection : public InputSectionBase {
  ArrayRef<Relocation> Relocations; // 重定位表
  
  void relocate(uint8_t *Buf, uint8_t *BufEnd);
};

class MergeInputSection : public InputSectionBase {
  // 字符串合并节（.rodata.str）
  void splitStrings();
  void splitNonStrings();
};

}
```

### 重定位处理

```cpp
struct Relocation {
  uint32_t Offset;          // 节内偏移
  uint32_t Type;            // 重定位类型
  int64_t Addend;           // 加数
  Symbol *Sym;              // 关联符号
};

void InputSection::relocate(uint8_t *Buf, uint8_t *BufEnd) {
  for (const Relocation &Rel : Relocations) {
    uint8_t *Loc = Buf + Rel.Offset;
    uint64_t Val = Rel.Sym->getVA() + Rel.Addend;
    
    // 根据重定位类型应用
    switch (Rel.Type) {
    case R_X86_64_64:
      write64le(Loc, Val);
      break;
    case R_X86_64_PC32:
      write32le(Loc, Val - (Loc - Buf + getOutputSection()->Addr));
      break;
    case R_X86_64_PLT32:
      write32le(Loc, Val - (Loc - Buf + getOutputSection()->Addr) - 4);
      break;
    // ...
    }
  }
}
```

## OutputSection 输出节

### 输出节结构

```cpp
namespace lld::elf {

class OutputSection {
public:
  StringRef Name;           // 节名
  uint32_t Index;           // 节索引
  uint64_t Flags;           // 节标志
  uint32_t Alignment;       // 对齐
  uint64_t Addr;            // 虚拟地址
  uint64_t Size;            // 大小
  uint64_t Offset;          // 文件偏移
  
  SmallVector<InputSection *, 0> Sections; // 输入节列表
  
  void finalize();
  void writeTo(uint8_t *Buf);
};

}
```

### 默认节布局

```
.text     : 代码段
.rodata   : 只读数据
.data     : 已初始化数据
.bss      : 未初始化数据
.init     : 初始化代码
.fini     : 结束代码
.got      : 全局偏移表
.plt      : 过程链接表
.eh_frame : 异常处理帧
.dynsym   : 动态符号表
.dynstr   : 动态字符串表
```

## SyntheticSections 合成节

合成节是链接器生成的特殊节。

### 核心合成节

| 类 | 说明 | 源码位置 |
|---|---|---|
| `GotSection` | 全局偏移表 | `lld/ELF/SyntheticSections.cpp` |
| `PltSection` | 过程链接表 | `lld/ELF/SyntheticSections.cpp` |
| `GotPltSection` | GOT PLT表 | `lld/ELF/SyntheticSections.cpp` |
| `RelocationSection` | 重定位表 | `lld/ELF/SyntheticSections.cpp` |
| `SymbolTableSection` | 符号表 | `lld/ELF/SyntheticSections.cpp` |
| `StringTableSection` | 字符串表 | `lld/ELF/SyntheticSections.cpp` |
| `DynamicSection` | 动态段 | `lld/ELF/SyntheticSections.cpp` |
| `EhFrameSection` | 异常处理帧 | `lld/ELF/SyntheticSections.cpp` |

### 合成节示例

```cpp
class GotSection : public SyntheticSection {
  std::vector<const Symbol *> Entries;
  
public:
  void addEntry(const Symbol *Sym);
  void writeTo(uint8_t *Buf) override {
    for (size_t I = 0; I < Entries.size(); ++I) {
      write64le(Buf + I * 8, Entries[I]->getVA());
    }
  }
};

class PltSection : public SyntheticSection {
  std::vector<const Symbol *> Entries;
  
public:
  void addEntry(const Symbol *Sym);
  void writeTo(uint8_t *Buf) override {
    for (size_t I = 0; I < Entries.size(); ++I) {
      // 写入PLT入口代码
      writePltEntry(Buf + I * PltEntrySize, I);
    }
  }
};
```

## LinkerScript 链接脚本

链接脚本提供对链接过程的精细控制。

### 脚本语法

```ld
/* 示例链接脚本 */
OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
  . = 0x400000;
  
  .text : {
    *(.text .text.*)
  }
  
  .rodata : {
    *(.rodata .rodata.*)
  }
  
  .data : {
    *(.data .data.*)
  }
  
  .bss : {
    *(.bss .bss.*)
    *(COMMON)
  }
  
  /DISCARD/ : {
    *(.comment)
    *(.note.*)
  }
}
```

### 脚本解析

```cpp
namespace lld::elf {

class LinkerScript {
public:
  void readScript(MemoryBufferRef MB);
  void processCommands();
  void assignAddresses();
  
  OutputSection *createOutputSection(StringRef Name);
  void addInputSection(OutputSection *OS, InputSection *IS);
};

class ScriptParser {
  void parse();
  void parseOutputFormat();
  void parseSections();
  void parseMemory();
  void parseVersionScript();
};

}
```

## ICF 相同代码折叠

ICF（Identical Code Folding）合并相同的函数以减小输出大小。

### ICF算法

```cpp
namespace lld::elf {

class ICF {
public:
  void run();
  
private:
  void segregate();
  bool equalsConstant(const InputSection *A, const InputSection *B);
  bool equalsVariable(const InputSection *A, const InputSection *B);
};

void ICF::run() {
  // 1. 初始分组（按内容哈希）
  segregate();
  
  // 2. 迭代细化（考虑重定位）
  while (true) {
    bool Changed = false;
    for (auto &Group : Groups) {
      // 检查组内节是否真正相同
      if (equalsConstant(A, B))
        Changed |= merge(A, B);
    }
    if (!Changed) break;
  }
  
  // 3. 标记保留的节
  for (InputSection *IS : Sections) {
    if (IS->Leader)
      IS->Live = false;
  }
}

}
```

## 垃圾回收

`--gc-sections` 选项启用未使用节的垃圾回收。

### 标记存活算法

```cpp
namespace lld::elf {

class MarkLive {
public:
  static void run();
  
private:
  void markLive(Symbol *Sym);
  void markLive(InputSection *IS);
  void scanEhFrameSection(EhInputSection *EH);
};

void MarkLive::run() {
  // 1. 标记入口点和导出符号
  for (Symbol *Sym : Symtab->getSymbols()) {
    if (Sym->isDefined() && (Sym->isGlobal() || Sym->isWeak()))
      markLive(Sym);
  }
  
  // 2. 标记入口点
  Symbol *Entry = Symtab->find(Config->Entry);
  if (Entry)
    markLive(Entry);
  
  // 3. 传播存活标记
  while (!Worklist.empty()) {
    InputSection *IS = Worklist.pop_back_val();
    for (Symbol *Sym : IS->getSymbols())
      markLive(Sym);
  }
  
  // 4. 清理死节
  for (InputSection *IS : InputSections) {
    if (!IS->Live)
      IS->markDead();
  }
}

}
```

## LTO 链接时优化

LLD支持LTO（Link Time Optimization），允许跨模块优化。

### LTO流程

```cpp
namespace lld::elf {

class BitcodeCompiler {
public:
  void add(BitcodeFile *F);
  std::vector<InputFile *> compile();
};

void BitcodeCompiler::add(BitcodeFile *F) {
  // 收集位码文件
  Files.push_back(F);
}

std::vector<InputFile *> BitcodeCompiler::compile() {
  // 1. 创建LTO后端
  lto::Config Conf;
  Conf.OptLevel = Config->LTOOptLevel;
  Conf.CPU = Config->MCPU;
  
  lto::LTO Lto(Conf);
  
  // 2. 添加模块
  for (BitcodeFile *F : Files) {
    Lto.add(std::move(F->Obj));
  }
  
  // 3. 执行优化和代码生成
  SmallVector<std::pair<std::string, SmallString<0>>, 0> Outputs;
  Lto.run([&](size_t Task) {
    return std::make_unique<cachify_stream>(Outputs[Task].second);
  });
  
  // 4. 返回生成的目标文件
  std::vector<InputFile *> Ret;
  for (auto &P : Outputs) {
    Ret.push_back(ObjFile::create(MemoryBufferRef(P.second, P.first)));
  }
  return Ret;
}

}
```

## Writer 输出写入器

`Writer` 负责生成最终的输出文件。

### 写入流程

```cpp
namespace lld::elf {

class Writer {
public:
  void run();
  
private:
  void createSyntheticSections();
  void finalizeSections();
  void assignAddresses();
  void writeHeader();
  void writeSections();
  void writeBuildId();
};

void Writer::run() {
  // 1. 创建合成节
  createSyntheticSections();
  
  // 2. 收集输出节
  for (InputSection *IS : InputSections) {
    getOrCreateOutputSection(IS->Name)->addSection(IS);
  }
  
  // 3. 分配地址
  assignAddresses();
  
  // 4. 应用重定位
  for (InputSection *IS : InputSections) {
    IS->relocate(Buf + IS->Offset);
  }
  
  // 5. 写入文件头
  writeHeader();
  
  // 6. 写入节内容
  writeSections();
  
  // 7. 计算并写入Build ID
  writeBuildId();
}

}
```

## 目标架构支持

LLD支持多种目标架构：

| 架构 | 目录 |
|---|---|
| X86/X86-64 | `lld/ELF/Arch/X86.cpp`, `lld/ELF/Arch/X86_64.cpp` |
| ARM | `lld/ELF/Arch/ARM.cpp` |
| AArch64 | `lld/ELF/Arch/AArch64.cpp` |
| RISC-V | `lld/ELF/Arch/RISCV.cpp` |
| PowerPC | `lld/ELF/Arch/PPC.cpp`, `lld/ELF/Arch/PPC64.cpp` |
| MIPS | `lld/ELF/Arch/Mips.cpp` |
| LoongArch | `lld/ELF/Arch/LoongArch.cpp` |

### 目标特定处理

```cpp
namespace lld::elf::x86_64 {

class X86_64 : public TargetInfo {
public:
  void relocate(uint8_t *Loc, uint64_t Val, uint32_t Type) const override {
    switch (Type) {
    case R_X86_64_64:
      write64le(Loc, Val);
      break;
    case R_X86_64_PC32:
      checkInt32(Loc, Val);
      write32le(Loc, Val);
      break;
    case R_X86_64_GOTPCREL:
      write32le(Loc, Val - 4);
      break;
    case R_X86_64_PLT32:
      write32le(Loc, Val - 4);
      break;
    // ...
    }
  }
  
  RelExpr getRelExpr(uint32_t Type) const override;
  void writeGotHeader(uint8_t *Buf) const override;
  void writePltHeader(uint8_t *Buf) const override;
  void writePltEntry(uint8_t *Buf, uint64_t GotPlt) const override;
};

}
```

## COFF链接器（Windows）

COFF链接器用于Windows平台。

### 核心类

| 类 | 说明 | 源码位置 |
|---|---|---|
| `COFFLinkerContext` | COFF链接上下文 | `lld/COFF/Driver.cpp` |
| `Symbol` | COFF符号 | `lld/COFF/Symbols.h` |
| `Chunk` | COFF块（节） | `lld/COFF/Chunks.cpp` |
| `Writer` | COFF输出写入器 | `lld/COFF/Writer.cpp` |

### COFF特性

- 支持SEH（结构化异常处理）
- 支持延迟加载DLL
- 支持增量链接
- 支持PDB调试信息生成

## 源码导航

| 功能 | 目录/文件 |
|---|---|
| ELF驱动 | `lld/ELF/Driver.cpp` |
| 符号表 | `lld/ELF/SymbolTable.cpp` |
| 符号定义 | `lld/ELF/Symbols.cpp` |
| 输入文件 | `lld/ELF/InputFiles.cpp` |
| 输入节 | `lld/ELF/InputSection.cpp` |
| 输出节 | `lld/ELF/OutputSections.cpp` |
| 合成节 | `lld/ELF/SyntheticSections.cpp` |
| 写入器 | `lld/ELF/Writer.cpp` |
| 链接脚本 | `lld/ELF/LinkerScript.cpp` |
| ICF | `lld/ELF/ICF.cpp` |
| 垃圾回收 | `lld/ELF/MarkLive.cpp` |
| LTO | `lld/ELF/LTO.cpp` |
| 架构支持 | `lld/ELF/Arch/` |
| COFF链接器 | `lld/COFF/` |

## 相关文档

- [MC层](mc-layer.md) - 目标文件生成
- [代码生成](codegen.md) - IR到机器码
- [Target后端](target-backend.md) - 目标后端实现
