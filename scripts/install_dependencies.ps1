<#
.SYNOPSIS
  Checks for the Windows host tools Sandal Core needs to build and run, and installs what is missing.

.DESCRIPTION
  Checks: winget, Git, Go, .NET 10 SDK (SandalLauncher, aoc-extractor, mapc), WSL2, and Docker (Docker Desktop or standalone Windows Docker CLI + Compose + Buildx connected to WSL2 Docker Engine).
  A Go older than src\server\services\go.mod asks for is upgraded with winget the same way; without winget the script points to the Windows installer at https://go.dev/dl/.
  All C++ (world) and Go microservices compile inside Linux Docker containers.

.PARAMETER CheckOnly
  Only report what is installed and what is missing; change nothing.
.PARAMETER Yes
  Install everything missing without asking.
.PARAMETER SkipDocker
  Do not offer Docker / WSL2 Docker Engine setup.
#>
#Requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$CheckOnly,
    [switch]$Yes,
    [switch]$SkipDocker = $false
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent

$wslCfgLib = Join-Path $repoRoot 'scripts\lib\wsl-config.ps1'
$wslDockerLib = Join-Path $repoRoot 'scripts\lib\wsl-docker.ps1'
if (Test-Path -LiteralPath $wslCfgLib -PathType Leaf) { . $wslCfgLib }
if (Test-Path -LiteralPath $wslDockerLib -PathType Leaf) { . $wslDockerLib }

function Find-Winget {
    $c = Get-Command winget -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    if ($env:LOCALAPPDATA) {
        $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
        if (Test-Path -LiteralPath $alias -PathType Leaf) { return $alias }
    }
    return $null
}

function Update-SessionPath {
    $sep = [System.IO.Path]::PathSeparator
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + $sep +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

function Find-DotnetSdk10 {
    $candidates = @()
    $dn = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dn) { $candidates += $dn.Source }
    $userDotnetDir = Join-Path $HOME '.dotnet'
    foreach ($binName in @('dotnet.exe', 'dotnet')) {
        $userExe = Join-Path $userDotnetDir $binName
        if ((Test-Path -LiteralPath $userExe -PathType Leaf) -and ($userExe -notin $candidates)) { $candidates += $userExe }
    }
    foreach ($exe in $candidates) {
        foreach ($s in @(& $exe --list-sdks 2>$null)) {
            if ($s -match '^(\d+)\.' -and [int]$Matches[1] -ge 10) { return $exe }
        }
    }
    return $null
}

function Get-GoRequired {
    $goMod = Join-Path $repoRoot 'src\server\services\go.mod'
    if (Test-Path -LiteralPath $goMod -PathType Leaf) {
        foreach ($line in (Get-Content -LiteralPath $goMod)) {
            if ($line -match '^go\s+(\d+\.\d+)') { return $Matches[1] }
        }
    }
    return '1.24'
}

function Get-GoVersion {
    $go = Get-Command go -ErrorAction SilentlyContinue
    if (-not $go) { return $null }
    # GOTOOLCHAIN=local: report the Go that is installed, never one that go would fetch for the go line of a go.mod in the current folder
    $oldToolchain = $env:GOTOOLCHAIN
    $env:GOTOOLCHAIN = 'local'
    try { $v = & go env GOVERSION 2>$null } finally { $env:GOTOOLCHAIN = $oldToolchain }
    if ($v) { return $v.Trim() }
    return 'unknown'
}

function Test-VersionAtLeast([string]$have, [string]$need) {
    $h = ($have -replace '^go', '') -replace '[^0-9.].*$', ''
    $n = ($need -replace '^go', '') -replace '[^0-9.].*$', ''
    try { return ([version]$h -ge [version]$n) } catch { return $false }
}

