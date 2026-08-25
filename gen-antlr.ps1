#!/usr/bin/env pwsh
#Requires -Version 5.1
<#
.SYNOPSIS
  Generate C++ parser sources from yuxLexer.g4 / yuxParser.g4 via ANTLR4 jar.
#>
$ErrorActionPreference = 'Stop'

$ProjectRoot = $PSScriptRoot
$LexerGrammar = Join-Path $ProjectRoot 'yux\ast\yuxLexer.g4'
$ParserGrammar = Join-Path $ProjectRoot 'yux\ast\yuxParser.g4'
$OutputDir = Join-Path $ProjectRoot 'yux\ast\gen\yux'
$JarFile = Join-Path $ProjectRoot 'bin\antlr-4.13.2-complete.jar'

function Write-Log([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::White) {
    Write-Host $Message -ForegroundColor $Color
}

Write-Log "`n=== ANTLR4 C++ Code Generator ===`n" Cyan

if (-not (Test-Path -LiteralPath $JarFile)) {
    Write-Log "ANTLR4 jar not found: $JarFile" Red
    Write-Log 'Run ./sync-deps.ps1 first to download the jar file.' Yellow
    exit 1
}

foreach ($g in @($LexerGrammar, $ParserGrammar)) {
    if (-not (Test-Path -LiteralPath $g)) {
        Write-Log "Grammar file not found: $g" Red
        exit 1
    }
}

New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null

Write-Log "Lexer:  $LexerGrammar"
Write-Log "Parser: $ParserGrammar"
Write-Log "Output: $OutputDir"
Write-Log "JAR:    $JarFile`n"

# Lexer first — parser uses tokenVocab=yuxLexer
foreach ($g in @($LexerGrammar, $ParserGrammar)) {
    $cmdArgs = @(
        '-jar', $JarFile,
        '-Dlanguage=Cpp',
        '-package', 'yux',
        '-visitor',
        '-no-listener',
        '-o', $OutputDir,
        '-lib', $OutputDir,
        $g
    )
    Write-Log ("Running: java {0}`n" -f ($cmdArgs -join ' '))
    & java @cmdArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Log "`nGeneration failed for $g!" Red
        exit 1
    }
}

Write-Log "`nGeneration completed successfully!" Green
$files = Get-ChildItem -LiteralPath $OutputDir -File |
    Where-Object { $_.Extension -in '.cpp', '.h' } |
    Select-Object -ExpandProperty Name
Write-Log "`nGenerated $($files.Count) files:" Cyan
foreach ($f in $files) { Write-Log "  - $f" }
