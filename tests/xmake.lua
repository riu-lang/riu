-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux 语言测试：使用 xmake 原生测试机制（xmake test）
--
-- 用例分布（截至 v0.18）：
--   - tests/projects/<case>/              项目模式用例；包含 yux.toml + 入口源文件 + expected.txt
--   - tests/projects/<case>/              格式化用例；包含 yux.toml + expected_format
--                                         `yux format <entry>` 的 stdout 需与 expected_format 完全一致（CRLF 归一）
--   - tests/check-cases/diag_*.yux        诊断回归用例（; check: EXXXX 注解），由 `yux-check test` 运行
--   - sdk/yux/src/yux/core/*.test.yux     纯逻辑 + 行为用例（#Test 注解），由 `yux test` 运行
--
-- 运行：
--   xmake build yux                         先构建编译器
--   xmake test                              运行全部用例
--   xmake test -v                           详细日志（失败时打印 stdout / stderr / errors）
--   xmake test yux_tests/<name>             单独运行（<name> = project_<dirname>）
--   xmake test "yux_tests/*"                通配符
--   xmake test -g yux/<cat>                 只跑某一分类（见下方 categorize 函数）
--
-- 分类：
--   yux/project   编译+运行项目用例        tests/projects/* (含 expected.txt)
--   yux/format    格式化用例              tests/projects/* (含 expected_format)

local projects_dir = path.join(os.scriptdir(), "projects")

-- 用例名 → 分组名。name 形如 "project_<dir>"。
local function categorize(name)
    if not name:startswith("project_") then return "yux/misc" end
    local dirname = name:sub(9) -- strip "project_" prefix
    local d = path.join(projects_dir, dirname)
    if os.isfile(path.join(d, "expected_format")) then return "yux/format" end
    if os.isfile(path.join(d, "expected.txt")) then return "yux/project" end
    return "yux/misc"
end

local function list_case_names()
    local r = {}
    for _, d in ipairs(os.dirs(path.join(projects_dir, "*"))) do
        if not os.isfile(path.join(d, "yux.toml")) then goto continue end
        if os.isfile(path.join(d, "expected.txt")) or os.isfile(path.join(d, "expected_format")) then
            r["project_" .. path.filename(d)] = true
        end
        ::continue::
    end
    return r
end

target("yux_tests")
    set_kind("phony")
    set_default(false)
    add_deps("yux")

    for name, _ in pairs(list_case_names()) do
        add_tests(name, {group = categorize(name)})
    end

    on_test(function (target, opt)
        -- 在 sandbox 里重新扫描用例（不复用 description-scope 的闭包变量）
        local pd = path.join(target:scriptdir(), "projects")
        local cases = {}
        for _, d in ipairs(os.dirs(path.join(pd, "*"))) do
            if not os.isfile(path.join(d, "yux.toml")) then goto continue end
            local dirname = path.filename(d)
            local entry = {
                project_dir = path.absolute(d),
                project_name = dirname,
            }
            if os.isfile(path.join(d, "expected.txt")) then
                entry.expected_file = path.absolute(path.join(d, "expected.txt"))
                entry.is_project = true
            elseif os.isfile(path.join(d, "expected_format")) then
                entry.expected_format_file = path.absolute(path.join(d, "expected_format"))
                entry.is_format = true
            end
            cases["project_" .. dirname] = entry
            ::continue::
        end

        local short = opt.name:match("/(.+)$") or opt.name
        local entry = cases[short]
        if not entry then
            opt.errors = "unknown test: " .. tostring(opt.name)
            return false
        end

        local yux_dep = target:dep("yux")
        local yux_exe = yux_dep and yux_dep:targetfile() or nil
        if not yux_exe or not os.isfile(path.absolute(yux_exe)) then
            yux_exe = "yux"
        end

        -- ==== 项目编译+运行测试 ====
        if entry.is_project then
            local pdir = entry.project_dir
            local pname = entry.project_name
            local build_dir = path.join(pdir, "build")
            local exe = path.join(build_dir, pname .. ".exe")
            os.tryrm(build_dir)

            local stdout_data, stderr_data
            local ok = try {
                function ()
                    stdout_data, stderr_data = os.iorunv(yux_exe, {"build", pname}, {curdir = pdir})
                    return true
                end,
                catch {
                    function (errs)
                        stderr_data = tostring(errs)
                        return nil
                    end
                }
            }
            opt.stdout = stdout_data
            opt.stderr = stderr_data
            if not ok or not os.isfile(exe) then
                opt.errors = "project compile failed: " .. pdir .. "\n" .. tostring(stderr_data or "")
                return false
            end

            local actual = ""
            local run_ok = try {
                function ()
                    actual = os.iorunv(exe, {}, {curdir = pdir})
                    return true
                end,
                catch {
                    function (errs)
                        opt.errors = "run failed: " .. exe .. "\n" .. tostring(errs)
                        return nil
                    end
                }
            }
            if not run_ok then
                return false
            end

            local expected = io.readfile(entry.expected_file) or ""
            if actual ~= expected then
                opt.errors = format("output mismatch for project %s\n--- expected ---\n%s\n--- actual ---\n%s",
                                    pname, expected, actual)
                opt.stdout = actual
                return false
            end
            os.tryrm(build_dir)
            return true
        end

        -- ==== 格式化测试 ====
        if entry.is_format then
            -- 从 yux.toml 读取 entry 字段确定待格式化的文件
            local toml_content = io.readfile(path.join(entry.project_dir, "yux.toml")) or ""
            local entry_file = toml_content:match('entry%s*=%s*"([^"]+)"')
            if not entry_file then
                opt.errors = "yux.toml missing entry in " .. entry.project_dir
                return false
            end
            local src_file = path.absolute(path.join(entry.project_dir, "src", entry_file))
            if not os.isfile(src_file) then
                opt.errors = "source file not found: " .. src_file
                return false
            end

            local stdout_data, stderr_data
            local ok = try {
                function ()
                    stdout_data, stderr_data = os.iorunv(yux_exe, {"format", src_file})
                    return true
                end,
                catch {
                    function (errs)
                        stderr_data = tostring(errs)
                        return nil
                    end
                }
            }
            opt.stdout = stdout_data
            opt.stderr = stderr_data
            if not ok then
                opt.errors = "format invocation failed: " .. src_file .. "\n" .. tostring(stderr_data or "")
                return false
            end

            local expected = io.readfile(entry.expected_format_file) or ""
            -- CRLF 归一：Windows 的 yux 进程 stdout 可能带 \r；expected 文件也可能含 \r
            local function strip_cr(s) return (s:gsub("\r", "")) end
            local actual_n = strip_cr(stdout_data or "")
            local expected_n = strip_cr(expected)
            if actual_n ~= expected_n then
                opt.errors = format("format output mismatch for %s\n--- expected ---\n%s\n--- actual ---\n%s",
                                    entry.project_name, expected_n, actual_n)
                return false
            end
            return true
        end

        opt.errors = "test entry has no recognized type: " .. entry.project_name
        return false
    end)
