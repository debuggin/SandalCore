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
$script:PresetScalars = @(
    'SkinColorHue', 'SkinColorPigmentation', 'NormalDetailStrength01', 'NormalDetailStrength02', 'SkinSet',
    'EyeColor', 'EyeShape', 'ScleraShape', 'Eyebrows', 'HeadHair', 'HeadHairRootColor', 'HeadHairTipColor',
    'HeadHairLength', 'HeadHairContrast', 'HeadHairGradient', 'FacialHairLip', 'FacialHairChin', 'FacialHairCheek',
    'FacialHairLipLength', 'FacialHairChinLength', 'FacialHairCheekLength', 'EyelashLength', 'FacialHairRootColor',
    'FacialHairTipColor', 'FacialHairContrast', 'FacialHairGradient', 'RacialHorns', 'RacialHornsLength',
    'NailColor', 'NailOpacity'
)
$script:PresetIds = [System.Collections.Generic.HashSet[string]]::new([string[]]@(
    'EyeColor', 'EyeShape', 'ScleraShape', 'Eyebrows', 'HeadHair', 'HeadHairRootColor', 'HeadHairTipColor',
    'FacialHairLip', 'FacialHairChin', 'FacialHairCheek', 'FacialHairRootColor', 'FacialHairTipColor',
    'RacialHorns', 'NailColor'
))
$script:DecalMembers = @(
    @('GUID', 'guid'), @('Strength', 'strength'), @('ScaleU', 'scaleU'), @('ScaleV', 'scaleV'),
    @('SlotIndex', 'slotIndex'), @('MergeSectionMaterialBase', 'mergeSectionMaterialBase'),
    @('OffsetU', 'offsetU'), @('OffsetV', 'offsetV'), @('Angle', 'angle'), @('Color', 'color'),
    @('Discoloration', 'discoloration')
)
$script:BlendMembers = @(
    @('DecalAppearanceGuid', 'decalAppearanceGuid'), @('ColorGuid', 'colorGuid'), @('Intensity', 'intensity')
)

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

function ConvertTo-CamelKey {
    param([string]$Name)
    if ([string]::IsNullOrEmpty($Name)) { return '' }
    return $Name.Substring(0, 1).ToLowerInvariant() + $Name.Substring(1)
}

function Get-JsonNumberRaw {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name)
    $prop = Get-JsonProp -Elem $Elem -Name $Name
    if ($null -eq $prop -or $prop.ValueKind -ne [System.Text.Json.JsonValueKind]::Number) { return '0' }
    if ($prop.GetDouble() -eq 0) { return '0' }
    return $prop.GetRawText()
}

