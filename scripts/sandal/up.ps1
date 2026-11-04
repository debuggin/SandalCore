#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
    [switch]$Detach,
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [string[]]$Arguments = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-UpOptions {
    param(
        [string]$BaseProfile,
        [bool]$BaseDetach,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $prof = $BaseProfile
    $det = $BaseDetach
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-Detach|--detach|-d)$' { $det = $true }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
        }
    }
    if ($prof -notin @('default', 'dev', 'admin', 'full')) {
        throw [System.ArgumentException]::new("Unsupported profile '$prof'. Valid profiles: default, dev, admin, full.")
    }
    return [pscustomobject]@{ Profile = $prof; Detach = $det; DryRun = $dry; Json = $jsn }
}

function Get-UpStageReadiness {
    param([string]$RootPath)
    $items = [System.Collections.Generic.List[object]]::new()
    foreach ($stage in (Get-SandalStageOrder)) {
        $upToDate = Test-SandalStageUpToDate -Stage $stage -RepoRoot $RootPath
        $items.Add([pscustomobject]@{
            stage      = $stage
            up_to_date = [bool]$upToDate
        })
    }
    return $items.ToArray()
}

function Build-ComposeUpArguments {
    param([string]$ComposeFile, [string]$EnvFile, [string]$ProfileName, [bool]$IsDetach)
    $cmdArgs = [System.Collections.Generic.List[string]]::new()
    $cmdArgs.AddRange([string[]]@('compose', '-f', $ComposeFile, '--env-file', $EnvFile))
    if ($ProfileName -ne 'default') {
        $cmdArgs.AddRange([string[]]@('--profile', $ProfileName))
    }
    $cmdArgs.Add('up')
    if ($IsDetach) {
        $cmdArgs.AddRange([string[]]@('-d', '--wait'))
    }
    return $cmdArgs.ToArray()
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Get-SandalStageOrder -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}
if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\Env.psm1') -Force -DisableNameChecking
}

$opts = Resolve-UpOptions -BaseProfile $Profile -BaseDetach ([bool]$Detach) -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -RawArgs (@($Arguments) + @($RemainingArgs))
$deploySync = Sync-SandalDeployStage -RepoRoot $rootPath
$composeFile = Join-Path $rootPath 'deploy\compose.yml'
$envFile = Join-Path $rootPath 'deploy\.env'
$composeArgs = Build-ComposeUpArguments -ComposeFile $composeFile -EnvFile $envFile -ProfileName $opts.Profile -IsDetach $opts.Detach
$stageChecks = @(Get-UpStageReadiness -RootPath $rootPath)
$allUpToDate = (@($stageChecks | Where-Object { -not $_.up_to_date }).Count -eq 0)

if (-not $opts.DryRun) {
    $null = Ensure-SandalDockerDaemon
    Push-Location $rootPath
    try {
        & docker @composeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "docker compose up failed with exit code $LASTEXITCODE."
        }
    } finally {
        Pop-Location
    }
}

$payload = [ordered]@{
    ok                = $true
    command           = 'up'
    status            = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    profile           = $opts.Profile
    detach            = [bool]$opts.Detach
    dry_run           = [bool]$opts.DryRun
    no_color          = $useNoColor
    env_path          = $envFile
    env_initialized   = ($null -ne $deploySync)
    compose_file      = $composeFile
    docker_args       = $composeArgs
    stages_up_to_date = $allUpToDate
    stages            = $stageChecks
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    Write-Output ("SandalCore 'up' (profile={0}, detach={1}, dry_run={2}): docker {3}" -f $opts.Profile, $opts.Detach, $opts.DryRun, ($composeArgs -join ' '))
}
