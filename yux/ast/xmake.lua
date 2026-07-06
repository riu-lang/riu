-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux/ast — ANTLR4 运行时 + AST 构建器 + parse tree 转储工具
-- 产出 antlr4_static.lib（ANTLR4 C++ 运行时）、yux_ast.lib（AST 构建器）、yux-ast（parse tree 转储）

target("antlr4_static")
    set_kind("static")
    set_languages("c++17")
    add_defines("ANTLR4CPP_STATIC", {public = true})
    add_includedirs(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src"), {public = true})
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/atn/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/dfa/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/misc/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/internal/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/support/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/tree/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/tree/pattern/*.cpp"))
    add_files(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src/tree/xpath/*.cpp"))
    add_cxxflags("-include chrono", {force = true})
    if is_plat("windows") then
        add_syslinks("shlwapi")
    end

target("yux_ast")
    set_kind("static")
    add_deps("antlr4_static")

    -- yux/ 顶层：让 #include "ast/..." 能找到 yux/ast/...
    add_includedirs("..", {public = true})
    -- yux/include/：公共头文件（types.h 等）
    add_includedirs("../include", {public = true})
    -- yux/frontend/：ast_builder_decl.cpp 使用 ConstEvaluator（构建期常量求值），
    -- expr_node.cpp 使用 sema::parseIntLiteral。AST 节点头文件已不再依赖 sema/。
    add_includedirs(path.join(os.projectdir(), "yux/frontend"))
    -- ANTLR 生成代码：让 #include "yux/..." 能找到 yux/ast/gen/yux/...
    add_includedirs("gen", {public = true})
    -- toml11（yux.cpp 项目配置解析需要）
    add_includedirs(path.join(os.projectdir(), "third_party", "toml11/single_include"), {public = true})

    add_files("ast_builder.cpp")
    add_files("ast_builder_decl.cpp")
    add_files("ast_builder_struct.cpp")
    add_files("ast_builder_fn.cpp")
    add_files("ast_builder_stmt.cpp")
    add_files("ast_builder_expr.cpp")
    add_files("ast_builder_type.cpp")
    add_files("yux.cpp")
    add_files("mangler.cpp")
    add_files("node/*.cpp")
    add_files("gen/yux/*.cpp")

-- yux-ast: 独立的 parse tree 转储工具，仅 ANTLR 词法 + 语法，不做 AST/语义/codegen
target("yux-ast")
    set_kind("binary")
    add_deps("yux_frontend")
    add_defines("YUX_VERSION=\"" .. _YUX_VERSION_STR .. "\"")
    add_includedirs(path.join(os.projectdir(), "third_party", "cli11/include"))
    add_files("ast_main.cpp")
    set_rundir("$(projectdir)")
