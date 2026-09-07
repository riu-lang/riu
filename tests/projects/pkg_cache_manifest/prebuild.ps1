$ErrorActionPreference = 'Stop'

$pkgPath = Join-Path $PSScriptRoot 'src\pkg'
$buildDir = Join-Path $PSScriptRoot 'build'
$utf8 = New-Object System.Text.UTF8Encoding $false
$finalPkg = "helper as public_helper`n"

try {
    [IO.File]::WriteAllText($pkgPath, "helper`n", $utf8)
    $null = & $env:YuxExe build pkg_cache_manifest 2>&1
    if ($LASTEXITCODE -ne 0) { throw 'initial build failed' }

    [IO.File]::WriteAllText($pkgPath, $finalPkg, $utf8)
    $second = (& $env:YuxExe build pkg_cache_manifest 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "incremental build failed`n$second" }
    if ($second -notmatch 'Compile IR') {
        throw "changing src/pkg did not invalidate package objects`n$second"
    }
} finally {
    [IO.File]::WriteAllText($pkgPath, $finalPkg, $utf8)
    if (Test-Path -LiteralPath $buildDir) {
        Remove-Item -LiteralPath $buildDir -Recurse -Force
    }
}
