-- yux/lsp — LSP 服务器（yux-lsp 可执行文件）
-- 叶子节点，直接编成二进制，不做静态库

target("yux-lsp")
    set_kind("binary")
    add_deps("yux_frontend")

    -- yux/ 顶层：让 #include "lsp/..." 能找到 yux/lsp/...
    add_includedirs("..", {public = true})

    add_files("*.cpp")
    set_rundir("$(projectdir)")

    -- Claude Code 插件走独立 server 进程 yux-lsp-claude (避免与 yux-vscode
    -- clientInfo 等串扰; 见 plugins/yux-claude-code/README.md). 同源, 仅文件名
    -- 不同 —— 直接构建后复制, 避免历史上 "手动 copy 后忘记同步, 旧副本不识别
    -- 新 g4 (例 #Fallible(E) 单参注解), Claude Code 看到误报" 一类的坑.
    -- Windows 上若 .exe 正在运行: 不能覆盖也不能删, 但可以 rename. 先把占用
    -- 的副本改名到 .old, 再 cp 新文件, 打警告提示用户重启 Claude Code.
    after_build(function (target)
        import("core.base.option")
        local src = target:targetfile()
        local dst = path.join(path.directory(src), "yux-lsp-claude" .. (is_host("windows") and ".exe" or ""))
        if not os.isfile(src) then return end
        local ok = try { function () os.cp(src, dst); return true end }
        if ok then
            cprint("${dim}  [yux-lsp-claude] %s${clear}", dst)
            return
        end
        -- 占用: rename → cp
        local stale = dst .. ".old"
        os.tryrm(stale)
        local moved = try { function () os.mv(dst, stale); return true end }
        if not moved then
            cprint("${yellow}warning: yux-lsp-claude 既无法覆盖也无法重命名 (%s); 跳过${clear}", dst)
            return
        end
        try { function () os.cp(src, dst) end }
        cprint("${yellow}warning: yux-lsp-claude.exe 正被占用, 已重命名旧副本到 %s 并写入新文件; 重启 Claude Code 后生效${clear}", stale)
    end)
