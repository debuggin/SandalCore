#Requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$Volumes,
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

function Resolve-DownOptions {
    param(
        [bool]$BaseVolumes,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $vol = $BaseVolumes
    $dry = $BaseDryRun
    $jsn = $BaseJson
    foreach ($arg in $RawArgs) {
        switch -Regex ($arg) {
            '^(-Volumes|--volumes|-v)$' { $vol = $true }
            '^(-DryRun|--dry-run)$'     { $dry = $true }
            '^(-Json|--json)$'          { $jsn = $true }
        }
    }
    return [pscustomobject]@{ Volumes = $vol; DryRun = $dry; Json = $jsn }
}

function Build-ComposeDownArguments {
    param([string]$ComposeFile, [string]$EnvFile, [bool]$IncludeVolumes)
    $cmdArgs = [System.Collections.Generic.List[string]]::new()
    $cmdArgs.AddRange([string[]]@('compose', '-f', $ComposeFile, '--env-file', $EnvFile, 'down', '--remove-orphans'))
    # WHY: Protect persistent PostgreSQL/Redis volumes unless -Volumes is explicitly requested.
    if ($IncludeVolumes) {
        $cmdArgs.Add('-v')
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

$opts = Resolve-DownOptions -BaseVolumes ([bool]$Volumes) -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -RawArgs (@($Arguments) + @($RemainingArgs))
$composeFile = Join-Path $rootPath 'deploy\compose.yml'
$envFile = Join-Path $rootPath 'deploy\.env'
$composeArgs = Build-ComposeDownArguments -ComposeFile $composeFile -EnvFile $envFile -IncludeVolumes $opts.Volumes

if (-not $opts.DryRun) {
    if (-not (Test-Path -LiteralPath $composeFile -PathType Leaf)) {
        $null = Sync-SandalDeployStage -RepoRoot $rootPath
    }
    Push-Location $rootPath
    try {
        & docker @composeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "docker compose down failed with exit code $LASTEXITCODE."
        }
    } finally {
        Pop-Location
    }
}

$payload = [ordered]@{
    ok                = $true
    command           = 'down'
    status            = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    volumes           = [bool]$opts.Volumes
    volumes_protected = (-not [bool]$opts.Volumes)
    dry_run           = [bool]$opts.DryRun
    no_color          = $useNoColor
    env_path          = $envFile
    compose_file      = $composeFile
    docker_args       = $composeArgs
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'down' (volumes={0}, dry_run={1}): docker {2}" -f $opts.Volumes, $opts.DryRun, ($composeArgs -join ' '))
}
