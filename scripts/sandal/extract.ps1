#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$PakDir = '',
    [string]$OutputDir = '',
    [switch]$DataFree,
    [switch]$Force,
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

function Resolve-ExtractOptions {
    param([hashtable]$Defaults, [string[]]$RawArgs)
    $pak = [string]$Defaults['PakDir']
    $out = [string]$Defaults['OutputDir']
    $df = [bool]$Defaults['DataFree']
    $frc = [bool]$Defaults['Force']
    $dry = [bool]$Defaults['DryRun']
    $jsn = [bool]$Defaults['Json']
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-PakDir|--pak-dir|--paks)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $pak = $RawArgs[$i] }
            }
            '^(-OutputDir|--output-dir|-o)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $out = $RawArgs[$i] }
            }
            '^(-DataFree|--data-free|verify-standin)$' { $df = $true }
            '^(-Force|--force|-f)$'                    { $frc = $true }
            '^(-DryRun|--dry-run)$'                    { $dry = $true }
            '^(-Json|--json)$'                         { $jsn = $true }
        }
    }
    return [pscustomobject]@{
        PakDir    = $pak
        OutputDir = $out
        DataFree  = $df
        Force     = $frc
        DryRun    = $dry
        Json      = $jsn
    }
}

function Get-ExtractStageMap {
    param()
    return [ordered]@{
        'extract-db'    = 'database'
        'extract-vmap'  = 'navmesh'
        'navbuild'      = 'navbuild'
        'extract-tiles' = 'region'
    }
}

function Test-ExtractStageReady {
    param([string]$StageName, [string]$GraphStage, [string]$RootPath)
    $graphReady = Test-SandalStageUpToDate -Stage $GraphStage -RepoRoot $RootPath
    if ($StageName -eq $GraphStage) {
        return [bool]$graphReady
    }
    try {
        $aliasReady = Test-SandalStageUpToDate -Stage $StageName -RepoRoot $RootPath
        return [bool]($graphReady -or $aliasReady)
    } catch {
        return [bool]$graphReady
    }
}

function Get-ExtractStagePlan {
    param([string]$RootPath, [bool]$ForceRun)
    $map = Get-ExtractStageMap
    $plan = [System.Collections.Generic.List[object]]::new()
    foreach ($entry in $map.GetEnumerator()) {
        $stageName = [string]$entry.Key
        $graphStage = [string]$entry.Value
        $upToDate = Test-ExtractStageReady -StageName $stageName -GraphStage $graphStage -RootPath $RootPath
        $hash = Compute-SandalStageHash -Stage $graphStage -RepoRoot $RootPath
        $plan.Add([pscustomobject]@{
            stage       = $stageName
            graph_stage = $graphStage
            up_to_date  = [bool]$upToDate
            would_run   = [bool]($ForceRun -or -not $upToDate)
            input_hash  = $hash
        })
    }
    return $plan.ToArray()
}

function Invoke-ExtractStagePipeline {
    param([string]$RootPath, [string]$TargetOutDir, [object[]]$StagePlan, [pscustomobject]$Options)
    if (-not (Test-Path -LiteralPath $TargetOutDir)) {
        $null = New-Item -ItemType Directory -Path $TargetOutDir -Force
    }
    if ($Options.DataFree) {
        $standinPath = Join-Path $TargetOutDir 'verify-standin.json'
        $standinJson = (@{ mode = 'verify-standin'; data_free = $true } | ConvertTo-Json -Compress)
        [System.IO.File]::WriteAllText($standinPath, $standinJson, [System.Text.UTF8Encoding]::new($false))
    } else {
        $extractScript = Join-Path $RootPath 'scripts\extract_game_data.ps1'
        if (Test-Path -LiteralPath $extractScript -PathType Leaf) {
            $extArgs = @()
            if (-not [string]::IsNullOrWhiteSpace($Options.PakDir)) {
                $extArgs += @('-GameRoot', $Options.PakDir)
            }
            if ($Options.Force) {
                $extArgs += '-Force'
            }
            & $extractScript @extArgs
            if ($LASTEXITCODE -ne 0) {
                throw "extract_game_data.ps1 failed with exit code $LASTEXITCODE."
            }
        }
    }
    $results = [System.Collections.Generic.List[object]]::new()
    foreach ($item in $StagePlan) {
        $action = 'skipped'
        if ($item.would_run) {
            $null = Write-SandalStageStamp -Stage $item.graph_stage -RepoRoot $RootPath -InputHash $item.input_hash
            if ($item.stage -ne $item.graph_stage) {
                $null = Write-SandalStageStamp -Stage $item.stage -RepoRoot $RootPath -InputHash $item.input_hash
            }
            $action = 'stamped'
        }
        $results.Add([pscustomobject]@{
            stage       = $item.stage
            graph_stage = $item.graph_stage
            action      = $action
            input_hash  = $item.input_hash
        })
    }
    return $results.ToArray()
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Invoke-WithHeavyOperationLock -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}

$defaults = @{
    PakDir    = $PakDir
    OutputDir = $OutputDir
    DataFree  = [bool]$DataFree
    Force     = [bool]$Force
    DryRun    = [bool]($DryRun -or $inheritedDryRun)
    Json      = [bool]($Json -or $inheritedJson)
}
$opts = Resolve-ExtractOptions -Defaults $defaults -RawArgs (@($Arguments) + @($RemainingArgs))
$targetOutDir = if ($opts.OutputDir) { [System.IO.Path]::GetFullPath($opts.OutputDir) } else { Join-Path $rootPath 'dist\extracted' }
$modeName = if ($opts.DataFree) { 'verify-standin' } else { 'standard' }
$stagePlan = @(Get-ExtractStagePlan -RootPath $rootPath -ForceRun $opts.Force)

$executedStages = @()
if (-not $opts.DryRun) {
    $lockFile = Join-Path $rootPath 'dist\.stamps\extract.lock.json'
    $stampDir = Split-Path -Parent $lockFile
    if (-not (Test-Path -LiteralPath $stampDir)) {
        $null = New-Item -ItemType Directory -Path $stampDir -Force
    }
    $executedStages = @(Invoke-WithHeavyOperationLock -Operation 'sandal-extract' -LockFilePath $lockFile -ScriptBlock {
        Invoke-ExtractStagePipeline -RootPath $rootPath -TargetOutDir $targetOutDir -StagePlan $stagePlan -Options $opts
    })
}

$payload = [ordered]@{
    ok         = $true
    command    = 'extract'
    mode       = $modeName
    data_free  = [bool]$opts.DataFree
    pak_dir    = $opts.PakDir
    output_dir = $targetOutDir
    force      = [bool]$opts.Force
    dry_run    = [bool]$opts.DryRun
    no_color   = $useNoColor
    stages     = if ($opts.DryRun) { $stagePlan } else { $executedStages }
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    Write-Output ("SandalCore 'extract' (mode={0}, data_free={1}, dry_run={2}, out={3})" -f $modeName, $opts.DataFree, $opts.DryRun, $targetOutDir)
}
