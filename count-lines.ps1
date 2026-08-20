#Requires -Version 5.1
<#
.SYNOPSIS
  Count lines of code with cloc (default: HEAD).
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$CommitHash = if ($args.Count -gt 0) { [string]$args[0] } else { 'HEAD' }
$LangDefFile = Join-Path $ProjectRoot 'yux_lang_def.txt'
$ClocExe = Join-Path $ProjectRoot 'bin\cloc-2.10.exe'

if (-not (Test-Path -LiteralPath $ClocExe)) {
    Write-Host "cloc not found: $ClocExe" -ForegroundColor Red
    Write-Host 'Run ./sync-deps.ps1 first to download cloc.' -ForegroundColor Yellow
    exit 1
}

& $ClocExe $CommitHash --read-lang-def $LangDefFile --not-match-f 'lock\.'
exit $LASTEXITCODE
