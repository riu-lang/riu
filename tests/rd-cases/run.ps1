#!/usr/bin/env pwsh
#Requires -Version 5.1
# rd 金样：有 *.tokens.txt 则 --rd-tokens 对金样；有 *.rd.txt 则 --rd dump 对金样。
# 有 *.diag.txt 则 --rd stderr（紧凑 E1001/E1002）对金样；无则 stderr 须空。
# Pos 用 UTF-8 字节；词法按首字节分流。
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$fail = 0
$n = 0
$utf8 = New-Object System.Text.UTF8Encoding $false
$norm = { param($s) ($s -replace "`r`n", "`n").TrimEnd() + "`n" }
function Read-Utf8([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return [IO.File]::ReadAllText($Path, $utf8)
}
function Invoke-RiuAst {
    param([string[]]$ArgList, [string]$ErrPath)
    # cmd 2> 原样写字节，避免 pwsh 把 UTF-8 stderr 当系统代码页解码。
    $quoted = foreach ($a in $ArgList) { '"' + ($a -replace '"', '\"') + '"' }
    cmd /c "riu-ast $($quoted -join ' ') 2>`"$ErrPath`""
    return $LASTEXITCODE
}
Get-ChildItem -LiteralPath $Here -Filter *.ut | Sort-Object Name | ForEach-Object {
    $n++
    $gold = Join-Path $Here ($_.BaseName + '.tokens.txt')
    if (Test-Path -LiteralPath $gold) {
        $tmpB = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".rd")
        $tmpErr = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".tok.err")
        $code = Invoke-RiuAst -ArgList @($_.FullName, '--rd-tokens', '-o', $tmpB) -ErrPath $tmpErr
        if ($code -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --rd-tokens"; $script:fail++; return }
        $b = Read-Utf8 $tmpB
        $g = Read-Utf8 $gold
        if ((& $norm $b) -ne (& $norm $g)) {
            Write-Host "FAIL $($_.Name) --rd-tokens != $($_.BaseName).tokens.txt"
            $script:fail++
            return
        }
    }
    $rdGold = Join-Path $Here ($_.BaseName + '.rd.txt')
    $diagGold = Join-Path $Here ($_.BaseName + '.diag.txt')
    if ((Test-Path -LiteralPath $rdGold) -or (Test-Path -LiteralPath $diagGold)) {
        $tmpC = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".flat")
        $tmpErr = Join-Path $env:TEMP ("rd-" + $_.BaseName + ".err")
        $code = Invoke-RiuAst -ArgList @($_.FullName, '--rd', '-o', $tmpC) -ErrPath $tmpErr
        if ($code -ne 0) { Write-Host "FAIL $($_.Name) riu-ast --rd"; $script:fail++; return }
        if (Test-Path -LiteralPath $rdGold) {
            $c = Read-Utf8 $tmpC
            $g = Read-Utf8 $rdGold
            if ((& $norm $c) -ne (& $norm $g)) {
                Write-Host "FAIL $($_.Name) --rd != $($_.BaseName).rd.txt"
                $script:fail++
                return
            }
        }
        $errText = Read-Utf8 $tmpErr
        if (Test-Path -LiteralPath $diagGold) {
            $g = Read-Utf8 $diagGold
            if ((& $norm $errText) -ne (& $norm $g)) {
                Write-Host "FAIL $($_.Name) --rd stderr != $($_.BaseName).diag.txt"
                $script:fail++
                return
            }
        } elseif ($errText.Trim().Length -ne 0) {
            Write-Host "FAIL $($_.Name) unexpected --rd stderr"
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
