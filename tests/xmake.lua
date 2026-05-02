-- Copyright (c) 2026. Yin-Jinlong@github
-- MPL-2.0

-- yux 语言测试：使用 xmake 原生测试机制（xmake test）
--
-- 每个 tests/cases/*.yux 配对一个 *.expected：
--   - cases/*.yux            编译应成功；运行产物 .exe，stdout 需与 .expected 完全一致
--   - cases/diag_*.yux       编译应失败；配对 *.expected_err，逐行子串匹配 stderr
--   - cases/error/*.yux      编译应失败；.expected 内容仅作占位（约定 "error"）
--
-- 项目级用例：tests/projects/<case>/ 下包含 yux.toml + 入口源文件 + expected.txt。
-- 以该目录为 CWD 调用 `yux build <case>`，运行 build/<case>/<case>.exe 并比对 expected.txt。
--
-- 诊断回归用例 (cases/diag_*.yux + *.expected_err)：
--   - expected_err 中每一非空、非 `;` 开头行视为子串断言，必须在 stderr 中出现
--   - 编译必须以非零退出码结束（否则即使 stderr 含期望内容也算失败）
--
-- 运行：
--   xmake build yux                         先构建编译器
--   xmake test                              运行全部用例
--   xmake test -v                           详细日志（失败时打印 stdout / stderr / errors）
--   xmake test yux_tests/<name>             单独运行（<name> 为用例文件基名）
--   xmake test "yux_tests/*"                通配符

local cases_dir = path.join(os.scriptdir(), "cases")
local projects_dir = path.join(os.scriptdir(), "projects")

local function list_case_names()
    local r = {}
    for _, f in ipairs(os.files(path.join(cases_dir, "*.yux"))) do
        if os.isfile((f:gsub("%.yux$", ".expected"))) then
            r[path.basename(f)] = true
        elseif os.isfile((f:gsub("%.yux$", ".expected_err"))) then
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
        add_tests(name, {group = "yux"})
    end

    on_test(function (target, opt)
        -- 在 sandbox 里重新扫描用例（不复用 description-scope 的闭包变量）
        local cd = path.join(target:scriptdir(), "cases")
        local cases = {}
        for _, f in ipairs(os.files(path.join(cd, "*.yux"))) do
            local exp = f:gsub("%.yux$", ".expected")
            local exp_err = f:gsub("%.yux$", ".expected_err")
            if os.isfile(exp) then
                cases[path.basename(f)] = {file = path.absolute(f)}
            elseif os.isfile(exp_err) then
                cases[path.basename(f)] = {
                    file = path.absolute(f),
                    expected_err_file = path.absolute(exp_err),
                    is_diag = true,
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
            local exe = path.join(build_dir, pname, pname .. ".exe")
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

        local case = entry.file
        local expected_file = case:gsub("%.yux$", ".expected")
        local workdir = path.directory(case)
        local stem = path.basename(case)
        local project_root = path.directory(target:scriptdir())
        -- 单文件模式：产物扁平放在源文件同级的 `build/` 下。
        local build_root = path.join(workdir, "build")
        local exe = path.join(build_root, stem .. ".exe")
        local obj = path.join(build_root, stem .. ".obj")
        local cache = obj .. ".cache"
        os.tryrm(exe)
        os.tryrm(obj)
        os.tryrm(cache)

        if entry.is_diag then
            -- 诊断回归：编译应失败；逐行子串匹配 expected_err
            local stdout_data, stderr_data
            local compile_ok = try {
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

            if compile_ok and os.isfile(exe) then
                opt.errors = "diag case unexpectedly compiled: " .. case
                os.tryrm(exe); os.tryrm(obj); os.tryrm(cache)
                return false
            end

            local expected_err = io.readfile(entry.expected_err_file) or ""
            local haystack = (stderr_data or "") .. "\n" .. (stdout_data or "")
            local missing = {}
            for line in expected_err:gmatch("[^\r\n]+") do
                local trimmed = line:match("^%s*(.-)%s*$")
                if trimmed ~= "" and trimmed:sub(1, 1) ~= ";" then
                    if not haystack:find(trimmed, 1, true) then
                        table.insert(missing, trimmed)
                    end
                end
            end
            os.tryrm(exe); os.tryrm(obj); os.tryrm(cache)
            if #missing > 0 then
                opt.errors = format("diag mismatch for %s\n--- missing lines ---\n%s\n--- stderr ---\n%s",
                                    case, table.concat(missing, "\n"), stderr_data or "")
                return false
            end
            return true
        end

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
