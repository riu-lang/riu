-- yux/analyzer — 语义分析器（借用检查、常量/可变检查、流终止检查、符号建议、spec 注册表/实现校验）
-- 产出 yux_analyzer.lib（静态库），0 LLVM 依赖，仅依赖 yux_ast

target("yux_analyzer")
    set_kind("static")
    add_deps("yux_ast")

    -- yux/ 顶层：让 #include "analyzer/..." 能找到 yux/analyzer/...
    add_includedirs("..", {public = true})
    -- yux/include/：公共头文件（types.h, error_code.h）
    add_includedirs("../include", {public = true})
    -- TODO: src/ 仅因 AST 节点反向依赖 sema/（层次违例），后续重构移除
    add_includedirs(path.join(os.projectdir(), "src"))

    add_files("*.cpp")
