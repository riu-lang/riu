#Requires -Version 5.1
<#
.SYNOPSIS
  Count lines of code with cloc (default: HEAD).
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$CommitHash = if ($args.Count -gt 0) { [string]$args[0] } else { 'HEAD' }
$LangDefFile = Join-Path $ProjectRoot 'yux_lang_def.txt'

if (-not (Get-Command cloc -ErrorAction SilentlyContinue)) {
    Write-Host 'cloc not found in PATH' -ForegroundColor Red
    exit 1
}

& cloc $CommitHash --read-lang-def $LangDefFile --not-match-f 'lock\.'
exit $LASTEXITCODE
