# Claude Code PostToolUse hook: Bash 执行后检查工作目录是否为项目根目录。
# 不是则通过 stderr + exit 2 将警告反馈给 Agent（Claude）。
# PostToolUse 的 exit 2 不会阻塞已完成的工具调用，仅将 stderr 注入 Agent 上下文。

[Console]::OutputEncoding = [Text.Encoding]::UTF8
[Console]::InputEncoding  = [Text.Encoding]::UTF8
$ErrorActionPreference = 'SilentlyContinue'

try {
    $raw = [Console]::In.ReadToEnd()
    if ([string]::IsNullOrWhiteSpace($raw)) { exit 0 }
    $payload = $raw | ConvertFrom-Json

    if ($payload.tool_name -ne 'Bash') { exit 0 }

    $cwd        = $payload.cwd
    $projectDir = $env:CLAUDE_PROJECT_DIR

    if (-not $cwd -or -not $projectDir) { exit 0 }

    $cwdResolved     = (Resolve-Path $cwd -ErrorAction SilentlyContinue).Path
    $projectResolved = (Resolve-Path $projectDir -ErrorAction SilentlyContinue).Path

    if ($cwdResolved -and $projectResolved -and $cwdResolved -ne $projectResolved) {
        $msg = "[WARN] 当前不在项目根目录！当前: $cwd，项目根: $projectDir。推荐始终在项目根目录执行Bash。"
        [Console]::Error.WriteLine($msg)
        exit 2
    }
} catch {
    # 静默
}

exit 0
