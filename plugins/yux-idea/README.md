# Yux Language — IntelliJ Plugin

IntelliJ IDEA / JetBrains 系 IDE 的 [yux](https://github.com/yjl/yux-lang) 语言支持插件。
通过 [LSP4IJ](https://github.com/redhat-developer/lsp4ij) 接入官方 `yux-lsp` 服务，
提供诊断、补全、跳转、悬浮提示等能力，并附带原生的语法高亮与代码风格配置。

## 功能

- `.yux` 文件类型识别与图标
- 语法高亮（关键字 / 字符串 / 数字 / 注释 / 运算符等），并提供 *Settings → Editor → Color Scheme → Yux* 配色页
- 代码风格设置（缩进、空格等），位于 *Settings → Editor → Code Style → Yux*
- 通过 LSP4IJ 启动外部 `yux-lsp` 进程，提供：
  - 诊断（错误/警告下划线）
  - 代码补全
  - 定义跳转、引用查找
  - 悬浮文档
  - 语义高亮（semantic tokens）

## 依赖

- IntelliJ Platform 兼容版本见 [`build.gradle.kts`](build.gradle.kts) 与 [`gradle.properties`](gradle.properties)
- 必装插件：[LSP4IJ](https://plugins.jetbrains.com/plugin/23257-lsp4ij)
- 可执行的 `yux-lsp` 语言服务（与 `yux` 编译器同 `bin/` 目录分发）

## 配置

打开 *Settings → Languages & Frameworks → Yux*，填写 **Yux home** 即可。

期望的目录布局：

```
<yux-home>/
├── bin/
│   ├── yux(.exe)        # 编译器二进制
│   └── yux-lsp(.exe)    # 语言服务二进制
├── sdk/
│   └── yux/core/        # 自举运行时（被编译器自动链接）
└── ...
```

行为：

- 设置页会实时校验 `bin/yux(.exe)` 与 `sdk/` 是否存在
- 启动 LSP 时使用 `<home>/bin/yux-lsp(.exe)`，并把 `YUX_HOME=<home>` 注入子进程环境
- 未配置时回退到 `PATH` 中的 `yux-lsp` 命令

设置实现见
[`YuxSettings.kt`](src/main/kotlin/org/yux/lang/ide/settings/YuxSettings.kt) /
[`YuxConfigurable.kt`](src/main/kotlin/org/yux/lang/ide/settings/YuxConfigurable.kt)，
持久化到项目目录下的 `.idea/yux.xml`。

## 开发

本模块基于 [IntelliJ Platform Gradle Plugin](https://github.com/JetBrains/intellij-platform-gradle-plugin) 构建。

```bash
# 运行带插件的沙箱 IDE
./gradlew runIde

# 单元测试
./gradlew test

# 校验插件兼容性
./gradlew verifyPlugin

# 构建可分发的 zip
./gradlew buildPlugin
```

产物位于 `build/distributions/`。

## 目录结构

```
yux-idea/
├── build.gradle.kts                 Gradle 构建脚本
├── gradle.properties                插件 / 平台版本
├── settings.gradle.kts
├── gradle/                          Gradle Wrapper
└── src/main/
    ├── kotlin/org/yux/lang/ide/
    │   ├── YuxFileType.kt           .yux 文件类型
    │   ├── YuxLanguage.kt           Language 实例
    │   ├── YuxIcons.kt
    │   ├── lexer/                   词法器（基于 yux 语法）
    │   ├── parser/                  最小 PSI 解析器
    │   ├── psi/
    │   ├── highlighter/             语法高亮 + 配色页
    │   ├── formatter/               Code Style 设置
    │   ├── lsp/                     LSP4IJ 接入（server / client / semantic tokens）
    │   └── settings/                项目级持久化设置
    └── resources/META-INF/plugin.xml
```

## 发布

参考 JetBrains 官方 [Publishing a Plugin](https://plugins.jetbrains.com/docs/intellij/publishing-plugin.html)。
本仓库尚未上架 Marketplace，可通过 `./gradlew buildPlugin` 产出 zip 后在 IDE 中
*Settings → Plugins → ⚙ → Install Plugin from Disk…* 手动安装。

## 许可证

随主仓库 [yux-lang](../) 一同分发。
