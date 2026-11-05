#!/usr/bin/env pwsh
#requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RacesMap = @{
    'Kaelar'   = 0
    'Vaelune'  = 1
    'Vek'      = 2
    'Renkai'   = 3
    'Empyrean' = 4
    'Pyrai'    = 5
    'Dunir'    = 6
    'Nikua'    = 7
    'Tulnar'   = 8
}
$script:SlotSpecs = @(
    @{ Field = 'HeadIds';              Slot = 0 }
    @{ Field = 'ChestIds';             Slot = 2 }
    @{ Field = 'WristsIds';            Slot = 3 }
    @{ Field = 'HandsIds';             Slot = 4 }
    @{ Field = 'LegsIds';              Slot = 5 }
    @{ Field = 'FeetIds';              Slot = 6 }
    @{ Field = 'ShouldersIds';         Slot = 7 }
    @{ Field = 'BackIds';              Slot = 8 }
    @{ Field = 'WaistIds';             Slot = 9 }
    @{ Field = 'PrimaryHeldItemIds';   Slot = 15 }
    @{ Field = 'SecondaryHeldItemIds'; Slot = 16 }
)
$script:InterpModes = @{
    'ERichCurveInterpMode::RCIM_Linear'   = 'L'
    'ERichCurveInterpMode::RCIM_Cubic'    = 'C'
    'ERichCurveInterpMode::RCIM_Constant' = 'K'
}

function Initialize-ManagedSqlite {
    if ('Microsoft.Data.Sqlite.SqliteConnection' -as [type]) { return }
    $repoRoot = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
    $rid = if ($IsWindows) { 'win-x64' } elseif ($IsLinux) { 'linux-x64' } else { 'osx-x64' }
    $binFile = Get-ChildItem -Path (Join-Path $repoRoot 'build/dotnet/bin') -Recurse -Filter 'Microsoft.Data.Sqlite.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $binFile) {
        $dir = $binFile.DirectoryName
        $native = Get-ChildItem -Path (Join-Path $dir "runtimes/$rid/native") -Filter '*e_sqlite3*' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $native) { [void][System.Runtime.InteropServices.NativeLibrary]::Load($native.FullName) }
        foreach ($dll in @('SQLitePCLRaw.core.dll', 'SQLitePCLRaw.provider.e_sqlite3.dll', 'SQLitePCLRaw.batteries_v2.dll', 'Microsoft.Data.Sqlite.dll')) {
            $p = Join-Path $dir $dll
            if (Test-Path -LiteralPath $p) { Add-Type -LiteralPath $p }
        }
        [SQLitePCL.Batteries_V2]::Init()
        return
    }
    throw 'Microsoft.Data.Sqlite.dll not found in build/dotnet/bin.'
}

function Expand-GzipJson {
    param([byte[]]$CompressedBytes)
    $ms = [System.IO.MemoryStream]::new($CompressedBytes)
    $gz = [System.IO.Compression.GZipStream]::new($ms, [System.IO.Compression.CompressionMode]::Decompress)
    try {
        return [System.Text.Json.JsonDocument]::Parse($gz)
    } finally {
        $gz.Dispose()
        $ms.Dispose()
    }
}

function Get-JsonProp {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name)
    if ($Elem.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { return $null }
    $prop = [System.Text.Json.JsonElement]::new()
    if ($Elem.TryGetProperty($Name, [ref]$prop)) { return $prop }
    return $null
}

function Get-JsonString {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name, [string]$Default = '')
    $prop = Get-JsonProp -Elem $Elem -Name $Name
    if ($null -eq $prop -or $prop.ValueKind -ne [System.Text.Json.JsonValueKind]::String) { return $Default }
    $val = $prop.GetString()
    if ([string]::IsNullOrEmpty($val)) { return $Default }
    return $val
}

function Get-JsonInt64 {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name)
    $prop = Get-JsonProp -Elem $Elem -Name $Name
    if ($null -eq $prop -or $prop.ValueKind -ne [System.Text.Json.JsonValueKind]::Number) { return [int64]0 }
    return $prop.GetInt64()
}

function Get-NestedGuid {
    param([System.Text.Json.JsonElement]$Elem, [string]$PropName)
    $sub = Get-JsonProp -Elem $Elem -Name $PropName
    if ($null -eq $sub) { return [int64]0 }
    return (Get-JsonInt64 -Elem $sub -Name 'Guid')
}

function Format-LookNumber {
    param([double]$Value)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    if ($Value -eq [Math]::Truncate($Value) -and [Math]::Abs($Value) -lt 1e15) {
        return ([int64]$Value).ToString($inv)
    }
    return $Value.ToString($inv)
}

