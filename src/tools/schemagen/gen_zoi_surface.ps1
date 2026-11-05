#Requires -Version 7.0
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $PSScriptRoot 'SchemaGenDb.ps1')

$csharpSource = @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

public sealed class ZoiSurfaceResult
{
    public string HeaderText { get; set; }
    public ulong[] Palette { get; set; }
}

public static class AocZoiSurfaceGen
{
    private const int Cells = 64;
    private const int Total = Cells * Cells * Cells;
    private const int SurfaceIz = Cells / 2;
    private static readonly int[] Spread = BuildSpread();
    private static readonly long[] Slab = BuildSlab();

    private static int[] BuildSpread()
    {
        var s = new int[Cells];
        for (int n = 0; n < Cells; n++)
        {
            int v = 0;
            for (int k = 0; k < 6; k++)
            {
                v |= ((n >> k) & 1) << (3 * k);
            }
            s[n] = v;
        }
        return s;
    }

    private static long[] BuildSlab()
    {
        var pairs = new long[Cells * Cells];
        int zBits = Spread[SurfaceIz] << 2;
        int idx = 0;
        for (int iy = 0; iy < Cells; iy++)
        {
            int yBits = Spread[iy] << 1;
            int rowBase = iy * Cells;
            for (int ix = 0; ix < Cells; ix++)
            {
                int m = Spread[ix] | yBits | zBits;
                int rm = rowBase + ix;
                pairs[idx++] = ((long)m << 32) | (uint)rm;
            }
        }
        Array.Sort(pairs);
        return pairs;
    }

    public static void DecodeChunk(string path, out ulong[] pal, out byte[] surf)
    {
        byte[] d = File.ReadAllBytes(path);
        uint hs = BitConverter.ToUInt32(d, 4);
        if (d[hs + 15] != 3)
        {
            throw new InvalidDataException(path + ": unknown format version " + d[hs + 15]);
        }
        int p = (int)BitConverter.ToUInt32(d, (int)hs + 16);
        pal = new ulong[p];
        int palOff = (int)hs + 20;
        for (int k = 0; k < p; k++)
        {
            pal[k] = BitConverter.ToUInt64(d, palOff + 8 * k);
        }
        int n = (int)BitConverter.ToUInt32(d, palOff + 8 * p);
        int i = (int)hs + 24 + 8 * p;
        var starts = new int[n];
        var rIdx = new byte[n];
        int pos = 0;
        for (int r = 0; r < n; r++)
        {
            uint v = 0;
            int sh = 0;
            while (true)
            {
                byte x = d[i++];
                v += (uint)(x >> 1) << sh;
                sh += 7;
                if ((x & 1) == 0) break;
            }
            byte idx = d[i++];
            starts[r] = pos;
            rIdx[r] = idx;
            pos += (int)v;
        }
        if (pos != Total || i != d.Length)
        {
            throw new InvalidDataException(path + ": cell count mismatch");
        }
        surf = new byte[Cells * Cells];
        int j = 0;
        int last = n - 1;
        for (int s = 0; s < Slab.Length; s++)
        {
            long pair = Slab[s];
            int m = (int)(pair >> 32);
            int rm = (int)(uint)pair;
            while (j < last && starts[j + 1] <= m) j++;
            surf[rm] = rIdx[j];
        }
    }

