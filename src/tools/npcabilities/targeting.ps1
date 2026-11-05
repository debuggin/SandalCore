#!/usr/bin/env pwsh
#requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Header = "ability`tkey`tdistance`tmin_distance`tangle`tangle_center`tcone_check`tfreeze`tfreeze_distance`tfreeze_scale`tignore_non_target`tdiscovery`tcheck_validity`tlos_check`tdistance_center`tvolumes`tshapes"
$script:FreezeMap = @{
    'ERootMotionFreezeBehavior::StopOnContact'  = 'stop'
    'ERootMotionFreezeBehavior::PauseOnContact' = 'pause'
}
$script:DiscoveryMap = @{
    'ETargetDiscoveryBehavior::None'                 = 'none'
    'ETargetDiscoveryBehavior::PrefersActiveTargets' = 'prefers_active'
    'ETargetDiscoveryBehavior::OnlyLockedTargets'    = 'only_locked'
    'ETargetDiscoveryBehavior::OnlyActiveTargets'    = 'only_active'
    'ETargetDiscoveryBehavior::PrefersLockedTargets' = 'prefers_locked'
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

function Get-JsonDouble {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name, [double]$Default = 0.0)
    $prop = Get-JsonProp -Elem $Elem -Name $Name
    if ($null -eq $prop -or $prop.ValueKind -ne [System.Text.Json.JsonValueKind]::Number) { return $Default }
    return $prop.GetDouble()
}

function Get-JsonBool {
    param([System.Text.Json.JsonElement]$Elem, [string]$Name)
    $prop = Get-JsonProp -Elem $Elem -Name $Name
    if ($null -eq $prop) { return $false }
    return ($prop.ValueKind -eq [System.Text.Json.JsonValueKind]::True)
}

function Format-TargetNumber {
    param([double]$Value)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    if ($Value -eq [Math]::Truncate($Value) -and [Math]::Abs($Value) -lt 1e15) {
        return ([int64]$Value).ToString($inv)
    }
    return $Value.ToString($inv)
}

function Get-VolumeRejectionReason {
    param([System.Text.Json.JsonElement]$Vol, [string]$Kind)
    if ($Kind -notin @('SHAPE_Sphere', 'SHAPE_Box')) {
        $stripped = $Kind.Replace('SHAPE_', '').ToLowerInvariant()
        return $(if ($stripped.Length -gt 0) { $stripped } else { 'shape' })
    }
    if (Get-JsonBool -Elem $Vol -Name 'bShapeIsHollow') { return 'hollow' }
    $animKeys = Get-JsonProp -Elem $Vol -Name 'AnimationKeys'
    if ($null -ne $animKeys -and $animKeys.ValueKind -eq [System.Text.Json.JsonValueKind]::Array -and $animKeys.GetArrayLength() -gt 0) {
        return 'moving'
    }
    $originRaw = Get-JsonString -Elem $Vol -Name 'Origin'
    $originParts = $originRaw -split '::'
    $origin = $originParts[$originParts.Count - 1]
    $bone = Get-JsonString -Elem $Vol -Name 'BoneName' -Default 'None'
    $originMesh = Get-JsonString -Elem $Vol -Name 'OriginMesh'
    $validOrigin = ($origin -eq 'OFFSET_Mesh') -or ($origin -eq 'OFFSET_Bone' -and $bone -in @('Root', 'None'))
    if (-not $validOrigin -or -not $originMesh.EndsWith('ORIGIN_Character')) { return 'origin' }
    $rot = Get-JsonProp -Elem $Vol -Name 'Rotation'
    if ($Kind -eq 'SHAPE_Box' -and $null -ne $rot) {
        if ((Get-JsonDouble -Elem $rot -Name 'Pitch') -ne 0.0 -or (Get-JsonDouble -Elem $rot -Name 'Roll') -ne 0.0) {
            return 'tilted'
        }
    }
    return ''
}

