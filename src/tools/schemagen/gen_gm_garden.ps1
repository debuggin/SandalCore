#Requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$script:Professions = @('Mining', 'Lumberjacking', 'Herbalism')
$script:Tiers = @('Novice', 'Apprentice', 'Journeyman', 'Master', 'GrandMaster')
$script:AvoidSubstrings = @('Freehold', 'FTUE', 'TEST', 'Test', 'NodeWar', 'Corrupted', 'Livestock', 'RELIC', 'ZDELETE')
$script:PreferMap = @{
    'Mining|Novice'            = @('Stone_Granite_Large_1', 'Stone_Basalt_Large_1')
    'Mining|Apprentice'        = @('Stone_Slate_Large_1')
    'Mining|Journeyman'        = @('Stone_Limestone_Large_1')
    'Lumberjacking|Novice'     = @('Tree_Oak_Large_1', 'Tree_Ash_Large_1')
    'Lumberjacking|Apprentice' = @('Tree_WeepingWillow_Large_1')
    'Herbalism|Novice'         = @('Plant_Daffodil_Medium_1', 'Plant_Snowdrop_Medium_1')
    'Herbalism|Apprentice'     = @('Plant_Moonbell_Medium_1')
    'Herbalism|Journeyman'     = @('Plant_Spindlevine_Medium_1')
}

function Resolve-GardenArgs {
    param([string[]]$CliArgs)
    $contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $script:RepoRoot 'dist\content\content.db' }
    $headerOut = Join-Path $script:RepoRoot 'build\gen\world\spawns\garden_defs.h'
    $checkOnly = $false
    $positional = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $CliArgs.Count; $i++) {
        $arg = $CliArgs[$i]
        switch -Regex ($arg) {
            '^--?check$' { $checkOnly = $true }
            '^--?(content-)?db$' {
                if ($i + 1 -ge $CliArgs.Count) { throw "Missing value for $arg" }
                $i++; $contentDb = $CliArgs[$i]
            }
            '^--?(out|header)$' {
                if ($i + 1 -ge $CliArgs.Count) { throw "Missing value for $arg" }
                $i++; $headerOut = $CliArgs[$i]
            }
            default {
                if ($arg.StartsWith('-')) { throw "Unknown option: $arg" }
                $positional.Add($arg)
            }
        }
    }
    if ($positional.Count -ge 1) { $contentDb = $positional[0] }
    if ($positional.Count -ge 2) { $headerOut = $positional[1] }
    return [pscustomobject]@{ ContentDb = $contentDb; HeaderOut = $headerOut; Check = $checkOnly }
}

function Initialize-ManagedSqlite {
    if ('Microsoft.Data.Sqlite.SqliteConnection' -as [type]) { return $true }
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $binDll = Get-ChildItem -Path (Join-Path $script:RepoRoot 'build\dotnet\bin') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($binDll) {
        $dir = $binDll.DirectoryName
        $native = Get-ChildItem -Path (Join-Path $dir "runtimes\$rid\native") -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $native) { $native = Get-ChildItem -Path $dir -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Select-Object -First 1 }
        if ($native) { [void][System.Runtime.InteropServices.NativeLibrary]::Load($native.FullName) }
        foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
            $p = Join-Path $dir $dll
            if (Test-Path -LiteralPath $p) { Add-Type -LiteralPath $p }
        }
        [SQLitePCL.Batteries_V2]::Init()
        return $true
    }
    $nuget = @($env:NUGET_PACKAGES, (Join-Path $HOME '.nuget/packages'), (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages')) |
        Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $nuget) { return $false }
    $nativePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.lib.e_sqlite3') -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -Last 1
    $corePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.core') -Recurse -Filter 'SQLitePCLRaw.core.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $provPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.provider.e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.provider.e_sqlite3.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $batPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.bundle_e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.batteries_v2.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $mdsPkg = Get-ChildItem -Path (Join-Path $nuget 'microsoft.data.sqlite.core/8.*') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net8\.0' } | Select-Object -Last 1
    if ($nativePkg -and $corePkg -and $provPkg -and $batPkg -and $mdsPkg) {
        [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativePkg.FullName)
        foreach ($dll in @($corePkg.FullName, $provPkg.FullName, $batPkg.FullName, $mdsPkg.FullName)) { Add-Type -LiteralPath $dll }
        [SQLitePCL.Batteries_V2]::Init()
        return $true
    }
    return $false
}

