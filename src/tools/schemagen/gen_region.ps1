#Requires -Version 7.0
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:CellCm = 512
$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))

function Resolve-RegionArgs {
    param([string[]]$CliArgs)
    $opts = @{
        RectsJson = Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_rects.json'
        ZoiJson   = Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_zoi_rects.json'
        HeaderOut = Join-Path $script:RepoRoot 'build\gen\world\spawns\region_data.h'
        NavTsvOut = Join-Path $script:RepoRoot 'build\gen\nav\region_rects.tsv'
        Check     = $false
    }
    $flagMap = @{
        '--nav-tsv'     = 'NavTsvOut'; '-nav-tsv'     = 'NavTsvOut'
        '--rects'       = 'RectsJson'; '--biome-rects' = 'RectsJson'
        '--zoi-rects'   = 'ZoiJson';   '-zoi-rects'   = 'ZoiJson'
        '--out'         = 'HeaderOut'; '--header'     = 'HeaderOut'
    }
    $positional = [System.Collections.Generic.List[string]]::new()
    for ($i = 0; $i -lt $CliArgs.Count; $i++) {
        $arg = $CliArgs[$i]
        $norm = $arg.ToLowerInvariant()
        if ($norm -eq '--check' -or $norm -eq '-check') { $opts['Check'] = $true; continue }
        if ($flagMap.ContainsKey($norm)) {
            if ($i + 1 -ge $CliArgs.Count) { throw "Missing value for $arg" }
            $i++; $opts[$flagMap[$norm]] = $CliArgs[$i]; continue
        }
        if ($arg.StartsWith('-')) { throw "Unknown option: $arg" }
        $positional.Add($arg)
    }
    $posKeys = @('RectsJson', 'ZoiJson', 'HeaderOut', 'NavTsvOut')
    for ($j = 0; $j -lt [Math]::Min($positional.Count, $posKeys.Count); $j++) {
        $opts[$posKeys[$j]] = $positional[$j]
    }
    return [pscustomobject]$opts
}

function ConvertTo-RectArray {
    param([object]$JsonRects)
    $list = [System.Collections.Generic.List[int[]]]::new()
    foreach ($q in $JsonRects) {
        $list.Add([int[]]@([int]$q[0], [int]$q[1], [int]$q[2], [int]$q[3]))
    }
    return ,$list.ToArray()
}

