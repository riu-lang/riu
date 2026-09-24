#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  便携入口：缺 bin/uv.exe 时经 ps-sync-deps 同步，再 uv run sync-deps。
#>
$ErrorActionPreference = 'Stop'

$Root = $PSScriptRoot
$Bin = Join-Path $Root 'bin'
$Uv = Join-Path $Bin 'uv.exe'
$Deps = Join-Path $Root 'DEPS.json'
$SyncTool = Join-Path $Root 'scripts\ps-sync-deps\Sync-Deps.ps1'

function Ensure-Uv {
    if (Test-Path -LiteralPath $Uv) { return }
    if (-not (Test-Path -LiteralPath $SyncTool)) {
        throw @(
            'ps-sync-deps submodule missing. Run:'
            '  git submodule update --init scripts/ps-sync-deps'
        )
    }
    Write-Host "syncing uv (DEPS.json) -> $Uv" -ForegroundColor Cyan
    & $SyncTool -DepsFile $Deps -Name uv
    if (-not (Test-Path -LiteralPath $Uv)) {
        throw "uv not found at $Uv after sync"
    }
}

Ensure-Uv
& $Uv run sync-deps @args
exit $LASTEXITCODE
