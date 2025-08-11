#!/usr/bin/env pwsh

if (-not (Get-Command node -ErrorAction SilentlyContinue)) {
    Write-Error "Error: node not found in PATH"
    exit 1
}

$scriptDir = $PSScriptRoot
& node "$scriptDir/sync-deps.js" @args
