@echo off
setlocal

where node >nul 2>&1
if %errorlevel% neq 0 (
    echo Error: node not found in PATH
    exit /b 1
)

node "%~dp0sync-deps.js" %*