function Format-CleanText {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return '' }
    $norm = $Text.Replace("`t", ' ').Replace("`r", ' ').Replace("`n", ' ').Trim()
    if ($norm.Length -eq 0) { return '' }
    return ([regex]::Split($norm, '\s+') -join ' ')
}

function Format-CurveText {
    param([object]$CurveElem)
    if ($null -eq $CurveElem -or $CurveElem.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { return '' }
    $ed = Get-JsonProp -Elem $CurveElem -Name 'EditorCurveData'
    $keys = if ($null -ne $ed) { Get-JsonProp -Elem $ed -Name 'Keys' } else { $null }
    if ($null -eq $keys -or $keys.ValueKind -ne [System.Text.Json.JsonValueKind]::Array) { return '' }
    $parts = [System.Collections.Generic.List[string]]::new()
    foreach ($k in $keys.EnumerateArray()) {
        $t = Format-LookNumber ((Get-JsonProp -Elem $k -Name 'Time').GetDouble())
        $v = Format-LookNumber ((Get-JsonProp -Elem $k -Name 'Value').GetDouble())
        $mRaw = Get-JsonString -Elem $k -Name 'InterpMode'
        $m = if ($script:InterpModes.ContainsKey($mRaw)) { $script:InterpModes[$mRaw] } else { 'K' }
        $parts.Add("${t}:${v}:${m}")
    }
    return ($parts -join ',')
}

function Get-DisplayTexts {
    param([object]$ListElem)
    $out = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $ListElem -or $ListElem.ValueKind -ne [System.Text.Json.JsonValueKind]::Array) { return $out }
    foreach ($t in $ListElem.EnumerateArray()) {
        $s = Get-JsonString -Elem $t -Name 'Source'
        if ($s.Length -eq 0) { $s = Get-JsonString -Elem $t -Name 'Key' }
        $out.Add((Format-CleanText -Text $s))
    }
    return $out
}

function Read-SchemaRows {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn, [string]$Schema)
    $tiCmd = $Conn.CreateCommand()
    $tiCmd.CommandText = 'SELECT table_index FROM record_table WHERE schema = @s'
    [void]$tiCmd.Parameters.AddWithValue('@s', "${Schema}RecordBase")
    $tiObj = $tiCmd.ExecuteScalar()
    $tiCmd.Dispose()
    if ($null -eq $tiObj) { throw "content.db has no $Schema table" }
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = 'SELECT guid, name, data FROM record WHERE table_index = @ti ORDER BY guid'
    [void]$cmd.Parameters.AddWithValue('@ti', [int64]$tiObj)
    $reader = $cmd.ExecuteReader()
    $rows = [System.Collections.Generic.List[pscustomobject]]::new()
    try {
        while ($reader.Read()) {
            $rows.Add([pscustomobject]@{ Guid = $reader.GetInt64(0); Name = $reader.GetString(1); Bytes = [byte[]]$reader.GetValue(2) })
        }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
    return $rows
}

function Build-AppearanceAndOutfitLines {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $appLines = [System.Collections.Generic.List[string]]::new()
    $outfitLines = [System.Collections.Generic.List[string]]::new()
    $slotLines = [System.Collections.Generic.List[string]]::new()
    $itemLines = [System.Collections.Generic.List[string]]::new()
    $sharedUsed = [System.Collections.Generic.HashSet[int64]]::new()
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'NPCAppearance')) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $d = $doc.RootElement
            $sg = Get-NestedGuid -Elem $d -PropName 'SharedAppearanceInfoId'
            [void]$sharedUsed.Add($sg)
            $optsProp = Get-JsonProp -Elem $d -Name 'HumanoidOutfitOptionsIds'
            $outs = [System.Collections.Generic.List[string]]::new()
            if ($null -ne $optsProp -and $optsProp.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
                foreach ($o in $optsProp.EnumerateArray()) { $outs.Add("$(Get-JsonInt64 -Elem $o -Name 'Guid')") }
            }
            $rc = if ((Get-JsonProp -Elem $d -Name 'bRandomizeCustomization').GetBoolean()) { '1' } else { '0' }
            $rr = if ((Get-JsonProp -Elem $d -Name 'bRandomizeRaceAndGender').GetBoolean()) { '1' } else { '0' }
            $hn = if ((Get-JsonProp -Elem $d -Name 'bIsHairNatural').GetBoolean()) { '1' } else { '0' }
            $seed = "$(Get-JsonInt64 -Elem $d -Name 'CustomizationSeed')"
            $curve = Format-CurveText -CurveElem (Get-JsonProp -Elem $d -Name 'OutfitWeightCurve')
            $appLines.Add((@('A', "$($row.Guid)", (Format-CleanText $row.Name), "$sg", $rc, $rr, $hn, $seed, $curve, ($outs -join ',')) -join "`t"))
        } finally { $doc.Dispose() }
    }
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'HumanoidOutfit')) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $d = $doc.RootElement
            $single = if ((Get-JsonProp -Elem $d -Name 'bUseSingleWeightCurve').GetBoolean()) { '1' } else { '0' }
            $wCurve = Format-CurveText -CurveElem (Get-JsonProp -Elem $d -Name 'WeightCurve')
            $outfitLines.Add((@('O', "$($row.Guid)", (Format-CleanText $row.Name), $single, $wCurve) -join "`t"))
            foreach ($spec in $script:SlotSpecs) {
                $arr = Get-JsonProp -Elem $d -Name $spec.Field
                if ($null -ne $arr -and $arr.ValueKind -eq [System.Text.Json.JsonValueKind]::Array -and $arr.GetArrayLength() -gt 0) {
                    $ids = [System.Collections.Generic.List[string]]::new()
                    foreach ($x in $arr.EnumerateArray()) { $ids.Add("$(Get-JsonInt64 -Elem $x -Name 'Guid')") }
                    $itemLines.Add("I`t$($row.Guid)`t$($spec.Slot)`t$($ids -join ',')")
                }
                $sc = Format-CurveText -CurveElem (Get-JsonProp -Elem $d -Name ($spec.Field.Replace('Ids', 'WeightCurve')))
                if ($sc.Length -gt 0) { $slotLines.Add("S`t$($row.Guid)`t$($spec.Slot)`t$sc") }
            }
        } finally { $doc.Dispose() }
    }
    return [pscustomobject]@{ App = $appLines; Outfit = $outfitLines; Slot = $slotLines; Item = $itemLines; SharedUsed = $sharedUsed }
}

