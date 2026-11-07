<#
.SYNOPSIS
    SandalCore Host & Container Build Orchestrator.
.DESCRIPTION
    Builds Windows host .NET 10 tools and SandalLauncher while verifying
    generated Go protobuf modules and RecastNavigation sources. All C++23 server
    and navigation binaries compile inside Linux containers (src/deploy/docker/cpp.Dockerfile).
#>
[CmdletBinding()]
param(
    [string]$Target = 'all',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Configure,
    [switch]$Test,
    [int]$Jobs = 0,
    [switch]$NoExtract,
    [switch]$SkipExtract
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Import-Module (Join-Path $PSScriptRoot 'lib\Env.psm1') -Force -DisableNameChecking

function Invoke-HostClean {
    param([string]$RepoRoot)

    foreach ($relDir in @('build\dotnet', 'build\gen')) {
        $dirPath = Join-Path $RepoRoot $relDir
        if (Test-Path -LiteralPath $dirPath) {
            Write-SandalStep -Step 'CLEAN' -Message "Removing $relDir..."
            Remove-Item -LiteralPath $dirPath -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Build-HostDotnetProjects {
    param(
        [string]$RepoRoot,
        [string]$Configuration
    )

    $dotnetCmd = Get-Command 'dotnet' -ErrorAction SilentlyContinue
    if ($null -eq $dotnetCmd) {
        throw ".NET 10 SDK ('dotnet') is not installed or not on PATH."
    }

    $cue4Proj = Join-Path $RepoRoot 'build\third_party\CUE4Parse\CUE4Parse\CUE4Parse.csproj'
    $fetchCue4 = Join-Path $RepoRoot 'src\tools\mapc\fetch_cue4parse.ps1'
    if (-not (Test-Path -LiteralPath $cue4Proj) -and (Test-Path -LiteralPath $fetchCue4)) {
        & (Get-Process -Id $PID).Path -NoProfile -File $fetchCue4
    }

    $dotnetConfig = if ($Configuration -eq 'Debug') { 'Debug' } else { 'Release' }
    $projects = @(
        'src\tools\mapc\mapc.csproj',
        'src\tools\npcabilities\npcabilities.csproj',
        'src\tools\itemnames\itemnames.csproj',
        'src\tools\pcap_spawns\pcap_spawns.csproj',
        'src\tools\aoc-extractor\AocExtractor.csproj',
        'src\clients\launcher\SandalLauncher.csproj'
    )

    foreach ($relProj in $projects) {
        $projPath = Join-Path $RepoRoot $relProj
        if (-not (Test-Path -LiteralPath $projPath)) {
            continue
        }
        Write-SandalStep -Step 'DOTNET' -Message "Building $relProj ($dotnetConfig)..."
        & $dotnetCmd.Source build $projPath -c $dotnetConfig --nologo -v quiet
        if ($LASTEXITCODE -ne 0) {
            throw "dotnet build failed for $relProj (exit $LASTEXITCODE)."
        }
    }
}

function Invoke-SandalHostBuild {
    param(
        [string]$RepoRoot,
        [string]$TargetName,
        [string]$Configuration,
        [bool]$RunClean,
        [bool]$RunTests
    )

    if ($RunClean) {
        Invoke-HostClean -RepoRoot $RepoRoot
    }

    [void](Ensure-SandalRecastNavigation -RepoRoot $RepoRoot)
    [void](Ensure-SandalGoProtoGen -RepoRoot $RepoRoot)

    if ($TargetName -ne 'aoc_navbuild') {
        Build-HostDotnetProjects -RepoRoot $RepoRoot -Configuration $Configuration
    }

    if ($RunTests) {
        $testScript = Join-Path $RepoRoot 'scripts\run_tests.ps1'
        if (Test-Path -LiteralPath $testScript) {
            & $testScript -Config $Configuration
            if ($LASTEXITCODE -ne 0) {
                exit $LASTEXITCODE
            }
        }
    }
}

Invoke-SandalHostBuild `
    -RepoRoot $repoRoot `
    -TargetName $Target `
    -Configuration $Config `
    -RunClean $Clean.IsPresent `
    -RunTests $Test.IsPresent
