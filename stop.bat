@echo off
setlocal
set "REPO_ROOT=%~dp0"
where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    if exist "%REPO_ROOT%scripts\services.ps1" (
        pwsh -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%scripts\services.ps1" -Action stop
    )
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%sandal.ps1" down %*
    exit /b %ERRORLEVEL%
)
if exist "%REPO_ROOT%scripts\services.ps1" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%scripts\services.ps1" -Action stop
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%sandal.ps1" down %*
exit /b %ERRORLEVEL%
