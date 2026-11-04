#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$Filter = '',
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-TestOptions {
    param(
        [string]$BaseConfig,
        [string]$BaseFilter,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $cfg = $BaseConfig
    $flt = $BaseFilter
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Config|--config|-c)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $cfg = $RawArgs[$i] }
            }
            '^(-Filter|--filter|-f)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $flt = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$' { $dry = $true }
            '^(-Json|--json)$'      { $jsn = $true }
        }
    }
    return [pscustomobject]@{ Config = $cfg; Filter = $flt; DryRun = $dry; Json = $jsn }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or ($Arguments -contains '-NoColor') -or ($Arguments -contains '--no-color') -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}

$opts = Resolve-TestOptions `
    -BaseConfig $Config `
    -BaseFilter $Filter `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -RawArgs @($Arguments)

if (-not $opts.DryRun) {
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    $testScript = Join-Path $rootPath 'scripts\run_tests.ps1'
    $testArgs = @('-Config', $opts.Config)
    if (-not [string]::IsNullOrWhiteSpace($opts.Filter)) {
        $testArgs += @('-Filter', $opts.Filter)
    }
    & $testScript @testArgs
    if ($LASTEXITCODE -ne 0) {
        throw "scripts/run_tests.ps1 failed with exit code $LASTEXITCODE."
    }
}

$payload = [ordered]@{
    ok       = $true
    command  = 'test'
    status   = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    config   = $opts.Config
    filter   = $opts.Filter
    dry_run  = [bool]$opts.DryRun
    no_color = $useNoColor
    steps    = @('sync-dist', 'run-tests')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'test' (config={0}, status={1}, dry_run={2})" -f $opts.Config, $payload.status, $opts.DryRun)
}
