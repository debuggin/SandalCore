@echo off
setlocal
if /i "%~1"=="--help" goto :help
if /i "%~1"=="-h" goto :help
if "%~1"=="/?" goto :help
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" build %*
exit /b %ERRORLEVEL%
:help
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" help
exit /b %ERRORLEVEL%
