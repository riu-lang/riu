-- yux 语言测试：使用 xmake 原生测试机制（xmake test）
--
-- 每个 tests/cases/*.yux 配对一个 *.expected：
--   - cases/*.yux          编译应成功；运行产物 .exe，stdout 需与 .expected 完全一致
--   - cases/error/*.yux    编译应失败；.expected 内容仅作占位（约定 "error"）
--
-- 运行：
--   xmake build yux                         先构建编译器
--   xmake test                              运行全部用例
--   xmake test -v                           详细日志（失败时打印 stdout / stderr / errors）
--   xmake test yux_tests/<name>             单独运行（<name> 为用例文件基名）
--   xmake test "yux_tests/*"                通配符

local cases_dir = path.join(os.scriptdir(), "cases")

local function list_case_names()
    local r = {}
    for _, f in ipairs(os.files(path.join(cases_dir, "*.yux"))) do
        if os.isfile((f:gsub("%.yux$", ".expected"))) then
            r[path.basename(f)] = true
        end
    end
    for _, f in ipairs(os.files(path.join(cases_dir, "error", "*.yux"))) do
        if os.isfile((f:gsub("%.yux$", ".expected"))) then
            r["error_" .. path.basename(f)] = true
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
            if os.isfile((f:gsub("%.yux$", ".expected"))) then
                cases[path.basename(f)] = {file = path.absolute(f), expect_error = false}
            end
        end
        for _, f in ipairs(os.files(path.join(cd, "error", "*.yux"))) do
            if os.isfile((f:gsub("%.yux$", ".expected"))) then
                cases["error_" .. path.basename(f)] = {file = path.absolute(f), expect_error = true}
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

        if entry.expect_error then
            if ok and os.isfile(exe) then
                opt.errors = "expected compile to fail but it succeeded: " .. case
                return false
            end
            return true
        end

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
