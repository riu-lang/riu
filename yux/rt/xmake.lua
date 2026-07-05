-- yuxrt — yux 运行时库 xmake 构建
-- 产出 yuxrt.lib（静态库），C99，无 CRT 依赖。
-- 项目使用 clang 工具链（set_toolchains("clang")），此处不设 MSVC 风格 flag。

target("yuxrt")
    set_kind("static")
    set_languages("c99")

    -- 无 CRT 依赖：禁用 Clang 在 MSVC target 下自动注入 /DEFAULTLIB 指令
    add_cxflags("-Xclang", "-fno-autolink", {force = true})

    add_includedirs(".", {public = true})

    add_files("port/win32.c")
    add_files("mem/alloc.c")
    add_files("mem/ops.c")
    add_files("math/*.c")
