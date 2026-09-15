#!/usr/bin/env pwsh
#Requires -Version 5.1
# clang-cl 把 c/ffi_demo.c 编成 lib/ffi_demo.lib（无 CRT）。
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$CDir = Join-Path $Root 'c'
$LibDir = Join-Path $Root 'lib'
New-Item -ItemType Directory -Path $LibDir -Force | Out-Null

function Find-LlvmBin([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($env:RiuExe) {
        $bin = Split-Path -Parent $env:RiuExe
        $cand = Join-Path (Split-Path -Parent $bin) "llvm\bin\$Name.exe"
        if (Test-Path -LiteralPath $cand) { return $cand }
    }
    $here = $Root
    for ($i = 0; $i -lt 6; $i++) {
        $cand = Join-Path $here "build\windows\x64\debug\llvm\bin\$Name.exe"
        if (Test-Path -LiteralPath $cand) { return $cand }
        $parent = Split-Path -Parent $here
        if ($parent -eq $here) { break }
        $here = $parent
    }
    throw "$Name not found (need clang-cl / lld-link on PATH or next to riu.exe)"
}

$clang = Find-LlvmBin 'clang-cl'
$lld = Find-LlvmBin 'lld-link'
$obj = Join-Path $LibDir 'ffi_demo.obj'
$lib = Join-Path $LibDir 'ffi_demo.lib'
& $clang /c /GS- /I$CDir /Fo$obj (Join-Path $CDir 'ffi_demo.c')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& $lld /lib "/out:$lib" $obj
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Host "wrote $lib"
