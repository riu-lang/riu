#!/usr/bin/env pwsh
#Requires -Version 5.1
# rd Scanner 对照：每个 .ut 的 --rd-tokens 须等于 --tokens；有 *.tokens.txt 则再对金样。
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$fail = 0
$n = 0
Get-ChildItem -LiteralPath $Here -Filter *.ut | Sort-Object Name | ForEach-Object {
    $n++
    $tmpA = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".antlr")
    $tmpB = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".rd")
    & riu-ast $_.FullName --tokens -o $tmpA
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --tokens"; $script:fail++; return }
    & riu-ast $_.FullName --rd-tokens -o $tmpB
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --rd-tokens"; $script:fail++; return }
    $a = [IO.File]::ReadAllText($tmpA)
    $b = [IO.File]::ReadAllText($tmpB)
    if ($a -ne $b) {
        Write-Host "FAIL $($_.Name) rd != antlr"
        $script:fail++
        return
    }
    $gold = Join-Path $Here ($_.BaseName + '.tokens.txt')
    if (Test-Path -LiteralPath $gold) {
        $g = [IO.File]::ReadAllText($gold)
        # git eol=lf；WriteAllText 可能带 CRLF
        $norm = { param($s) ($s -replace "`r`n", "`n").TrimEnd() + "`n" }
        if ((& $norm $a) -ne (& $norm $g)) {
            Write-Host "FAIL $($_.Name) antlr != $($_.BaseName).tokens.txt"
            $script:fail++
            return
        }
    }
    Write-Host "OK  $($_.Name)"
}
if ($fail -ne 0) {
    Write-Host "rd-cases: $fail / $n failed"
    exit 1
}
Write-Host "rd-cases: $n ok"
