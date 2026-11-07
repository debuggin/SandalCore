@echo off
setlocal
call "%~dp0ensure_pwsh.bat" -NoProfile -File "%~dp0test_functional_apis.ps1" %*
exit /b %ERRORLEVEL%
