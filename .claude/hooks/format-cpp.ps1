# Claude Code PostToolUse hook: 对刚编辑/写入的 C/C++ 源文件运行 clang-format -i.
# 仅作用于 .cpp/.cc/.cxx/.h/.hpp; 其它文件直接跳过.
# 失败不阻塞工具调用 (始终 exit 0); clang-format 不存在也静默.

$ErrorActionPreference = 'SilentlyContinue'

try {
    $raw = [Console]::In.ReadToEnd()
    if ([string]::IsNullOrWhiteSpace($raw)) { exit 0 }
    $payload = $raw | ConvertFrom-Json
    $path = $payload.tool_input.file_path
    if (-not $path) { exit 0 }
    if ($path -notmatch '\.(cpp|cc|cxx|c|h|hpp|hh|hxx)$') { exit 0 }
    if (-not (Test-Path $path)) { exit 0 }

    $clang = Get-Command clang-format -ErrorAction SilentlyContinue
    if (-not $clang) { exit 0 }

    & $clang.Source -i -- $path 2>&1 | Out-Null
} catch {
    # 任何异常都不阻塞工具调用
}

exit 0
