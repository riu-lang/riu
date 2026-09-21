#!/usr/bin/env pwsh
# 先编一次写出 pair.ud，再删主模块 obj / exe，让正式 build 从 .ud 加载带默认的 typeParams。
$ErrorActionPreference = 'Stop'
if (-not $env:RiuExe) { throw 'RiuExe not set' }
& $env:RiuExe build generic_default
if ($LASTEXITCODE -ne 0) { throw 'prebuild compile failed' }
$mainObj = Join-Path $PSScriptRoot 'build\src\main.obj'
if (Test-Path -LiteralPath $mainObj) { Remove-Item -LiteralPath $mainObj -Force }
$exe = Join-Path $PSScriptRoot 'build\generic_default.exe'
if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }
