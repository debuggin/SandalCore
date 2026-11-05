@echo off
setlocal
echo "%*" | findstr /I "\-Json \-\-json" >nul
if %ERRORLEVEL% NEQ 0 (
    echo [SandalCore] Starting SandalCore stack ^& Launcher ^(start.bat^)...
)
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" run %*
exit /b %ERRORLEVEL%
