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

function Add-TableSchemaLines {
    param(
        [Microsoft.Data.Sqlite.SqliteConnection]$Conn,
        [string]$TableName,
        [System.Collections.Generic.List[string]]$Lines
    )
    $ddlList = Get-ScalarStringList -Conn $Conn -Sql "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = '$TableName'"
    if ($ddlList.Count -gt 0) { $Lines.Add($ddlList[0].Trim() + ';') }
    $idxList = Get-ScalarStringList -Conn $Conn -Sql "SELECT sql FROM sqlite_master WHERE type = 'index' AND tbl_name = '$TableName' AND sql IS NOT NULL"
    foreach ($idx in $idxList) { $Lines.Add($idx.Trim() + ';') }
}

function Get-TableColumns {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn, [string]$TableName)
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = "PRAGMA table_info($TableName)"
    $reader = $cmd.ExecuteReader()
    $cols = [System.Collections.Generic.List[string]]::new()
    try {
        while ($reader.Read()) { $cols.Add($reader.GetString(1)) }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
    return ($cols -join ', ')
}

function Add-TableInsertLines {
    param(
        [Microsoft.Data.Sqlite.SqliteConnection]$Conn,
        [string]$TableName,
        [string]$Query,
        [bool]$OwnedByFixture,
        [System.Collections.Generic.List[string]]$Lines
    )
    if (-not $OwnedByFixture) { Add-TableSchemaLines -Conn $Conn -TableName $TableName -Lines $Lines }
    $colList = Get-TableColumns -Conn $Conn -TableName $TableName
    $verb = $OwnedByFixture ? 'INSERT OR REPLACE' : 'INSERT'
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = $Query
    $reader = $cmd.ExecuteReader()
    try {
        $fieldCount = $reader.FieldCount
        while ($reader.Read()) {
            $vals = [string[]]::new($fieldCount)
            for ($i = 0; $i -lt $fieldCount; $i++) { $vals[$i] = Format-SqlLiteral -Val $reader.GetValue($i) }
            $Lines.Add("$verb INTO $TableName ($colList) VALUES ($($vals -join ', '));")
        }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$resolvedDb = $DbPath ? $DbPath : (Join-Path $repoRoot 'dist/content/content.db')
$resolvedOut = $OutPath ? $OutPath : (Join-Path $repoRoot 'build/gen/world/tests/talent_fixture.sql')

Initialize-ManagedSqlite -RepoRoot $repoRoot
$conn = Open-ReadOnlySqlite -Path $resolvedDb
try {
    $trees = Get-ScalarStringList -Conn $conn -Sql @"
SELECT guid FROM skill_tree WHERE kind IN ('class', 'universal', 'weapon')
   OR (kind = 'profession' AND EXISTS (SELECT 1 FROM skill_node n WHERE n.tree_guid = skill_tree.guid AND n.auto_granted = 1 AND n.level_requirement <= 1))
ORDER BY guid
"@
    $treeList = $trees -join ','
    $types = Get-ScalarStringList -Conn $conn -Sql "SELECT DISTINCT leveling_type_guid FROM skill_tree WHERE guid IN ($treeList) ORDER BY 1"
    $typeList = $types -join ','
    $skills = Get-ScalarStringList -Conn $conn -Sql "SELECT DISTINCT skill_guid FROM skill_node WHERE tree_guid IN ($treeList) ORDER BY 1"
    $skillList = $skills -join ','
    $ranks = Get-ScalarStringList -Conn $conn -Sql "SELECT DISTINCT rank_guid FROM skill_rank WHERE skill_guid IN ($skillList) ORDER BY 1"
    $rankList = $ranks -join ','
    $kitItems = Get-ScalarStringList -Conn $conn -Sql 'SELECT DISTINCT item_guid FROM starting_equipment ORDER BY 1'
    $itemList = $kitItems -join ','

    $queries = [ordered]@{
        'skill_point_type'     = 'SELECT * FROM skill_point_type ORDER BY guid'
        'leveling_point'       = "SELECT * FROM leveling_point WHERE type_guid IN ($typeList) ORDER BY type_guid, level, point_type_guid"
        'skill_tree'           = "SELECT * FROM skill_tree WHERE guid IN ($treeList) ORDER BY guid"
        'skill_node'           = "SELECT * FROM skill_node WHERE tree_guid IN ($treeList) ORDER BY tree_guid, ord"
        'skill_node_require'   = "SELECT * FROM skill_node_require WHERE tree_guid IN ($treeList) ORDER BY tree_guid, node_guid, ord"
        'skill_node_exclusive' = "SELECT * FROM skill_node_exclusive WHERE tree_guid IN ($treeList) ORDER BY tree_guid, node_guid, ord"
        'skill_rank'           = "SELECT * FROM skill_rank WHERE skill_guid IN ($skillList) ORDER BY skill_guid, ord"
        'skill_rank_cost'      = "SELECT * FROM skill_rank_cost WHERE rank_guid IN ($rankList) ORDER BY rank_guid, ord"
        'skill_rank_stat_mod'  = "SELECT * FROM skill_rank_stat_mod WHERE rank_guid IN ($rankList) ORDER BY rank_guid, ord"
        'item_weapon'          = "SELECT * FROM item_weapon WHERE item_guid IN ($itemList) ORDER BY item_guid"
    }

    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('-- GENERATED by tools/worldd/gen_talent_fixture.py from a content.db (contentc version 5, plan D1 stage 2); do not edit.')
    $lines.Add('-- The real rows of the talents catalog for the class, Universal, weapon and gathering trees (see the generator).')
    foreach ($entry in $queries.GetEnumerator()) {
        $owned = ($entry.Key -eq 'skill_rank_stat_mod')
        Add-TableInsertLines -Conn $conn -TableName $entry.Key -Query $entry.Value -OwnedByFixture $owned -Lines $lines
    }
    $text = ($lines -join "`n") + "`n"
    $outDir = Split-Path -Parent ([System.IO.Path]::GetFullPath($resolvedOut))
    if ($outDir) { [void][System.IO.Directory]::CreateDirectory($outDir) }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($resolvedOut), $text, $utf8NoBom)
    Write-Output "${resolvedOut}: $($trees.Count) trees, $($skills.Count) skills, $($ranks.Count) ranks, $($lines.Count) lines, $($text.Length) bytes"
} finally {
    $conn.Dispose()
}