function Expand-GzipJsonNode {
    param([byte[]]$CompressedBytes)
    $ms = [System.IO.MemoryStream]::new($CompressedBytes)
    try {
        $gz = [System.IO.Compression.GZipStream]::new($ms, [System.IO.Compression.CompressionMode]::Decompress)
        try {
            $sr = [System.IO.StreamReader]::new($gz, [System.Text.Encoding]::UTF8)
            try {
                return ($sr.ReadToEnd() | ConvertFrom-Json -AsHashtable)
            } finally { $sr.Dispose() }
        } finally { $gz.Dispose() }
    } finally { $ms.Dispose() }
}

function Read-GardenDbWithManagedSqlite {
    param([string]$DbPath, [string[]]$Schemas)
    $cb = [Microsoft.Data.Sqlite.SqliteConnectionStringBuilder]::new()
    $cb.DataSource = [System.IO.Path]::GetFullPath($DbPath)
    $cb.Mode = [Microsoft.Data.Sqlite.SqliteOpenMode]::ReadOnly
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new($cb.ToString())
    $bySchema = @{}
    foreach ($s in $Schemas) { $bySchema[$s] = @{} }
    $meta = @{}
    try {
        $conn.Open()
        $metaCmd = $conn.CreateCommand()
        $metaCmd.CommandText = 'SELECT key, value FROM meta'
        $metaReader = $metaCmd.ExecuteReader()
        try {
            while ($metaReader.Read()) { $meta[$metaReader.GetString(0)] = $metaReader.GetString(1) }
        } finally { $metaReader.Dispose() }
        $inList = ($Schemas | ForEach-Object { "'$_'" }) -join ', '
        $recCmd = $conn.CreateCommand()
        $recCmd.CommandText = "SELECT schema, guid, type_id, name, dev_only, data FROM record WHERE schema IN ($inList)"
        $recReader = $recCmd.ExecuteReader()
        try {
            while ($recReader.Read()) {
                $schema = $recReader.GetString(0)
                $guid = $recReader.GetInt64(1)
                $bySchema[$schema][$guid] = [pscustomobject]@{
                    Guid    = $guid
                    TypeId  = $recReader.GetInt64(2)
                    Name    = $recReader.GetString(3)
                    DevOnly = ($recReader.GetInt64(4) -ne 0)
                    Data    = (Expand-GzipJsonNode -CompressedBytes ([byte[]]$recReader.GetValue(5)))
                }
            }
        } finally { $recReader.Dispose() }
    } finally { $conn.Dispose() }
    return [pscustomobject]@{ BySchema = $bySchema; Meta = $meta }
}

function Read-GardenDbWithSqliteCli {
    param([string]$SqliteExe, [string]$DbPath, [string[]]$Schemas)
    $fullPath = [System.IO.Path]::GetFullPath($DbPath)
    $bySchema = @{}
    foreach ($s in $Schemas) { $bySchema[$s] = @{} }
    $meta = @{}
    $metaRaw = @(& $SqliteExe -readonly -batch -noheader -separator "`t" $fullPath 'SELECT key, value FROM meta;' 2>&1)
    if ($LASTEXITCODE -ne 0) { throw (($metaRaw | ForEach-Object { "$_" }) -join ' ').Trim() }
    foreach ($line in $metaRaw) {
        $t = "$line".TrimEnd("`r", "`n")
        if ([string]::IsNullOrWhiteSpace($t)) { continue }
        $p = $t -split "`t", 2
        if ($p.Count -eq 2) { $meta[$p[0]] = $p[1] }
    }
    $inList = ($Schemas | ForEach-Object { "'$_'" }) -join ', '
    $sql = "SELECT schema, guid, type_id, name, dev_only, hex(data) FROM record WHERE schema IN ($inList);"
    $recRaw = @(& $SqliteExe -readonly -batch -noheader -separator "`t" $fullPath $sql 2>&1)
    if ($LASTEXITCODE -ne 0) { throw (($recRaw | ForEach-Object { "$_" }) -join ' ').Trim() }
    foreach ($line in $recRaw) {
        $t = "$line".TrimEnd("`r", "`n")
        if ([string]::IsNullOrWhiteSpace($t)) { continue }
        $p = $t -split "`t", 6
        $guid = [int64]$p[1]
        $bySchema[$p[0]][$guid] = [pscustomobject]@{
            Guid    = $guid
            TypeId  = [int64]$p[2]
            Name    = $p[3]
            DevOnly = ([int64]$p[4] -ne 0)
            Data    = (Expand-GzipJsonNode -CompressedBytes ([Convert]::FromHexString($p[5])))
        }
    }
    return [pscustomobject]@{ BySchema = $bySchema; Meta = $meta }
}

