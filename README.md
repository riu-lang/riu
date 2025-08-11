# yux-lang

一个独立的编译器，类似于 clang，一个程序能完成所有编译流程。

## 项目简介

yux 是一门简单的编程语言，使用 ANTLR4 解析语法，LLVM 作为编译后端。编译器将 `.yux` 源文件编译为可执行文件。

### 语言特性

- 简洁的语法设计
- 静态类型系统
- 支持函数定义、变量声明、条件表达式等
- 支持多种整数和浮点数类型（i8, i16, i32, i64, u8, u16, u32, u64, f32, f64）

### 示例代码

```yux
/ 注释，顶行/，可加空格缩进

fn p(a i32) {
  println(a + 1)
}

fn main() {
  val a = 1.5 + 2.to_f64()
  println(a)
  p(123)
}
```

## 开发

### 环境要求

- **编译器**: Clang（仅支持 Clang）
- **构建工具**: CMake + Ninja
- **系统**: Windows

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
├── src/                # 源代码
│   ├── yux.g4          # 语法文件
│   ├── main.cpp        # 程序入口
│   ├── compiler.*      # 编译器核心
│   ├── ast_builder.*   # AST 构建器
│   └── node/           # AST 节点定义
├── gen/                # ANTLR4 生成的代码
├── rt/                 # 运行时静态库
├── include/            # 公共头文件
├── libs/               # 依赖库 CMake 配置
├── tests/              # 测试用例
├── third_party/        # 外部依赖
└── yux-vscode/         # VSCode 扩展
```

### 同步依赖

```powershell
./sync-deps.ps1
```

## 构建

使用 Ninja 构建指定目标：

```powershell
# 构建 yux 编译器
ninja -C cmake-build-debug yux

# 构建运行时库
ninja -C cmake-build-debug yux_rt
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
ninja -C cmake-build-debug yux_test
./cmake-build-debug/yux_test.exe
```

## License

Copyright (c) 2025. Yin-Jinlong@github
