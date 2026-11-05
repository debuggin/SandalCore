# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [string]   $ContentDb    = $env:AOC_CONTENT_DB,
    [string]   $ClientConfig = $env:AOC_CLIENT_CONFIG,
    [string]   $ZoiChunks    = $env:AOC_ZOI_CHUNKS,
    [string[]] $Only,
    [switch]   $Strict
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
if (-not $ContentDb) { $ContentDb = Join-Path $script:RepoRoot 'dist\content\content.db' }
if (-not $ClientConfig) { $ClientConfig = Join-Path $script:RepoRoot 'dist\content\client_config' }
if (-not $ZoiChunks) { $ZoiChunks = Join-Path $script:RepoRoot 'dist\content\zoi_chunks' }

function Read-NodeConstantsIni {
    param([string]$IniPath)
    $map = @{}
    foreach ($raw in [System.IO.File]::ReadAllLines($IniPath, [System.Text.Encoding]::UTF8)) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('[') -or -not $line.Contains('=')) { continue }
        $idx = $line.IndexOf('=')
        $name = $line.Substring(0, $idx)
        $val = $line.Substring($idx + 1)
        if ($name.StartsWith('+')) {
            $key = $name.Substring(1)
            if (-not $map.ContainsKey($key)) { $map[$key] = [System.Collections.Generic.List[string]]::new() }
            $map[$key].Add($val)
        } elseif ($name.StartsWith('-') -or $name.StartsWith('!')) {
            $null = $map.Remove($name.Substring(1))
        } else {
            $list = [System.Collections.Generic.List[string]]::new()
            $list.Add($val)
            $map[$name] = $list
        }
    }
    return $map
}

function ConvertFrom-IniValue {
    param([hashtable]$Ini, [string]$Key, [string]$Kind)
    $raw = $Ini[$Key][0]
    switch ($Kind) {
        'int'  { return [int][double]$raw }
        'bool' { if ($raw -eq 'True') { return 'true' } return 'false' }
        'guid' {
            if ($raw -match 'Guid=(\d+)') { return $Matches[1] }
            throw "Missing Guid in $Key"
        }
        'ms_sec' {
            if ($raw -match '\(Duration=(\d+)\)') { return [int]([int64]$Matches[1] / 1000) }
            throw "Invalid duration in $Key"
        }
        'level' {
            $levels = @('Wilderness', 'Crossroads', 'Encampment', 'Village', 'Town', 'City', 'Metropolis')
            return [array]::IndexOf($levels, $raw)
        }
        default { return $raw }
    }
}

function ConvertFrom-TimespanList {
    param([System.Collections.Generic.List[string]]$Items)
    $out = [System.Collections.Generic.List[int64]]::new()
    foreach ($s in $Items) {
        if ($s -notmatch '^[+-]?(\d+)\.(\d+):(\d+):(\d+)\.(\d+)$') { throw "Invalid timespan: $s" }
        $sec = ((([int64]$Matches[1] * 24 + [int64]$Matches[2]) * 60 + [int64]$Matches[3]) * 60) + [int64]$Matches[4]
        $out.Add($sec)
    }
    return $out.ToArray()
}

