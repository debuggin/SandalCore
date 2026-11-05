@echo off
setlocal
if /i "%~1"=="--help" goto :help
if /i "%~1"=="-h" goto :help
if "%~1"=="/?" goto :help
echo "%*" | findstr /I "\-Json \-\-json" >nul
if %ERRORLEVEL% NEQ 0 (
    echo [SandalCore] Building SandalCore C++, Go ^& Launcher artifacts ^(build.bat^)...
)
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" build %*
exit /b %ERRORLEVEL%
:help
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" help
exit /b %ERRORLEVEL%
