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

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}

$opts = Resolve-RunOptions `
    -BaseProfile $Profile `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -BaseNoColor ([bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))) `
    -RawArgs @($Arguments)

if (-not $opts.DryRun) {
    $null = Ensure-SandalGameData -RepoRoot $rootPath
    $worlddExe = Join-Path $rootPath 'dist\Release\worldd.exe'
    if (-not (Test-Path -LiteralPath $worlddExe -PathType Leaf)) {
        & (Join-Path $rootPath 'scripts\build.ps1') -Config Release
        if ($LASTEXITCODE -ne 0) {
            throw "scripts/build.ps1 failed with exit code $LASTEXITCODE."
        }
    }
    $gatewayExe = Join-Path $rootPath 'dist\services\gateway.exe'
    if (-not (Test-Path -LiteralPath $gatewayExe -PathType Leaf)) {
        & (Join-Path $rootPath 'scripts\services.ps1') -Action build
        if ($LASTEXITCODE -ne 0) {
            throw "scripts/services.ps1 -Action build failed with exit code $LASTEXITCODE."
        }
    }
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config 'Release'
    $null = Sync-SandalDeployStage -RepoRoot $rootPath
    $serverCrt = Join-Path $rootPath 'dist\certs\server.crt'
    $certsScript = Join-Path $rootPath 'scripts\sandal\certs.ps1'
    if (-not (Test-Path -LiteralPath $serverCrt -PathType Leaf) -and (Test-Path -LiteralPath $certsScript -PathType Leaf)) {
        $null = & $certsScript -OutputDir (Join-Path $rootPath 'dist\certs') -RepoRoot $rootPath -Json
    }
    $null = & (Join-Path $rootPath 'scripts\sandal\up.ps1') -Profile $opts.Profile -Detach -RepoRoot $rootPath -Json:$opts.Json
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
