#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-BuildOptions {
    param(
        [string]$BaseConfig,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [bool]$BaseNoColor,
        [string[]]$RawArgs
    )
    $cfg = $BaseConfig
    $dry = $BaseDryRun
    $jsn = $BaseJson
    $nc = $BaseNoColor
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Config|--config|-c)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $cfg = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
            '^(-NoColor|--no-color)$' { $nc = $true }
        }
    }
    return [pscustomobject]@{ Config = $cfg; DryRun = $dry; Json = $jsn; NoColor = $nc }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}

$opts = Resolve-BuildOptions `
    -BaseConfig $Config `
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

function Write-BuildStep {
    param([string]$Message, [bool]$Quiet = $false)
    if (-not $Quiet -and $env:SANDAL_QUIET -ne '1') {
        [Console]::Out.WriteLine("[SandalCore] $Message")
    }
}

$quiet = [bool]$opts.Json
if ($quiet) { $env:SANDAL_QUIET = '1' }
if (-not $opts.DryRun) {
    Write-BuildStep -Message '[Step 1/4] Checking extracted game data in dist/content/ and staging dist/...' -Quiet $quiet
    $null = Ensure-SandalGameData -RepoRoot $rootPath -CheckOnly
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    Write-BuildStep -Message "[Step 2/4] Building C++ & .NET targets (config=$($opts.Config))..." -Quiet $quiet
    if ($opts.Json) {
        & (Join-Path $rootPath 'scripts\build.ps1') -Config $opts.Config 6>&1 | Out-Host
    } else {
        & (Join-Path $rootPath 'scripts\build.ps1') -Config $opts.Config
    }
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/build.ps1 failed with exit code $LASTEXITCODE."
    }
    Write-BuildStep -Message '[Step 3/4] Building Go microservices (scripts/services.ps1 -Action build)...' -Quiet $quiet
    if ($opts.Json) {
        & (Join-Path $rootPath 'scripts\services.ps1') -Action build 6>&1 | Out-Host
    } else {
        & (Join-Path $rootPath 'scripts\services.ps1') -Action build
    }
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/services.ps1 -Action build failed with exit code $LASTEXITCODE."
    }
    Write-BuildStep -Message '[Step 4/4] Staging deploy/ directory...' -Quiet $quiet
    $null = Ensure-SandalContentDbCurrent -RepoRoot $rootPath -CheckOnly
    $null = Sync-SandalDeployStage -RepoRoot $rootPath
} else {
    Write-BuildStep -Message "[Step 1/4] (dry-run) Would build C++, .NET, and Go targets (config=$($opts.Config))." -Quiet $quiet
}

$payload = [ordered]@{
    ok       = $true
    command  = 'build'
    status   = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    config   = $opts.Config
    dry_run  = [bool]$opts.DryRun
    no_color = [bool]$opts.NoColor
    steps    = @('ensure-game-data', 'build-cpp', 'build-services', 'sync-dist', 'sync-deploy')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'build' (config={0}, status={1}, dry_run={2})" -f $opts.Config, $payload.status, $opts.DryRun)
}
