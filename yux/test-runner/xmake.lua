-- yux/test-runner — 独立测试运行器（yux-test-runner 可执行文件）
-- 加载单个 *.test.dll，顺序执行测试并打印结果。由 yux test 多子进程并行调用。
-- 0 依赖，纯 Windows API + 标准库。

target("yux-test-runner")
    set_kind("binary")
    add_files("runner_main.cpp")
