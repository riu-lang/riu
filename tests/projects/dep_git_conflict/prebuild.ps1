#!/usr/bin/env pwsh
$ErrorActionPreference = 'Stop'

$utf8 = New-Object System.Text.UTF8Encoding $false
$work = Join-Path $PSScriptRoot 'build\_git_work'
$origin = Join-Path $PSScriptRoot 'build\origin.git'
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $work 'src') | Out-Null

[IO.File]::WriteAllText((Join-Path $work 'riu.toml'), "name=`"utils`"`nversion=`"1.0.0`"`n`n[library]`nlib_mod=`"utils`"`n", $utf8)
[IO.File]::WriteAllText((Join-Path $work 'src\utils.ut'), "fn git_hi() {}`n", $utf8)

$git = @('-c', 'user.name=riu-test', '-c', 'user.email=riu-test@example.invalid', '-c', 'core.autocrlf=false')
Push-Location $work
try {
    & git @git init -q
    if ($LASTEXITCODE -ne 0) { throw 'git init failed' }
    & git @git add -A
    if ($LASTEXITCODE -ne 0) { throw 'git add failed' }
    & git @git commit -q -m init
    if ($LASTEXITCODE -ne 0) { throw 'git commit failed' }
    $rev1 = ((& git @git rev-parse HEAD) | Out-String).Trim()
    [IO.File]::AppendAllText((Join-Path $work 'src\utils.ut'), "; v2`n", $utf8)
    & git @git add -A
    if ($LASTEXITCODE -ne 0) { throw 'git add v2 failed' }
    & git @git commit -q -m v2
    if ($LASTEXITCODE -ne 0) { throw 'git commit v2 failed' }
    $rev2 = ((& git @git rev-parse HEAD) | Out-String).Trim()
    if ($rev1.Length -ne 40 -or $rev2.Length -ne 40 -or $rev1 -eq $rev2) {
        throw "need two commits: $rev1 / $rev2"
    }
} finally {
    Pop-Location
}

& git -c core.autocrlf=false clone -q --bare $work $origin
if ($LASTEXITCODE -ne 0) { throw 'git clone --bare failed' }

function Write-Side([string]$Name, [string]$Rev) {
    $dir = Join-Path $PSScriptRoot "build\$Name"
    New-Item -ItemType Directory -Force -Path (Join-Path $dir 'src') | Out-Null
    $toml = @"
name="$Name"
version="1.0.0"

[dependencies]
utils = { git = "build/origin.git", rev = "$Rev" }

[library]
lib_mod="$Name"
"@
    [IO.File]::WriteAllText((Join-Path $dir 'riu.toml'), (($toml -replace "`r`n", "`n").Trim() + "`n"), $utf8)
    [IO.File]::WriteAllText((Join-Path $dir "src\$Name.ut"), "fn ${Name}_hi() {}`n", $utf8)
}

Write-Side 'a' $rev1
Write-Side 'b' $rev2