function Format-RacePairsFromMap {
    param([byte[]]$MapBytes)
    $doc = Expand-GzipJson -CompressedBytes $MapBytes
    try {
        $codes = [System.Collections.Generic.List[int]]::new()
        foreach ($prop in $doc.RootElement.EnumerateObject()) {
            if ($prop.Value.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) { continue }
            if ((Get-JsonInt64 -Elem $prop.Value -Name 'Guid') -eq 0) { continue }
            $k = $prop.Name
            $gender = if ($k.StartsWith('Male')) { 0 } elseif ($k.StartsWith('Female')) { 1 } else { -1 }
            $rName = if ($gender -eq 0 -and $k.Length -gt 6) { $k.Substring(4, $k.Length - 6) } elseif ($gender -eq 1 -and $k.Length -gt 8) { $k.Substring(6, $k.Length - 8) } else { '' }
            if ($gender -ge 0 -and $script:RacesMap.ContainsKey($rName)) {
                $codes.Add(($script:RacesMap[$rName] * 10) + $gender)
            }
        }
        $codes.Sort()
        $pairs = [System.Collections.Generic.List[string]]::new($codes.Count)
        foreach ($c in $codes) { $pairs.Add("$([Math]::Truncate($c / 10)):$($c % 10)") }
        return ($pairs -join ',')
    } finally { $doc.Dispose() }
}

function Build-RaceLines {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn, [System.Collections.Generic.HashSet[int64]]$SharedUsed)
    $maps = [System.Collections.Generic.Dictionary[int64, byte[]]]::new()
    foreach ($r in (Read-SchemaRows -Conn $Conn -Schema 'RaceGenderAppearanceMap')) { $maps[$r.Guid] = $r.Bytes }
    $raceLines = [System.Collections.Generic.List[string]]::new()
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'AppearanceInfo')) {
        if (-not $SharedUsed.Contains($row.Guid)) { continue }
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $mapGuid = Get-NestedGuid -Elem $doc.RootElement -PropName 'RaceGenderAppearanceMapId'
            $pairsText = if ($maps.ContainsKey($mapGuid)) { Format-RacePairsFromMap -MapBytes $maps[$mapGuid] } else { '' }
            $raceLines.Add("R`t$($row.Guid)`t$(Format-CleanText $row.Name)`t$pairsText")
        } finally { $doc.Dispose() }
    }
    return $raceLines
}