function Format-PresetArrayField {
    param([System.Text.Json.JsonElement]$Save, [string]$PropName, [object[]]$Pairs)
    $arr = Get-JsonProp -Elem $Save -Name $PropName
    $items = [System.Collections.Generic.List[string]]::new()
    if ($null -eq $arr -or $arr.ValueKind -ne [System.Text.Json.JsonValueKind]::Array) { return $items }
    foreach ($el in $arr.EnumerateArray()) {
        $fields = [System.Collections.Generic.List[string]]::new()
        foreach ($pair in $Pairs) {
            $raw = Get-JsonNumberRaw -Elem $el -Name $pair[0]
            $fields.Add("`"$($pair[1])`":$raw")
        }
        $items.Add('{' + ($fields -join ',') + '}')
    }
    return $items
}

function Format-PresetMapFields {
    param([System.Text.Json.JsonElement]$Save)
    $secArr = Get-JsonProp -Elem $Save -Name 'SectionsValues'
    $secPairs = [System.Collections.Generic.List[string]]::new()
    $secSeen = [System.Collections.Generic.HashSet[string]]::new()
    if ($null -ne $secArr -and $secArr.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        foreach ($e in $secArr.EnumerateArray()) {
            $rawKey = (Get-JsonString -Elem $e -Name 'Key') -split '::' | Select-Object -Last 1
            $ck = ConvertTo-CamelKey -Name $rawKey
            if (-not $secSeen.Add($ck)) { throw 'a body section named twice' }
            $v = Get-JsonProp -Elem $e -Name 'Value'
            $x = if ($null -ne $v) { Get-JsonNumberRaw -Elem $v -Name 'X' } else { '0' }
            $y = if ($null -ne $v) { Get-JsonNumberRaw -Elem $v -Name 'Y' } else { '0' }
            $z = if ($null -ne $v) { Get-JsonNumberRaw -Elem $v -Name 'Z' } else { '0' }
            $secPairs.Add("`"$ck`":{`"x`":$x,`"y`":$y,`"z`":$z}")
        }
    }
    $mArr = Get-JsonProp -Elem $Save -Name 'FaceMorphWeightMaps'
    $mPairs = [System.Collections.Generic.List[string]]::new()
    $mSeen = [System.Collections.Generic.HashSet[string]]::new()
    $lowers = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $mArr -and $mArr.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
        foreach ($e in $mArr.EnumerateArray()) {
            $name = Get-JsonString -Elem $e -Name 'Key'
            $ck = ConvertTo-CamelKey -Name $name
            if ($name.Length -eq 0 -or (-not $mSeen.Add($ck)) -or $name.Contains("`t") -or $name.Contains(',')) {
                throw "a morph name the client JSON cannot carry: $name"
            }
            if ([char]::IsLower($name[0])) { $lowers.Add($name) }
            $mPairs.Add("`"$ck`":$(Get-JsonNumberRaw -Elem $e -Name 'Value')")
        }
    }
    return [pscustomobject]@{ Sections = $secPairs; Morphs = $mPairs; Lowers = ($lowers -join ',') }
}

function ConvertTo-PresetRow {
    param([int64]$Guid, [string]$Name, [System.Text.Json.JsonElement]$Root)
    $save = Get-JsonProp -Elem $Root -Name 'Save'
    if ($null -eq $save) { throw 'missing Save' }
    $seed = Get-JsonInt64 -Elem $save -Name 'RandomSeed'
    $jsonFields = [System.Collections.Generic.List[string]]::new()
    if ($seed -ne 0) { $jsonFields.Add("`"randomSeed`":$seed") }
    foreach ($key in $script:PresetScalars) {
        $raw = Get-JsonNumberRaw -Elem $save -Name $key
        if ($script:PresetIds.Contains($key) -and $raw -ne '0' -and ([int64]$raw -lt 1000000000000L)) {
            throw "$key $raw is not a record id"
        }
        if ($raw -ne '0' -or $seed -ne 0) {
            $jsonFields.Add("`"$(ConvertTo-CamelKey -Name $key)`":$raw")
        }
    }
    $decals = @(Format-PresetArrayField -Save $save -PropName 'DecalData' -Pairs $script:DecalMembers)
    $blends = @(Format-PresetArrayField -Save $save -PropName 'DecalBlendGroups' -Pairs $script:BlendMembers)
    $maps = Format-PresetMapFields -Save $save
    $secs = @($maps.Sections)
    $morphs = @($maps.Morphs)
    if ($decals.Count -gt 0 -or $seed -ne 0) { $jsonFields.Add("`"decalData`":[$($decals -join ',')]") }
    if ($blends.Count -gt 0 -or $seed -ne 0) { $jsonFields.Add("`"decalBlendGroups`":[$($blends -join ',')]") }
    if ($secs.Count -gt 0 -or $seed -ne 0) { $jsonFields.Add("`"sectionsValues`":{$($secs -join ',')}") }
    if ($morphs.Count -gt 0 -or $seed -ne 0) { $jsonFields.Add("`"faceMorphWeightMaps`":{$($morphs -join ',')}") }
    $raceKey = (Get-JsonString -Elem $save -Name 'Race') -split '::' | Select-Object -Last 1
    $genKey = (Get-JsonString -Elem $save -Name 'Gender') -split '::' | Select-Object -Last 1
    if (-not $script:RacesMap.ContainsKey($raceKey) -or $genKey -notin @('Male', 'Female')) {
        throw "no race or gender: $raceKey $genKey"
    }
    $race = $script:RacesMap[$raceKey]
    $gender = if ($genKey -eq 'Male') { 0 } else { 1 }
    $jsonText = '{' + ($jsonFields -join ',') + '}'
    $line = (@('P', "$Guid", (Format-CleanText $Name), $jsonText, $maps.Lowers, "$race", "$gender") -join "`t")
    return [pscustomobject]@{ Line = $line; Pair = "${race}:${gender}" }
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

function Read-AppearanceContext {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $maps = [System.Collections.Generic.Dictionary[int64, byte[]]]::new()
    foreach ($r in (Read-SchemaRows -Conn $Conn -Schema 'RaceGenderAppearanceMap')) { $maps[$r.Guid] = $r.Bytes }
    $infoPairs = @{}; $infoRest = @{}; $infoNames = @{}; $infoGuids = [System.Collections.Generic.List[int64]]::new()
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'AppearanceInfo')) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $d = $doc.RootElement
            $mapGuid = Get-NestedGuid -Elem $d -PropName 'RaceGenderAppearanceMapId'
            $pairsText = if ($maps.ContainsKey($mapGuid)) { Format-RacePairsFromMap -MapBytes $maps[$mapGuid] } else { '' }
            $pairSet = [System.Collections.Generic.HashSet[string]]::new()
            foreach ($p in ($pairsText -split ',' | Where-Object { $_ })) { [void]$pairSet.Add($p) }
            $restParts = [System.Collections.Generic.List[string]]::new()
            foreach ($prop in $d.EnumerateObject()) {
                if ($prop.Name -ne 'RaceGenderAppearanceMapId') { $restParts.Add("$($prop.Name)=$($prop.Value.GetRawText())") }
            }
            $infoGuids.Add($row.Guid)
            $infoNames[$row.Guid] = Format-CleanText $row.Name
            $infoPairs[$row.Guid] = [pscustomobject]@{ Text = $pairsText; Set = $pairSet }
            $infoRest[$row.Guid] = ($restParts -join '|')
        } finally { $doc.Dispose() }
    }
    return @{ Guids = $infoGuids; Names = $infoNames; Pairs = $infoPairs; Rest = $infoRest }
}

function Resolve-BodyShared {
    param([int64]$Sg, [string]$BodyPair, [hashtable]$InfoCtx, [hashtable]$InfoUses)
    if ($InfoCtx.Pairs.ContainsKey($Sg) -and $InfoCtx.Pairs[$Sg].Set.Contains($BodyPair)) { return $Sg }
    if (-not $InfoCtx.Rest.ContainsKey($Sg)) { return [int64]0 }
    $rest = $InfoCtx.Rest[$Sg]
    $bestGuid = [int64]0; $bestUses = -1
    foreach ($g in $InfoCtx.Guids) {
        if ($g -eq $Sg -or $InfoCtx.Rest[$g] -ne $rest -or (-not $InfoCtx.Pairs[$g].Set.Contains($BodyPair))) { continue }
        $u = if ($InfoUses.ContainsKey($g)) { [int]$InfoUses[$g] } else { 0 }
        if ($u -gt $bestUses -or ($u -eq $bestUses -and ($bestGuid -eq 0 -or $g -lt $bestGuid))) {
            $bestGuid = $g; $bestUses = $u
        }
    }
    return $bestGuid
}

function Read-PinnedPresets {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn, [System.Collections.Generic.HashSet[int64]]$PresetUsed)
    $presetLines = [System.Collections.Generic.List[string]]::new()
    $presetBodies = @{}
    $skipped = 0
    foreach ($row in (Read-SchemaRows -Conn $Conn -Schema 'CustomizationPreset')) {
        if (-not $PresetUsed.Contains($row.Guid)) { continue }
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $pr = ConvertTo-PresetRow -Guid $row.Guid -Name $row.Name -Root $doc.RootElement
            $presetLines.Add($pr.Line)
            $presetBodies[$row.Guid] = $pr.Pair
        } catch {
            $skipped++
            [Console]::Out.WriteLine("looks: preset $($row.Name) left out: $($_.Exception.Message)")
        } finally { $doc.Dispose() }
    }
    return @{ Lines = $presetLines; Bodies = $presetBodies; Skipped = $skipped }
}

function Build-OutfitLines {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $outfitLines = [System.Collections.Generic.List[string]]::new()
    $slotLines = [System.Collections.Generic.List[string]]::new()
    $itemLines = [System.Collections.Generic.List[string]]::new()
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
    return @{ Outfit = $outfitLines; Slot = $slotLines; Item = $itemLines }
}

function Build-AppearanceAndOutfitLines {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $npcRaw = Read-SchemaRows -Conn $Conn -Schema 'NPCAppearance'
    $presetUsed = [System.Collections.Generic.HashSet[int64]]::new()
    $infoUses = @{}
    foreach ($row in $npcRaw) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $d = $doc.RootElement
            $sg = Get-NestedGuid -Elem $d -PropName 'SharedAppearanceInfoId'
            $infoUses[$sg] = $(if ($infoUses.ContainsKey($sg)) { $infoUses[$sg] } else { 0 }) + 1
            $pin = Get-NestedGuid -Elem $d -PropName 'CustomizationPresetId'
            $randCustom = (Get-JsonProp -Elem $d -Name 'bRandomizeCustomization').GetBoolean()
            if ($pin -ne 0 -and $sg -ne 0 -and (-not $randCustom)) { [void]$presetUsed.Add($pin) }
        } finally { $doc.Dispose() }
    }
    $presets = Read-PinnedPresets -Conn $Conn -PresetUsed $presetUsed
    $infoCtx = Read-AppearanceContext -Conn $Conn
    $appLines = [System.Collections.Generic.List[string]]::new()
    $sharedUsed = [System.Collections.Generic.HashSet[int64]]::new()
    $bodyMoves = 0
    foreach ($row in $npcRaw) {
        $doc = Expand-GzipJson -CompressedBytes $row.Bytes
        try {
            $d = $doc.RootElement
            $sg = Get-NestedGuid -Elem $d -PropName 'SharedAppearanceInfoId'
            $pin = Get-NestedGuid -Elem $d -PropName 'CustomizationPresetId'
            $randCustom = (Get-JsonProp -Elem $d -Name 'bRandomizeCustomization').GetBoolean()
            $bodyShared = [int64]0
            if ($presets.Bodies.ContainsKey($pin) -and $sg -ne 0 -and (-not $randCustom)) {
                $bodyShared = Resolve-BodyShared -Sg $sg -BodyPair $presets.Bodies[$pin] -InfoCtx $infoCtx -InfoUses $infoUses
                if ($bodyShared -eq 0) {
                    [Console]::Out.WriteLine("looks: record $($row.Name): no shared appearance info holds its preset's race and gender, its NPCs keep their own")
                } elseif ($bodyShared -ne $sg) {
                    $bodyMoves++; [void]$sharedUsed.Add($bodyShared)
                }
            }
            [void]$sharedUsed.Add($sg)
            $optsProp = Get-JsonProp -Elem $d -Name 'HumanoidOutfitOptionsIds'
            $outs = [System.Collections.Generic.List[string]]::new()
            if ($null -ne $optsProp -and $optsProp.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
                foreach ($o in $optsProp.EnumerateArray()) { $outs.Add("$(Get-JsonInt64 -Elem $o -Name 'Guid')") }
            }
            $rc = if ($randCustom) { '1' } else { '0' }
            $rr = if ((Get-JsonProp -Elem $d -Name 'bRandomizeRaceAndGender').GetBoolean()) { '1' } else { '0' }
            $hn = if ((Get-JsonProp -Elem $d -Name 'bIsHairNatural').GetBoolean()) { '1' } else { '0' }
            $seed = "$(Get-JsonInt64 -Elem $d -Name 'CustomizationSeed')"
            $curve = Format-CurveText -CurveElem (Get-JsonProp -Elem $d -Name 'OutfitWeightCurve')
            $appLines.Add((@('A', "$($row.Guid)", (Format-CleanText $row.Name), "$sg", $rc, $rr, $hn, $seed, $curve, ($outs -join ','), "$pin", "$bodyShared") -join "`t"))
        } finally { $doc.Dispose() }
    }
    $raceLines = [System.Collections.Generic.List[string]]::new()
    foreach ($g in $infoCtx.Guids) {
        if ($sharedUsed.Contains($g)) { $raceLines.Add("R`t$g`t$($infoCtx.Names[$g])`t$($infoCtx.Pairs[$g].Text)") }
    }
    $outfits = Build-OutfitLines -Conn $Conn
    return [pscustomobject]@{
        App = $appLines; Outfit = $outfits.Outfit; Slot = $outfits.Slot; Item = $outfits.Item
        Race = $raceLines; Preset = $presets.Lines; SkippedPresets = $presets.Skipped; BodyMoves = $bodyMoves
    }
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
        $names = @(Build-NameLines -Conn $conn)
    } finally {
        $conn.Dispose()
    }
    $body = [System.Collections.Generic.List[string]]::new()
    $body.Add('# npc_appearances.tsv: A (NPCAppearance), S (SharedAppearanceInfo), O (Outfit), M (AppearanceMorph), P (CustomizationPreset)')
    foreach ($section in @($parts.App, $parts.Outfit, $parts.Slot, $parts.Item, $parts.Race, $parts.Preset)) {
        foreach ($line in $section) { $body.Add([string]$line) }
    }
    Write-AtomicUtf8Lf -FilePath $opts.AppearancesOut -Lines $body.ToArray()
    Write-AtomicUtf8Lf -FilePath $opts.NamesOut -Lines $names
    Write-Output "looks: $($parts.App.Count) appearances, $($parts.Outfit.Count) outfits, $($parts.Item.Count) item lists, $($parts.Slot.Count) slot curves, $($parts.Race.Count) race maps, $($parts.Preset.Count) presets ($($parts.SkippedPresets) left out; $($parts.BodyMoves) records need another shared info for the body), $($names.Count - 1) names"
}

Invoke-LooksMain -RawArgs $args
