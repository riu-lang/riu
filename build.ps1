#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  GN + Ninja 构建入口：vcvars → gn gen → ninja。

.DESCRIPTION
  ./build.ps1                 构建全部默认目标（riu 及附属工具）
  ./build.ps1 riu             只构建主编译器
  ./build.ps1 riu riu-check   一次构建多个目标
  ./build.ps1 llvm            只确保 LLVM
  ./build.ps1 test            项目/格式化回归（tests/projects，默认并行）
  ./build.ps1 pack            打包 zip
  ./build.ps1 -Release ...    release 配置
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$Mode = 'debug'
$GenOnly = $false
$DoTest = $false
$DoPack = $false
$NinjaTargets = New-Object System.Collections.Generic.List[string]
$Forward = New-Object System.Collections.Generic.List[string]

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

function Show-Help {
    Write-Host @'
用法:
  ./build.ps1 [options] [ninja-targets...]
  ./build.ps1 test [test-args...]
  ./build.ps1 test -Jobs 8
  ./build.ps1 pack

选项:
  -Release / --release   release 配置（默认 debug）
  -GenOnly / --gen-only  只 gn gen，不 ninja
  -h / --help            帮助

test 参数（转发到 tests/run.ps1）:
  -Jobs / --jobs / -j N  并行用例数（默认 CPU 核数；1 = 串行）
  -Group project|format  只跑一类
  <name>                 只跑指定用例目录

常用目标: riu  riu-lsp  riu-ast  riu-check  riu-test-runner  riurt  llvm
无目标时构建 default（全部 exe）。
'@
}

foreach ($a in $args) {
    $s = [string]$a
    if ($s -in @('-h', '--help', '-Help', '/?')) { Show-Help; exit 0 }
    elseif ($s -in @('-Release', '--release')) { $Mode = 'release' }
    elseif ($s -in @('-GenOnly', '--gen-only')) { $GenOnly = $true }
    elseif ($s -in @('test', '-Test', '--test')) { $DoTest = $true }
    elseif ($s -in @('pack', '-Pack', '--pack')) { $DoPack = $true }
    elseif ($DoTest) { [void]$Forward.Add($s) }
    else { [void]$NinjaTargets.Add($s) }
}

function ConvertTo-GnPath([string]$Path) {
    return ($Path -replace '\\', '/')
}

function Get-RiuVersion {
    $gni = Join-Path $ProjectRoot 'build\version.gni'
    foreach ($line in Get-Content -LiteralPath $gni) {
        if ($line -match 'riu_version\s*=\s*"([^"]+)"') { return $Matches[1] }
    }
    throw 'riu_version not found in build/version.gni'
}

function Find-Tool([string]$Name) {
    $candidates = @(
        (Join-Path $ProjectRoot "bin\$Name.exe")
    )
    foreach ($p in $candidates) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "$Name not found (run ./sync-deps.ps1 for ninja, or put $Name.exe in bin\ / PATH)"
}

