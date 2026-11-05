#!/usr/bin/env pwsh
#Requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 SandalCore contributors.

[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string]$DbPath = '',
    [Parameter(Position = 1)]
    [string]$OutPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-SqliteAssemblies {
    param([string]$BaseDir, [string]$NativePath)
    if ($NativePath -and (Test-Path -LiteralPath $NativePath)) {
        [void][System.Runtime.InteropServices.NativeLibrary]::Load($NativePath)
    }
    foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
        $full = Join-Path $BaseDir $dll
        if (Test-Path -LiteralPath $full) { Add-Type -LiteralPath $full }
    }
    [SQLitePCL.Batteries_V2]::Init()
}

function Initialize-ManagedSqlite {
    param([string]$RepoRoot)
    if ('Microsoft.Data.Sqlite.SqliteConnection' -as [type]) { return }
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $binDir = Get-ChildItem -Path (Join-Path $RepoRoot 'build/dotnet/bin') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($binDir) {
        $dir = $binDir.DirectoryName
        $native = Get-ChildItem -Path $dir -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -First 1
        Import-SqliteAssemblies -BaseDir $dir -NativePath ($native ? $native.FullName : '')
        return
    }
    $nuget = @($env:NUGET_PACKAGES, (Join-Path $HOME '.nuget/packages'), (Join-Path ([Environment]::GetFolderPath('UserProfile')) '.nuget/packages')) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $nuget) { throw 'Microsoft.Data.Sqlite assemblies not found in build/dotnet/bin or NuGet cache.' }
    $nativePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.lib.e_sqlite3') -Recurse -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match $rid } | Select-Object -Last 1
    $corePkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.core') -Recurse -Filter 'SQLitePCLRaw.core.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $provPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.provider.e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.provider.e_sqlite3.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $batPkg = Get-ChildItem -Path (Join-Path $nuget 'sqlitepclraw.bundle_e_sqlite3') -Recurse -Filter 'SQLitePCLRaw.batteries_v2.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'netstandard2\.0|net8\.0' } | Select-Object -Last 1
    $mdsPkg = Get-ChildItem -Path (Join-Path $nuget 'microsoft.data.sqlite.core') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -match 'net8\.0|netstandard2\.0' } | Select-Object -Last 1
    if ($nativePkg) { [void][System.Runtime.InteropServices.NativeLibrary]::Load($nativePkg.FullName) }
    foreach ($pkg in @($corePkg, $provPkg, $batPkg, $mdsPkg)) { Add-Type -LiteralPath $pkg.FullName }
    [SQLitePCL.Batteries_V2]::Init()
}

function Open-ReadOnlySqlite {
    param([string]$Path)
    $cb = [Microsoft.Data.Sqlite.SqliteConnectionStringBuilder]::new()
    $cb.DataSource = [System.IO.Path]::GetFullPath($Path)
    $cb.Mode = [Microsoft.Data.Sqlite.SqliteOpenMode]::ReadOnly
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new($cb.ToString())
    $conn.Open()
    return $conn
}

function Format-SqlLiteral {
    param([object]$Val)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    if ($null -eq $Val -or $Val -is [System.DBNull]) { return 'NULL' }
    if ($Val -is [string]) { return "'" + $Val.Replace("'", "''") + "'" }
    if ($Val -is [double] -or $Val -is [float]) {
        $d = [double]$Val
        if ($d -eq 0.0) { return ([System.BitConverter]::DoubleToInt64Bits($d) -lt 0) ? '-0.0' : '0.0' }
        $abs = [Math]::Abs($d)
        if ($abs -ge 1e-4 -and $abs -lt 1e16) {
            $s = $d.ToString('R', $inv)
            if ($s -match '[eE]') { $s = ([decimal]$d).ToString($inv) }
            return ($s -match '^[+-]?\d+$') ? "$s.0" : $s
        }
        return $d.ToString('R', $inv).ToLowerInvariant()
    }
    return [System.Convert]::ToString($Val, $inv)
}

function Get-ScalarStringList {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn, [string]$Sql)
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = $Sql
    $reader = $cmd.ExecuteReader()
    $out = [System.Collections.Generic.List[string]]::new()
    try {
        while ($reader.Read()) {
            $out.Add([System.Convert]::ToString($reader.GetValue(0), [System.Globalization.CultureInfo]::InvariantCulture))
        }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
    return ,$out.ToArray()
}

