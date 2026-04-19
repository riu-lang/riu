# yux

独立的编译器，类似于clang，一个程序能完成所有。

**不要改语法文件**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

## 快速入门

### 1. 项目概述

Yux 是一个自举的编程语言编译器，使用 LLVM 作为后端，支持：
- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Box<T>, Ptr<T>, Array<T>）
- 自动内存管理（Box<T> 引用计数）
- 与 C/Windows API 互操作

### 2. 快速构建

```powershell
# 构建 yux 编译器（推荐）
xmake build yux

# 测试编译
yux main.yux
```

## 环境

项目用`xmake`+`Clang`构建

命令行为 windows `powershell`，无msvc开发环境变量

系统PATH包含 llvm/bin

命令环境含有`windows`兼容 `head` `tail`，不支持直接`-行数`，用`-n 行数`，始终返回0，不管上游有什么错误退出了。

## 项目结构

### 核心目录

```
yux-lang/
├── src/                  # 源代码
│   ├── main.cpp         # 程序入口
│   ├── yux.g4           # 语法文件（不要修改）
│   ├── compiler.cpp/h   # LLVM IR 生成
│   ├── ast_builder.cpp/h # AST 构建
│   └── node/            # AST 节点定义
│       ├── expr_node.*  # 表达式节点
│       ├── fn_node.*    # 函数节点
│       ├── struct_node.* # 结构体节点
│       └── statement_node.* # 语句节点
├── include/
│   └── types.h          # 基本类型定义
├── sdk/
│   └── sdk.yux          # 自举运行时库
├── gen/                 # ANTLR4 生成代码
├── third_party/         # 外部依赖
│   ├── antlr4/          # 解析器生成器
│   ├── llvm/            # 编译器后端
│   ├── googletest/      # 测试框架
│   ├── utfcpp/          # UTF-8 处理
│   └── zlib/            # 压缩库
└── tests/               # 测试用例
    └── cases/           # 测试文件
```

### 编译流程

```
源代码 (.yux)
    ↓
词法分析 (ANTLR4 Lexer)
    ↓
语法分析 (ANTLR4 Parser)
    ↓
AST 构建 (ASTBuilder)
    ↓
语义分析 (符号表、类型检查)
    ↓
IR 生成 (Compiler → LLVM IR)
    ↓
目标代码生成 (LLVM)
    ↓
链接 (LLD)
    ↓
可执行文件 (build/*.exe)
```

## 构建

目标：
- `yux`

## 调试编译exe

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
-d始终会有信息，信息大多为入参出参，信息非常多，请配合`tail`或其它过滤的使用。

## 测试

[main.yux](main.yux)

## 语法要点

### 注释规则

**只有两种注释：**
- **行注释**：顶行或缩进，行匹配 `^\s*/.*`
- **尾随注释**：用 ` ;`，代码行或空行后，不能用 `/` 尾随

```yux
/ 这是行注释，顶行或缩进
fn some() {
  / 也可缩进
  println(123) ; 这是尾随注释，空格;开头
}
```

### 空格规则

- 关键字后必须有空格
- 二元运算符两边必须有空格
- `()` `[]` 内部无空格
- `,` 后有空格

```yux
/ 正确
fn add(a i32, b i32) i32 = a + b
val arr = [1, 2, 3]

/ 错误
fn add(a i32,b i32)i32=a+b ; 缺少空格
val arr = [ 1,2,3 ]        ; 括号内多余空格
```

### 类型系统

**基本类型：**
- 整数：`i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`, `u64`
- 浮点：`f32`, `f64`
- 布尔：`bool`
- 字符串：`String`

**特殊类型：**
- `Ref<T>` - 不可空引用
- `Box<T>` - 堆对象智能指针（引用计数）
- `Ptr<T>` - 可空原始指针
- `Array<T>` - 动态数组
- `[T * N]` - 固定大小数组

**类型转换：**
- 无隐式转换，必须显式转换
- 使用 `.to_类型()` 方法：`a.to_i32()`, `b.to_f64()`

### 变量声明

```yux
var a i32 = 10      ; 可变变量
val b i64 = 100     ; 不可变变量
cval MAX i32 = 1024 ; 常量
```

### 函数

```yux
/ 表达式函数体
fn add(a i32, b i32) i32 = a + b

/ 块函数体
fn sub(a i32, b i32) i32 {
  ret a - b
}

/ 空返回
fn early_exit(flag bool) {
  if flag {
    ret; ; 必须尾随 ;
  }
  println(123)
}
```

### 控制流

```yux
/ if 表达式
val max = if a > b { a } else { b }

/ if 语句（尾随 ;）
if x > 0 { println(x); }

/ loop 循环
loop {
  if i >= 10 { break; } ; break 必须尾随 ;
  i = i + 1
}
```

### 结构体

```yux
struct Point {
  x f64
  y f64
}

Point {
  fn ~() {
    / 析构函数（必须在第一位）
  }
  
  fn Point(x f64, y f64) {
    / 构造函数
    self.x = x
    self.y = y
  }
  
  fn add(other Point) Point {
    / 成员函数
    Point(self.x + other.x, self.y + other.y)
  }
}
```

### 关键字

不能做标识符：
- `fn`, `var`, `val`, `cval`
- `if`, `elif`, `else`
- `ret`, `break`
- `null`, `true`, `false`
- `loop`, `struct`

详情见[语法](语法.md)

## 示例

**空格规则**：关键字后必须有空格，二元运算符两边必须有空格，`()` `[]` 内部无空格，`,` 后有空格。

**不存在隐式转换，所有类型必须显式转换。**

详情见[语法](语法.md)

只有两种注释：
- 行注释，顶行或缩进reg=`^\s*/.*`
- 尾随注释，用` ;`，代码行或空行后。不能用/尾随

```yux
/ 顶行/，其它位置用为除号
fn some() {
  / 也可缩进
  println(123) ; 空格;为尾随注释，并非/
}

```yux
/ add
fn add(a i32, b i32) i32 = a + b

/ sub
fn sub(a i32, b i32) i32 {
  ret a + b ; 也可以直接 a + b 类似rust
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

  val str String = "end"
  println(str)
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

  / 成员函数
  fn some() i32 {
    123
  }

}
```
