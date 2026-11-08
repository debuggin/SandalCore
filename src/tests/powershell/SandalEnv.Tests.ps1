#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inPester = ($null -ne (Get-Module -Name Pester)) -and (
    @(Get-PSCallStack | Where-Object {
        ($null -ne $_.Command -and $_.Command -match 'Pester') -or
        ($null -ne $_.ScriptName -and $_.ScriptName -match 'Pester')
    }).Count -gt 0
)

if (-not $inPester) {
    function Describe([string]$Name, [scriptblock]$Fixture) { Write-Host "[Describe] $Name"; & $Fixture }
    function Context([string]$Name, [scriptblock]$Fixture) { Write-Host "  [Context] $Name"; & $Fixture }
    function BeforeAll([scriptblock]$Action) { & $Action }
    function It([string]$Name, [scriptblock]$Test) { & $Test; Write-Host "    [PASS] $Name" }
    function Should {
        param([switch]$BeTrue, [switch]$BeFalse, [object]$Be)
        $actual = @($input)[0]
        if ($BeTrue -and -not [bool]$actual) { throw "Expected True, got '$actual'" }
        if ($BeFalse -and [bool]$actual) { throw "Expected False, got '$actual'" }
        if ($PSBoundParameters.ContainsKey('Be') -and $actual -ne $Be) { throw "Expected '$Be', got '$actual'" }
    }
} elseif ((Get-Module Pester).Version.Major -lt 5) {
    function Should {
        $mapped = [System.Collections.Generic.List[object]]::new()
        foreach ($a in $args) {
            if ($a -is [string] -and $a -eq '-BeTrue') {
                $mapped.AddRange([object[]]@('Be', $true))
            } elseif ($a -is [string] -and $a -eq '-BeFalse') {
                $mapped.AddRange([object[]]@('Be', $false))
            } elseif ($a -is [string] -and $a.StartsWith('-')) {
                $mapped.Add($a.Substring(1))
            } else {
                $mapped.Add($a)
            }
        }
        $input | Pester\Should @mapped
    }
}

function New-SyntheticCompleteGameDataDir {
    param([Parameter(Mandatory)][string]$RootPath, [string]$ContentRelPath = 'dist\content')
    $outDir = Join-Path $RootPath $ContentRelPath
    $reqFiles = @(
        'content.db', 'aoc.usmap', 'artisan_curves.tsv', 'mount_speeds.tsv', 'mount_riders.tsv', 'item_names.tsv',
        'roads.tsv', 'roads_levels.tsv', 'town_platforms.bin', 'town_layout_items.tsv', 'town_layouts.bin', 'verra.pmtiles',
        'npc_abilities.tsv', 'npc_montages.tsv', 'npc_targeting.tsv', 'npc_senses.tsv', 'npc_aggro_curves.tsv',
        'npc_looks.tsv', 'npc_appearances.tsv', 'npc_names.tsv', 'npc_trees.tsv',
        'npc_motion.tsv', 'region_foliage.tsv', 'pcap_resource_spawners.tsv', 'area_geom.bin', 'area_nav.bin', 'client_config\DefaultCombat.ini'
    )
    foreach ($rel in $reqFiles) {
        $fullPath = Join-Path $outDir $rel
        $parent = Split-Path -Parent $fullPath
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
            $null = New-Item -ItemType Directory -Path $parent -Force
        }
        [System.IO.File]::WriteAllText($fullPath, "fixture-$rel")
    }
    $zoiDir = Join-Path $outDir 'zoi_chunks'
    $null = New-Item -ItemType Directory -Path $zoiDir -Force
    [System.IO.File]::WriteAllText((Join-Path $zoiDir 'RVR_Zone_01.uasset'), 'uasset-fixture')
    return $outDir
}

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$script:EnvModule = Join-Path $script:RepoRoot 'scripts\lib\Env.psm1'
Import-Module $script:EnvModule -Force -DisableNameChecking

Describe 'Test-SandalRuntimeGameDataComplete 25-Artifact Game Data Readiness Contract' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:EnvModule = Join-Path $script:RepoRoot 'scripts\lib\Env.psm1'
        Import-Module $script:EnvModule -Force -DisableNameChecking
    }

    Context 'Synthetic Hermetic 25-Artifact Positive & Negative-Space Verification' {
        It 'returns true when all 25 runtime game data files and zoi_chunks/*.uasset exist and are non-empty' {
            $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-gamedata-' + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-SyntheticCompleteGameDataDir -RootPath $tmpRoot
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeTrue
            } finally {
                if (Test-Path -LiteralPath $tmpRoot) {
                    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }

        It 'returns false when npc_motion.tsv or area_nav.bin is missing or empty, or zoi_chunks has no .uasset' {
            $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-gamedata-neg-' + [guid]::NewGuid().ToString('N'))
            try {
                $outDir = New-SyntheticCompleteGameDataDir -RootPath $tmpRoot
                Remove-Item -LiteralPath (Join-Path $outDir 'npc_motion.tsv') -Force
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeFalse

                [System.IO.File]::WriteAllText((Join-Path $outDir 'npc_motion.tsv'), 'restored')
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeTrue

                Remove-Item -LiteralPath (Join-Path $outDir 'area_nav.bin') -Force
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeFalse

                [System.IO.File]::WriteAllText((Join-Path $outDir 'area_nav.bin'), '')
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeFalse

                [System.IO.File]::WriteAllText((Join-Path $outDir 'area_nav.bin'), 'nav-bytes')
                Remove-Item -LiteralPath (Join-Path $outDir 'zoi_chunks\RVR_Zone_01.uasset') -Force
                (Test-SandalRuntimeGameDataComplete -RepoRoot $tmpRoot) | Should -BeFalse
            } finally {
                if (Test-Path -LiteralPath $tmpRoot) {
                    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Live Repository dist/content and deploy/content Completeness' {
        It 'confirms dist/content and deploy/content contain all 25 extracted game data artifacts' {
            (Test-SandalRuntimeGameDataComplete -RepoRoot $script:RepoRoot -ContentRelPath 'dist\content') | Should -BeTrue
            (Test-SandalRuntimeGameDataComplete -RepoRoot $script:RepoRoot -ContentRelPath 'deploy\content') | Should -BeTrue
        }
    }
}