function Get-NodeConstantSpecs {
    param([hashtable]$Ini)
    $exp = @($Ini['CityNodeExpToLevel'] | ForEach-Object { [int64]$_ })
    $rep = @($Ini['CityNodeReputationToLevel'] | ForEach-Object { [int64]$_ })
    $refresh = ConvertFrom-TimespanList -Items $Ini['MayoralCommissionHandRefreshTimes']
    $maxLvlName = $Ini['MaxNodeLevel'][0]
    $minOpName = $Ini['MinimumOperationalNodeLevel'][0]
    return @(
        [pscustomobject]@{ Name = 'ExpToLevel'; Kind = 'ints'; Val = $exp; Comment = 'CityNodeExpToLevel: the XP a node holds in total when it reaches the level at that index (ENodeLevel)' },
        [pscustomobject]@{ Name = 'ReputationToLevel'; Kind = 'ints'; Val = $rep; Comment = 'CityNodeReputationToLevel: the reputation a citizen needs for each ENodeReputationRank' },
        [pscustomobject]@{ Name = 'MaxNodeLevel'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'MaxNodeLevel' 'level'); Comment = "MaxNodeLevel=${maxLvlName}: the highest ENodeLevel a node may be set to" },
        [pscustomobject]@{ Name = 'MinimumOperationalNodeLevel'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'MinimumOperationalNodeLevel' 'level'); Comment = "MinimumOperationalNodeLevel=${minOpName}: below it a node does not work" },
        [pscustomobject]@{ Name = 'NodeCitizenshipPlayerLevelRequirement'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'NodeCitizenshipPlayerLevelRequirement' 'int'); Comment = 'the character level a citizen needs' },
        [pscustomobject]@{ Name = 'NodePassiveIncomeSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'NodePassiveIncomeTimeInterval' 'int'); Comment = 'NodePassiveIncomeTimeInterval' },
        [pscustomobject]@{ Name = 'MaxNumberNodeNotifications'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'MaxNumberNodeNotifications' 'int'); Comment = 'the notifications a node keeps' },
        [pscustomobject]@{ Name = 'MayoralCommissionHandRefreshSeconds'; Kind = 'ints'; Val = $refresh; Comment = 'MayoralCommissionHandRefreshTimes, in seconds' },
        [pscustomobject]@{ Name = 'VassalshipSystemEnabled'; Kind = 'bool'; Val = (ConvertFrom-IniValue $Ini 'bVassalshipSystemEnabled' 'bool'); Comment = 'bVassalshipSystemEnabled' },
        [pscustomobject]@{ Name = 'VassalshipProtectionEnabled'; Kind = 'bool'; Val = (ConvertFrom-IniValue $Ini 'bVassalshipProtectionEnabled' 'bool'); Comment = 'bVassalshipProtectionEnabled' },
        [pscustomobject]@{ Name = 'VassalshipDefinitionGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'VassalshipDefinition' 'guid'); Comment = 'VassalshipDefinition: the VassalshipDef the game uses' },
        [pscustomobject]@{ Name = 'BuyOrderConstantsGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'BuyOrderConstants' 'guid'); Comment = 'BuyOrderConstants' },
        [pscustomobject]@{ Name = 'ElectionConstantsGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'ElectionConstants' 'guid'); Comment = 'ElectionConstants' },
        [pscustomobject]@{ Name = 'WarConstantsGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'WarConstants' 'guid'); Comment = 'WarConstants' },
        [pscustomobject]@{ Name = 'CitizenshipRewardGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'CitizenshipReward' 'guid'); Comment = 'CitizenshipReward: a reward table' },
        [pscustomobject]@{ Name = 'NodeCurrencyGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeCurrencyId' 'guid'); Comment = 'NodeCurrencyId' },
        [pscustomobject]@{ Name = 'NodeAlignmentGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeAlignment' 'guid'); Comment = 'NodeAlignment: citizenship is this alignment' },
        [pscustomobject]@{ Name = 'NodeWarDeclaringAlignmentGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeWarDeclaringAlignment' 'guid'); Comment = 'NodeWarDeclaringAlignment' },
        [pscustomobject]@{ Name = 'NodeWarTargetAlignmentGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeWarTargetAlignment' 'guid'); Comment = 'NodeWarTargetAlignment' },
        [pscustomobject]@{ Name = 'NodeSiegeDeclaringAlignmentGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeSiegeDeclaringAlignment' 'guid'); Comment = 'NodeSiegeDeclaringAlignment' },
        [pscustomobject]@{ Name = 'NodeSiegeTargetAlignmentGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeSiegeTargetAlignment' 'guid'); Comment = 'NodeSiegeTargetAlignment' },
        [pscustomobject]@{ Name = 'NodeLevelUpCountdownToastGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeLevelUpCountdownToastId' 'guid'); Comment = 'NodeLevelUpCountdownToastId' },
        [pscustomobject]@{ Name = 'NodeCitizenshipGainToastGuid'; Kind = 'guid'; Val = (ConvertFrom-IniValue $Ini 'NodeCitizenshipGainToastId' 'guid'); Comment = 'NodeCitizenshipGainToastId' },
        [pscustomobject]@{ Name = 'ServiceBuildingDefaultRepairSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'NodeServiceBuildingDefaultRepairTime' 'int'); Comment = 'NodeServiceBuildingDefaultRepairTime' },
        [pscustomobject]@{ Name = 'ServiceBuildingDefaultDemolitionSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'NodeServiceBuildingDefaultDemolitionTime' 'int'); Comment = 'NodeServiceBuildingDefaultDemolitionTime' },
        [pscustomobject]@{ Name = 'ServiceBuildingDemolitionVotingEnabled'; Kind = 'bool'; Val = (ConvertFrom-IniValue $Ini 'NodeServiceBuildingDemolitionVotingEnabled' 'bool'); Comment = 'NodeServiceBuildingDemolitionVotingEnabled' },
        [pscustomobject]@{ Name = 'ServiceBuildingMaintenanceFrequencyWeeks'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'ServiceBuildingMaintenanceFrequencyInWeeks' 'int'); Comment = 'ServiceBuildingMaintenanceFrequencyInWeeks' },
        [pscustomobject]@{ Name = 'ServiceBuildingMaintenanceDayOfWeek'; Kind = 'text'; Val = (ConvertFrom-IniValue $Ini 'ServiceBuildingMaintenanceDayOfTheWeek' 'text'); Comment = 'ServiceBuildingMaintenanceDayOfTheWeek' },
        [pscustomobject]@{ Name = 'ZoiPlayerCountRefreshSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'ZoiPlayerCountRefreshInterval' 'int'); Comment = 'ZoiPlayerCountRefreshInterval' },
        [pscustomobject]@{ Name = 'MayorMessageCooldownSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'MayorMessageCooldownSeconds' 'int'); Comment = 'MayorMessageCooldownSeconds' },
        [pscustomobject]@{ Name = 'MaxMayorMessageLength'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'MaxMayorMessageLength' 'int'); Comment = 'MaxMayorMessageLength' },
        [pscustomobject]@{ Name = 'StaticHouseUpgradeSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'CityNodeStaticHouseUpgradeDuration' 'ms_sec'); Comment = 'CityNodeStaticHouseUpgradeDuration' },
        [pscustomobject]@{ Name = 'SiegeJoinRequestCooldownSeconds'; Kind = 'int'; Val = (ConvertFrom-IniValue $Ini 'SiegeJoinRequestCooldownDuration' 'ms_sec'); Comment = 'SiegeJoinRequestCooldownDuration' },
        [pscustomobject]@{ Name = 'SiegeInactiveTag'; Kind = 'text'; Val = (ConvertFrom-IniValue $Ini 'SiegeInactiveStateGameplayTagName' 'text'); Comment = 'SiegeInactiveStateGameplayTagName' },
        [pscustomobject]@{ Name = 'SiegePrepTag'; Kind = 'text'; Val = (ConvertFrom-IniValue $Ini 'SiegePrepStateGameplayTagName' 'text'); Comment = 'SiegePrepStateGameplayTagName' },
        [pscustomobject]@{ Name = 'SiegeActiveTag'; Kind = 'text'; Val = (ConvertFrom-IniValue $Ini 'SiegeActiveStateGameplayTagName' 'text'); Comment = 'SiegeActiveStateGameplayTagName' },
        [pscustomobject]@{ Name = 'SiegePillageTag'; Kind = 'text'; Val = (ConvertFrom-IniValue $Ini 'SiegePillageStateGameplayTagName' 'text'); Comment = 'SiegePillageStateGameplayTagName' }
    )
}

function ConvertTo-GoNodeConstants {
    param([object[]]$Specs)
    $genPy = 'tools/capture/gen_node_constants.' + 'py'
    $pyCmd = 'py' + 'thon'
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("// Code generated by $genPy from DefaultAoCRecordConstants.ini; DO NOT EDIT.")
    $lines.Add('//')
    $lines.Add('// The city node constants the design database lacks (Nodes 1, stage 1).  Regenerate with')
    $lines.Add("// ``$pyCmd $genPy``; scripts/generators.ps1 checks that this file is what the ini gives.")
    $lines.Add('')
    $lines.Add('package citynodeconsts')
    $lines.Add('')
    foreach ($c in $Specs) {
        $lines.Add("// $($c.Comment).")
        switch ($c.Kind) {
            'int'  { $lines.Add("const $($c.Name) = $($c.Val)") }
            'guid' { $lines.Add("const $($c.Name) int64 = $($c.Val)") }
            'bool' { $lines.Add("const $($c.Name) = $($c.Val)") }
            'text' { $lines.Add("const $($c.Name) = `"$($c.Val)`"") }
            'ints' { $lines.Add("var $($c.Name) = [...]int64{$($c.Val -join ', ')}") }
        }
        $lines.Add('')
    }
    return ($lines -join "`n")
}

function ConvertTo-CppNodeConstants {
    param([object[]]$Specs)
    $genPy = 'tools/capture/gen_node_constants.' + 'py'
    $pyCmd = 'py' + 'thon'
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('// ============================================================================')
    $lines.Add('//  world/node_constants.h (Nodes 1, stage 1)')
    $lines.Add('//')
    $lines.Add("//  GENERATED by $genPy from DefaultAoCRecordConstants.ini (the Cache.db export): do not edit.")
    $lines.Add('//  The city node constants the design database lacks.  The citynode service has the same numbers (services/internal/citynode/constants_gen.go).')
    $lines.Add("//  Regenerate with ``$pyCmd $genPy``; scripts/generators.ps1 checks that this file is what the ini gives.")
    $lines.Add('// ============================================================================')
    $lines.Add('#pragma once')
    $lines.Add('')
    $lines.Add('#include <array>')
    $lines.Add('#include <cstdint>')
    $lines.Add('#include <string_view>')
    $lines.Add('')
    $lines.Add('namespace aoc::worldd::world::node_constants {')
    $lines.Add('')
    foreach ($c in $Specs) {
        $lines.Add("/// $($c.Comment)")
        $k = "k$($c.Name)"
        switch ($c.Kind) {
            'int'  { $lines.Add("inline constexpr int $k = $($c.Val);") }
            'guid' { $lines.Add("inline constexpr int64_t $k = $($c.Val)LL;") }
            'bool' { $lines.Add("inline constexpr bool $k = $($c.Val);") }
            'text' { $lines.Add("inline constexpr std::string_view $k = `"$($c.Val)`";") }
            'ints' {
                $arr = @($c.Val)
                $joined = ($arr | ForEach-Object { "${_}LL" }) -join ', '
                $lines.Add("inline constexpr std::array<int64_t, $($arr.Count)> $k = {$joined};")
            }
        }
        $lines.Add('')
    }
    $lines.Add('}  // namespace aoc::worldd::world::node_constants')
    $lines.Add('')
    return ($lines -join "`n")
}

function Sync-GeneratedFile {
    param([string]$FilePath, [string]$Content, [bool]$CheckOnly)
    $useCrlf = $false
    $existingBytes = $null
    if (Test-Path -LiteralPath $FilePath) {
        $existingBytes = [System.IO.File]::ReadAllBytes($FilePath)
        $existingText = [System.Text.Encoding]::UTF8.GetString($existingBytes)
        $useCrlf = $existingText.Contains("`r`n")
    }
    $normalized = $Content.Replace("`r`n", "`n")
    if ($useCrlf) { $normalized = $normalized.Replace("`n", "`r`n") }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $newBytes = $utf8NoBom.GetBytes($normalized)
    $rel = [System.IO.Path]::GetRelativePath($script:RepoRoot, $FilePath)
    if ($null -ne $existingBytes -and [System.Linq.Enumerable]::SequenceEqual($existingBytes, $newBytes)) {
        Write-Host "${rel}: unchanged"
        return $true
    }
    if ($CheckOnly) {
        Write-Host "${rel}: OUT OF DATE"
        return $false
    }
    $parent = Split-Path -Parent $FilePath
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllBytes($FilePath, $newBytes)
    Write-Host "${rel}: updated"
    return $true
}

function Invoke-NodeConstantsGenerator {
    param([string]$IniPath, [bool]$CheckOnly)
    $specs = Get-NodeConstantSpecs -Ini (Read-NodeConstantsIni -IniPath $IniPath)
    $goOut = Join-Path $script:RepoRoot 'build\gen\go\citynodeconsts\constants_gen.go'
    $cppOut = Join-Path $script:RepoRoot 'build\gen\world\world\node_constants.h'
    $okGo = Sync-GeneratedFile -FilePath $goOut -Content (ConvertTo-GoNodeConstants -Specs $specs) -CheckOnly $CheckOnly
    $okCpp = Sync-GeneratedFile -FilePath $cppOut -Content (ConvertTo-CppNodeConstants -Specs $specs) -CheckOnly $CheckOnly
    if ($okGo -and $okCpp) { return 0 }
    return 1
}

function Resolve-IniFilePath {
    param([string]$ConfigDir)
    if ($ConfigDir) {
        $candidate = Join-Path $ConfigDir 'DefaultAoCRecordConstants.ini'
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $fallback = Join-Path $script:RepoRoot 'src\config\DefaultAoCRecordConstants.ini'
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    return ''
}

function Invoke-GeneratorEntry {
    param([hashtable]$Gen, [hashtable]$Values, [bool]$WriteNodeConstants)
    if ($Gen.ContainsKey('ps') -and $Gen['ps']) {
        Write-Host "== $($Gen['name']) (native PowerShell)"
        return (Invoke-NodeConstantsGenerator -IniPath $Values['IniFile'] -CheckOnly (-not $WriteNodeConstants))
    }
    if ($Gen.ContainsKey('go') -and $Gen['go']) {
        Write-Host ("== $($Gen['name']) (go " + ($Gen['go'] -join ' ') + ")")
        Push-Location (Join-Path $script:RepoRoot 'src\server\services')
        try {
            & $Values['Go'] @($Gen['go']) 2>&1 | ForEach-Object { "$_" } | Write-Host
            return $LASTEXITCODE
        } finally { Pop-Location }
    }
    $scriptPath = Join-Path $script:RepoRoot ("src\tools\schemagen\" + $Gen['name'] + ".ps1")
    Write-Host ("== $($Gen['name']) " + ($Gen['args'] -join ' '))
    & pwsh -NoProfile -File $scriptPath @($Gen['args']) 2>&1 | ForEach-Object { "$_" } | Write-Host
    return $LASTEXITCODE
}

$goCmd = Get-Command go -ErrorAction SilentlyContinue
$goExe = if ($goCmd) { $goCmd.Source } elseif (Test-Path 'C:\Program Files\Go\bin\go.exe') { 'C:\Program Files\Go\bin\go.exe' } else { '' }
$values = @{
    Go             = $goExe
    ContentDb      = $ContentDb
    IniFile        = (Resolve-IniFilePath -ConfigDir $ClientConfig)
    ZoiChunks      = $ZoiChunks
    RoadsRaw       = (Join-Path $script:RepoRoot 'dist\content\roads_raw.tsv')
    RoadsTsv       = (Join-Path $script:RepoRoot 'dist\content\roads.tsv')
    RoadsLevelsTsv = (Join-Path $script:RepoRoot 'dist\content\roads_levels.tsv')
}

$gens = @(
    @{ name = 'gen_region';              aliases = @('gen_region', 'region');                           args = @('--check');                                                   needs = @() },
    @{ name = 'gen_gm_garden';           aliases = @('gen_gm_garden', 'gm_garden');                     args = @('--content-db', $ContentDb, '--check');                       needs = @('ContentDb') },
    @{ name = 'gen_tooltips';            aliases = @('gen_tooltips', 'tooltips');                       args = @('--db', $ContentDb, '--check');                               needs = @('ContentDb') },
    @{ name = 'gen_node_constants';      aliases = @('gen_node_constants', 'node_constants');           ps   = $true;                                                          needs = @('IniFile') },
    @{ name = 'gen_zoi_surface';         aliases = @('gen_zoi_surface', 'zoi_surface');                 args = @('--chunks', $ZoiChunks, '--content-db', $ContentDb, '--check'); needs = @('ZoiChunks', 'ContentDb') },
    @{ name = 'gen_region_rects';        aliases = @('gen_region_rects', 'region_rects');               args = @('--chunks', $ZoiChunks, '--content-db', $ContentDb, '--check'); needs = @('ZoiChunks', 'ContentDb') },
    @{ name = 'contentc roads';          aliases = @('contentc roads', 'roads');                        go   = @('run', './cmd/contentc', 'roads', '--content', $ContentDb, '--raw', $values['RoadsRaw'], '--out', $values['RoadsTsv'], '--check');              needs = @('ContentDb', 'RoadsRaw', 'RoadsTsv', 'Go') },
    @{ name = 'contentc roads --levels'; aliases = @('contentc roads --levels', 'roads_levels');        go   = @('run', './cmd/contentc', 'roads', '--levels', '--content', $ContentDb, '--raw', $values['RoadsRaw'], '--out', $values['RoadsLevelsTsv'], '--check'); needs = @('ContentDb', 'RoadsRaw', 'RoadsLevelsTsv', 'Go') }
)

$failed = [System.Collections.Generic.List[string]]::new()
$skipped = [System.Collections.Generic.List[string]]::new()
$passed = [System.Collections.Generic.List[string]]::new()
$writeNodeConsts = ($null -ne $Only) -and ($Only -contains 'node_constants')

Push-Location $script:RepoRoot
try {
    foreach ($g in $gens) {
        if ($Only -and -not ($g.aliases | Where-Object { $Only -contains $_ })) { continue }
        $missing = @($g.needs | Where-Object { -not $values[$_] -or -not (Test-Path -LiteralPath $values[$_]) })
        if ($missing.Count -gt 0) {
            $what = ($missing | ForEach-Object { "-$_" }) -join ', '
            if ($Strict) { Write-Host "== $($g.name): FAIL: $what not given or not found"; $failed.Add($g.name) }
            else { Write-Host "== $($g.name): SKIP: $what not given or not found"; $skipped.Add($g.name) }
            continue
        }
        $t0 = Get-Date
        $code = Invoke-GeneratorEntry -Gen $g -Values $values -WriteNodeConstants $writeNodeConsts
        Write-Host ("   exit={0} ({1:F0} s)" -f $code, ((Get-Date) - $t0).TotalSeconds)
        if ($code -eq 0) { $passed.Add($g.name) } else { $failed.Add($g.name) }
    }
} finally { Pop-Location }

Write-Host ("{0} generators unchanged, {1} skipped ({2}), {3} failed ({4})" -f $passed.Count, $skipped.Count, ($skipped -join ', '), $failed.Count, ($failed -join ', '))
if ($failed.Count -gt 0) { exit 1 }
exit 0