    public static ZoiSurfaceResult Generate(string rootDir)
    {
        var cols = new List<KeyValuePair<(int cx, int cy), string>>();
        foreach (string f in Directory.EnumerateFiles(rootDir, "C_*_*_0.uasset", SearchOption.AllDirectories))
        {
            string stem = Path.GetFileNameWithoutExtension(f);
            string[] parts = stem.Split('_');
            if (parts.Length == 4 && parts[0] == "C" && parts[3] == "0")
            {
                int cx = int.Parse(parts[1], CultureInfo.InvariantCulture);
                int cy = int.Parse(parts[2], CultureInfo.InvariantCulture);
                cols.Add(new KeyValuePair<(int cx, int cy), string>((cx, cy), f));
            }
        }
        if (cols.Count == 0)
        {
            throw new InvalidOperationException("no C_<cx>_<cy>_0.uasset under " + rootDir);
        }
        cols.Sort((a, b) =>
        {
            int c = a.Key.cx.CompareTo(b.Key.cx);
            return c != 0 ? c : a.Key.cy.CompareTo(b.Key.cy);
        });

        var prov = new HashSet<ulong> { 0UL };
        var raw = new List<(int cx, int cy, ulong[] pal, byte[] cells)>(cols.Count);
        int minCx = int.MaxValue, maxCx = int.MinValue;
        int minCy = int.MaxValue, maxCy = int.MinValue;
        foreach (var kv in cols)
        {
            ulong[] pal;
            byte[] cells;
            DecodeChunk(kv.Value, out pal, out cells);
            foreach (ulong g in pal) prov.Add(g);
            int cx = kv.Key.cx;
            int cy = kv.Key.cy;
            if (cx < minCx) minCx = cx;
            if (cx > maxCx) maxCx = cx;
            if (cy < minCy) minCy = cy;
            if (cy > maxCy) maxCy = cy;
            raw.Add((cx, cy, pal, cells));
        }

        var palette = new ulong[prov.Count];
        prov.CopyTo(palette);
        Array.Sort(palette);
        if (palette.Length > 256)
        {
            throw new InvalidOperationException("palette overflow (" + palette.Length + " > 256)");
        }
        var gidToIdx = new Dictionary<ulong, byte>(palette.Length);
        for (int i = 0; i < palette.Length; i++)
        {
            gidToIdx[palette[i]] = (byte)i;
        }

        int width = maxCx - minCx + 1;
        int height = maxCy - minCy + 1;
        var index = new uint[width * height];
        for (int i = 0; i < index.Length; i++) index[i] = 0xFFFFFFFFu;

        var runs = new List<byte>();
        var area = new Dictionary<ulong, int>(palette.Length);
        foreach (ulong g in palette) area[g] = 0;
        int uniform = 0;
        int nruns = 0;

        foreach (var item in raw)
        {
            var remap = new byte[item.pal.Length];
            for (int i = 0; i < item.pal.Length; i++) remap[i] = gidToIdx[item.pal[i]];
            var mapped = new byte[Cells * Cells];
            byte first = remap[item.cells[0]];
            bool allSame = true;
            for (int i = 0; i < mapped.Length; i++)
            {
                byte m = remap[item.cells[i]];
                mapped[i] = m;
                if (m != first) allSame = false;
                ulong g = palette[m];
                area[g] = area[g] + 1;
            }
            int slot = (item.cy - minCy) * width + (item.cx - minCx);
            if (allSame)
            {
                index[slot] = 0x80000000u | first;
                uniform++;
            }
            else
            {
                index[slot] = (uint)runs.Count;
                int curVal = mapped[0];
                int curLen = 1;
                for (int i = 1; i < mapped.Length; i++)
                {
                    if (mapped[i] == curVal)
                    {
                        curLen++;
                    }
                    else
                    {
                        runs.Add((byte)(curLen & 0xFF));
                        runs.Add((byte)((curLen >> 8) & 0xFF));
                        runs.Add((byte)curVal);
                        nruns++;
                        curVal = mapped[i];
                        curLen = 1;
                    }
                }
                runs.Add((byte)(curLen & 0xFF));
                runs.Add((byte)((curLen >> 8) & 0xFF));
                runs.Add((byte)curVal);
                nruns++;
            }
        }

        string text = RenderHeader(palette, minCx, minCy, width, height, index, runs, area, raw.Count, uniform, nruns);
        return new ZoiSurfaceResult { HeaderText = text, Palette = palette };
    }

