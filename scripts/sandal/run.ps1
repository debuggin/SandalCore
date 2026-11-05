#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
    [switch]$SpawnLauncher,
    [int]$AutomationPort = 0,
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-RunOptions {
    param(
        [string]$BaseProfile,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [bool]$BaseNoColor,
        [string[]]$RawArgs
    )
    $prof = $BaseProfile
    $dry = $BaseDryRun
    $jsn = $BaseJson
    $nc = $BaseNoColor
    $spawn = [bool]$SpawnLauncher -or ($env:SANDAL_SPAWN_LAUNCHER -eq '1')
    $autoPort = if ($AutomationPort -gt 0) { $AutomationPort } elseif ($env:SANDAL_AUTOMATION_PORT -match '^\d+$') { [int]$env:SANDAL_AUTOMATION_PORT } else { 0 }
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-SpawnLauncher|--spawn-launcher)$' { $spawn = $true }
            '^(-AutomationPort|--automation-port)$' {
                if (($i + 1) -lt $RawArgs.Count -and $RawArgs[$i + 1] -match '^\d+$') {
                    $i++
                    $autoPort = [int]$RawArgs[$i]
                }
            }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
            '^(-NoColor|--no-color)$' { $nc = $true }
        }
    }
    return [pscustomobject]@{
        Profile        = $prof
        DryRun         = $dry
        Json           = $jsn
        NoColor        = $nc
        SpawnLauncher  = $spawn
        AutomationPort = $autoPort
    }
}

function Start-SandalLauncherProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [AllowEmptyString()][string]$ExeExtension = '',
        [int]$Port = 0
    )
    $primary = Join-Path $RootDirPath "dist/launcher/SandalLauncher$ExeExtension"
    $fallback = Join-Path $RootDirPath "dist/Release/SandalLauncher$ExeExtension"
    $exePath = if (Test-Path -LiteralPath $primary -PathType Leaf) {
        $primary
    } elseif (Test-Path -LiteralPath $fallback -PathType Leaf) {
        $fallback
    } else {
        throw "SandalLauncher binary not found at '$primary' or '$fallback'."
    }
    $launchArgs = [System.Collections.Generic.List[string]]::new()
    if ($Port -gt 0) {
        $launchArgs.AddRange([string[]]@(
            '--automation-port', "$Port",
            '--keep-alive',
            '--timeout-sec', '120',
            '--gateway-url', 'http://127.0.0.1:8080',
            '--world-endpoint', '127.0.0.1:7777'
        ))
    }
    $workDir = Split-Path -Parent $exePath
    $proc = if ($launchArgs.Count -gt 0) {
        Start-Process -FilePath $exePath -ArgumentList $launchArgs.ToArray() -WorkingDirectory $workDir -PassThru
    } else {
        Start-Process -FilePath $exePath -WorkingDirectory $workDir -PassThru
    }
    return [int]$proc.Id
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$pathSep = [System.IO.Path]::PathSeparator
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/SandalCore.psd1') -Force -DisableNameChecking
}
if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/Env.psm1') -Force -DisableNameChecking
}

