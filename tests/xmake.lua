-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux 语言测试：使用 xmake 原生测试机制（xmake test）
--
-- 用例分布（截至 v0.16）：
--   - tests/cases/format_*.yux        格式化用例；配对 *.expected_format，
--                                    `yux format <case>` 的 stdout 需与 expected 完全一致（CRLF 归一）
--   - tests/cases/extern_ptr_*.yux / ptr_*.yux  extern fn 边界用例；配对 *.expected，
--                                    编译+运行 stdout 需与 expected 完全一致
--   - tests/projects/<case>/          项目模式用例；包含 yux.toml + 入口源文件 + expected.txt
--   - tests/check-cases/diag_*.yux   诊断回归用例（; check: EXXXX 注解），由 `yux-check test` 运行
--   - sdk/yux/src/yux/core/*.test.yux  纯逻辑 + 行为用例（#Test 注解），由 `yux test` 运行
--
-- 运行：
--   xmake build yux                         先构建编译器
--   xmake test                              运行全部用例
--   xmake test -v                           详细日志（失败时打印 stdout / stderr / errors）
--   xmake test yux_tests/<name>             单独运行（<name> 为用例文件基名）
--   xmake test "yux_tests/*"                通配符
--   xmake test -g yux/<cat>                 只跑某一分类（见下方 categorize 函数）
--
-- 分类：
--   yux/project   项目模式用例            tests/projects/* (前缀 project_)
--   yux/format    格式化用例              format_*
--   yux/extern    extern fn 边界          extern_*、ptr_*

local cases_dir = path.join(os.scriptdir(), "cases")
local projects_dir = path.join(os.scriptdir(), "projects")

-- 用例名 → 分组名。name 为不带 .yux 的基名；项目模式用例形如 "project_<dir>"。
local function categorize(name)
    if name:startswith("project_") then return "yux/project" end
    if name:startswith("format_")  then return "yux/format"  end
    if name:startswith("ptr_") or name == "extern_ptr_auto" then
        return "yux/extern"
    end
    return "yux/misc"
end

local function list_case_names()
    local r = {}
    for _, f in ipairs(os.files(path.join(cases_dir, "*.yux"))) do
        if os.isfile((f:gsub("%.yux$", ".expected"))) then
            r[path.basename(f)] = true
        elseif os.isfile((f:gsub("%.yux$", ".expected_format"))) then
            r[path.basename(f)] = true
        end
    end
    for _, d in ipairs(os.dirs(path.join(projects_dir, "*"))) do
        if os.isfile(path.join(d, "yux.toml")) and os.isfile(path.join(d, "expected.txt")) then
            r["project_" .. path.filename(d)] = true
        end
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
        local cd = path.join(target:scriptdir(), "cases")
        local cases = {}
        for _, f in ipairs(os.files(path.join(cd, "*.yux"))) do
            local exp = f:gsub("%.yux$", ".expected")
            local exp_fmt = f:gsub("%.yux$", ".expected_format")
            if os.isfile(exp) then
                cases[path.basename(f)] = {file = path.absolute(f)}
            elseif os.isfile(exp_fmt) then
                cases[path.basename(f)] = {
                    file = path.absolute(f),
                    expected_format_file = path.absolute(exp_fmt),
                    is_format = true,
                }
            end
        end
        local pd = path.join(target:scriptdir(), "projects")
        for _, d in ipairs(os.dirs(path.join(pd, "*"))) do
            if os.isfile(path.join(d, "yux.toml")) and os.isfile(path.join(d, "expected.txt")) then
                cases["project_" .. path.filename(d)] = {
                    project_dir = path.absolute(d),
                    project_name = path.filename(d),
                    expected_file = path.absolute(path.join(d, "expected.txt")),
                    is_project = true,
                }
            end
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

        if entry.is_format then
            -- 格式化用例：调 `yux format <case>`，stdout 与 expected_format 完全一致
            local stdout_data, stderr_data
            local ok = try {
                function ()
                    stdout_data, stderr_data = os.iorunv(yux_exe, {"format", entry.file})
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
                opt.errors = "format invocation failed: " .. entry.file .. "\n" .. tostring(stderr_data or "")
                return false
            end

            local expected = io.readfile(entry.expected_format_file) or ""
            -- CRLF 归一：Windows 的 yux 进程 stdout 可能带 \r；expected 文件也可能含 \r
            local function strip_cr(s) return (s:gsub("\r", "")) end
            local actual_n = strip_cr(stdout_data or "")
            local expected_n = strip_cr(expected)
            if actual_n ~= expected_n then
                opt.errors = format("format output mismatch for %s\n--- expected ---\n%s\n--- actual ---\n%s",
                                    entry.file, expected_n, actual_n)
                return false
            end
            return true
        end

        -- extern/ptr 用例：编译 + 运行，stdout 与 .expected 完全一致
        local case = entry.file
        local expected_file = case:gsub("%.yux$", ".expected")
        local workdir = path.directory(case)
        local stem = path.basename(case)
        local project_root = path.directory(target:scriptdir())
        local build_root = path.join(workdir, "build")
        local exe = path.join(build_root, stem .. ".exe")
        local obj = path.join(build_root, stem .. ".obj")
        local cache = obj .. ".cache"
        os.tryrm(exe)
        os.tryrm(obj)
        os.tryrm(cache)

        local stdout_data, stderr_data
        local ok = try {
            function ()
                stdout_data, stderr_data = os.iorunv(yux_exe, {case}, {curdir = project_root})
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
            opt.errors = "compile failed: " .. case .. "\n" .. tostring(stderr_data or "")
            return false
        end

        local actual = ""
        local run_ok = try {
            function ()
                actual = os.iorunv(exe, {}, {curdir = workdir})
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

        local expected = os.isfile(expected_file) and io.readfile(expected_file) or ""
        if actual ~= expected then
            opt.errors = format("output mismatch for %s\n--- expected ---\n%s\n--- actual ---\n%s",
                                case, expected, actual)
            opt.stdout = actual
            return false
        end
        os.tryrm(exe)
        os.tryrm(obj)
        os.tryrm(cache)
        return true
    end)
