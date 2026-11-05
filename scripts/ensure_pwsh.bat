@echo off
setlocal EnableExtensions

set "PWSH_EXE="
where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set "PWSH_EXE=pwsh"
    goto :run_pwsh
)

if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" (
    set "PWSH_EXE=%ProgramFiles%\PowerShell\7\pwsh.exe"
    goto :run_pwsh
)

echo PowerShell 7 (pwsh) is required to run SandalCore scripts.
set "INSTALL_PWSH=Y"
if not "%SANDAL_ASSUME_YES%"=="1" (
    set /p "INSTALL_PWSH=Install the latest PowerShell 7 via winget now? [Y/n]: "
)

if /i "%INSTALL_PWSH%"=="" set "INSTALL_PWSH=Y"
if /i not "%INSTALL_PWSH%"=="Y" if /i not "%INSTALL_PWSH%"=="YES" (
    echo ERROR: PowerShell 7 installation declined. Install Microsoft.PowerShell 7+ and retry. 1>&2
    exit /b 1
)

where winget >nul 2>nul
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: winget is not available. Install PowerShell 7 from https://aka.ms/powershell-release?tag=stable and retry. 1>&2
    exit /b 1
)

echo Installing PowerShell 7 (Microsoft.PowerShell) via winget...
winget install --id Microsoft.PowerShell --source winget --accept-package-agreements --accept-source-agreements
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: winget failed to install Microsoft.PowerShell. Install PowerShell 7 manually and retry. 1>&2
    exit /b 1
)

set "PATH=%ProgramFiles%\PowerShell\7;%PATH%"
where pwsh >nul 2>nul
if %ERRORLEVEL% EQU 0 (
    set "PWSH_EXE=pwsh"
    goto :run_pwsh
)
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" (
    set "PWSH_EXE=%ProgramFiles%\PowerShell\7\pwsh.exe"
    goto :run_pwsh
)

echo ERROR: PowerShell 7 (pwsh.exe) could not be found after installation. Open a new terminal and retry. 1>&2
exit /b 1

:run_pwsh
echo "%*" | findstr /I "\-Json \-\-json" >nul
if %ERRORLEVEL% NEQ 0 if not "%SANDAL_QUIET%"=="1" (
    echo [SandalCore] Using PowerShell 7: %PWSH_EXE%
)
"%PWSH_EXE%" -NoProfile -ExecutionPolicy Bypass %*
exit /b %ERRORLEVEL%