function Test-Wsl2HostReady {
    if (-not $IsWindows -or $null -eq (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
        return (-not $IsWindows)
    }
    $null = & wsl.exe --status *> $null
    if ($LASTEXITCODE -ne 0) { return $false }
    $distros = @((@(& wsl.exe -l -q 2>$null) -replace "`0", '') | ForEach-Object { ($_ -replace '^\*\s*', '').Trim() } | Where-Object { $_ -and $_ -notlike 'docker-desktop*' })
    return ($distros.Count -gt 0)
}

function Get-DockerCliState {
    $hasCli = $null -ne (Get-Command docker -ErrorAction SilentlyContinue)
    $hasCompose = $false
    $hasBuildx = $false
    $desktopInstalled = $IsWindows -and (Test-Path -LiteralPath (Join-Path $env:ProgramFiles 'Docker\Docker\Docker Desktop.exe') -PathType Leaf)
    $desktopActive = $false
    if ($hasCli) {
        $null = & docker compose version *> $null
        $hasCompose = ($LASTEXITCODE -eq 0)
        $null = & docker buildx version *> $null
        $hasBuildx = ($LASTEXITCODE -eq 0)
        if ($env:DOCKER_HOST -notin @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
            $null = & docker info *> $null
            if ($LASTEXITCODE -eq 0) { $desktopActive = $true }
        }
        $hasDesktopPipe = $IsWindows -and (
            ($null -ne (Get-Process -Name 'Docker Desktop' -ErrorAction SilentlyContinue)) -or
            [System.IO.File]::Exists('\\.\pipe\docker_engine') -or
            [System.IO.File]::Exists('\\.\pipe\dockerDesktopLinuxEngine')
        )
        if (-not $desktopActive -and $hasDesktopPipe) {
            $null = & docker --host npipe:////./pipe/docker_engine info *> $null
            if ($LASTEXITCODE -eq 0) { $desktopActive = $true }
        }
        if (-not $desktopActive -and $hasDesktopPipe) {
            $null = & docker --context desktop-linux info *> $null
            if ($LASTEXITCODE -eq 0) { $desktopActive = $true }
        }
    }
    return [pscustomobject]@{
        HasCli           = $hasCli
        HasCompose       = $hasCompose
        HasBuildx        = $hasBuildx
        DesktopInstalled = $desktopInstalled
        DesktopActive    = $desktopActive
        AllCliReady      = ($hasCli -and $hasCompose -and $hasBuildx)
    }
}

function Confirm-Install([string]$what) {
    if ($Yes) { return $true }
    $a = Read-Host "Install $what now? [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

function Install-Winget([string]$id, [string]$name, [string[]]$extra = @(), [switch]$Upgrade) {
    # -Upgrade: the package is installed but too old; "winget install" does not replace an installed version, "winget upgrade" does
    $verb = if ($Upgrade) { 'upgrade' } else { 'install' }
    Write-Host ("{0} {1} (winget id {2}) ..." -f $(if ($Upgrade) { 'Upgrading' } else { 'Installing' }), $name, $id) -ForegroundColor Cyan
    $wingetArgs = @($verb, '--id', $id, '-e', '--source', 'winget',
              '--accept-package-agreements', '--accept-source-agreements') + $extra
    & $script:winget @wingetArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "winget could not $verb $name (exit $LASTEXITCODE). Do it by hand, then run this script again."
        return $false
    }
    Update-SessionPath
    return $true
}

# Go comes from go.dev: its Windows installer (.msi) replaces an older Go in place, so this works with or without winget
function Show-GoByHand {
    $have = if ($goHave) { " (this PC has $goHave)" } else { '' }
    Write-Warning ("Go $goNeed or newer is needed$have. Download the Windows installer from https://go.dev/dl/ and run it (it replaces an older Go), " +
                   'then open a new window and run this script again.')
}

function Install-MissingDockerComponents {
    $state = Get-DockerCliState
    if ($state.HasCli) {
        Write-Host 'INFO: Docker CLI already installed; skipping Docker.DockerCLI installation.' -ForegroundColor Cyan
    } else {
        [void](Install-Winget 'Docker.DockerCLI' 'Docker CLI')
    }
    $state = Get-DockerCliState
    if (-not $state.HasCompose) {
        [void](Install-Winget 'Docker.DockerCompose' 'Docker Compose')
    }
    if (-not $state.HasBuildx) {
        [void](Install-Winget 'Docker.DockerBuildx' 'Docker Buildx')
    }
    $state = Get-DockerCliState
    if ($state.DesktopActive) {
        if ($env:DOCKER_HOST -in @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
            Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
        }
        Write-Host 'INFO: Active Docker Desktop daemon detected; preserving existing Docker context and skipping DOCKER_HOST=tcp://127.0.0.1:2375 override.' -ForegroundColor Cyan
        return
    }
    [Environment]::SetEnvironmentVariable('DOCKER_HOST', 'tcp://127.0.0.1:2375', 'User')
    $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
    if (Get-Command Ensure-LinuxDockerEngine -ErrorAction SilentlyContinue) {
        $null = Ensure-LinuxDockerEngine -Distro 'Ubuntu-24.04'
    } else {
        $wslDockerInstaller = Join-Path $PSScriptRoot 'sandal\Install-WslDocker.ps1'
        if (Test-Path -LiteralPath $wslDockerInstaller -PathType Leaf) {
            & $wslDockerInstaller
        }
    }
}

$script:winget = Find-Winget
$goNeed = Get-GoRequired
$goHave = Get-GoVersion
$goOk = [bool]($goHave -and (-not $goNeed -or (Test-VersionAtLeast $goHave $goNeed)))
$goOld = [bool]($goHave -and -not $goOk)
$dockerCliState = if ($SkipDocker) { [pscustomobject]@{ AllCliReady = [bool](Get-Command docker -ErrorAction SilentlyContinue) } } else { Get-DockerCliState }
$items = [ordered]@{
    'winget'      = [bool]$script:winget
    'Git'         = [bool](Get-Command git -ErrorAction SilentlyContinue)
    'Go'          = $goOk
    '.NET 10 SDK' = [bool](Find-DotnetSdk10)
    'WSL2'        = [bool](Test-Wsl2HostReady)
    'Docker'      = [bool]$dockerCliState.AllCliReady
}

Write-Host ''
Write-Host 'Sandal Core host dependency check' -ForegroundColor White
Write-Host '---------------------------------'
foreach ($k in $items.Keys) {
    $state = if ($items[$k]) { 'OK' } else { 'MISSING' }
    $color = if ($items[$k]) { 'Green' } else { 'Yellow' }
    $note = if (($k -in @('WSL2', 'Docker')) -and $SkipDocker) { '  (skipped: -SkipDocker)' } else { '' }
    Write-Host ("  {0,-14} {1}{2}" -f $k, $state, $note) -ForegroundColor $color
}
Write-Host ''

$missing = @($items.Keys | Where-Object { -not $items[$_] -and -not (($_ -in @('WSL2', 'Docker')) -and $SkipDocker) })
if ($CheckOnly) {
    if ($missing.Count) { Write-Host "Missing: $($missing -join ', ')"; exit 1 }
    Write-Host 'Everything required is installed.'; exit 0
}
if (-not $missing.Count) {
    Write-Host 'Everything is installed. Next: pwsh ./sandal.ps1 run (or start.bat).' -ForegroundColor Green
    exit 0
}

if (-not $script:winget) {
    if (-not $items['Go']) { Show-GoByHand }
    if ($missing.Count -eq 1 -and $missing[0] -eq 'Go') { exit 1 }
    Write-Warning 'winget is missing. Install "App Installer" from the Microsoft Store, then run this script again.'
    exit 1
}

if (-not $items['Git'] -and (Confirm-Install 'Git')) {
    [void](Install-Winget 'Git.Git' 'Git')
}
if (-not $items['Go'] -and (Confirm-Install $(if ($goOld) { "Go $goNeed or newer (it replaces the Go $goHave that is installed)" } else { "Go $goNeed or newer" }))) {
    [void](Install-Winget 'GoLang.Go' 'Go' @() -Upgrade:$goOld)
    if ($goOld) {
        $goNow = Get-GoVersion
        if (-not ($goNow -and (Test-VersionAtLeast $goNow $goNeed))) { Show-GoByHand }
    }
}
if (-not $items['.NET 10 SDK'] -and (Confirm-Install '.NET 10 SDK (builds SandalLauncher, aoc-extractor, and mapc)')) {
    [void](Install-Winget 'Microsoft.DotNet.SDK.10' '.NET 10 SDK')
}
if (-not $SkipDocker -and -not $items['WSL2'] -and (Confirm-Install 'WSL2 Ubuntu environment (wsl.exe --status / Ensure-WslUbuntu)')) {
    if (Get-Command Ensure-WslUbuntu -ErrorAction SilentlyContinue) {
        $null = Ensure-WslUbuntu -Distro 'Ubuntu-24.04'
    } else {
        & wsl.exe --install -d Ubuntu-24.04
    }
}
if (-not $SkipDocker -and -not $items['Docker'] -and
    (Confirm-Install 'Windows Docker CLI + Compose + Buildx + WSL2 Docker Engine (or Docker Desktop)')) {
    Install-MissingDockerComponents
}

Write-Host ''
Write-Host 'Done. Open a NEW terminal (so PATH changes apply) and run this script with -CheckOnly to confirm.' -ForegroundColor Green
Write-Host 'Then run: pwsh ./sandal.ps1 run (or start.bat)'
exit 0
