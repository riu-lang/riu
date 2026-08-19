#Requires -Version 5.1
<#
.SYNOPSIS
  项目编译+运行 / 格式化回归（原 xmake test）。
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectsDir = Join-Path $PSScriptRoot 'projects'
$YuxExe = $null
$Group = $null
$VerboseLog = $false
$Names = New-Object System.Collections.Generic.List[string]

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

$i = 0
$argv = @($args)
while ($i -lt $argv.Count) {
    $a = [string]$argv[$i]
    if ($a -in @('-YuxExe', '--yux')) {
        $i++
        $YuxExe = [string]$argv[$i]
    } elseif ($a -in @('-Group', '--group', '-g')) {
        $i++
        $Group = [string]$argv[$i]
    } elseif ($a -in @('-Verbose', '--verbose', '-v')) {
        $VerboseLog = $true
    } elseif ($a.Length -gt 0 -and -not $a.StartsWith('-')) {
        $n = $a
        if ($n.StartsWith('project_')) { $n = $n.Substring(8) }
        if ($n.StartsWith('yux_tests/')) { $n = $n.Substring(10) }
        if ($n.StartsWith('project_')) { $n = $n.Substring(8) }
        [void]$Names.Add($n)
    }
    $i++
}

if (-not $YuxExe) {
    $cand = Join-Path $ProjectRoot 'build\windows\x64\debug\bin\yux.exe'
    if (Test-Path -LiteralPath $cand) { $YuxExe = $cand }
}
if (-not $YuxExe) {
    $cmd = Get-Command yux -ErrorAction SilentlyContinue
    if ($cmd) { $YuxExe = $cmd.Source }
}
if (-not $YuxExe -or -not (Test-Path -LiteralPath $YuxExe)) {
    throw 'yux.exe not found. Build with ./build.ps1 yux first, or pass -YuxExe.'
}

function Get-Cases {
    $list = @()
    Get-ChildItem -LiteralPath $ProjectsDir -Directory | ForEach-Object {
        $d = $_.FullName
        $toml = Join-Path $d 'yux.toml'
        if (-not (Test-Path -LiteralPath $toml)) { return }
        $expected = Join-Path $d 'expected.txt'
        $fmt = Join-Path $d 'expected_format'
        $kind = $null
        if (Test-Path -LiteralPath $expected) { $kind = 'project' }
        elseif (Test-Path -LiteralPath $fmt) { $kind = 'format' }
        if (-not $kind) { return }
        $list += [pscustomobject]@{
            Name = $_.Name
            Dir  = $d
            Kind = $kind
        }
    }
    return $list
}

function Strip-Cr([string]$s) {
    if ($null -eq $s) { return '' }
    return ($s -replace "`r", '')
}

function Invoke-Capture([string]$File, [string[]]$CmdArgs, [string]$WorkDir) {
    $outFile = [IO.Path]::GetTempFileName()
    $errFile = [IO.Path]::GetTempFileName()
    try {
    $p = if ($CmdArgs.Count -eq 0) {
        Start-Process -FilePath $File -WorkingDirectory $WorkDir `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    } else {
        Start-Process -FilePath $File -ArgumentList $CmdArgs -WorkingDirectory $WorkDir `
            -NoNewWindow -Wait -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    }
        $stdout = [IO.File]::ReadAllText($outFile)
        $stderr = [IO.File]::ReadAllText($errFile)
        return [pscustomobject]@{
            ExitCode = $p.ExitCode
            Stdout   = $stdout
            Stderr   = $stderr
        }
    } finally {
        Remove-Item -LiteralPath $outFile, $errFile -ErrorAction SilentlyContinue
    }
}

$cases = @(Get-Cases)
if ($Group) {
    $g = $Group.ToLowerInvariant()
    if ($g -in @('yux/project', 'project')) { $cases = @($cases | Where-Object { $_.Kind -eq 'project' }) }
    elseif ($g -in @('yux/format', 'format')) { $cases = @($cases | Where-Object { $_.Kind -eq 'format' }) }
    else { throw "unknown group: $Group (use project or format)" }
}
if ($Names.Count -gt 0) {
    $want = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in $Names) { [void]$want.Add($n) }
    $cases = @($cases | Where-Object { $want.Contains($_.Name) })
    if ($cases.Count -eq 0) { throw "no matching tests: $($Names -join ', ')" }
}

$pass = 0
$fail = 0
$failed = New-Object System.Collections.Generic.List[string]

Write-Log "yux: $YuxExe" DarkGray
Write-Log "cases: $($cases.Count)`n" Cyan

foreach ($c in $cases) {
    $ok = $false
    $err = ''
    if ($c.Kind -eq 'project') {
        $buildDir = Join-Path $c.Dir 'build'
        if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
        $r = Invoke-Capture $YuxExe @('build', $c.Name) $c.Dir
        $exe = Join-Path $buildDir "$($c.Name).exe"
        if ($r.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
            $err = "compile failed`n$($r.Stderr)$($r.Stdout)"
        } else {
            $run = Invoke-Capture $exe @() $c.Dir
            $expected = [IO.File]::ReadAllText((Join-Path $c.Dir 'expected.txt'))
            if ($run.ExitCode -ne 0) {
                $err = "run failed exit $($run.ExitCode)`n$($run.Stderr)$($run.Stdout)"
            } elseif ($run.Stdout -ne $expected) {
                $err = "output mismatch`n--- expected ---`n$expected`n--- actual ---`n$($run.Stdout)"
            } else {
                $ok = $true
            }
        }
        if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue }
    } else {
        $toml = Get-Content -LiteralPath (Join-Path $c.Dir 'yux.toml') -Raw
        if ($toml -notmatch 'entry\s*=\s*"([^"]+)"') {
            $err = "yux.toml missing entry"
        } else {
            $srcFile = Join-Path $c.Dir "src\$($Matches[1])"
            if (-not (Test-Path -LiteralPath $srcFile)) {
                $err = "source not found: $srcFile"
            } else {
                $r = Invoke-Capture $YuxExe @('format', $srcFile) $c.Dir
                if ($r.ExitCode -ne 0) {
                    $err = "format failed`n$($r.Stderr)$($r.Stdout)"
                } else {
                    $expected = [IO.File]::ReadAllText((Join-Path $c.Dir 'expected_format'))
                    if ((Strip-Cr $r.Stdout) -ne (Strip-Cr $expected)) {
                        $err = "format mismatch`n--- expected ---`n$expected`n--- actual ---`n$($r.Stdout)"
                    } else {
                        $ok = $true
                    }
                }
            }
        }
    }

    if ($ok) {
        $pass++
        Write-Log "[pass] $($c.Kind)/$($c.Name)" Green
    } else {
        $fail++
        [void]$failed.Add("$($c.Kind)/$($c.Name)")
        Write-Log "[fail] $($c.Kind)/$($c.Name)" Red
        if ($VerboseLog -or $true) {
            Write-Host $err
        }
    }
}

Write-Log "`n$pass passed, $fail failed, $($cases.Count) total" $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if ($fail -ne 0) {
    Write-Log ("failed: " + ($failed -join ', ')) Red
    exit 1
}
exit 0