function Read-GardenDatabase {
    param([string]$DbPath)
    $schemas = @(
        'GatherableActorDefRecordBase',
        'GatheringFoliageDataRecordBase',
        'GatheringResourceTypeRecordBase',
        'CertificationLevelRecordBase',
        'CharacterProfessionsRecordBase',
        'GatheringFoliageStateRecordBase',
        'GatheringInteractionDataRecordBase'
    )
    if (Initialize-ManagedSqlite) {
        return Read-GardenDbWithManagedSqlite -DbPath $DbPath -Schemas $schemas
    }
    $sqliteCli = Get-Command sqlite3 -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $sqliteCli) {
        return Read-GardenDbWithSqliteCli -SqliteExe $sqliteCli.Source -DbPath $DbPath -Schemas $schemas
    }
    throw 'Neither Microsoft.Data.Sqlite nor sqlite3 CLI is available.'
}

function Get-FoliageStateInteractions {
    param([object]$FoliageNode, [hashtable]$States, [hashtable]$Inter, [hashtable]$Profs, [hashtable]$Certs)
    $seenKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $list = [System.Collections.Generic.List[object]]::new()
    foreach ($kv in $FoliageNode.Data['FoliageStateDataIds']) {
        $sGuid = [int64]$kv['Value']['Guid']
        if (-not $States.ContainsKey($sGuid)) { continue }
        $sNode = $States[$sGuid].Data['FoliageInteractionsIds']
        if ($null -eq $sNode) { continue }
        foreach ($ii in $sNode) {
            $iGuid = [int64]$ii['Guid']
            if (-not $Inter.ContainsKey($iGuid)) { continue }
            $iRec = $Inter[$iGuid]
            $ipGuid = [int64]$iRec.Data['ProfessionId']['Guid']
            $icGuid = [int64]$iRec.Data['CertificationLevelId']['Guid']
            $ipName = if ($Profs.ContainsKey($ipGuid)) { $Profs[$ipGuid].Name } else { '?' }
            $icName = if ($Certs.ContainsKey($icGuid)) { $Certs[$icGuid].Name } else { '?' }
            $toolRaw = [string]$iRec.Data['ToolType']
            $tool = ($toolRaw -split '::')[-1]
            $dur = [double]$iRec.Data['BaseGatheringDuration']
            $key = "$ipName|$icName|$tool|$($dur.ToString('R', [System.Globalization.CultureInfo]::InvariantCulture))"
            if ($seenKeys.Add($key)) {
                $list.Add([pscustomobject]@{ Prof = $ipName; Cert = $icName; Tool = $tool; Duration = $dur })
            }
        }
    }
    return ,$list.ToArray()
}

