#Requires -Version 5.1
<#
.SYNOPSIS
  clang-tidy wrapper via xmake check clang.tidy.

.DESCRIPTION
  Default: git-changed (incl. untracked) .h/.hpp/.cpp/.cc, excluding gen/third_party/build.
  --all: full targets. Positional paths: only those files (-f).
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$Targets = @('yux_frontend', 'yux_codegen', 'yux-test-runner', 'yux', 'yux-lsp', 'yux-ast', 'yux-check')
$SrcExts = @('.h', '.hpp', '.cpp', '.cc', '.cxx')

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
        if (Test-Path -LiteralPath $full) { [void]$result.Add($rel) }
    }
    return ,@($result.ToArray())
}

$flagAll = $false
$positional = New-Object System.Collections.Generic.List[string]
foreach ($a in $args) {
    if ($a -eq '--all') { $flagAll = $true }
    elseif (-not ([string]$a).StartsWith('--')) { [void]$positional.Add([string]$a) }
}

if ($positional.Count -gt 0) {
    $files = Filter-Cxx @($positional.ToArray())
    if ($files.Count -eq 0) { Write-Log 'no matching files.' Gray; exit 0 }
    Write-Log "scope: $($files.Count) explicit file(s)" Cyan
    $xmakeArgs = @('check', 'clang.tidy', '-f', ($files -join ';'))
} elseif ($flagAll) {
    Write-Log ("scope: --all (targets: {0})" -f ($Targets -join ' ')) Cyan
    $xmakeArgs = @('check', 'clang.tidy') + $Targets
} else {
    $files = Filter-Cxx @(Get-GitChangedFiles)
    if ($files.Count -eq 0) {
        Write-Log 'no git-changed C/C++ files; nothing to lint.' Gray
        exit 0
    }
    Write-Log "scope: git changed ($($files.Count) file(s))" Cyan
    $xmakeArgs = @('check', 'clang.tidy', '-f', ($files -join ';'))
}

Write-Log ("`n=== xmake {0} ===`n" -f ($xmakeArgs -join ' ')) Cyan
Push-Location $ProjectRoot
try {
    & xmake @xmakeArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($code -ne 0) {
    Write-Log "`nclang-tidy exited with status $code" Red
    exit $code
}
Write-Log "`nclang-tidy done." Green
