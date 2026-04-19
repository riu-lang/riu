# yux-lang

一个独立的编译器，类似于 clang，一个程序能完成所有编译流程。

## 项目简介

yux 是一门自举的编程语言，使用 ANTLR4 解析语法，LLVM 作为编译后端。编译器将 `.yux` 源文件编译为可执行文件。

### 语言特性

- 静态类型系统，无隐式类型转换
- 结构体和成员函数
- 泛型类型（Ref<T>, Box<T>, Ptr<T>, Array<T>）
- 自动内存管理（Box<T> 引用计数）
- 与 C/Windows API 互操作

### 示例代码

```yux
/ 注释，顶行/，可加空格缩进

fn add(a i32, b i32) i32 = a + b

fn main() {
  val a = add(1, 2)
  println(a)
  
  / if 表达式
  val max = if a > 0 { a } else { -a }
  println(max)
}
```

详细语法说明请参考 [语法.md](语法.md)。

## 开发

### 环境要求

- **编译器**: Clang
- **构建工具**: xmake
- **系统**: Windows
- `build/windows/x64/debug`的绝对路径添加到`PATH`，以便调用

### 依赖

项目依赖以下库（位于 `third_party/` 目录）：

| 依赖 | 说明 |
|------|------|
| LLVM | 编译器后端基础设施 |
| ANTLR4 | 语法解析器生成器 |
| utfcpp | UTF-8 编码处理 |
| zlib | 压缩库 |
| googletest | 测试框架 |

### 目录结构

```
yux-lang/
├── src/                  # 源代码
│   ├── yux.g4            # 语法文件
│   ├── main.cpp          # 程序入口
│   ├── compiler.*        # LLVM IR 生成
│   ├── ast_builder.*     # AST 构建
│   └── node/             # AST 节点定义
├── include/              # 公共头文件
├── sdk/                  # 自举运行时库
├── gen/                  # ANTLR4 生成的代码
├── tests/                # 测试用例
├── third_party/          # 外部依赖
├── build/                # 编译输出
└── yux-vscode/           # VSCode 扩展
```

详细的项目结构说明请参考 [AGENTS.md](AGENTS.md)。

### 同步依赖

```powershell
./sync-deps.ps1
```

## 构建

```powershell
# 构建 yux 编译器
xmake build yux
```

## 使用

```powershell
yux <input.yux>
```

### 命令行参数

| 参数 | 说明 |
|------|------|
| `input` | 输入的 .yux 文件（必需） |
| `--emit-ir` | 输出 LLVM IR 到 .ll 文件 |
| `-d, --debug` | 输出编译 IR 调试信息（仅 Debug 构建） |

编译成功后，在工作目录输出 `文件名.exe`。

## 测试

测试用例位于 `tests/cases/` 目录，使用 googletest 框架。

```powershell
xmake build yux_test
./build/windows/x64/debug/yux_test.exe
```

## License

Copyright (c) 2025. Yin-Jinlong@github
