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
} finally {
    Pop-Location
}

& git -c core.autocrlf=false clone -q --bare $work $origin
if ($LASTEXITCODE -ne 0) { throw 'git clone --bare failed' }
