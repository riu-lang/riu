# yux-vscode

yux 语言的 Visual Studio Code 扩展，提供语法高亮和代码补全支持。

## 功能

- **语法高亮**: 支持 yux 语言的关键字、类型、字符串、数字、注释等语法高亮
- **代码补全**: 提供关键字和基本类型的智能补全

### 支持的关键字

| 关键字 | 说明 |
|--------|------|
| `fn` | 函数声明 |
| `ret` | 返回语句 |
| `if` / `elif` / `else` | 条件语句 |
| `true` / `false` / `null` | 布尔值和空值 |
| `var` / `val` / `cval` | 变量声明 |

### 支持的基本类型

| 类型 | 说明 |
|------|------|
| `i8` / `i16` / `i32` / `i64` | 有符号整数 |
| `u8` / `u16` / `u32` / `u64` | 无符号整数 |
| `f32` / `f64` | 浮点数 |

## 安装

1. 下载 `.vsix` 文件
2. 在 VS Code 中按 `Ctrl+Shift+P`，输入 `Install from VSIX`
3. 选择下载的文件安装

## 开发

```bash
# 安装依赖
pnpm install

# 编译
pnpm run compile

# 打包
pnpm run build
```

按 `F5` 启动调试，会打开一个新的 VS Code 窗口用于测试扩展。

## License

MIT