function Format-VolumeShape {
    param([System.Text.Json.JsonElement]$Vol)
    $idRaw = Get-JsonString -Elem $Vol -Name 'id'
    $idParts = $idRaw -split '::'
    $kind = $idParts[$idParts.Count - 1]
    $why = Get-VolumeRejectionReason -Vol $Vol -Kind $kind
    if ($why.Length -gt 0) { return "other:$why" }
    $los = if (Get-JsonBool -Elem $Vol -Name 'bEnableLOSCheck') { '1' } else { '0' }
    $loc = Get-JsonProp -Elem $Vol -Name 'Location'
    $lx = if ($null -ne $loc) { Format-TargetNumber (Get-JsonDouble -Elem $loc -Name 'X') } else { '0' }
    $ly = if ($null -ne $loc) { Format-TargetNumber (Get-JsonDouble -Elem $loc -Name 'Y') } else { '0' }
    $lz = if ($null -ne $loc) { Format-TargetNumber (Get-JsonDouble -Elem $loc -Name 'Z') } else { '0' }
    if ($kind -eq 'SHAPE_Sphere') {
        $rad = Format-TargetNumber (Get-JsonDouble -Elem $Vol -Name 'SphereRadius')
        return "sphere:${lx}:${ly}:${lz}:${rad}:${los}"
    }
    $box = Get-JsonProp -Elem $Vol -Name 'BoxExtent'
    $rot = Get-JsonProp -Elem $Vol -Name 'Rotation'
    $bx = if ($null -ne $box) { Format-TargetNumber (Get-JsonDouble -Elem $box -Name 'X') } else { '0' }
    $by = if ($null -ne $box) { Format-TargetNumber (Get-JsonDouble -Elem $box -Name 'Y') } else { '0' }
    $bz = if ($null -ne $box) { Format-TargetNumber (Get-JsonDouble -Elem $box -Name 'Z') } else { '0' }
    $yaw = if ($null -ne $rot) { Format-TargetNumber (Get-JsonDouble -Elem $rot -Name 'Yaw') } else { '0' }
    return "box:${lx}:${ly}:${lz}:${bx}:${by}:${bz}:${yaw}:${los}"
}

function Read-TargetingDatabase {
    param([Microsoft.Data.Sqlite.SqliteConnection]$Conn)
    $records = [System.Collections.Generic.Dictionary[int64, byte[]]]::new()
    $cmd = $Conn.CreateCommand()
    $cmd.CommandText = "SELECT r.guid, r.data FROM record r JOIN record_table rt ON r.table_index = rt.table_index WHERE rt.schema IN ('AoCAbilityRecordBase', 'AbilityTargetingRecordBase')"
    $reader = $cmd.ExecuteReader()
    try {
        while ($reader.Read()) {
            $records[$reader.GetInt64(0)] = [byte[]]$reader.GetValue(1)
        }
    } finally {
        $reader.Dispose()
        $cmd.Dispose()
    }
    $byAbility = [System.Collections.Generic.Dictionary[int64, System.Collections.Generic.List[pscustomobject]]]::new()
    $tCmd = $Conn.CreateCommand()
    $tCmd.CommandText = 'SELECT ability_guid, key, targeting_guid FROM ab_targeting ORDER BY ability_guid, key'
    $tReader = $tCmd.ExecuteReader()
    try {
        while ($tReader.Read()) {
            $ag = $tReader.GetInt64(0)
            if (-not $byAbility.ContainsKey($ag)) {
                $byAbility[$ag] = [System.Collections.Generic.List[pscustomobject]]::new()
            }
            $byAbility[$ag].Add([pscustomobject]@{ Key = $tReader.GetString(1); TargetingGuid = $tReader.GetInt64(2) })
        }
    } finally {
        $tReader.Dispose()
        $tCmd.Dispose()
    }
    return [pscustomobject]@{ Records = $records; ByAbility = $byAbility }
}

