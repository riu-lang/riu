# yux 文档

欢迎来到 yux 编程语言文档。yux 是一门自举的静态类型编程语言，使用 LLVM 作为编译后端，专注于简洁、安全和性能。

## 概述

yux 是一门独立的编译型语言，具有以下特性：

- **静态类型系统**：无隐式类型转换，所有类型转换必须显式进行
- **自举运行时**：SDK 使用 yux 语言自身编写，提供核心功能
- **自动内存管理**：`Box<T>` 类型使用引用计数自动管理堆内存
- **泛型支持**：内置 `Ref<T>`、`Box<T>`、`Ptr<T>`、`Array<T>` 等泛型类型
- **C/系统互操作**：通过 `extern` 声明调用外部函数，支持 Windows API

空格和换行为语法的一部分，不可省略。

## 文档目录

### 入门指南

- [安装指南](安装指南.md) - 环境配置和安装步骤
- [基础语法](基础语法.md) - 注释、变量、表达式等基础语法

### 语言参考

- [类型系统](类型系统.md) - 基本类型、泛型类型、类型转换
- [内置类型](内置类型.md) - 内置泛型类型详细说明
- [函数](函数.md) - 函数定义、泛型函数、参数传递
- [结构体](结构体.md) - 结构体定义、成员函数、析构函数
- [枚举与匹配](枚举与匹配.md) - `enum` 声明、构造、`match` 模式匹配
- [控制流](控制流.md) - if/else、loop 循环
- [模块系统](模块系统.md) - 模块导入、项目配置
- [构建注解](构建注解.md) - `#CompilerInner` 等编译期注解

### 语言规范（草案）

- [docs/spec/](spec/index.md) - 规范级文档，关心"语义如何被定义"，与教程并列
- [docs/spec/CHANGELOG.md](spec/CHANGELOG.md) - 规范变更记录（上新下旧）

### 更多资源

- [语法文件 yux.g4](../src/yux.g4) - 权威的 ANTLR4 语法定义
- [AGENTS.md](../AGENTS.md) - 编译器开发指南
- [README.md](../README.md) - 项目概述

## 快速开始

### Hello World

创建项目目录结构：

```
myproject/
├── yux.toml
└── main.yux
```

**yux.toml**：

```toml
name="hello"
version="1.0.0"
entry="main.yux"
```

**main.yux**：

```yux
fn main() {
  println("Hello, World!")
}
```

编译并运行：

```powershell
cd myproject
yux build hello
./build/hello/hello.exe
```

## 示例代码

### 基本运算

```yux
fn main() {
  val a = 10 + 5
  val b = a * 2
  val c = b.to_f64() / 3.0
  
  println(c.to_string())
}
```

### 结构体与引用

```yux
struct Counter {
  value i32
}

Counter {
  fn Counter(initial i32) {
    $.value = initial
  }
  
  fn increment() {
    $.value = $.value + 1
  }
}

fn main() {
  var c = Counter(0)
  val ref = &c
  ref.increment()
  println(c.value.to_string())
}
```

### 堆对象

```yux
struct Data {
  value i32
}

fn main() {
  var box Box<Data> = Data(42)
  println(box.value.to_string())
  
  var box2 Box<Data> = box  ; 引用计数 +1
} ; 离开作用域时自动释放
```

### 动态数组

```yux
fn main() {
  var arr Array<i32> = [1, 2, 3, 4, 5]
  arr[0] = 10
  
  var i i32 = 0
  loop {
    if i >= arr._len().to_i32() {
      break;
    }
    println(arr[i].to_string())
    i += 1
  }
}
```

## License

Copyright (c) 2025. Yin-Jinlong@github
