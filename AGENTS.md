# yux

独立的编译器，类似于clang，一个程序能完成所有。

**不要改语法文件**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

应优先用`clangd`工具，更快速准确。

## 环境

项目用`Clang`构建

命令行为 windows `powershell`，无msvc开发环境变量

系统PATH包含 llvm/bin：
amdgpu-arch
bbc
clang
clang*:
  ++
  -apply-replacements
  -change-namespace
  -check
  -cl
  -cpp
  -doc
  -extdef-mapping
  -format
  -include-cleaner
  -include-fixer
  -installapi
  -linker-wrapper
  -move
  -nvlink-wrapper
  -offload-bundler
  -query
  -refactor
  -reorder-fields
  -repl
  -scan-deps
  -sycl-linker
  -tidy
  d
diagtool
f18-parse-demo
find-all-symbols
fir-lsp-server
fir-opt
flang-new
flang
git-clang-format
ld.lld
ld64.lld
lld-link
lld
lldb
lldb-*:
  argdumper
  dap
  instr
  mcp
  server
llvm-*:
  ar
  cov
  cxxfilt
  dlltool
  dwp
  lib
  mca
  ml
  ml64
  mt
  nm
  objcopy
  objdump
  pdbutil
  profdata
  profgen
  ranlib
  rc
  readobj
  size
  strings
  strip
  symbolizer
modularize
nvptx-arch
offload-arch
pp-trace
tco
wasm-ld
yaml2macho-core

命令环境含有`windows`兼容 `head` `tail`，不支持直接`-行数`，用`-n 行数`

## 基本文件

源文件全部使用`\n`换行

```
include/types.h  基本类型定义
sdk/             自举rt
src/yux.g4       语法文件
src/main.cpp     程序入口
gen/             g4生成代码
third_party/     外部依赖
```

## 源码依赖（third_party目录）

可能搜不到文件内容

- [antlr4](third_party/antlr4)
- [googletest](third_party/googletest)
- [llvm](third_party/llvm)
- [utfcpp](third_party/utfcpp)
- [zlib](third_party/zlib)

## 构建

目标：
- `yux`

- `ninja` 构建`yux`目标 推荐使用；无法构建全部目标，**不指定目标会构建失败**
- 目录`cmake-build-debug` 配置会自动重新加载。 只能执行构建命令，不要改动配置，如需操作，告诉用户（用户会重新配置）
    不要直接cmake重新生成/清理该目录，没有编译环境无法成功，让用户去执行这些操作。
    无法构建全部目标，除非你知道在干什么，否则用`ninja`

## 调试编译exe

debug构建目录已加到`PATH`，可直接用`yux`

```debug build help
yux compiler


yux.exe [OPTIONS] input


POSITIONALS:
  input TEXT REQUIRED         Input .yux file

OPTIONS:
  -h,     --help              Print this help message and exit
          --emit-ir           Emit LLVM IR to .ll file
  -d,     --debug             Output compilation IR debug information
```

编译成功后在工作目录输出到`build`目录，返回`0`。
编译成功才会有IR。
-d始终会有信息，信息大多为入参出参。

## 测试

[main.yux](main.yux)

## 示例

**空格规则**：关键字后必须有空格，二元运算符两边必须有空格，`()` `[]` 内部无空格，`,` 后有空格。

**不存在隐式转换，所有类型必须显式转换。**

详情见[语法](语法.md)

```yux
/ add 注释为顶行/，其它位置用为除号
fn add(a i32, b i32) i32 = a + b

/ sub
fn sub(a i32, b i32) i32 {
  ret a + b ; 也可以直接 a + b 类似rust，空格+; 为尾随注释
}

/ 入口，空返回
fn main() {
  var a i32 = add(1, 2)
  var b i32 = sub(a, 1)
  println(a + b)
  val arr [i32 * 2] = [0, 2] ; 默认i32类型
  arr[0] = b - 1
  println(arr[0] < arr[1])

  / if 是表达式，也可单独用
  val c = if a > b {
    1
  } else {
    0
  }
  / 表达式尾随;表示空返回，类似rust
  println(c)

  / 循环
  loop {
    if b <= 0 {
      break; ; break强制尾随;表示空返回
    }
    println(b)
    b = b - 1
  }
}

struct Abc {
  a i32
  b u8
}

/ 实现结构体
Abc {
  / 构造函数，同结构体名
  fn Abc(a i32, b i32){
    self.a = a
    self.b = b.to_u8()
  }
}
```
