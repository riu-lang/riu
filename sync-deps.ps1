#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  Sync third_party / bin from DEPS.json via scripts/ps-sync-deps.
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$Tool = Join-Path $ProjectRoot 'scripts\ps-sync-deps\Sync-Deps.ps1'
$LlvmDir = Join-Path $ProjectRoot 'third_party\llvm'

if (-not (Test-Path -LiteralPath $Tool)) {
    Write-Host 'ps-sync-deps submodule missing. Run:' -ForegroundColor Red
    Write-Host '  git submodule update --init scripts/ps-sync-deps' -ForegroundColor Yellow
    exit 1
}

# Accept JS-style --dry-run for compatibility with old docs.
$forward = @(
    foreach ($a in $args) {
        if ($a -eq '--dry-run') { '-DryRun' } else { $a }
    }
)

$isDryRun = ($forward -contains '-DryRun') -or ($forward -contains '-n')
$touchesLlvm = ($forward.Count -eq 0) -or ($forward -contains 'llvm')

function Get-LlvmHead {
    if (-not (Test-Path -LiteralPath (Join-Path $LlvmDir '.git'))) { return $null }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $head = & git -C $LlvmDir rev-parse HEAD 2>$null
    $ErrorActionPreference = $prev
    if ($LASTEXITCODE -ne 0) { return $null }
    return ([string]$head).Trim()
}

$llvmBefore = if ($touchesLlvm -and -not $isDryRun) { Get-LlvmHead } else { $null }

function Write-LlvmSyncCommit([string]$Commit) {
    if ([string]::IsNullOrWhiteSpace($Commit)) { return }
    $stamp = Join-Path $ProjectRoot 'third_party\llvm.sync_commit'
    $parent = Split-Path -Parent $stamp
    if (-not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($stamp, $Commit.Trim() + "`n")
}

# Layout is in DEPS.json. Pass names as -Name so they are not bound as SyncDir.
$params = @{ DepsFile = Join-Path $ProjectRoot 'DEPS.json' }
$names = @()
foreach ($a in $forward) {
    if ($a -eq '-DryRun' -or $a -eq '-n') {
        $params['DryRun'] = $true
        continue
    }
    if ($a -eq '-Name') { continue }
    if ([string]$a -like '-*') {
        Write-Host "Unknown argument: $a" -ForegroundColor Red
        exit 1
    }
    $names += $a
}
if (@($names).Count -gt 0) {
    $params['Name'] = @($names)
}

& $Tool @params
$code = $LASTEXITCODE
if ($code -ne 0) { exit $code }

if ($touchesLlvm -and -not $isDryRun) {
    $llvmAfter = Get-LlvmHead
    if ($llvmAfter) { Write-LlvmSyncCommit $llvmAfter }
    if ($llvmAfter -and $llvmAfter -ne $llvmBefore) {
        $beforeShort = if ($llvmBefore) { $llvmBefore.Substring(0, [Math]::Min(8, $llvmBefore.Length)) } else { 'none' }
        $afterShort = $llvmAfter.Substring(0, [Math]::Min(8, $llvmAfter.Length))
        Write-Host ''
        Write-Host "llvm source updated ($beforeShort → $afterShort)" -ForegroundColor Yellow
        Write-Host '  next: ./build.ps1 llvm   (or ./build.ps1 riu)' -ForegroundColor Yellow
        Write-Host '  GN will reconfigure + rebuild LLVM when the source stamp differs.' -ForegroundColor DarkGray
    }
}

exit 0
