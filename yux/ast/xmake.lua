-- yux/ast — AST 构建器 + ANTLR 生成代码  xmake 构建
-- 产出 yux_ast.lib（静态库），依赖 antlr4_static

target("yux_ast")
    set_kind("static")
    add_deps("antlr4_static")

    -- yux/ 顶层：让 #include "ast/..." 能找到 yux/ast/...
    add_includedirs("..", {public = true})
    -- yux/include/：公共头文件（types.h 等）
    add_includedirs("../include", {public = true})
    -- TODO: src/ 仅因 AST 节点反向依赖 sema/（层次违例），后续重构移除
    add_includedirs(path.join(os.projectdir(), "src"))
    -- ANTLR 生成代码：让 #include "yux/..." 能找到 yux/ast/gen/yux/...
    add_includedirs("gen", {public = true})
    -- ANTLR 运行时头文件
    add_includedirs(path.join(os.projectdir(), "third_party", "antlr4/runtime/Cpp/runtime/src"), {public = true})
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

    add_defines("ANTLR4CPP_STATIC", {public = true})