function Get-AbilityFreezeFields {
    param([int64]$Guid, [System.Collections.Generic.Dictionary[int64, byte[]]]$Records)
    if (-not $Records.ContainsKey($Guid)) {
        return @('none', '0', '1', '0')
    }
    $doc = Expand-GzipJson -CompressedBytes $Records[$Guid]
    try {
        $root = $doc.RootElement
        $behRaw = Get-JsonString -Elem $root -Name 'RootMotionFreezeBehavior'
        $beh = if ($script:FreezeMap.ContainsKey($behRaw)) { $script:FreezeMap[$behRaw] } else { 'none' }
        $dist = Format-TargetNumber (Get-JsonDouble -Elem $root -Name 'RootMotionFreezeDistance')
        $scale = Format-TargetNumber (Get-JsonDouble -Elem $root -Name 'RootMotionFreezeRadiusScale' -Default 1.0)
        $ign = if (Get-JsonBool -Elem $root -Name 'RootMotionIgnoreNonTarget') { '1' } else { '0' }
        return @($beh, $dist, $scale, $ign)
    } finally {
        $doc.Dispose()
    }
}

function Format-TargetingRow {
    param([int64]$AbilityGuid, [string]$Key, [string[]]$FreezeFields, [byte[]]$TargetBytes)
    $doc = Expand-GzipJson -CompressedBytes $TargetBytes
    try {
        $root = $doc.RootElement
        $lock = Get-JsonProp -Elem $root -Name 'LockOnInfo'
        $discRaw = if ($null -ne $lock) { Get-JsonString -Elem $lock -Name 'TargetDiscoveryBehavior' -Default 'ETargetDiscoveryBehavior::None' } else { 'ETargetDiscoveryBehavior::None' }
        if (-not $script:DiscoveryMap.ContainsKey($discRaw)) {
            [Console]::Error.WriteLine("targeting: ability $AbilityGuid key $Key names a target discovery this tool does not know: $discRaw")
            exit 1
        }
        $vDist = if ($null -ne $lock) { Format-TargetNumber (Get-JsonDouble -Elem $lock -Name 'ValidDistance') } else { '0' }
        $vMin = if ($null -ne $lock) { Format-TargetNumber (Get-JsonDouble -Elem $lock -Name 'ValidMinDistance') } else { '0' }
        $vAng = if ($null -ne $lock) { Format-TargetNumber (Get-JsonDouble -Elem $lock -Name 'ValidAngle') } else { '0' }
        $aCen = if ($null -ne $lock) { Format-TargetNumber (Get-JsonDouble -Elem $lock -Name 'AngleCenter') } else { '0' }
        $cone = if ($null -ne $lock -and (Get-JsonBool -Elem $lock -Name 'bPerformConeCheck')) { '1' } else { '0' }
        $valid = if ($null -ne $lock -and (Get-JsonBool -Elem $lock -Name 'bCheckTargetValidity')) { '1' } else { '0' }
        $los = if ($null -ne $lock -and (Get-JsonBool -Elem $lock -Name 'bPerformLoSCheck')) { '1' } else { '0' }
        $dCen = if ($null -ne $lock) { Format-TargetNumber (Get-JsonDouble -Elem $lock -Name 'DistanceCenter') } else { '0' }
        $hurt = Get-JsonProp -Elem $root -Name 'HurtBoxInfo'
        $volsProp = if ($null -ne $hurt) { Get-JsonProp -Elem $hurt -Name 'TargetingVolumes' } else { $null }
        $shapes = [System.Collections.Generic.List[string]]::new()
        if ($null -ne $volsProp -and $volsProp.ValueKind -eq [System.Text.Json.JsonValueKind]::Array) {
            foreach ($v in $volsProp.EnumerateArray()) { $shapes.Add((Format-VolumeShape -Vol $v)) }
        }
        $prefix = @("$AbilityGuid", $Key, $vDist, $vMin, $vAng, $aCen, $cone) + $FreezeFields
        $suffix = @($script:DiscoveryMap[$discRaw], $valid, $los, $dCen, "$($shapes.Count)", ($shapes -join ';'))
        return ($prefix + $suffix) -join "`t"
    } finally {
        $doc.Dispose()
    }
}