function Ensure-SdkLink([string]$OutDir) {
    $link = Join-Path $OutDir 'sdk'
    $src = Join-Path $ProjectRoot 'sdk'
    $want = [System.IO.Path]::GetFullPath($src).TrimEnd('\', '/')
    $ok = $false
    if (Test-Path -LiteralPath $link) {
        $item = Get-Item -LiteralPath $link -Force
        $t = $null
        if ($item.LinkType -in @('Junction', 'SymbolicLink')) {
            $t = $item.Target
            if ($t -is [array]) { $t = $t[0] }
        }
        if ($t) {
            $got = [System.IO.Path]::GetFullPath([string]$t).TrimEnd('\', '/')
            if ($got.ToLowerInvariant() -eq $want.ToLowerInvariant()) { $ok = $true }
        }
        if (-not $ok) {
            $was = if ($t) { $t } else { 'not a junction' }
            Write-Log "sdk junction retarget: $link ($was) → $src" DarkYellow
            # rmdir 只拆 junction，不删目标；普通目录非空会失败
            cmd.exe /c "rmdir `"$link`"" | Out-Null
            if (Test-Path -LiteralPath $link) {
                throw "failed to remove stale sdk path at $link (want junction → $src)"
            }
        }
    }
    if ($ok) { return }
    Write-Log "sdk junction: $link → sdk/" DarkGray
    cmd.exe /c "mklink /J `"$link`" `"$src`"" | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $link)) {
        throw "failed to create sdk junction at $link"
    }
}

function Write-ArgsGn([string]$OutDir, [string]$LlvmDir, [string]$VersionStr) {
    $argsFile = Join-Path $OutDir 'args.gn'
    $isDebug = if ($Mode -eq 'debug') { 'true' } else { 'false' }
    $content = @"
is_debug = $isDebug
riu_version_str = "$VersionStr"
llvm_build_dir = "$(ConvertTo-GnPath $LlvmDir)"
"@
    $existing = ''
    if (Test-Path -LiteralPath $argsFile) {
        $existing = Get-Content -LiteralPath $argsFile -Raw -ErrorAction SilentlyContinue
    }
    $normNew = ($content -replace '\r\n', "`n").Trim() + "`n"
    $normOld = if ($existing) { ($existing -replace '\r\n', "`n").Trim() + "`n" } else { '' }
    if ($normOld -ne $normNew) {
        [System.IO.File]::WriteAllText($argsFile, $normNew)
        return $true
    }
    return $false
}

$OutDir = Join-Path $ProjectRoot "build\windows\x64\$Mode"
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$LlvmDir = Join-Path $OutDir 'llvm'

$version = Get-RiuVersion
$versionStr = "v$version-$(Get-Date -Format 'yyyy-MM-dd')"
$argsChanged = Write-ArgsGn -OutDir $OutDir -LlvmDir $LlvmDir -VersionStr $versionStr

$gn = Find-Tool 'gn'
$ninja = Find-Tool 'ninja'
$buildNinja = Join-Path $OutDir 'build.ninja'
$needGen = $argsChanged -or -not (Test-Path -LiteralPath $buildNinja)

if ($needGen) {
    Write-Log "`n=== gn gen $OutDir ===" Cyan
    Push-Location $ProjectRoot
    try {
        & $gn gen $OutDir --export-compile-commands
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        Pop-Location
    }
} else {
    Write-Log "gn: $OutDir (up to date)" DarkGray
}

Ensure-SdkLink $OutDir

if ($GenOnly) {
    Write-Log 'gen-only; skip ninja.' Green
    exit 0
}

if ($DoPack) {
    $pack = Join-Path $ProjectRoot 'scripts\pack.ps1'
    & $pack -OutDir $OutDir -Version $version
    exit $LASTEXITCODE
}

if ($DoTest) {
    if ($NinjaTargets.Count -eq 0) {
        Write-Log "`n=== ninja riu ===" Cyan
        & $ninja -C $OutDir riu
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    $riuExe = Join-Path $OutDir 'bin\riu.exe'
    $runner = Join-Path $ProjectRoot 'tests\run.ps1'
    if ($Forward.Count -gt 0) {
        $runnerArgs = @('-RiuExe', $riuExe)
        $runnerArgs += $Forward.ToArray()
        & $runner @runnerArgs
    } else {
        & $runner -RiuExe $riuExe
    }
    exit $LASTEXITCODE
}

$ninjaArgs = @('-C', $OutDir)
if ($NinjaTargets.Count -gt 0) {
    $ninjaArgs += $NinjaTargets.ToArray()
}
Write-Log ("`n=== ninja {0} ===" -f ($ninjaArgs -join ' ')) Cyan
& $ninja @ninjaArgs
$code = $LASTEXITCODE
if ($code -ne 0) { exit $code }
Write-Log "`nbuild ok: $OutDir" Green
exit 0
