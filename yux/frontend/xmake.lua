-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux/frontend — 前端静态库（yux_frontend.lib）
-- 0 LLVM 依赖：词法/语法（ANTLR）+ AST + 语义分析 + 格式化 + 诊断 + SDK 加载
-- yux-lsp / yux-check / yux-ast / yux 四个二进制共用此 lib

target("yux_frontend")
    set_kind("static")
    add_deps("yux_ast", "yux_analyzer")

    -- 暴露 sema/ 和 tools/ 子目录给消费者（#include "sema/..." / "tools/..."）
    add_includedirs(".", {public = true})
    add_includedirs(path.join(os.projectdir(), "third_party", "utfcpp/source"), {public = true})
    add_includedirs(path.join(os.projectdir(), "third_party", "toml11/single_include"), {public = true})
    add_includedirs(path.join(os.projectdir(), "third_party", "nlohmann_json/single_include"), {public = true})

    add_files("sema/*.cpp")
    add_files("tools/*.cpp")
    add_files("tools/format/*.cpp")

    add_defines("UNICODE", "NOMINMAX", {public = true})