function Get-GardenCandidates {
    param([string]$DbPath)
    $db = Read-GardenDatabase -DbPath $DbPath
    $defs = $db.BySchema['GatherableActorDefRecordBase']
    $foliage = $db.BySchema['GatheringFoliageDataRecordBase']
    $rtypes = $db.BySchema['GatheringResourceTypeRecordBase']
    $certs = $db.BySchema['CertificationLevelRecordBase']
    $profs = $db.BySchema['CharacterProfessionsRecordBase']
    $states = $db.BySchema['GatheringFoliageStateRecordBase']
    $inter = $db.BySchema['GatheringInteractionDataRecordBase']
    $cands = @{}
    foreach ($g in $defs.Keys) {
        $d = $defs[$g]
        $fGuid = [int64]$d.Data['FoliageActorDefId']['Guid']
        if (-not $foliage.ContainsKey($fGuid)) { continue }
        $f = $foliage[$fGuid]
        $rtGuid = [int64]$f.Data['GatheringResourceTypeId']['Guid']
        if (-not $rtypes.ContainsKey($rtGuid)) { continue }
        $t = $rtypes[$rtGuid]
        $ps = [System.Collections.Generic.List[string]]::new()
        foreach ($p in $t.Data['RequiredProfessionIds']) {
            $pGuid = [int64]$p['Guid']
            if ($profs.ContainsKey($pGuid)) { $ps.Add($profs[$pGuid].Name) }
        }
        $cGuid = [int64]$t.Data['RequiredCertificationLevelId']['Guid']
        $c = if ($certs.ContainsKey($cGuid)) { $certs[$cGuid] } else { $null }
        if ($ps.Count -ne 1 -or ($script:Professions -notcontains $ps[0]) -or $null -eq $c -or ($script:Tiers -notcontains $c.Name)) { continue }
        $interactions = Get-FoliageStateInteractions -FoliageNode $f -States $states -Inter $inter -Profs $profs -Certs $certs
        $bestTierIdx = -1
        foreach ($item in $interactions) {
            if ($item.Prof -eq $ps[0]) {
                $idx = [array]::IndexOf($script:Tiers, $item.Cert)
                if ($idx -gt $bestTierIdx) { $bestTierIdx = $idx }
            }
        }
        $tier = if ($bestTierIdx -ge 0) { $script:Tiers[$bestTierIdx] } else { $c.Name }
        $cands[$g] = [pscustomobject]@{
            Guid         = [uint64]$g
            TypeId       = [uint64]$d.TypeId
            Name         = $d.Name
            DevOnly      = $d.DevOnly
            Profession   = $ps[0]
            Tier         = $tier
            Interactions = $interactions
            Resource     = $t.Name
            Interaction  = [uint64]([int64]$d.Data['InteractionDefId']['Guid'])
        }
    }
    return [pscustomobject]@{ Candidates = $cands; Meta = $db.Meta }
}

function Compare-GardenCandidate {
    param([pscustomobject]$A, [pscustomobject]$B, [AllowEmptyCollection()][string[]]$Pref)
    if ($null -eq $Pref) { $Pref = [string[]]::new(0) }
    $idxA = [array]::IndexOf($Pref, $A.Name); if ($idxA -lt 0) { $idxA = $Pref.Count }
    $idxB = [array]::IndexOf($Pref, $B.Name); if ($idxB -lt 0) { $idxB = $Pref.Count }
    if ($idxA -ne $idxB) { return $idxA.CompareTo($idxB) }
    $avA = 0; foreach ($s in $script:AvoidSubstrings) { if ($A.Name.Contains($s, [System.StringComparison]::Ordinal)) { $avA = 1; break } }
    $avB = 0; foreach ($s in $script:AvoidSubstrings) { if ($B.Name.Contains($s, [System.StringComparison]::Ordinal)) { $avB = 1; break } }
    if ($avA -ne $avB) { return $avA.CompareTo($avB) }
    $szA = if ($A.Name.Contains('_Large_', [System.StringComparison]::Ordinal)) { 0 } elseif ($A.Name.Contains('_Medium_', [System.StringComparison]::Ordinal)) { 1 } else { 2 }
    $szB = if ($B.Name.Contains('_Large_', [System.StringComparison]::Ordinal)) { 0 } elseif ($B.Name.Contains('_Medium_', [System.StringComparison]::Ordinal)) { 1 } else { 2 }
    if ($szA -ne $szB) { return $szA.CompareTo($szB) }
    $v1A = if ($A.Name.EndsWith('_1', [System.StringComparison]::Ordinal)) { 0 } else { 1 }
    $v1B = if ($B.Name.EndsWith('_1', [System.StringComparison]::Ordinal)) { 0 } else { 1 }
    if ($v1A -ne $v1B) { return $v1A.CompareTo($v1B) }
    return [System.StringComparer]::Ordinal.Compare($A.Name, $B.Name)
}

