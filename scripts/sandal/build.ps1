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

if (-not $opts.DryRun) {
    $null = Ensure-SandalGameData -RepoRoot $rootPath -CheckOnly
    & (Join-Path $rootPath 'scripts\build.ps1') -Config $opts.Config
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/build.ps1 failed with exit code $LASTEXITCODE."
    }
    & (Join-Path $rootPath 'scripts\services.ps1') -Action build
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/services.ps1 -Action build failed with exit code $LASTEXITCODE."
    }
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    $null = Sync-SandalDeployStage -RepoRoot $rootPath
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
