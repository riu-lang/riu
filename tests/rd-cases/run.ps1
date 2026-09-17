#!/usr/bin/env pwsh
#Requires -Version 5.1
# rd 金样：有 *.tokens.txt 则 --rd-tokens 对金样；有 *.rd.txt 则 --rd dump 对金样。
# 不再要求 --rd-tokens 与 ANTLR --tokens 逐字节一致（Pos 用 UTF-8 字节，词法按分流而非 g4 最长匹配）。
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$fail = 0
$n = 0
$norm = { param($s) ($s -replace "`r`n", "`n").TrimEnd() + "`n" }
Get-ChildItem -LiteralPath $Here -Filter *.ut | Sort-Object Name | ForEach-Object {
    $n++
    $gold = Join-Path $Here ($_.BaseName + '.tokens.txt')
    if (Test-Path -LiteralPath $gold) {
        $tmpB = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".rd")
        & riu-ast $_.FullName --rd-tokens -o $tmpB
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --rd-tokens"; $script:fail++; return }
        $b = [IO.File]::ReadAllText($tmpB)
        $g = [IO.File]::ReadAllText($gold)
        if ((& $norm $b) -ne (& $norm $g)) {
            Write-Host "FAIL $($_.Name) --rd-tokens != $($_.BaseName).tokens.txt"
            $script:fail++
            return
        }
    }
    $rdGold = Join-Path $Here ($_.BaseName + '.rd.txt')
    if (Test-Path -LiteralPath $rdGold) {
        $tmpC = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".flat")
        & riu-ast $_.FullName --rd -o $tmpC
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --rd"; $script:fail++; return }
        $c = [IO.File]::ReadAllText($tmpC)
        $g = [IO.File]::ReadAllText($rdGold)
        if ((& $norm $c) -ne (& $norm $g)) {
            Write-Host "FAIL $($_.Name) --rd != $($_.BaseName).rd.txt"
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