function Read-RegionInputs {
    param([string]$RectsPath, [string]$ZoiPath)
    $rRaw = [System.IO.File]::ReadAllText($RectsPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $zRaw = [System.IO.File]::ReadAllText($ZoiPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $rects = ConvertTo-RectArray -JsonRects $rRaw.rects_xy_cm
    $zois = [System.Collections.Generic.List[object]]::new()
    foreach ($q in $zRaw.zois) {
        $zRects = ConvertTo-RectArray -JsonRects $q.rects_xy_cm
        $zois.Add([pscustomobject]@{
            Name  = [string]$q.name
            Guid  = [uint64]$q.guid
            Cells = [int64]$q.cells
            Rects = $zRects
        })
    }
    return [pscustomobject]@{
        Count     = [int64]$rRaw.count
        AreaCells = [int64]$rRaw.area_cells
        Rects     = $rects
        Zois      = $zois.ToArray()
    }
}

function Get-RectsAreaCells {
    param([int[][]]$Rects)
    [int64]$total = 0
    foreach ($q in $Rects) {
        $w = [int64](($q[2] - $q[0]) / $script:CellCm)
        $h = [int64](($q[3] - $q[1]) / $script:CellCm)
        $total += ($w * $h)
    }
    return $total
}

function ConvertTo-CanonicalRowMap {
    param([int[][]]$Rects, [string]$OverlapPrefix)
    $byRow = [System.Collections.Generic.Dictionary[int, System.Collections.Generic.List[int64]]]::new()
    foreach ($q in $Rects) {
        $cx0 = [int]($q[0] / $script:CellCm)
        $cy0 = [int]($q[1] / $script:CellCm)
        $cx1 = [int]($q[2] / $script:CellCm)
        $cy1 = [int]($q[3] / $script:CellCm)
        # WHY: Pack [cx0, cx1) into a signed 64-bit integer with biased 32-bit halves so Sort() orders intervals by cx0 then cx1 without custom comparers.
        [int64]$packed = (([int64]$cx0 + 0x40000000L) -shl 32) -bor ([int64]($cx1 + 0x40000000) -band 0xFFFFFFFFL)
        for ($cy = $cy0; $cy -lt $cy1; $cy++) {
            $rowList = $null
            if (-not $byRow.TryGetValue($cy, [ref]$rowList)) {
                $rowList = [System.Collections.Generic.List[int64]]::new()
                $byRow[$cy] = $rowList
            }
            $rowList.Add($packed)
        }
    }
    $canonical = [System.Collections.Generic.Dictionary[int, string]]::new()
    foreach ($kv in $byRow.GetEnumerator()) {
        $cy = $kv.Key
        $list = $kv.Value
        $list.Sort()
        $parts = [System.Collections.Generic.List[string]]::new()
        $curStart = [int](($list[0] -shr 32) - 0x40000000L)
        $curEnd = [int](($list[0] -band 0xFFFFFFFFL) - 0x40000000L)
        for ($i = 1; $i -lt $list.Count; $i++) {
            $nextStart = [int](($list[$i] -shr 32) - 0x40000000L)
            $nextEnd = [int](($list[$i] -band 0xFFFFFFFFL) - 0x40000000L)
            if ($nextStart -lt $curEnd) {
                return [pscustomobject]@{ Error = "$OverlapPrefix at cell ($nextStart, $cy)"; Rows = $null }
            }
            if ($nextStart -eq $curEnd) { $curEnd = $nextEnd }
            else { $parts.Add("$curStart..$curEnd"); $curStart = $nextStart; $curEnd = $nextEnd }
        }
        $parts.Add("$curStart..$curEnd")
        $canonical[$cy] = ($parts -join ',')
    }
    return [pscustomobject]@{ Error = $null; Rows = $canonical }
}

function Test-RegionInputs {
    param([pscustomobject]$Data)
    $problems = [System.Collections.Generic.List[string]]::new()
    if ($Data.Rects.Count -ne $Data.Count) {
        $problems.Add("riverlands_rects.json: count $($Data.Count) but $($Data.Rects.Count) rectangles")
    }
    foreach ($q in $Data.Rects) {
        if (-not ($q[0] -lt $q[2] -and $q[1] -lt $q[3]) -or ($q[0] % $script:CellCm) -ne 0 -or ($q[1] % $script:CellCm) -ne 0 -or ($q[2] % $script:CellCm) -ne 0 -or ($q[3] % $script:CellCm) -ne 0) {
            $problems.Add("a rectangle is empty or off the $($script:CellCm) cm grid: ($($q -join ', '))")
            break
        }
    }
    $actualCells = Get-RectsAreaCells -Rects $Data.Rects
    if ($actualCells -ne $Data.AreaCells) {
        $problems.Add("the rectangles hold $actualCells cells, the file says $($Data.AreaCells)")
    }
    $footMap = ConvertTo-CanonicalRowMap -Rects $Data.Rects -OverlapPrefix 'the rectangles overlap'
    if ($footMap.Error) { $problems.Add($footMap.Error); return ,$problems.ToArray() }
    $allZoiRects = [System.Collections.Generic.List[int[]]]::new()
    foreach ($z in $Data.Zois) {
        $zCells = Get-RectsAreaCells -Rects $z.Rects
        if ($zCells -ne $z.Cells) {
            $problems.Add("$($z.Name): its rectangles hold $zCells cells, the file says $($z.Cells)")
        }
        foreach ($zr in $z.Rects) { $allZoiRects.Add($zr) }
    }
    $zoiMap = ConvertTo-CanonicalRowMap -Rects $allZoiRects.ToArray() -OverlapPrefix 'two ZOIs overlap'
    if ($zoiMap.Error) { $problems.Add($zoiMap.Error); return ,$problems.ToArray() }
    $sameTiling = ($footMap.Rows.Count -eq $zoiMap.Rows.Count)
    if ($sameTiling) {
        foreach ($kv in $footMap.Rows.GetEnumerator()) {
            $other = $null
            if (-not $zoiMap.Rows.TryGetValue($kv.Key, [ref]$other) -or $other -ne $kv.Value) { $sameTiling = $false; break }
        }
    }
    if (-not $sameTiling) {
        $zoiCellsTotal = Get-RectsAreaCells -Rects $allZoiRects.ToArray()
        $problems.Add("the per-ZOI rectangles do not tile the footprint: $zoiCellsTotal cells against $actualCells")
    }
    return ,$problems.ToArray()
}

function Add-RegionHeaderPreamble {
    param([System.Collections.Generic.List[string]]$Lines, [int]$ZoiCount, [int]$RectCount, [int64]$AreaCells, [string]$Km2)
    $Lines.Add('// region_data.h: the Riverlands as rectangles (G1, stage 1, Part A).')
    $Lines.Add('//')
    $Lines.Add('//  GENERATED by tools/capture/gen_region.py from fixtures/region/*.json; do not edit.')
    $Lines.Add('//')
    $Lines.Add("//  The world of G1 is the whole Riverlands: the $ZoiCount zones of influence (ZOIs) whose biome is Riverlands, $Km2 km^2 ($AreaCells cells of $($script:CellCm) cm), one connected piece.")
    $Lines.Add("//  kRiverlandsRects is the footprint as $RectCount disjoint rectangles [x0, x1) x [y0, y1) in world cm (multiples of $($script:CellCm)); kZoiRects holds the same area cut per ZOI")
    $Lines.Add('//  (each kZois row names its rows by first and count).  Heights do not matter: a ZOI is a prism above sea level.  Nothing here reads the game''s files.')
    $Lines.Add('#pragma once')
    $Lines.Add('')
    $Lines.Add('#include <cstddef>')
    $Lines.Add('#include <cstdint>')
    $Lines.Add('')
    $Lines.Add('namespace aoc::worldd::spawns::region {')
    $Lines.Add('')
    $Lines.Add('struct Rect {')
    $Lines.Add('    int32_t x0, y0, x1, y1;   // world cm, [x0, x1) x [y0, y1)')
    $Lines.Add('};')
    $Lines.Add('')
    $Lines.Add('struct ZoiRows {')
    $Lines.Add('    uint64_t guid;            // the ZOIRecordBase guid')
    $Lines.Add('    const char* name;         // its record name, e.g. Verra_RVR_StartingArea_RuinsOfAela')
    $Lines.Add('    uint32_t first, count;    // its rows of kZoiRects')
    $Lines.Add("    uint32_t cells;           // its area in cells of $($script:CellCm) cm")
    $Lines.Add('};')
    $Lines.Add('')
    $Lines.Add("inline constexpr int32_t kCellCm = $($script:CellCm);")
    $Lines.Add("inline constexpr uint32_t kRiverlandsAreaCells = $AreaCells;")
    $Lines.Add('')
}

function ConvertTo-RegionHeader {
    param([int[][]]$Rects, [object[]]$Zois)
    $inv = [System.Globalization.CultureInfo]::InvariantCulture
    $areaCells = Get-RectsAreaCells -Rects $Rects
    $km2 = ([double]$areaCells * $script:CellCm * $script:CellCm / 1e10).ToString('F4', $inv)
    $lines = [System.Collections.Generic.List[string]]::new()
    Add-RegionHeaderPreamble -Lines $lines -ZoiCount $Zois.Count -RectCount $Rects.Count -AreaCells $areaCells -Km2 $km2
    $lines.Add('inline constexpr Rect kRiverlandsRects[] = {')
    foreach ($q in $Rects) { $lines.Add("    {$($q[0]), $($q[1]), $($q[2]), $($q[3])},") }
    $lines.Add('};')
    $lines.Add('inline constexpr size_t kRiverlandsRectCount = sizeof(kRiverlandsRects) / sizeof(kRiverlandsRects[0]);')
    $lines.Add('')
    $lines.Add('inline constexpr Rect kZoiRects[] = {')
    $first = 0
    $rows = [System.Collections.Generic.List[object]]::new()
    foreach ($z in $Zois) {
        $lines.Add("    // $($z.Name) ($($z.Rects.Count) rectangles)")
        foreach ($q in $z.Rects) { $lines.Add("    {$($q[0]), $($q[1]), $($q[2]), $($q[3])},") }
        $rows.Add([pscustomobject]@{ Zoi = $z; First = $first; Count = $z.Rects.Count })
        $first += $z.Rects.Count
    }
    $lines.Add('};')
    $lines.Add('inline constexpr size_t kZoiRectCount = sizeof(kZoiRects) / sizeof(kZoiRects[0]);')
    $lines.Add('')
    $lines.Add('inline constexpr ZoiRows kZois[] = {')
    foreach ($r in $rows) {
        $lines.Add("    {$($r.Zoi.Guid)ull, `"$($r.Zoi.Name)`", $($r.First), $($r.Count), $($r.Zoi.Cells)},")
    }
    $lines.Add('};')
    $lines.Add('inline constexpr size_t kZoiCount = sizeof(kZois) / sizeof(kZois[0]);')
    $lines.Add('')
    $lines.Add('}  // namespace aoc::worldd::spawns::region')
    return ($lines -join "`n") + "`n"
}

function ConvertTo-RegionMapcTsv {
    param([int[][]]$Rects)
    $areaCells = Get-RectsAreaCells -Rects $Rects
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add('# region_rects v1: the Riverlands footprint as disjoint rectangles, world cm, [x0,x1) x [y0,y1) (tools/mapc reads it; GENERATED by tools/capture/gen_region.py, do not edit)')
    $lines.Add("# cells`t$areaCells`tcell_cm`t$($script:CellCm)`tcount`t$($Rects.Count)")
    foreach ($q in $Rects) {
        $lines.Add("rect`t$($q[0])`t$($q[1])`t$($q[2])`t$($q[3])")
    }
    return ($lines -join "`n") + "`n"
}

function Sync-RegionOutputFile {
    param([string]$Path, [string]$Text, [bool]$Check)
    $old = if (Test-Path -LiteralPath $Path) { [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8) } else { $null }
    $same = ($null -ne $old) -and ($old.Replace("`r`n", "`n") -eq $Text)
    $relName = [System.IO.Path]::GetRelativePath($script:RepoRoot, $Path).Replace('\', '/')
    if ($Check) {
        $state = if ($same) { 'unchanged' } else { 'WOULD CHANGE' }
        Write-Host "${relName}: $state"
        return $same
    }
    if ($same) {
        Write-Host "${relName}: unchanged"
        return $true
    }
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllBytes($Path, $utf8NoBom.GetBytes($Text))
    Write-Host "${relName}: updated"
    return $true
}

$cfg = Resolve-RegionArgs -CliArgs $args
$data = Read-RegionInputs -RectsPath $cfg.RectsJson -ZoiPath $cfg.ZoiJson
$problems = Test-RegionInputs -Data $data
foreach ($p in $problems) { Write-Host "PROBLEM: $p" }
if ($problems.Count -gt 0) { exit 1 }

$inv = [System.Globalization.CultureInfo]::InvariantCulture
$areaCells = Get-RectsAreaCells -Rects $data.Rects
$km2 = ([double]$areaCells * $script:CellCm * $script:CellCm / 1e10).ToString('F4', $inv)
$zoiRectTotal = 0
foreach ($z in $data.Zois) { $zoiRectTotal += $z.Rects.Count }
Write-Host "$($data.Rects.Count) rectangles, $areaCells cells = $km2 km^2; $($data.Zois.Count) ZOIs with $zoiRectTotal rectangles"

$okHeader = Sync-RegionOutputFile -Path $cfg.HeaderOut -Text (ConvertTo-RegionHeader -Rects $data.Rects -Zois $data.Zois) -Check $cfg.Check
$okTsv = Sync-RegionOutputFile -Path $cfg.NavTsvOut -Text (ConvertTo-RegionMapcTsv -Rects $data.Rects) -Check $cfg.Check
if ($cfg.Check -and -not ($okHeader -and $okTsv)) { exit 1 }
exit 0
