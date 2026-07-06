# yux-test-runner 手册

测试运行器。0 依赖，叶子节点 exe。**由 `yux test` 内部 spawn**，用户不直接调。

## 工作流程

1. `yux test` → 先 `yux build --test`（编译 `*.test.yux` → DLL）
2. 并行 spawn N 个 `yux-test-runner` 子进程（N = `--threads` 或 CPU 核数）
3. 每子进程加载一个 DLL，顺序执行其中 `#Test fn`
4. SEH（Structured Exception Handling）包裹每次测试调用

## 输出

- 每个 DLL 末尾必有一行摘要：`<dll>: X passed, Y failed, [T]`
- 失败的测试：输出测试名 + 文件名 + 行号 + 断言表达式 + 实际值

## 崩溃排查

**现象**：某 DLL 末尾没有摘要行

**步骤**：
1. `yux test --verbose` → 确认哪个 DLL 挂掉
2. `yux test --test-mod <模块名>` → 隔离到单个 DLL
3. `yux build --test -d` → 看编译端是否有异常
4. 如果 DLL 本身能加载但特定测试崩溃，手动在测试里加 `println` 定位

## 进程退出码

| 退出码 | 含义 |
|--------|------|
| 0 | 所有测试通过 |
| 1 | 至少一个测试失败 |
| 非 0/1 | 进程崩溃/异常终止 |

测试断言失败不会让进程崩溃——`assert_eq` / `assert_true` 失败计入失败数，继续跑下一个测试。
