#Requires -Version 5.1
<#
.SYNOPSIS
  Sync third_party / bin from DEPS.json via scripts/ps-sync-deps.
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$Tool = Join-Path $ProjectRoot 'scripts\ps-sync-deps\Sync-Deps.ps1'

if (-not (Test-Path -LiteralPath $Tool)) {
    Write-Host 'ps-sync-deps submodule missing. Run:' -ForegroundColor Red
    Write-Host '  git submodule update --init scripts/ps-sync-deps' -ForegroundColor Yellow
    exit 1
}

# Accept JS-style --dry-run for compatibility with old docs.
# Wrap in @() so a single arg stays a string[], not a scalar (splat-safe).
$forward = @(
    foreach ($a in $args) {
        if ($a -eq '--dry-run') { '-DryRun' } else { $a }
    }
)

& $Tool `
    -DepsFile (Join-Path $ProjectRoot 'DEPS.json') `
    -SyncDir (Join-Path $ProjectRoot 'third_party') `
    -BinDir (Join-Path $ProjectRoot 'bin') `
    @forward
exit $LASTEXITCODE