function Add-QueryStatements {
    param(
        [Microsoft.Data.Sqlite.SqliteConnection]$Conn,
        [string]$Table,
        [string[]]$Cols,
        [string]$Query,
        [System.Collections.Generic.List[string]]$Stmts
    )
    $verb = ($Table -eq 'item_equip_slot') ? 'INSERT OR IGNORE' : 'INSERT OR REPLACE'
    $colList = $Cols -join ', '
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = $Query
    $reader = $cmd.ExecuteReader()
    try {
        $fieldCount = $reader.FieldCount
        while ($reader.Read()) {
            $vals = [string[]]::new($fieldCount)
            for ($i = 0; $i -lt $fieldCount; $i++) { $vals[$i] = Format-SqlLiteral -Val $reader.GetValue($i) }
            $Stmts.Add("$verb INTO $Table ($colList) VALUES ($($vals -join ', '))")
        }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
}

function Get-ClassStartingKits {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $kits = [ordered]@{}
    $classNames = Get-ScalarStringList -Conn $Conn -Sql 'select distinct class_name from starting_equipment order by class_name'
    foreach ($cls in $classNames) {
        $cmd = $Conn.CreateCommand()
        $cmd.CommandText = "select slot, item_guid from starting_equipment where class_name='$cls' and container='Equipment' order by ord"
        $reader = $cmd.ExecuteReader()
        $items = [System.Collections.Generic.List[object]]::new()
        try {
            while ($reader.Read()) {
                $items.Add([pscustomobject]@{ Slot = $reader.GetInt32(0); Item = $reader.GetInt64(1) })
            }
        } finally {
            $reader.Dispose()
            $cmd.Dispose()
        }
        $kits[$cls] = $items.ToArray()
    }
    return $kits
}

function Get-ClassGuidMap {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $map = @{}
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = 'select guid, name from class'
    $reader = $cmd.ExecuteReader()
    try {
        while ($reader.Read()) { $map[$reader.GetString(1)] = $reader.GetInt64(0) }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
    return $map
}

function Build-StatBlockColumns {
    $cols = [System.Collections.Generic.List[string]]::new()
    $cols.Add('block_guid')
    $cols.Add('stat_guid')
    foreach ($r in @('common', 'uncommon', 'rare', 'heroic', 'epic', 'legendary', 'artifact')) {
        foreach ($m in @('min', 'max')) { $cols.Add("${r}_${m}") }
    }
    return ,$cols.ToArray()
}

function Format-GearHeaderLines {
    param(
        [System.Collections.Generic.List[string]]$Stmts,
        [System.Collections.Specialized.OrderedDictionary]$Kits,
        [hashtable]$ClassGuids
    )
    $enumOrder = @('Ranger', 'Bard', 'Cleric', 'Fighter', 'Rogue', 'Summoner', 'Tank', 'Mage')
    $enumMap = @{ 'Ranger' = 1; 'Bard' = 2; 'Cleric' = 3; 'Fighter' = 4; 'Rogue' = 5; 'Summoner' = 6; 'Tank' = 7; 'Mage' = 8 }
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add((@'
// ============================================================================
//  server/worldd/tests/gear_fixture.h
//
//  GENERATED by tools/worldd/gen_gear_fixture.py from a content.db (contentc version 4); do not edit.
//
//  The real rows the stat engine reads for the eight classes' starting kits (plan B3): EEquipSlot names, the eight classes, the
//  kits' item records with their stat blocks, quality curve, tags and sets. With them the fixture content.db (content_db_fixture.h)
//  lets the tests run the real stat program on the real kits without the 275 MB file: the stats of each class's level-1 kit then
//  equal the corpus's `kit_<class>_l1` case, and the captured Bard's equal pkt#107.
// ============================================================================
#pragma once

#include <cstdint>

namespace gear_fixture {

struct KitItem {
    int     slot;      // the EEquipSlot value
    int64_t record;    // the ItemRecord Guid; its quality is the record's default (1)
};

/// One statement each, run in order after the tables are created.
inline constexpr const char* kStatements[] = {
'@ -replace "`r`n", "`n"))
    foreach ($s in $Stmts) { $lines.Add("    `"$s`",") }
    $lines.Add("};`n")
    foreach ($cls in $enumOrder) {
        if (-not $Kits.Contains($cls)) { continue }
        $lines.Add("inline constexpr KitItem k${cls}Kit[] = {")
        foreach ($entry in $Kits[$cls]) { $lines.Add("    {$($entry.Slot), $($entry.Item)ll},") }
        $lines.Add('};')
    }
    $lines.Add((@'

struct ClassKit {
    int            class_id;   // ECharacterClass
    const char*    name;
    int64_t        guid;
    const KitItem* items;
    int            count;
};
inline constexpr ClassKit kClassKits[] = {
'@ -replace "`r`n", "`n"))
    foreach ($cls in $enumOrder) {
        if (-not $Kits.Contains($cls)) { continue }
        $lines.Add("    {$($enumMap[$cls]), `"$cls`", $($ClassGuids[$cls])ll, k${cls}Kit, $($Kits[$cls].Count)},")
    }
    $lines.Add("};`n`n}  // namespace gear_fixture`n")
    return ($lines -join "`n")
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$resolvedDb = $DbPath ? $DbPath : (Join-Path $repoRoot 'dist/content/content.db')
$resolvedOut = $OutPath ? $OutPath : (Join-Path $repoRoot 'build/gen/world/tests/gear_fixture.h')

Initialize-ManagedSqlite -RepoRoot $repoRoot
$conn = Open-ReadOnlySqlite -Path $resolvedDb
try {
    $kits = Get-ClassStartingKits -Conn $conn
    $itemSet = [System.Collections.Generic.SortedSet[long]]::new()
    foreach ($k in $kits.Values) {
        foreach ($entry in $k) { [void]$itemSet.Add($entry.Item) }
    }
    $q = ($itemSet | ForEach-Object { $_.ToString([System.Globalization.CultureInfo]::InvariantCulture) }) -join ','
    $blocks = Get-ScalarStringList -Conn $conn -Sql "select distinct stat_block_guid from item where guid in ($q) and stat_block_guid<>0 order by 1"
    $curves = Get-ScalarStringList -Conn $conn -Sql "select distinct quality_curve_guid from item where guid in ($q) and quality_curve_guid<>0 order by 1"
    $sets = Get-ScalarStringList -Conn $conn -Sql "select distinct set_guid from item_set where item_guid in ($q) order by 1"

    $stmts = [System.Collections.Generic.List[string]]::new()
    Add-QueryStatements -Conn $conn -Table 'equip_slot' -Cols @('slot', 'name') -Query 'select slot, name from equip_slot order by slot' -Stmts $stmts
    Add-QueryStatements -Conn $conn -Table 'class' -Cols @('guid', 'name', 'archetype_id') -Query 'select guid, name, archetype_id from class order by guid' -Stmts $stmts
    $itemCols = @('guid', 'name', 'level', 'can_equip', 'occupies_all_equip_slots', 'default_quality', 'stat_block_guid', 'quality_curve_guid')
    Add-QueryStatements -Conn $conn -Table 'item' -Cols $itemCols -Query "select $($itemCols -join ', ') from item where guid in ($q) order by guid" -Stmts $stmts
    Add-QueryStatements -Conn $conn -Table 'item_equip_slot' -Cols @('item_guid', 'ord', 'slot') -Query "select item_guid, ord, slot from item_equip_slot where item_guid in ($q) order by item_guid, ord" -Stmts $stmts
    if ($curves.Count -gt 0) {
        Add-QueryStatements -Conn $conn -Table 'quality_curve' -Cols @('guid', 'ord', 'x', 'y') -Query "select guid, ord, x, y from quality_curve where guid in ($($curves -join ',')) order by guid, ord" -Stmts $stmts
    }
    $statCols = Build-StatBlockColumns
    if ($blocks.Count -gt 0) {
        Add-QueryStatements -Conn $conn -Table 'stat_block_stat' -Cols $statCols -Query "select $($statCols -join ', ') from stat_block_stat where block_guid in ($($blocks -join ',')) order by block_guid, stat_guid" -Stmts $stmts
    }
    Add-QueryStatements -Conn $conn -Table 'item_tag' -Cols @('item_guid', 'ord', 'tag') -Query "select item_guid, ord, tag from item_tag where item_guid in ($q) order by item_guid, ord" -Stmts $stmts
    Add-QueryStatements -Conn $conn -Table 'item_set' -Cols @('item_guid', 'ord', 'set_guid') -Query "select item_guid, ord, set_guid from item_set where item_guid in ($q) order by item_guid, ord" -Stmts $stmts
    if ($sets.Count -gt 0) {
        $bonusCols = @('set_guid', 'tier_ord', 'pieces', 'bonus_ord', 'stat_guid', 'common', 'uncommon', 'rare', 'heroic', 'epic', 'legendary', 'artifact')
        Add-QueryStatements -Conn $conn -Table 'set_bonus' -Cols $bonusCols -Query "select $($bonusCols -join ', ') from set_bonus where set_guid in ($($sets -join ',')) order by set_guid, tier_ord, bonus_ord" -Stmts $stmts
    }

    $classGuids = Get-ClassGuidMap -Conn $conn
    $headerText = Format-GearHeaderLines -Stmts $stmts -Kits $kits -ClassGuids $classGuids
    $outDir = Split-Path -Parent ([System.IO.Path]::GetFullPath($resolvedOut))
    if ($outDir) { [void][System.IO.Directory]::CreateDirectory($outDir) }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($resolvedOut), $headerText, $utf8NoBom)
    Write-Output "$($stmts.Count) statements; $($itemSet.Count) items $($blocks.Count) blocks $($curves.Count) curves $($sets.Count) sets"
} finally {
    $conn.Dispose()
}
