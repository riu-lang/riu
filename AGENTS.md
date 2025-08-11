# yux

独立的编译器，类似于clang，一个程序能完成所有。

**不要改语法文件**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

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
src/yux.g4   语法文件
src/main.cpp 程序入口
gen/         g4生成代码
rt/          运行静态库，用于编译后链接
third_party/ 外部依赖
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
- `yux_rt`

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

编译成功后在工作目录输出`文件名.exe`，返回`0`。
编译成功才会有IR。
-d始终会有信息，信息大多为入参出参。

## 测试

[main.yux](main.yux)
