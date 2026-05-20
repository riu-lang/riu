# 行为约束

适用于 Claude Code / Trae 等 agent 在本仓库工作时。

## 不要改语法文件

`src/yux*.g4` **只读**。如果任务看起来需要改语法，立刻暂停，列出遇到的问题与可能的修改方向，交给用户决定。**不要"先改一点试试"**。

## 多步任务先落到 `CURRENT.md`

接到多步骤任务时，先在 `CURRENT.md` 写入分阶段计划（格式参照该文件现有条目），每完成一个阶段就地更新；整个任务完成后删除该条目。单步小修不需要写。

详细模板与归档规则见 [tasks-and-bugs.md](tasks-and-bugs.md)。

## 新发现的 bug 写入 `BUGS.md`

指的是**与当前任务无关**、或需大量排查、或临时绕过的 bug。按文件里的模板填写，然后暂停相关任务并告知用户。

**进度 → `CURRENT.md`，bug → `BUGS.md`，两者不混用**。

## 信息不足时先查证，不要编造

涉及到具体的测试名、目录路径、文件内容、命令参数时，用 Read/Grep/Glob 查实际文件，不要凭命名推断。

当 docs、`yux.g4`、编译器三者冲突时，以 `src/yux*.g4` 和编译器源码为准，随后更新 docs，**不要反过来**。

## 写 yux 代码前先看速查 + 按顺序读文档

写 `*.yux` 时先翻 [yux-syntax.md](yux-syntax.md)（简版速查，列易踩坑点），再按需要往下查：

1. `docs/*.md`（中文教程）
2. `src/yux*.g4`（权威语法）
3. `src/*.cpp`（编译器实现，最后查）

**不要拿 Rust / C++ / Go 的语义去套 yux**。

## 改完 C++ 必须 lint + format，提交时 0 警告

仓库根有两个**本地包装器**（不在 PATH，需带 `./` 前缀）；都是 `init.js` 生成、各平台一份（`.cmd` / `.sh` / `.ps1`）。**改完 C++ 别手敲 `xmake check clang.tidy ...`，跑包装器即可**：

```powershell
./format.cmd            ; 仅排 git 已变动 / 未跟踪 C++ 文件的 #include 块
./lint.cmd              ; 仅 lint git 已变动 / 未跟踪文件
./format.cmd --all      ; 全仓
./lint.cmd --all        ; 三个 target 全量
./format.cmd src/x.cpp  ; 指定文件
./lint.cmd src/x.cpp    ; 指定文件
./format.cmd --check    ; 只检查不改, 有差异退出码 1
```

包装器内部仍走 `xmake check clang.tidy` + JS 包内 #include 排序器；默认增量便于 agent 在每次改完后无脑跑。

**提交门槛**：`./lint.cmd` 输出必须 `0 warnings`。**唯一例外**：一次清理任务需要拆成多个提交按主题逐批落地时，中间提交可以保留尚未处理的剩余警告，但主题工作完成后的**收尾提交必须归零**。

例外不适用于功能 / bug fix 提交。这类提交本身就不该引入新警告，撞到非自身代码的旧警告时，与用户对齐后再决定单独清还是顺手带。

#include 块以"连续 `#include` 行"为单位，空行 / 注释 / 其他行打断块；排序仅在块内进行，不会破坏 `main.cpp` 的 `windows.h → #undef ERROR → types.h` 形态。新加 include 时直接放进对应块尾，下次 `./format.cmd` 会就地排好。

> 已知尚未处理：未使用 include 清理（clangd `unused-includes` / IWYU），后续单独排专项再开。

## 改语言面必须回写规范

凡是新增 / 修改 / 删除语言特性、语法形态、用法语义、ABI 协议、内置类型行为等"涉及标准"的变更，落地前**先与用户确认条款措辞**，确认后同步更新 `docs/spec/` 与 CHANGELOG。详见 [spec-writeback.md](spec-writeback.md)。
