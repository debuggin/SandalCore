@echo off
setlocal
set "REPO_ROOT=%~dp0"
where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%sandal.ps1" run %*
    exit /b %ERRORLEVEL%
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%sandal.ps1" run %*
exit /b %ERRORLEVEL%
