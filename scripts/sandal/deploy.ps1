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

function Resolve-DeployOptions {
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

$opts = Resolve-DeployOptions `
    -BaseConfig $Config `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -BaseNoColor ([bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))) `
    -RawArgs @($Arguments)

$distDir = Join-Path $rootPath 'dist'
$deployDir = Join-Path $rootPath 'deploy'
$certsDir = Join-Path $distDir 'certs'
$composeFile = Join-Path $deployDir 'compose.yml'
$envFile = Join-Path $deployDir '.env'

if (-not $opts.DryRun) {
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    $null = Sync-SandalDeployStage -RepoRoot $rootPath
    $srcDockerIgnore = Join-Path $rootPath 'src\deploy\.dockerignore'
    $destDockerIgnore = Join-Path $deployDir '.dockerignore'
    if (Test-Path -LiteralPath $srcDockerIgnore -PathType Leaf) {
        Copy-Item -LiteralPath $srcDockerIgnore -Destination $destDockerIgnore -Force
    }
    $serverCrt = Join-Path $certsDir 'server.crt'
    $certsScript = Join-Path $rootPath 'scripts\sandal\certs.ps1'
    if (-not (Test-Path -LiteralPath $serverCrt -PathType Leaf) -and (Test-Path -LiteralPath $certsScript -PathType Leaf)) {
        $null = & $certsScript -OutputDir $certsDir -RepoRoot $rootPath -Json
    }
}

$payload = [ordered]@{
    ok           = $true
    command      = 'deploy'
    status       = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    config       = $opts.Config
    dry_run      = [bool]$opts.DryRun
    no_color     = [bool]$opts.NoColor
    dist_dir     = $distDir
    deploy_dir   = $deployDir
    certs_dir    = $certsDir
    compose_file = $composeFile
    env_path     = $envFile
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'deploy' (status={0}, dry_run={1}, deploy={2})" -f $payload.status, $opts.DryRun, $deployDir)
}