    private static string RenderHeader(
        ulong[] palette, int minCx, int minCy, int width, int height,
        uint[] index, List<byte> runs, Dictionary<ulong, int> area,
        int nchunks, int uniform, int nruns)
    {
        double cellKm2 = (512.0 / 100000.0) * (512.0 / 100000.0);
        var sb = new StringBuilder(runs.Count * 5 + 32768);
        sb.Append("// ============================================================================\n");
        sb.Append("//  world/zoi_surface_table.h (Nodes 1, stage 1)\n");
        sb.Append("//\n");
        sb.Append("//  GENERATED by tools/capture/gen_zoi_surface.py from the ZOI chunk grid of the Cache.db export: do not edit.\n");
        sb.Append("//  The owner (a ZOI guid) of every 512 cm cell of the world, at the surface height z = 16,384 cm; see the generator for the encoding.\n");
        sb.Append("//  Regenerate with `python tools/capture/gen_zoi_surface.py`; scripts/generators.ps1 checks that this file is what the export gives.\n");
        sb.Append("//\n");
        sb.AppendFormat(CultureInfo.InvariantCulture,
            "//  {0} chunk columns ({1} one owner, {2} with {3} runs), {4} owners (0 = no ZOI) over {5} bytes of runs.\n",
            nchunks, uniform, nchunks - uniform, nruns, palette.Length, runs.Count);
        sb.Append("//  palette index, guid, surface km2:\n");
        for (int i = 0; i < palette.Length; i++)
        {
            ulong g = palette[i];
            sb.AppendFormat(CultureInfo.InvariantCulture, "//    {0,2}  {1}  {2:F4}\n", i, g, area[g] * cellKm2);
        }
        sb.Append("// ============================================================================\n");
        sb.Append("#pragma once\n\n#include <cstdint>\n\nnamespace aoc::worldd::world::zoi_surface {\n\n");
        sb.Append("inline constexpr int kChunkCm = 32768;\n");
        sb.Append("inline constexpr int kCellCm = 512;\n");
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr int kCells = {0};   // cells per chunk side\n", Cells);
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr int kMinCx = {0};\n", minCx);
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr int kMinCy = {0};\n", minCy);
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr int kColumns = {0};   // chunk columns in x\n", width);
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr int kRowsOfChunks = {0};   // chunk columns in y\n", height);
        sb.Append("inline constexpr uint32_t kNone = 0xFFFFFFFFu;\n");
        sb.Append("inline constexpr uint32_t kUniform = 0x80000000u;\n\n");
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr uint64_t kPalette[{0}] = {{\n", palette.Length);
        for (int i = 0; i < palette.Length; i += 3)
        {
            int end = Math.Min(i + 3, palette.Length);
            sb.Append("    ");
            for (int j = i; j < end; j++)
            {
                if (j > i) sb.Append(", ");
                sb.AppendFormat(CultureInfo.InvariantCulture, "{0}ULL", palette[j]);
            }
            sb.Append(",\n");
        }
        sb.Append("};\n\n");
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr uint32_t kIndex[{0}] = {{\n", index.Length);
        for (int i = 0; i < index.Length; i += 8)
        {
            int end = Math.Min(i + 8, index.Length);
            sb.Append("    ");
            for (int j = i; j < end; j++)
            {
                if (j > i) sb.Append(", ");
                sb.AppendFormat(CultureInfo.InvariantCulture, "0x{0:X8}u", index[j]);
            }
            sb.Append(",\n");
        }
        sb.Append("};\n\n");
        sb.AppendFormat(CultureInfo.InvariantCulture, "inline constexpr uint8_t kRuns[{0}] = {{\n", runs.Count);
        for (int i = 0; i < runs.Count; i += 32)
        {
            int end = Math.Min(i + 32, runs.Count);
            sb.Append("    ");
            for (int j = i; j < end; j++)
            {
                if (j > i) sb.Append(", ");
                sb.AppendFormat(CultureInfo.InvariantCulture, "{0}", runs[j]);
            }
            sb.Append(",\n");
        }
        sb.Append("};\n\n}  // namespace aoc::worldd::world::zoi_surface\n");
        return sb.ToString();
    }
}
'@

if (-not ('AocZoiSurfaceGen' -as [type])) {
    Add-Type -TypeDefinition $csharpSource -Language CSharp
}

function Parse-CliArgs {
    [CmdletBinding()]
    param([string[]]$ArgsList)
    $defaultChunks = if (-not [string]::IsNullOrWhiteSpace($env:AOC_ZOI_CHUNKS)) {
        $env:AOC_ZOI_CHUNKS
    } else {
        Join-Path $script:RepoRoot 'dist\content\zoi_chunks'
    }
    $opts = @{
        Chunks    = $defaultChunks
        ContentDb = ''
        Out       = (Join-Path $script:RepoRoot 'build\gen\world\world\zoi_surface_table.h')
        Check     = $false
    }
    $i = 0
    $count = if ($null -eq $ArgsList) { 0 } else { $ArgsList.Count }
    while ($i -lt $count) {
        switch ($ArgsList[$i]) {
            '--chunks'     { $i++; $opts.Chunks = $ArgsList[$i] }
            '--content-db' { $i++; $opts.ContentDb = $ArgsList[$i] }
            '--out'        { $i++; $opts.Out = $ArgsList[$i] }
            '--check'      { $opts.Check = $true }
            default        { throw "Unknown argument: $($ArgsList[$i])" }
        }
        $i++
    }
    return $opts
}

function Test-KnownPaletteGuids {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ContentDb,
        [Parameter(Mandatory)][uint64[]]$Palette
    )
    $rows = Invoke-SchemaGenSqliteQuery -DbPath $ContentDb -Query 'SELECT guid FROM city_zoi'
    $known = [System.Collections.Generic.HashSet[uint64]]::new()
    foreach ($r in $rows) {
        [void]$known.Add([uint64]$r['guid'])
    }
    $unmapped = [System.Collections.Generic.List[uint64]]::new()
    foreach ($g in $Palette) {
        if ($g -ne 0UL -and -not $known.Contains($g)) {
            $unmapped.Add($g)
        }
    }
    if ($unmapped.Count -gt 0) {
        $joined = ($unmapped | ForEach-Object { $_.ToString() }) -join ', '
        Write-Host "$($unmapped.Count) of the $($Palette.Count) owners are no ZOIRecord of ${ContentDb}: [$joined]"
    }
}

function Invoke-Main {
    [CmdletBinding()]
    param([string[]]$ArgsList)
    $opts = Parse-CliArgs -ArgsList $ArgsList
    if (-not (Test-Path -LiteralPath $opts.Chunks)) {
        throw "ZOI chunks directory not found: $($opts.Chunks)"
    }
    $res = [AocZoiSurfaceGen]::Generate($opts.Chunks)
    if (-not [string]::IsNullOrWhiteSpace($opts.ContentDb)) {
        Test-KnownPaletteGuids -ContentDb $opts.ContentDb -Palette $res.Palette
    }
    if ($opts.Check) {
        $cur = if (Test-Path -LiteralPath $opts.Out) {
            [System.IO.File]::ReadAllText($opts.Out, [System.Text.Encoding]::UTF8).Replace("`r`n", "`n")
        } else {
            $null
        }
        if ($cur -ne $res.HeaderText) {
            [Console]::Error.WriteLine("$($opts.Out): out of date -- re-run gen_zoi_surface.ps1")
            exit 1
        }
        return
    }
    $parent = Split-Path -Parent $opts.Out
    if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($opts.Out, $res.HeaderText, $utf8NoBom)
    Write-Host "wrote $($opts.Out) ($($res.HeaderText.Length) B)"
}

Invoke-Main -ArgsList $RemainingArgs
