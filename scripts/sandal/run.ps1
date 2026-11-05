#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
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
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
            '^(-NoColor|--no-color)$' { $nc = $true }
        }
    }
    return [pscustomobject]@{ Profile = $prof; DryRun = $dry; Json = $jsn; NoColor = $nc }
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

if (-not $opts.DryRun) {
    $distRelDir = Join-Path $rootPath 'dist/Release'
    $distSvcDir = Join-Path $rootPath 'dist/services'
    $prevPath = $env:PATH
    $env:PATH = "$distRelDir$pathSep$distSvcDir$pathSep$prevPath"
    try {
        $null = Ensure-SandalGameData -RepoRoot $rootPath
        $worlddExe = Join-Path $distRelDir "worldd$exeExt"
        if (-not (Test-Path -LiteralPath $worlddExe -PathType Leaf)) {
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
    } finally {
        $env:PATH = $prevPath
    }
}

$payload = [ordered]@{
    ok       = $true
    command  = 'run'
    status   = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    profile  = $opts.Profile
    dry_run  = [bool]$opts.DryRun
    no_color = [bool]$opts.NoColor
    pipeline = @('extract', 'build', 'deploy', 'up')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'run' (profile={0}, status={1}, dry_run={2})" -f $opts.Profile, $payload.status, $opts.DryRun)
}