function Parse-CliArguments {
    param([string[]]$RawArgs)
    $parsed = @{ ContentDb = ''; Abilities = ''; Out = '' }
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        switch ($RawArgs[$i]) {
            '--content-db' { if ($i + 1 -lt $RawArgs.Count) { $parsed.ContentDb = $RawArgs[++$i] } }
            '--abilities'  { if ($i + 1 -lt $RawArgs.Count) { $parsed.Abilities = $RawArgs[++$i] } }
            '--out'        { if ($i + 1 -lt $RawArgs.Count) { $parsed.Out = $RawArgs[++$i] } }
        }
    }
    if ([string]::IsNullOrWhiteSpace($parsed.ContentDb) -or [string]::IsNullOrWhiteSpace($parsed.Abilities) -or [string]::IsNullOrWhiteSpace($parsed.Out)) {
        throw 'Usage: targeting.ps1 --content-db <path> --abilities <path> --out <path>'
    }
    return $parsed
}

function Get-SortedAbilityGuids {
    param([string]$AbilitiesPath, [System.Collections.Generic.Dictionary[int64, System.Collections.Generic.List[pscustomobject]]]$ByAbility)
    $set = [System.Collections.Generic.SortedSet[int64]]::new()
    if (Test-Path -LiteralPath $AbilitiesPath -PathType Leaf) {
        $lines = [System.IO.File]::ReadAllLines($AbilitiesPath, [System.Text.Encoding]::UTF8)
        for ($i = 1; $i -lt $lines.Count; $i++) {
            if ([string]::IsNullOrWhiteSpace($lines[$i])) { continue }
            $parts = $lines[$i] -split "`t"
            if ($parts.Count -gt 1) { [void]$set.Add([int64]$parts[1]) }
        }
    } else {
        foreach ($k in $ByAbility.Keys) { [void]$set.Add($k) }
    }
    return $set
}

function Invoke-TargetingMain {
    param([string[]]$RawArgs)
    $opts = Parse-CliArguments -RawArgs $RawArgs
    Initialize-ManagedSqlite
    $cb = [Microsoft.Data.Sqlite.SqliteConnectionStringBuilder]::new()
    $cb.DataSource = [System.IO.Path]::GetFullPath($opts.ContentDb)
    $cb.Mode = [Microsoft.Data.Sqlite.SqliteOpenMode]::ReadOnly
    $conn = [Microsoft.Data.Sqlite.SqliteConnection]::new($cb.ToString())
    try {
        $conn.Open()
        $dbData = Read-TargetingDatabase -Conn $conn
    } finally {
        $conn.Dispose()
    }
    $guids = Get-SortedAbilityGuids -AbilitiesPath $opts.Abilities -ByAbility $dbData.ByAbility
    $outLines = [System.Collections.Generic.List[string]]::new()
    $outLines.Add($script:Header)
    foreach ($guid in $guids) {
        if (-not $dbData.ByAbility.ContainsKey($guid)) { continue }
        $freeze = Get-AbilityFreezeFields -Guid $guid -Records $dbData.Records
        foreach ($entry in $dbData.ByAbility[$guid]) {
            $tGuid = [int64]$entry.TargetingGuid
            if ($tGuid -eq 0 -or (-not $dbData.Records.ContainsKey($tGuid))) { continue }
            $outLines.Add((Format-TargetingRow -AbilityGuid $guid -Key $entry.Key -FreezeFields $freeze -TargetBytes $dbData.Records[$tGuid]))
        }
    }
    $outDir = Split-Path -Path ([System.IO.Path]::GetFullPath($opts.Out)) -Parent
    if ($outDir -and -not (Test-Path -LiteralPath $outDir)) { [void][System.IO.Directory]::CreateDirectory($outDir) }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText([System.IO.Path]::GetFullPath($opts.Out), ($outLines -join "`n") + "`n", $utf8NoBom)
    Write-Output "targeting: $($guids.Count) abilities, $($outLines.Count - 1) records"
}

Invoke-TargetingMain -RawArgs $args
