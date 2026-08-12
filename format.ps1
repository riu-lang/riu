#Requires -Version 5.1
<#
.SYNOPSIS
  C/C++ format wrapper: clang-format -i (incl. #include sort).

.DESCRIPTION
  Default: git-changed files. --all: yux/, include/, sdk/yux/src/.
  Positional: listed files only. --check: dry-run -Werror.
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$SrcExts = @('.h', '.hpp', '.cpp', '.cc', '.cxx')

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

function Test-ExcludedRel([string]$Rel) {
    $n = $Rel -replace '/', '\'
    return (
        $n -match '^gen\\' -or
        $n -match '^third_party\\' -or
        $n -match '\\build\\' -or
        $n -match '^build\\' -or
        $n -match '\\.xmake\\' -or
        $n -match '\\.cache\\'
    )
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

function Get-GitChangedFiles {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & git -C $ProjectRoot status --porcelain --untracked-files=all 2>&1
    $ErrorActionPreference = $prev
    $files = @()
    foreach ($line in @($out)) {
        $line = [string]$line
        if ([string]::IsNullOrWhiteSpace($line) -or $line.Length -lt 4) { continue }
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

function Walk-Dir([string]$Dir, [System.Collections.Generic.List[string]]$Out) {
    if (-not (Test-Path -LiteralPath $Dir)) { return }
    Get-ChildItem -LiteralPath $Dir -Force | ForEach-Object {
        $full = $_.FullName
        $rel = Get-RelativeToRoot $full
        if (Test-ExcludedRel $rel) { return }
        if ($_.PSIsContainer) {
            Walk-Dir $full $Out
        } else {
            [void]$Out.Add($full)
        }
    }
}

function Get-AllRepoFiles {
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($r in @('yux', 'include', 'sdk\yux\src')) {
        Walk-Dir (Join-Path $ProjectRoot $r) $out
    }
    return ,@($out.ToArray())
}

function Filter-Cxx([string[]]$Files) {
    $result = New-Object System.Collections.Generic.List[string]
    foreach ($f in $Files) {
        $full = if ([System.IO.Path]::IsPathRooted($f)) {
            $f
        } else {
            Join-Path $ProjectRoot ($f -replace '/', [IO.Path]::DirectorySeparatorChar)
        }
        $ext = [System.IO.Path]::GetExtension($full).ToLowerInvariant()
        if ($SrcExts -notcontains $ext) { continue }
        $rel = Get-RelativeToRoot $full
        if (Test-ExcludedRel $rel) { continue }
        if (Test-Path -LiteralPath $full) { [void]$result.Add($full) }
    }
    return ,@($result.ToArray())
}

function Find-ClangFormat {
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Invoke-ClangFormat([string]$Clang, [string[]]$Files, [bool]$Check) {
    $batchSize = 50
    $failed = 0
    for ($i = 0; $i -lt $Files.Count; $i += $batchSize) {
        $end = [Math]::Min($i + $batchSize - 1, $Files.Count - 1)
        $batch = $Files[$i..$end]
        $cfArgs = if ($Check) { @('--dry-run', '-Werror') + $batch } else { @('-i') + $batch }
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        & $Clang @cfArgs 2>&1 | ForEach-Object { Write-Host $_ }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
        if ($code -ne 0) { $failed++ }
    }
    return $failed
}

$flagAll = $false
$flagCheck = $false
$positional = New-Object System.Collections.Generic.List[string]
foreach ($a in $args) {
    if ($a -eq '--all') { $flagAll = $true }
    elseif ($a -eq '--check') { $flagCheck = $true }
    elseif (-not ([string]$a).StartsWith('--')) { [void]$positional.Add([string]$a) }
}

if ($positional.Count -gt 0) {
    $candidates = @($positional.ToArray())
    Write-Log "scope: $($positional.Count) explicit file(s)" Cyan
} elseif ($flagAll) {
    $candidates = @(Get-AllRepoFiles | ForEach-Object { Get-RelativeToRoot $_ })
    Write-Log "scope: --all ($($candidates.Count) candidate file(s))" Cyan
} else {
    $candidates = @(Get-GitChangedFiles)
    Write-Log "scope: git changed ($($candidates.Count) candidate file(s))" Cyan
}

$files = Filter-Cxx $candidates
if ($files.Count -eq 0) {
    Write-Log 'nothing to do (no matching C/C++ source files).' Gray
    exit 0
}

$clang = Find-ClangFormat
if (-not $clang) {
    Write-Log 'clang-format not found in PATH; aborting.' Red
    exit 2
}
Write-Log "clang-format: $clang" Gray

$failed = Invoke-ClangFormat -Clang $clang -Files $files -Check $flagCheck

if ($flagCheck) {
    if ($failed -gt 0) {
        Write-Log "`n$failed batch(es) report format diff. Run ``./format.ps1`` to fix." Red
        exit 1
    }
    Write-Log "`nall $($files.Count) file(s) match clang-format style." Green
} else {
    Write-Log "`nformatted $($files.Count) file(s)." Green
}
