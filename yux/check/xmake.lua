-- yux/check — 快速语义检查工具（yux-check 可执行文件）
-- 0 LLVM 依赖，仅链 yux_frontend。用法: yux-check <file> | yux-check test <dir>

target("yux-check")
    set_kind("binary")
    add_deps("yux_frontend")
    add_includedirs(path.join(os.projectdir(), "third_party", "cli11/include"))
    add_files("check_main.cpp")
    set_rundir("$(projectdir)")
