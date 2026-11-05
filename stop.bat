@echo off
setlocal
if exist "%~dp0scripts\services.ps1" (
    call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0scripts\services.ps1" -Action stop >nul 2>&1
)
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" down %*
exit /b %ERRORLEVEL%