function Select-GardenSlots {
    param([hashtable]$Candidates)
    $slots = [System.Collections.Generic.List[object]]::new()
    foreach ($p in $script:Professions) {
        for ($ti = 0; $ti -lt $script:Tiers.Count; $ti++) {
            $t = $script:Tiers[$ti]
            $here = [System.Collections.Generic.List[object]]::new()
            foreach ($c in $Candidates.Values) {
                if ($c.Profession -eq $p -and $c.Tier -eq $t -and -not $c.DevOnly) { $here.Add($c) }
            }
            $prefKey = "$p|$t"
            $pref = if ($script:PreferMap.ContainsKey($prefKey)) { $script:PreferMap[$prefKey] } else { @() }
            $arr = $here.ToArray()
            for ($i = 1; $i -lt $arr.Count; $i++) {
                $cur = $arr[$i]
                $j = $i - 1
                while ($j -ge 0 -and (Compare-GardenCandidate -A $arr[$j] -B $cur -Pref $pref) -gt 0) {
                    $arr[$j + 1] = $arr[$j]
                    $j--
                }
                $arr[$j + 1] = $cur
            }
            $picked = if ($arr.Count -gt 0) { $arr[0] } else { $null }
            $slots.Add([pscustomobject]@{ Profession = $p; TierIndex = $ti; TierName = $t; Pick = $picked; Count = $arr.Count })
        }
    }
    return ,$slots.ToArray()
}

function Format-PythonFloat {
    param([double]$Val)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    if ($Val -eq [Math]::Floor($Val) -and -not [double]::IsInfinity($Val)) {
        return $Val.ToString('F1', $inv)
    }
    return $Val.ToString('R', $inv)
}

