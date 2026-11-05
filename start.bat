@echo off
setlocal
call "%~dp0scripts\ensure_pwsh.bat" -File "%~dp0sandal.ps1" run %*
exit /b %ERRORLEVEL%
