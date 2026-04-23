# yux 编译器开发指南

独立编译器，单程序完成全流程（解析→IR→链接→exe）。

**不要改语法文件 `src/yux.g4`**，有问题直接暂停任务，告诉用户有哪些问题，需要修改什么。

## 硬性规则

- 环境：Windows PowerShell + Clang（无 MSVC 环境变量）
- LLVM 工具链在 PATH（`llvm/bin`）
- `build/windows/x64/debug` 默认在 PATH，构建后可直接调用 `yux`
- 调试工具：Windows 兼容的 `head`、`tail`
- **遇到难以解决的问题时，写入 `BUGS.md`**：包括需要大量改动的、非本次修改引起的、需要大量排查的问题。暂停相关任务，向用户说明情况。

## 常用命令

```powershell
# 同步依赖（克隆后执行一次）
./sync-deps.ps1

# 构建编译器
xmake build yux

# 单文件模式：产物扁平放在 <源文件目录>/build/
yux main.yux              # 生成 <srcDir>/build/main.exe
yux --emit-ir input.yux   # 同时生成 .ll 文件
yux -d input.yux          # 调试 IR 输出（信息量大，配合 tail 使用）

# 项目模式：必须在项目根目录（含 yux.toml）执行
yux build <name>          # <name> 必须与 yux.toml 的 name 一致；入口取 toml 的 entry
                          # 产物落在 <projectRoot>/build/<name>/<name>.exe

# 运行编译结果
./build/main.exe
```

`yux.toml` 字段（详见 语法.md）：`name`（项目 / exe 名）、`entry`（入口 .yux，相对项目根）、`version`。单文件模式完全忽略 yux.toml，不引入项目名子目录。

**测试流程：**
1. **简易测试**：`yux main.yux && ./build/main.exe`（或创建新 yux 文件）快速验证
2. **完整测试**：简易测试通过后，`xmake test` 验证所有用例

测试运行器（`tests/xmake.lua`）以**单文件模式**调用 `yux` 编译每个 `tests/cases/*.yux`，比较 stdout 与配对的 `.expected`；`error/err_*.yux` 期望编译失败。每个用例的产物放在 `tests/cases/build/<stem>.exe`（错误用例在 `tests/cases/error/build/`）。项目级测试将来单独写一套 harness。当测试用例与语言规范冲突时，更新测试用例——`src/yux.g4` 和 `语法.md` 是权威规范。

## 架构

编译器单二进制（`yux.exe`）完成全流程，无外部汇编器/链接器：

```
.yux → ANTLR4 Lexer/Parser → ASTBuilder → 语义分析
     → Compiler (LLVM IR) → LLVM codegen → LLD link → .exe
```

**源码边界（`src/`）：**
- `main.cpp` — CLI 入口、参数解析
- `yux.cpp/h` — 编译器主类，编排流水线
- `ast_builder.cpp/h` — ANTLR 解析树 → AST 节点（`src/node/` 下 `expr_node`, `fn_node`, `struct_node`, `statement_node`）
- `compiler.cpp/h` — AST → LLVM IR
- `build_cache.cpp/h` — 源文件 mtime+size 缓存，存储于 `build/build.cache`；时间戳和大小都匹配时跳过编译
- ANTLR 生成代码在 `gen/`（非 `src/`）

`sdk/` 是自举运行时（yux 自身编写），编译为 `build/sdk.ll` / `build/sdk.obj`，链接到每个 yux 程序。

## 构建输出布局

`build/` **由 xmake 和 yux 编译器共用**：

- xmake 输出：`build/windows/x64/debug/` 及点开头目录（`.objs/`, `.deps/`, `.build_cache/` 等）
- yux 单文件模式：`<srcDir>/build/*.exe`, `*.ll`, `*.obj`, `*.obj.cache`；多段模块 `A.B.C` 展开为 `<buildDir>/A/B/C.obj`
- yux 项目模式：主模块 + 单段导入落在 `<projectRoot>/build/<projectName>/`；多段模块仍按点分路径展开

**清理：**
- 安全清理：删除 `build/*.exe build/*.ll build/*.obj build/*.obj.cache`
- 完全清理：`xmake clean -a`
- **不要直接删除整个 `build/` 目录**

## 语言语法（影响代码生成和测试）

**注释：**
- 行注释：`^\s*/.*`（顶行或缩进的 `/` 开头）
- 尾随注释：` ;`（代码行或空行后）
- 尾随 `/` 不是注释

**强制尾随 `;` 规则：**
- `ret;` 用于 void 函数提前返回，必须尾随 `;`
- `break;` 用于 loop 循环跳出，必须尾随 `;`
- 表达式尾随 `;` 表示空返回，否则返回表达式值

**空格：**
- 关键字后必须有空格
- 二元运算符两边必须有空格
- `()` `[]` 内部无空格
- `,` 后有空格

**类型：**
- 无隐式转换，使用 `.to_<类型>()` 方法
- 无后缀整数字面量默认 `i32`，但可按上下文推断：二元运算另一侧类型、显式类型声明、函数返回类型、唯一匹配的重载参数。多个重载均可匹配时报错，需用类型后缀消歧（如 `2u8`）

**关键字：** `fn var val cval if elif else ret break null true false loop struct`

**内置泛型：** `Ref<T>`, `Box<T>`（引用计数）, `Ptr<T>`, `Array<T>`

**字符串字面量：**
- 普通字符串 `"..."` 支持转义：`\n \t \\ \" \'`
- 原始字符串 `r"..."` 不处理转义，内容原样保留

**代码点字面量：**
- 语法 `c'X'`，类型 `u32`，表示单个 Unicode 代码点
- 引号内仅允许一个字符或一个转义序列
- 支持转义：`\n \r \t \v \b \0 \\ \'`
- **不支持** `\xNN` / `\uNNNN` 数值转义

详细语法见 [语法.md](语法.md)