function ConvertTo-GardenHeader {
    param([object[]]$Slots, [hashtable]$Meta)
    $ver = if ($Meta.ContainsKey('contentc.version')) { $Meta['contentc.version'] } else { '?' }
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('// ============================================================================')
    $lines.Add('//  server/worldd/spawns/garden_defs.h')
    $lines.Add('//')
    $lines.Add("//  GENERATED by tools/capture/gen_gm_garden.py from content.db (contentc version $ver); do not edit.")
    $lines.Add('//')
    $lines.Add('//  The GatherableActorDefs of the GM garden (spawns/gm_garden.h): one real def for each gathering profession and certification tier the design data has an actor node')
    $lines.Add('//  for.  The data has none at Master, and none at GrandMaster for Mining and Lumberjacking (those tiers'' nodes are painted foliage, no actor): kGardenGaps lists them.')
    $lines.Add('// ============================================================================')
    $lines.Add('#pragma once')
    $lines.Add('')
    $lines.Add('#include <cstddef>')
    $lines.Add('#include <cstdint>')
    $lines.Add('')
    $lines.Add('namespace aoc::worldd::spawns::garden {')
    $lines.Add('')
    $lines.Add('/// A GatherableActorDef record: its Guid and TypeId as the actor''s replicated GatherableActorDef struct names them.')
    $lines.Add('struct Def {')
    $lines.Add('    const char* profession;     // Mining, Lumberjacking, Herbalism')
    $lines.Add('    int         tier;           // 0 Novice .. 4 GrandMaster')
    $lines.Add('    const char* tier_name;')
    $lines.Add('    uint64_t    guid;')
    $lines.Add('    uint64_t    type_id;')
    $lines.Add('    uint64_t    interaction;    // its PlayerInteractionDef record (the node''s InteractableComponent names it)')
    $lines.Add('    const char* name;           // the record''s name')
    $lines.Add('    const char* resource;       // its GatheringResourceType ("Tree_Oak")')
    $lines.Add('    const char* tool;           // the interaction''s tool slot (Axe, Pickaxe, Sickle)')
    $lines.Add('    double      base_duration;  // the interaction''s BaseGatheringDuration, seconds')
    $lines.Add('    int         candidates;     // the defs the slot had to choose from')
    $lines.Add('};')
    $lines.Add('')
    $lines.Add('/// A profession and tier the design data has no actor node for.')
    $lines.Add('struct Gap {')
    $lines.Add('    const char* profession;')
    $lines.Add('    int         tier;')
    $lines.Add('    const char* tier_name;')
    $lines.Add('};')
    $lines.Add('')
    $lines.Add('inline constexpr Def kGardenDefs[] = {')
    $gaps = [System.Collections.Generic.List[object]]::new()
    foreach ($s in $Slots) {
        if ($null -eq $s.Pick) { $gaps.Add($s); continue }
        $c = $s.Pick
        $bestInter = $null
        foreach ($i in $c.Interactions) {
            if ($i.Prof -eq $s.Profession -and $i.Cert -eq $s.TierName) {
                if ($null -eq $bestInter -or $i.Duration -gt $bestInter.Duration -or ($i.Duration -eq $bestInter.Duration -and [System.StringComparer]::Ordinal.Compare($i.Tool, $bestInter.Tool) -lt 0)) {
                    $bestInter = $i
                }
            }
        }
        $tool = if ($null -ne $bestInter) { $bestInter.Tool } else { '?' }
        $dur = if ($null -ne $bestInter) { [double]$bestInter.Duration } else { 0.0 }
        $durStr = Format-PythonFloat -Val $dur
        $lines.Add("    {`"$($s.Profession)`", $($s.TierIndex), `"$($s.TierName)`", $($c.Guid)ull, $($c.TypeId)ull, $($c.Interaction)ull, `"$($c.Name)`", `"$($c.Resource)`", `"$tool`", $durStr, $($s.Count)},")
    }
    $lines.Add('};')
    $lines.Add('inline constexpr size_t kGardenDefCount = sizeof(kGardenDefs) / sizeof(kGardenDefs[0]);')
    $lines.Add('')
    $lines.Add('inline constexpr Gap kGardenGaps[] = {')
    foreach ($g in $gaps) { $lines.Add("    {`"$($g.Profession)`", $($g.TierIndex), `"$($g.TierName)`"},") }
    $lines.Add('};')
    $lines.Add('inline constexpr size_t kGardenGapCount = sizeof(kGardenGaps) / sizeof(kGardenGaps[0]);')
    $lines.Add('')
    $lines.Add('}  // namespace aoc::worldd::spawns::garden')
    return ($lines -join "`n") + "`n"
}

$cfg = Resolve-GardenArgs -CliArgs $args
if (-not (Test-Path -LiteralPath $cfg.ContentDb -PathType Leaf)) {
    [Console]::Error.WriteLine("content.db not found: $($cfg.ContentDb)")
    exit 1
}
$collected = Get-GardenCandidates -DbPath $cfg.ContentDb
$slots = Select-GardenSlots -Candidates $collected.Candidates
foreach ($s in $slots) {
    $desc = if ($null -ne $s.Pick) { "$($s.Pick.Name) ($($s.Count) candidates)" } else { 'NO ACTOR DEF' }
    Write-Host ('{0,-14} {1,-11} {2}' -f $s.Profession, $s.TierName, $desc)
}
$text = ConvertTo-GardenHeader -Slots $slots -Meta $collected.Meta
$old = if (Test-Path -LiteralPath $cfg.HeaderOut) { [System.IO.File]::ReadAllText($cfg.HeaderOut, [System.Text.Encoding]::UTF8) } else { $null }
$same = ($null -ne $old) -and ($old.Replace("`r`n", "`n") -eq $text)
if ($cfg.Check) {
    $state = if ($same) { 'unchanged' } else { 'WOULD CHANGE' }
    Write-Host "garden_defs.h: $state"
    if ($same) { exit 0 } else { exit 1 }
}
if ($same) {
    Write-Host 'garden_defs.h: unchanged'
    exit 0
}
$dir = Split-Path -Parent $cfg.HeaderOut
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllBytes($cfg.HeaderOut, $utf8NoBom.GetBytes($text))
Write-Host 'garden_defs.h: updated'
exit 0
