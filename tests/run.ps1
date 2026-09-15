#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  项目编译+运行 / 格式化回归 / 预期编译失败（原 xmake test）。

.PARAMETER Jobs
  并行用例数。0 / 省略 = CPU 核数；1 = 串行。
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ProjectsDir = Join-Path $PSScriptRoot 'projects'
$RiuExe = $null
$Group = $null
$VerboseLog = $false
$Jobs = 0
$Names = New-Object System.Collections.Generic.List[string]

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

$i = 0
$argv = @($args)
while ($i -lt $argv.Count) {
    $a = [string]$argv[$i]
    if ($a -in @('-RiuExe', '--riu')) {
        $i++
        if ($i -ge $argv.Count) { throw '-RiuExe requires a path' }
        $RiuExe = [string]$argv[$i]
    } elseif ($a -in @('-Group', '--group', '-g')) {
        $i++
        if ($i -ge $argv.Count) { throw '-Group requires a name' }
        $Group = [string]$argv[$i]
    } elseif ($a -in @('-Jobs', '--jobs', '-j')) {
        $i++
        if ($i -ge $argv.Count) { throw '-Jobs requires an integer' }
        $Jobs = [int]$argv[$i]
    } elseif ($a -in @('-Verbose', '--verbose', '-v')) {
        $VerboseLog = $true
    } elseif ($a.Length -gt 0 -and -not $a.StartsWith('-')) {
        $n = $a
        if ($n.StartsWith('project_')) { $n = $n.Substring(8) }
        if ($n.StartsWith('riu_tests/')) { $n = $n.Substring(10) }
        if ($n.StartsWith('project_')) { $n = $n.Substring(8) }
        [void]$Names.Add($n)
    }
    $i++
}

if (-not $RiuExe) {
    $cand = Join-Path $ProjectRoot 'build\windows\x64\debug\bin\riu.exe'
    if (Test-Path -LiteralPath $cand) { $RiuExe = $cand }
}
if (-not $RiuExe) {
    $cmd = Get-Command riu -ErrorAction SilentlyContinue
    if ($cmd) { $RiuExe = $cmd.Source }
}
if (-not $RiuExe -or -not (Test-Path -LiteralPath $RiuExe)) {
    throw 'riu.exe not found. Build with ./build.ps1 riu first, or pass -RiuExe.'
}

