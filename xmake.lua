-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- 版本号
-- 正在开发的v...-alpha
-- 已完成v...

set_project("yux-lang")
local yux_version = "0.18.0-alpha"
set_version(yux_version)
set_languages("c++23")

-- 生成版本字符串供各 target 通过 add_defines 注入（格式 v0.18-2026-07-06）
-- 从 set_version 提取 major.minor，拼接当前日期
do
    local major_minor = yux_version or "0.0"
    local date = os.date("%Y-%m-%d")
    _YUX_VERSION_STR = "v" .. major_minor .. "-" .. date
end

add_rules("mode.debug", "mode.release")

set_toolchains("clang")

-- 输出目录：build/<plat>/<arch>/<profile>/{bin,lib}
-- 编译期 SDK symlink 在 xmake 内维护（不再由 init.js 手写）
set_targetdir("$(builddir)/$(plat)/$(arch)/$(mode)", { bindir = "bin", libdir = "lib" })

add_cxxflags("-Wno-language-extension-token", {force = true})

includes("tests")
includes("yux/rt")
includes("yux/ast")
includes("yux/analyzer")
includes("yux/frontend")
includes("yux/check")
includes("yux/lsp")
includes("yux/yux")
includes("yux/test-runner")
includes("@builtin/xpack")

local third_party_licenses = {
    {lib = "antlr4", license = "LICENSE.txt"},
    {lib = "cli11", license = "LICENSE"},
    {lib = "llvm", license = "LICENSE.TXT", subdir = "llvm"},
    {lib = "utfcpp", license = "LICENSE"},
    {lib = "zlib", license = "LICENSE"},
    {lib = "toml11", license = "LICENSE"},
    {lib = "nlohmann_json", license = "LICENSE.MIT"},
}

xpack("yux")
    set_formats("zip")
    set_basename("yux-$(version)")

    add_targets("yux", "yux-lsp", "yux-ast", "yux-check", "yux-test-runner", "yuxrt")
    add_installfiles("sdk/(**)", {prefixdir = "sdk"})
    add_installfiles("__remove_sdk/(**/build/**)")
    add_installfiles("__remove_sdk/(**/.yux/**)")
    add_installfiles("examples/(**)", {prefixdir = "examples"})
    add_installfiles("README.md", {prefixdir = "."})
    add_installfiles("LICENSE.txt", {prefixdir = "."})
    add_installfiles("docs/(**)", {prefixdir = "docs"})
    for _, t in ipairs(third_party_licenses) do
        local lib_path = t.subdir and path.join("third_party", t.lib, t.subdir, t.license)
                         or path.join("third_party", t.lib, t.license)
        add_installfiles(lib_path, {prefixdir = path.join("shared/licenses", t.lib), filename = t.license})
    end
