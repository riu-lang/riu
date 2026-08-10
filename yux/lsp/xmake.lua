-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux/lsp — LSP 服务器（yux-lsp 可执行文件）
-- 叶子节点，直接编成二进制，不做静态库

target("yux-lsp")
    set_kind("binary")
    add_deps("yux_frontend")
    add_defines("YUX_VERSION=\"" .. _YUX_VERSION_STR .. "\"")

    -- yux/ 顶层：让 #include "lsp/..." 能找到 yux/lsp/...
    add_includedirs("..", {public = true})

    add_files("*.cpp")
    set_rundir("$(projectdir)")