function Get-Cases {
    $list = @()
    Get-ChildItem -LiteralPath $ProjectsDir -Directory | ForEach-Object {
        $d = $_.FullName
        $toml = Join-Path $d 'riu.toml'
        if (-not (Test-Path -LiteralPath $toml)) { return }
        $expected = Join-Path $d 'expected.txt'
        $fmt = Join-Path $d 'expected_format'
        $fail = Join-Path $d 'expected_fail.txt'
        $kind = $null
        if (Test-Path -LiteralPath $expected) { $kind = 'project' }
        elseif (Test-Path -LiteralPath $fmt) { $kind = 'format' }
        elseif (Test-Path -LiteralPath $fail) { $kind = 'fail' }
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
    # 进程内读管道，避免并行时 GetTempFileName + Start-Process 抢同一临时文件。
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.WorkingDirectory = $WorkDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $utf8 = New-Object System.Text.UTF8Encoding $false
    $psi.StandardOutputEncoding = $utf8
    $psi.StandardErrorEncoding = $utf8

    if ($null -eq $CmdArgs) { $CmdArgs = @() }
    $argListProp = $psi.GetType().GetProperty('ArgumentList')
    if ($null -ne $argListProp -and $CmdArgs.Count -gt 0) {
        foreach ($a in $CmdArgs) { [void]$psi.ArgumentList.Add($a) }
    } elseif ($CmdArgs.Count -gt 0) {
        $quoted = foreach ($a in $CmdArgs) {
            if ($a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
        }
        $psi.Arguments = [string]::Join(' ', [string[]]$quoted)
    }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $proc.WaitForExit()
    $stdout = $outTask.GetAwaiter().GetResult()
    $stderr = $errTask.GetAwaiter().GetResult()
    $code = $proc.ExitCode
    $proc.Dispose()
    return [pscustomobject]@{
        ExitCode = $code
        Stdout   = $stdout
        Stderr   = $stderr
    }
}

function Invoke-OneCase {
    param(
        [Parameter(Mandatory)]
        $Case,
        [Parameter(Mandatory)]
        [string]$RiuExe
    )
    $ok = $false
    $err = ''
    try {
        if ($Case.Kind -eq 'project') {
            $buildDir = Join-Path $Case.Dir 'build'
            if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
            $pre = Join-Path $Case.Dir 'prebuild.ps1'
            if (Test-Path -LiteralPath $pre) {
                $env:RiuExe = $RiuExe
                $pwsh = $null
                $cmd = Get-Command pwsh -ErrorAction SilentlyContinue
                if ($cmd) { $pwsh = $cmd.Source } else {
                    $cmd = Get-Command powershell -ErrorAction SilentlyContinue
                    if ($cmd) { $pwsh = $cmd.Source }
                }
                if (-not $pwsh) {
                    $err = "prebuild.ps1 present but pwsh/powershell not found"
                } else {
                    $preR = Invoke-Capture $pwsh @('-NoProfile', '-File', $pre) $Case.Dir
                    if ($preR.ExitCode -ne 0) {
                        $err = "prebuild failed`n$($preR.Stderr)$($preR.Stdout)"
                    }
                }
            }
            if ($err -eq '') {
                $r = Invoke-Capture $RiuExe @('build', $Case.Name) $Case.Dir
                $exe = Join-Path $buildDir "$($Case.Name).exe"
                if ($r.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $exe)) {
                    $err = "compile failed`n$($r.Stderr)$($r.Stdout)"
                } else {
                    $exeArgs = @()
                    $runArgsFile = Join-Path $Case.Dir 'run_args.txt'
                    if (Test-Path -LiteralPath $runArgsFile) {
                        $exeArgs = @(
                            Get-Content -LiteralPath $runArgsFile |
                                Where-Object { $_.Trim().Length -gt 0 -and -not $_.Trim().StartsWith('#') }
                        )
                    }
                    $run = Invoke-Capture $exe $exeArgs $Case.Dir
                    $expected = [IO.File]::ReadAllText((Join-Path $Case.Dir 'expected.txt'))
                    if ($run.ExitCode -ne 0) {
                        $err = "run failed exit $($run.ExitCode)`n$($run.Stderr)$($run.Stdout)"
                    } elseif ($run.Stdout -ne $expected) {
                        $err = "output mismatch`n--- expected ---`n$expected`n--- actual ---`n$($run.Stdout)"
                    } else {
                        $ok = $true
                    }
                }
                if (Test-Path -LiteralPath $buildDir) {
                    Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        } elseif ($Case.Kind -eq 'fail') {
            $buildDir = Join-Path $Case.Dir 'build'
            if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
            $r = Invoke-Capture $RiuExe @('build', $Case.Name) $Case.Dir
            $combined = (Strip-Cr "$($r.Stderr)$($r.Stdout)")
            if ($r.ExitCode -eq 0) {
                $err = "expected compile failure, got exit 0`n$combined"
            } else {
                $needles = Get-Content -LiteralPath (Join-Path $Case.Dir 'expected_fail.txt')
                $missing = @()
                foreach ($line in $needles) {
                    $t = $line.Trim()
                    if ($t.Length -eq 0 -or $t.StartsWith('#')) { continue }
                    if ($combined.IndexOf($t) -lt 0) { $missing += $t }
                }
                if ($missing.Count -gt 0) {
                    $err = "stderr missing:`n$($missing -join "`n")`n--- actual ---`n$combined"
                } else {
                    $ok = $true
                }
            }
            if (Test-Path -LiteralPath $buildDir) {
                Remove-Item -LiteralPath $buildDir -Recurse -Force -ErrorAction SilentlyContinue
            }
        } else {
            $toml = Get-Content -LiteralPath (Join-Path $Case.Dir 'riu.toml') -Raw
            if ($toml -notmatch 'entry\s*=\s*"([^"]+)"') {
                $err = "riu.toml missing entry"
            } else {
                $srcFile = Join-Path $Case.Dir "src\$($Matches[1])"
                if (-not (Test-Path -LiteralPath $srcFile)) {
                    $err = "source not found: $srcFile"
                } else {
                    $r = Invoke-Capture $RiuExe @('format', $srcFile) $Case.Dir
                    if ($r.ExitCode -ne 0) {
                        $err = "format failed`n$($r.Stderr)$($r.Stdout)"
                    } else {
                        $expected = [IO.File]::ReadAllText((Join-Path $Case.Dir 'expected_format'))
                        if ((Strip-Cr $r.Stdout) -ne (Strip-Cr $expected)) {
                            $err = "format mismatch`n--- expected ---`n$expected`n--- actual ---`n$($r.Stdout)"
                        } else {
                            $ok = $true
                        }
                    }
                }
            }
        }
    } catch {
        $ok = $false
        $err = $_.Exception.Message
    }
    return [pscustomobject]@{
        Name = $Case.Name
        Kind = $Case.Kind
        Ok   = [bool]$ok
        Err  = $err
    }
}

function Write-CaseResult {
    param($Result)
    if ($Result.Ok) {
        $script:pass++
        Write-Log "[pass] $($Result.Kind)/$($Result.Name)" Green
    } else {
        $script:fail++
        [void]$script:failed.Add("$($Result.Kind)/$($Result.Name)")
        Write-Log "[fail] $($Result.Kind)/$($Result.Name)" Red
        if ($script:VerboseLog -or $true) {
            Write-Host $Result.Err
        }
    }
}

function Invoke-CasesParallel {
    param(
        [array]$Cases,
        [string]$RiuExe,
        [int]$JobCount
    )

    $iss = [System.Management.Automation.Runspaces.InitialSessionState]::CreateDefault()
    foreach ($fn in @('Strip-Cr', 'Invoke-Capture', 'Invoke-OneCase')) {
        $def = (Get-Command $fn).Definition
        [void]$iss.Commands.Add(
            (New-Object System.Management.Automation.Runspaces.SessionStateFunctionEntry $fn, $def)
        )
    }

    $pool = [runspacefactory]::CreateRunspacePool($iss)
    [void]$pool.SetMinRunspaces(1)
    [void]$pool.SetMaxRunspaces($JobCount)
    $pool.Open()

    $running = New-Object System.Collections.Generic.List[object]
    try {
        foreach ($c in $Cases) {
            $ps = [powershell]::Create()
            $ps.RunspacePool = $pool
            [void]$ps.AddCommand('Invoke-OneCase').AddParameter('Case', $c).AddParameter('RiuExe', $RiuExe)
            $running.Add([pscustomobject]@{
                    PS     = $ps
                    Handle = $ps.BeginInvoke()
                    Case   = $c
                })
        }

        while ($running.Count -gt 0) {
            $moved = $false
            for ($idx = $running.Count - 1; $idx -ge 0; $idx--) {
                $j = $running[$idx]
                if (-not $j.Handle.IsCompleted) { continue }
                $moved = $true
                $r = $null
                try {
                    $out = $j.PS.EndInvoke($j.Handle)
                    $r = @($out)[0]
                    if ($null -eq $r) {
                        $errText = 'no result'
                        if ($j.PS.Streams.Error.Count -gt 0) {
                            $errText = $j.PS.Streams.Error[0].ToString()
                        }
                        $r = [pscustomobject]@{
                            Name = $j.Case.Name
                            Kind = $j.Case.Kind
                            Ok   = $false
                            Err  = $errText
                        }
                    }
                } catch {
                    $r = [pscustomobject]@{
                        Name = $j.Case.Name
                        Kind = $j.Case.Kind
                        Ok   = $false
                        Err  = $_.Exception.Message
                    }
                } finally {
                    $j.PS.Dispose()
                }
                Write-CaseResult $r
                $running.RemoveAt($idx)
            }
            if (-not $moved -and $running.Count -gt 0) {
                Start-Sleep -Milliseconds 50
            }
        }
    } finally {
        foreach ($j in $running) {
            try { $j.PS.Stop() } catch {}
            try { $j.PS.Dispose() } catch {}
        }
        $pool.Close()
        $pool.Dispose()
    }
}

$cases = @(Get-Cases)
if ($Group) {
    $g = $Group.ToLowerInvariant()
    if ($g -in @('riu/project', 'project')) { $cases = @($cases | Where-Object { $_.Kind -eq 'project' }) }
    elseif ($g -in @('riu/format', 'format')) { $cases = @($cases | Where-Object { $_.Kind -eq 'format' }) }
    else { throw "unknown group: $Group (use project or format)" }
}
if ($Names.Count -gt 0) {
    $want = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($n in $Names) { [void]$want.Add($n) }
    $cases = @($cases | Where-Object { $want.Contains($_.Name) })
    if ($cases.Count -eq 0) { throw "no matching tests: $($Names -join ', ')" }
}

$jobCount = $Jobs
if ($jobCount -le 0) { $jobCount = [Environment]::ProcessorCount }
if ($jobCount -lt 1) { $jobCount = 1 }
if ($cases.Count -gt 0 -and $jobCount -gt $cases.Count) { $jobCount = $cases.Count }

$pass = 0
$fail = 0
$failed = New-Object System.Collections.Generic.List[string]

Write-Log "riu: $RiuExe" DarkGray
Write-Log "cases: $($cases.Count)  jobs: $jobCount`n" Cyan

if ($jobCount -le 1 -or $cases.Count -le 1) {
    foreach ($c in $cases) {
        Write-CaseResult (Invoke-OneCase -Case $c -RiuExe $RiuExe)
    }
} else {
    Invoke-CasesParallel -Cases $cases -RiuExe $RiuExe -JobCount $jobCount
}

Write-Log "`n$pass passed, $fail failed, $($cases.Count) total" $(if ($fail -eq 0) { 'Green' } else { 'Red' })
if ($fail -ne 0) {
    Write-Log ("failed: " + ($failed -join ', ')) Red
    exit 1
}
exit 0
