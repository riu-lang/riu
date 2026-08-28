#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  clang-tidy wrapper via compile_commands.json (gn gen --export-compile-commands).

.DESCRIPTION
  Default: git-changed (incl. untracked) .h/.hpp/.cpp/.cc, excluding gen/third_party/build.
  --all: files listed in compile_commands.json (project sources only).
  Positional paths: only those files.
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$SrcExts = @('.h', '.hpp', '.cpp', '.cc', '.cxx')
$OutDir = Join-Path $ProjectRoot 'build\windows\x64\debug'
if ($env:YUX_OUT_DIR) { $OutDir = $env:YUX_OUT_DIR }

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

function Get-RelativeToRoot([string]$Path) {
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        return ($Path -replace '\\', '/').TrimStart('./')
    }
    $full = [System.IO.Path]::GetFullPath($Path)
    $root = $ProjectRoot.TrimEnd('\', '/')
    if ($full.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
        return $full.Substring($root.Length).TrimStart('\', '/') -replace '\\', '/'
    }
    return ($full -replace '\\', '/')
}

function Test-ExcludedPath([string]$Rel) {
    $n = $Rel -replace '/', '\'
    return $n -match '(^|\\)(gen|third_party|build|\.xmake|\.cache)(\\|$)'
}

function Get-GitChangedFiles {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & git -C $ProjectRoot status --porcelain --untracked-files=all 2>&1
    $ErrorActionPreference = $prev
    $files = @()
    foreach ($line in @($out)) {
        $line = [string]$line
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        if ($line.Length -lt 4) { continue }
        $status = $line.Substring(0, 2)
        $rest = $line.Substring(3)
        if ($status.StartsWith('R') -or $status.StartsWith('C')) {
            $arrow = $rest.IndexOf(' -> ')
            if ($arrow -ge 0) { $rest = $rest.Substring($arrow + 4) }
        }
        if ($status[0] -eq [char]'D' -or $status[1] -eq [char]'D') { continue }
        if ($rest.StartsWith('"') -and $rest.EndsWith('"')) {
            $rest = $rest.Substring(1, $rest.Length - 2) -replace '\\"', '"'
        }
        $files += $rest
    }
    return $files
}

function Filter-Cxx([string[]]$Files) {
    $result = New-Object System.Collections.Generic.List[string]
    foreach ($f in $Files) {
        $rel = Get-RelativeToRoot $f
        $ext = [System.IO.Path]::GetExtension($rel).ToLowerInvariant()
        if ($SrcExts -notcontains $ext) { continue }
        if (Test-ExcludedPath $rel) { continue }
        $full = Join-Path $ProjectRoot ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
        if (Test-Path -LiteralPath $full) { [void]$result.Add((Get-RelativeToRoot $full)) }
    }
    return ,@($result.ToArray())
}

function Get-CompileCommandsFiles {
    $cc = Join-Path $OutDir 'compile_commands.json'
    if (-not (Test-Path -LiteralPath $cc)) {
        throw "compile_commands.json not found. Run ./build.ps1 --gen-only first."
    }
    $json = Get-Content -LiteralPath $cc -Raw -Encoding utf8 | ConvertFrom-Json
    $result = New-Object System.Collections.Generic.List[string]
    $seen = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($e in $json) {
        $rel = Get-RelativeToRoot $e.file
        if (Test-ExcludedPath $rel) { continue }
        if ($seen.Add($rel)) { [void]$result.Add($rel) }
    }
    return ,@($result.ToArray())
}

function Find-ClangTidy {
    $cmd = Get-Command clang-tidy -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Show-Help {
    Write-Host @'
用法:
  ./lint.ps1              仅 git 变动/未跟踪的 C/C++
  ./lint.ps1 --all        compile_commands.json 中的项目源
  ./lint.ps1 yux/x.cpp    指定文件
  ./lint.ps1 -h / --help  帮助

需要 compile_commands.json（./build.ps1 --gen-only）。提交须 0 warnings。
'@
}

$flagAll = $false
$positional = New-Object System.Collections.Generic.List[string]
foreach ($a in $args) {
    $s = [string]$a
    if ($s -in @('-h', '--help', '-Help', '/?')) { Show-Help; exit 0 }
    elseif ($s -eq '--all') { $flagAll = $true }
    elseif (-not $s.StartsWith('--')) { [void]$positional.Add($s) }
}

if ($positional.Count -gt 0) {
    $files = Filter-Cxx @($positional.ToArray())
    if ($files.Count -eq 0) { Write-Log 'no matching files.' Gray; exit 0 }
    Write-Log "scope: $($files.Count) explicit file(s)" Cyan
} elseif ($flagAll) {
    $files = Get-CompileCommandsFiles
    Write-Log "scope: --all ($($files.Count) file(s) from compile_commands.json)" Cyan
} else {
    $files = Filter-Cxx @(Get-GitChangedFiles)
    if ($files.Count -eq 0) {
        Write-Log 'no git-changed C/C++ files; nothing to lint.' Gray
        exit 0
    }
    Write-Log "scope: git changed ($($files.Count) file(s))" Cyan
}

$tidy = Find-ClangTidy
if (-not $tidy) {
    Write-Log 'clang-tidy not found in PATH; aborting.' Red
    exit 2
}
if (-not (Test-Path -LiteralPath (Join-Path $OutDir 'compile_commands.json'))) {
    Write-Log 'compile_commands.json missing. Run ./build.ps1 --gen-only' Red
    exit 2
}

Write-Log "clang-tidy: $tidy" DarkGray
Write-Log "compile_commands: $OutDir`n" DarkGray

Push-Location $ProjectRoot
try {
    & $tidy -p $OutDir @files
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) {
    Write-Log "`nclang-tidy exited with status $code" Red
    exit $code
}
Write-Log "`nclang-tidy done." Green