function Build-NameLines {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $names = [System.Collections.Generic.List[string]]::new()
    $names.Add("kind`tsource`tmember`ttext")
    foreach ($spec in @(@{ Schema = 'PopulationInstance'; Kind = 'instance' }, @{ Schema = 'PopulationAsset'; Kind = 'asset' })) {
        foreach ($row in (Read-SchemaRows -Conn $Conn -Schema $spec.Schema)) {
            $doc = Expand-GzipJson -CompressedBytes $row.Bytes
            try {
                foreach ($t in (Get-DisplayTexts -ListElem (Get-JsonProp -Elem $doc.RootElement -Name 'DisplayNameOverrides'))) {
                    $names.Add("$($spec.Kind)`t$($row.Guid)`t0`t$t")
                }
            } finally { $doc.Dispose() }
        }
    }
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'AssetSet')) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $mapArr = Get-JsonProp -Elem $doc.RootElement -Name 'AssetDefMap'
            if ($null -ne $mapArr -and $mapArr.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
                foreach ($e in $mapArr.EnumerateArray()) {
                    $mGuid = Get-NestedGuid -Elem $e -PropName 'Key'
                    $valObj = Get-JsonProp -Elem $e -Name 'Value'
                    $ov = if ($null -ne $valObj) { Get-JsonProp -Elem $valObj -Name 'DisplayNameOverrides' } else { $null }
                    foreach ($t in (Get-DisplayTexts -ListElem $ov)) { $names.Add("set`t$($row.Guid)`t$mGuid`t$t") }
                }
            }
        } finally { $doc.Dispose() }
    }
    return $names
}

function Parse-LooksCliArguments {
    param([string[]]$RawArgs)
    $p = @{ ContentDb = ''; OutDir = ''; AppearancesOut = ''; NamesOut = ''; Abilities = '' }
    $flagToKey = @{
        '--content-db'      = 'ContentDb'
        '--out-dir'         = 'OutDir'
        '--appearances-out' = 'AppearancesOut'
        '--names-out'       = 'NamesOut'
        '--abilities'       = 'Abilities'
    }
    for ($i = 0; $i -lt ($RawArgs.Count - 1); $i++) {
        $flag = $RawArgs[$i]
        if ($flagToKey.ContainsKey($flag)) {
            $p[$flagToKey[$flag]] = $RawArgs[++$i]
        }
    }
    if ($p.OutDir.Length -gt 0 -and $p.AppearancesOut.Length -eq 0) { $p.AppearancesOut = Join-Path $p.OutDir 'npc_appearances.tsv' }
    if ($p.OutDir.Length -gt 0 -and $p.NamesOut.Length -eq 0) { $p.NamesOut = Join-Path $p.OutDir 'npc_names.tsv' }
    if ([string]::IsNullOrWhiteSpace($p.ContentDb) -or [string]::IsNullOrWhiteSpace($p.AppearancesOut) -or [string]::IsNullOrWhiteSpace($p.NamesOut)) {
        throw 'Usage: looks.ps1 --content-db <path> (--out-dir <dir> | --appearances-out <path> --names-out <path>)'
    }
    return $p
}

function Write-AtomicUtf8Lf {
    param([string]$FilePath, [string[]]$Lines)
    $full = [System.IO.Path]::GetFullPath($FilePath)
    $dir = Split-Path -Path $full -Parent
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { [void][System.IO.Directory]::CreateDirectory($dir) }
    $tmp = "$full.tmp"
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($tmp, ($Lines -join "`n") + "`n", $utf8NoBom)
    Move-Item -LiteralPath $tmp -Destination $full -Force
}

function Invoke-LooksMain {
    param([string[]]$RawArgs)
    $opts = Parse-LooksCliArguments -RawArgs $RawArgs
    Initialize-ManagedSqlite
    $cb = [Microsoft.Data.Sqlite.SqliteConnectionStringBuilder]::new()
    $cb.DataSource = [System.IO.Path]::GetFullPath($opts.ContentDb)
    $cb.Mode = [Microsoft.Data.Sqlite.SqliteOpenMode]::ReadOnly
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new($cb.ToString())
    try {
        $conn.Open()
        $parts = Build-AppearanceAndOutfitLines -Conn $conn
        $raceLines = @(Build-RaceLines -Conn $conn -SharedUsed $parts.SharedUsed)
        $names = @(Build-NameLines -Conn $conn)
    } finally {
        $conn.Dispose()
    }
    $body = [System.Collections.Generic.List[string]]::new()
    $body.Add('# npc_appearances.tsv: see tools/npcabilities/looks.py')
    foreach ($section in @($parts.App, $parts.Outfit, $parts.Slot, $parts.Item, $raceLines)) {
        foreach ($line in $section) { $body.Add([string]$line) }
    }
    Write-AtomicUtf8Lf -FilePath $opts.AppearancesOut -Lines $body.ToArray()
    Write-AtomicUtf8Lf -FilePath $opts.NamesOut -Lines $names
    Write-Output "looks: $($parts.App.Count) appearances, $($parts.Outfit.Count) outfits, $($parts.Item.Count) item lists, $($parts.Slot.Count) slot curves, $($raceLines.Count) race maps, $($names.Count - 1) names"
}

Invoke-LooksMain -RawArgs $args
