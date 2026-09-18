#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  Pack riu zip (replaces xmake pack / xpack).
#>
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $OutDir "pack-stage\riu"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-To([string]$Src, [string]$Dst) {
    $dir = Split-Path $Dst -Parent
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    Copy-Item -LiteralPath $Src -Destination $Dst -Force
}

$binDir = Join-Path $stage 'bin'
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
$isDebug = ($OutDir -match '[\\/]debug([\\/]|$)')
foreach ($exe in @('riu.exe', 'riu-lsp.exe', 'riu-ast.exe', 'riu-check.exe', 'riu-test-runner.exe')) {
    $src = Join-Path $OutDir "bin\$exe"
    if (-not (Test-Path -LiteralPath $src)) { throw "missing $src — build all targets first" }
    Copy-Item -LiteralPath $src -Destination (Join-Path $binDir $exe)
    $pdb = [System.IO.Path]::ChangeExtension($src, '.pdb')
    if (Test-Path -LiteralPath $pdb) {
        Copy-Item -LiteralPath $pdb -Destination (Join-Path $binDir ([System.IO.Path]::GetFileName($pdb)))
    } elseif ($isDebug) {
        throw "missing $pdb — debug pack requires PDB next to each exe"
    }
}

$libSrc = Join-Path $OutDir 'lib\riurt.lib'
if (-not (Test-Path -LiteralPath $libSrc)) { throw "missing $libSrc" }
New-Item -ItemType Directory -Path (Join-Path $stage 'lib') -Force | Out-Null
Copy-Item -LiteralPath $libSrc -Destination (Join-Path $stage 'lib\riurt.lib')

# sdk：排除 build/ 与 .ut/
$sdkDst = Join-Path $stage 'sdk'
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'sdk') -Destination $sdkDst -Recurse -Force
Get-ChildItem -LiteralPath $sdkDst -Recurse -Directory -Force | Where-Object {
    $_.Name -eq 'build' -or $_.Name -eq '.ut'
} | ForEach-Object { Remove-Item -LiteralPath $_.FullName -Recurse -Force -ErrorAction SilentlyContinue }

Copy-Item -LiteralPath (Join-Path $ProjectRoot 'examples') -Destination (Join-Path $stage 'examples') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'docs') -Destination (Join-Path $stage 'docs') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'README.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $ProjectRoot 'LICENSE.txt') -Destination (Join-Path $stage 'LICENSE.txt')

$licenses = @(
    @{ Lib = 'cli11'; Rel = 'cli11\LICENSE' },
    @{ Lib = 'llvm'; Rel = 'llvm\llvm\LICENSE.TXT' },
    @{ Lib = 'utfcpp'; Rel = 'utfcpp\LICENSE' },
    @{ Lib = 'zlib'; Rel = 'zlib\LICENSE' },
    @{ Lib = 'toml11'; Rel = 'toml11\LICENSE' },
    @{ Lib = 'nlohmann_json'; Rel = 'nlohmann_json\LICENSE.MIT' }
)
foreach ($t in $licenses) {
    $src = Join-Path $ProjectRoot "third_party\$($t.Rel)"
    if (Test-Path -LiteralPath $src) {
        $dstDir = Join-Path $stage "shared\licenses\$($t.Lib)"
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
        Copy-Item -LiteralPath $src -Destination (Join-Path $dstDir (Split-Path $t.Rel -Leaf))
    }
}

$zipName = "riu-$Version.zip"
$zipPath = Join-Path $OutDir $zipName
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$stageParent = Split-Path $stage -Parent
# Compress-Archive loads whole files into memory and OOMs on large debug PDBs;
# tar -a streams and is available on Windows 10+.
Push-Location $stageParent
try {
    & tar -a -c -f $zipPath (Split-Path $stage -Leaf)
    if ($LASTEXITCODE -ne 0) { throw "tar zip failed: $LASTEXITCODE" }
} finally {
    Pop-Location
}
Write-Host "packed $zipPath" -ForegroundColor Green