$opts = Resolve-RunOptions `
    -BaseProfile $Profile `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -BaseNoColor ([bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))) `
    -RawArgs @($Arguments)

function Out-Host {
    [CmdletBinding()]
    param([Parameter(ValueFromPipeline = $true)] $InputObject)
    begin {}
    process {
        if (-not $opts.Json) {
            Microsoft.PowerShell.Utility\Out-Host -InputObject $InputObject
        }
    }
    end {}
}

$launcherPid = $null
if (-not $opts.DryRun) {
    $distRelDir = Join-Path $rootPath 'dist/Release'
    $distSvcDir = Join-Path $rootPath 'dist/services'
    $prevPath = $env:PATH
    $env:PATH = "$distRelDir$pathSep$distSvcDir$pathSep$prevPath"
    try {
        $null = Ensure-SandalGameData -RepoRoot $rootPath
        $hasWorld = (Test-Path -LiteralPath (Join-Path $distRelDir "world$exeExt") -PathType Leaf) -or (Test-Path -LiteralPath (Join-Path $distRelDir "worldd$exeExt") -PathType Leaf)
        if (-not $hasWorld) {
            $buildScript = Join-Path $rootPath 'scripts/build.ps1'
            if ($opts.Json) {
                & $buildScript -Config Release 6>&1 | Out-Host
            } else {
                & $buildScript -Config Release
            }
            if ($LASTEXITCODE -ne 0) {
                throw "scripts/build.ps1 failed with exit code $LASTEXITCODE."
            }
        }
        $launcherExe = Join-Path $rootPath "dist/launcher/SandalLauncher$exeExt"
        $launcherCsproj = Join-Path $rootPath 'src/clients/launcher/SandalLauncher.csproj'
        if (-not (Test-Path -LiteralPath $launcherExe -PathType Leaf) -and (Test-Path -LiteralPath $launcherCsproj -PathType Leaf)) {
            $dotnetCmd = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($dotnetCmd) {
                $null = & $dotnetCmd.Source publish $launcherCsproj -c Release -o (Join-Path $rootPath 'dist/launcher') -p:DebugType=None -p:DebugSymbols=false --nologo 2>&1
                $null = & $dotnetCmd.Source publish $launcherCsproj -c Release -o $distRelDir -p:DebugType=None -p:DebugSymbols=false --nologo 2>&1
            }
        }
        $gatewayExe = Join-Path $distSvcDir "gateway$exeExt"
        if (-not (Test-Path -LiteralPath $gatewayExe -PathType Leaf)) {
            $servicesScript = Join-Path $rootPath 'scripts/services.ps1'
            if ($opts.Json) {
                & $servicesScript -Action build 6>&1 | Out-Host
            } else {
                & $servicesScript -Action build
            }
            if ($LASTEXITCODE -ne 0) {
                throw "scripts/services.ps1 -Action build failed with exit code $LASTEXITCODE."
            }
        }
        $null = Sync-SandalDistStage -RepoRoot $rootPath -Config 'Release'
        $null = Sync-SandalDeployStage -RepoRoot $rootPath
        $certsDir = Join-Path $rootPath 'dist/certs'
        $serverCrt = Join-Path $certsDir 'server.crt'
        $certsScript = Join-Path $rootPath 'scripts/sandal/certs.ps1'
        if (-not (Test-Path -LiteralPath $serverCrt -PathType Leaf) -and (Test-Path -LiteralPath $certsScript -PathType Leaf)) {
            $null = & $certsScript -OutputDir $certsDir -RepoRoot $rootPath -Json
        }
        $null = Ensure-SandalDockerDaemon
        $null = & (Join-Path $rootPath 'scripts/sandal/up.ps1') -Profile $opts.Profile -Detach -RepoRoot $rootPath -Json:$opts.Json
        if ($opts.SpawnLauncher) {
            $launcherPid = Start-SandalLauncherProcess -RootDirPath $rootPath -ExeExtension $exeExt -Port $opts.AutomationPort
        }
    } finally {
        $env:PATH = $prevPath
    }
}

$payload = [ordered]@{
    ok              = $true
    command         = 'run'
    status          = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    profile         = $opts.Profile
    dry_run         = [bool]$opts.DryRun
    no_color        = [bool]$opts.NoColor
    spawn_launcher  = [bool]$opts.SpawnLauncher
    launcher_pid    = $launcherPid
    automation_port = if ($opts.AutomationPort -gt 0) { [int]$opts.AutomationPort } else { $null }
    pipeline        = @('extract', 'build', 'deploy', 'up')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'run' (profile={0}, status={1}, dry_run={2})" -f $opts.Profile, $payload.status, $opts.DryRun)
}
