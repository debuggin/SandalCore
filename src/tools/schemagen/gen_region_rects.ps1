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

public sealed class RegionRectsResult
{
    public string BiomeJson { get; set; }
    public string ZoiJson { get; set; }
    public int BiomeRectCount { get; set; }
    public int RiverCellCount { get; set; }
    public int ZoiZoneCount { get; set; }
    public int TotalZoiRectCount { get; set; }
    public string AllBiomesJson { get; set; }
    public string AllBiomesZoiJson { get; set; }
    public string AllBiomesMapcTsv { get; set; }
    public int AllBiomesRectCount { get; set; }
    public int AllBiomesCellCount { get; set; }
    public int AllBiomesZoiCount { get; set; }
    public int AllBiomesZoiRectCount { get; set; }
}

internal sealed class PyTupleSet
{
    private const int MinSize = 8;
    private const int LinearProbes = 9;
    private const ulong P1 = 11400714785074694791UL;
    private const ulong P2 = 14029467366897019727UL;
    private const ulong P5 = 2870177450012600261UL;

    private struct Entry
    {
        public bool Used;
        public int A;
        public int B;
        public ulong Hash;
    }

    private Entry[] _table = new Entry[MinSize];
    private int _used;

    private static ulong PyIntHash(int v)
    {
        if (v == 0) return 0UL;
        if (v > 0) return (ulong)v;
        ulong h = unchecked((ulong)(-(long)v));
        return h == ulong.MaxValue ? unchecked(ulong.MaxValue - 1UL) : h;
    }

    private static ulong Tuple2Hash(int a, int b)
    {
        ulong acc = P5;
        acc = unchecked(acc + PyIntHash(a) * P2);
        acc = (acc << 31) | (acc >> 33);
        acc = unchecked(acc * P1);
        acc = unchecked(acc + PyIntHash(b) * P2);
        acc = (acc << 31) | (acc >> 33);
        acc = unchecked(acc * P1);
        acc = unchecked(acc + (2UL ^ (P5 ^ 3527539UL)));
        if (acc == ulong.MaxValue) return 1546275796UL;
        return acc;
    }

    public void Add(int a, int b)
    {
        ulong h = Tuple2Hash(a, b);
        if (InsertCleanOrExisting(a, b, h))
        {
            _used++;
            if (_used * 5 >= (_table.Length - 1) * 3)
            {
                int minUsed = _used * 4;
                int newSize = MinSize;
                while (newSize <= minUsed) newSize <<= 1;
                Resize(newSize);
            }
        }
    }

    private bool InsertCleanOrExisting(int a, int b, ulong h)
    {
        ulong mask = (ulong)(_table.Length - 1);
        ulong i = h & mask;
        ulong perturb = h;
        while (true)
        {
            ref Entry e = ref _table[i];
            if (!e.Used)
            {
                e.Used = true;
                e.A = a;
                e.B = b;
                e.Hash = h;
                return true;
            }
            if (e.Hash == h && e.A == a && e.B == b) return false;
            if (i + LinearProbes <= mask)
            {
                for (ulong j = 1; j <= LinearProbes; j++)
                {
                    ref Entry e2 = ref _table[i + j];
                    if (!e2.Used)
                    {
                        e2.Used = true;
                        e2.A = a;
                        e2.B = b;
                        e2.Hash = h;
                        return true;
                    }
                    if (e2.Hash == h && e2.A == a && e2.B == b) return false;
                }
            }
            perturb >>= 5;
            i = (i * 5UL + 1UL + perturb) & mask;
        }
    }

    private void Resize(int newSize)
    {
        var old = _table;
        _table = new Entry[newSize];
        for (int k = 0; k < old.Length; k++)
        {
            if (old[k].Used)
            {
                InsertCleanOrExisting(old[k].A, old[k].B, old[k].Hash);
            }
        }
    }

    public bool Contains(int a, int b)
    {
        ulong h = Tuple2Hash(a, b);
        ulong mask = (ulong)(_table.Length - 1);
        ulong i = h & mask;
        ulong perturb = h;
        while (true)
        {
            ref Entry e = ref _table[i];
            if (!e.Used) return false;
            if (e.Hash == h && e.A == a && e.B == b) return true;
            if (i + LinearProbes <= mask)
            {
                for (ulong j = 1; j <= LinearProbes; j++)
                {
                    ref Entry e2 = ref _table[i + j];
                    if (!e2.Used) return false;
                    if (e2.Hash == h && e2.A == a && e2.B == b) return true;
                }
            }
            perturb >>= 5;
            i = (i * 5UL + 1UL + perturb) & mask;
        }
    }

    public List<(int a, int b)> ToOrderedList()
    {
        var res = new List<(int a, int b)>(_used);
        for (int k = 0; k < _table.Length; k++)
        {
            if (_table[k].Used)
            {
                res.Add((_table[k].A, _table[k].B));
            }
        }
        return res;
    }
}

public static class AocRegionRectsGen
{
    private const int Cells = 64;
    private const int CellCm = 512;
    private const int ChunkCm = Cells * CellCm;
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

    private static void DecodeChunk(string path, out ulong[] pal, out byte[] surf)
    {
        byte[] d = File.ReadAllBytes(path);
        uint hs = BitConverter.ToUInt32(d, 4);
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

    private static List<(int a, int b, int c, int d)> Decompose(byte[][] rows)
    {
        var openList = new List<(int i0, int i1, int j0)>();
        var openMap = new Dictionary<(int i0, int i1), int>();
        var outRects = new List<(int a, int b, int c, int d)>();
        int h = rows.Length;
        int w = rows[0].Length;

        for (int j = 0; j < h; j++)
        {
            byte[] row = rows[j];
            var runs = new PyTupleSet();
            int idx = 0;
            while (idx < w)
            {
                while (idx < w && row[idx] == 0) idx++;
                if (idx >= w) break;
                int start = idx;
                while (idx < w && row[idx] != 0) idx++;
                runs.Add(start, idx);
            }

            var closed = new List<(int i0, int i1, int j0)>();
            var remaining = new List<(int i0, int i1, int j0)>(openList.Count);
            for (int k = 0; k < openList.Count; k++)
            {
                var item = openList[k];
                if (!runs.Contains(item.i0, item.i1))
                {
                    closed.Add(item);
                    openMap.Remove((item.i0, item.i1));
                }
                else
                {
                    remaining.Add(item);
                }
            }
            openList = remaining;
            for (int k = 0; k < closed.Count; k++)
            {
                var c = closed[k];
                outRects.Add((c.i0, c.j0, c.i1, j));
            }
            var runItems = runs.ToOrderedList();
            for (int k = 0; k < runItems.Count; k++)
            {
                var s = runItems[k];
                if (!openMap.ContainsKey((s.a, s.b)))
                {
                    openMap[(s.a, s.b)] = j;
                    openList.Add((s.a, s.b, j));
                }
            }
        }
        for (int k = 0; k < openList.Count; k++)
        {
            var c = openList[k];
            outRects.Add((c.i0, c.j0, c.i1, h));
        }
        return outRects;
    }

    private static byte[][] Transpose(byte[][] rows)
    {
        int h = rows.Length;
        int w = rows[0].Length;
        var cols = new byte[w][];
        for (int x = 0; x < w; x++)
        {
            var col = new byte[h];
            for (int y = 0; y < h; y++)
            {
                col[y] = rows[y][x];
            }
            cols[x] = col;
        }
        return cols;
    }

    private static List<int[]> Cut(byte[][] rows, int x0, int y0)
    {
        var alongY = Decompose(rows);
        var alongXRaw = Decompose(Transpose(rows));
        var chosen = new List<(int a, int b, int c, int d)>();
        if (alongY.Count <= alongXRaw.Count)
        {
            chosen = alongY;
        }
        else
        {
            chosen.Capacity = alongXRaw.Count;
            for (int i = 0; i < alongXRaw.Count; i++)
            {
                var b = alongXRaw[i];
                chosen.Add((b.b, b.a, b.d, b.c));
            }
        }
        var res = new List<int[]>(chosen.Count);
        for (int i = 0; i < chosen.Count; i++)
        {
            var r = chosen[i];
            res.Add(new int[] { x0 + r.a * CellCm, y0 + r.b * CellCm, x0 + r.c * CellCm, y0 + r.d * CellCm });
        }
        return res;
    }

    private static void LoadWorldGrid(
        string chunksDir,
        out byte[][] rows,
        out List<ulong> guids,
        out int minCx,
        out int minCy)
    {
        var chunkMap = new Dictionary<(int cx, int cy), string>();
        minCx = int.MaxValue; int maxCx = int.MinValue;
        minCy = int.MaxValue; int maxCy = int.MinValue;
        foreach (string f in Directory.EnumerateFiles(chunksDir, "C_*_*_0.uasset", SearchOption.AllDirectories))
        {
            string[] parts = Path.GetFileNameWithoutExtension(f).Split('_');
            if (parts.Length != 4 || parts[0] != "C" || parts[3] != "0") continue;
            int cx = int.Parse(parts[1], CultureInfo.InvariantCulture);
            int cy = int.Parse(parts[2], CultureInfo.InvariantCulture);
            chunkMap[(cx, cy)] = f;
            if (cx < minCx) minCx = cx;
            if (cx > maxCx) maxCx = cx;
            if (cy < minCy) minCy = cy;
            if (cy > maxCy) maxCy = cy;
        }
        int nx = (maxCx - minCx + 1) * Cells;
        int ny = (maxCy - minCy + 1) * Cells;
        rows = new byte[ny][];
        for (int j = 0; j < ny; j++) rows[j] = new byte[nx];
        guids = new List<ulong> { 0UL };
        var ids = new Dictionary<ulong, byte> { [0UL] = 0 };
        for (int cx = minCx; cx <= maxCx; cx++)
        {
            for (int cy = minCy; cy <= maxCy; cy++)
            {
                string p;
                if (!chunkMap.TryGetValue((cx, cy), out p)) continue;
                DecodeAndBlitChunk(p, cx - minCx, cy - minCy, rows, ids, guids);
            }
        }
    }

    private static void DecodeAndBlitChunk(
        string path, int offCx, int offCy, byte[][] rows,
        Dictionary<ulong, byte> ids, List<ulong> guids)
    {
        ulong[] pal;
        byte[] cells;
        DecodeChunk(path, out pal, out cells);
        var local = new byte[pal.Length];
        for (int k = 0; k < pal.Length; k++)
        {
            ulong g = pal[k];
            byte gid;
            if (!ids.TryGetValue(g, out gid))
            {
                gid = (byte)ids.Count;
                ids[g] = gid;
                guids.Add(g);
            }
            local[k] = gid;
        }
        int baseX = offCx * Cells;
        int baseY = offCy * Cells;
        for (int iy = 0; iy < Cells; iy++)
        {
            byte[] dstRow = rows[baseY + iy];
            int srcOff = iy * Cells;
            for (int ix = 0; ix < Cells; ix++)
            {
                dstRow[baseX + ix] = local[cells[srcOff + ix]];
            }
        }
    }

    private static List<int[]> CutTargetSet(
        byte[][] rows, List<ulong> guids, HashSet<ulong> targetSet,
        int x0, int y0, out int totalCells)
    {
        int ny = rows.Length;
        int nx = rows[0].Length;
        var maskIds = new bool[guids.Count];
        for (int i = 0; i < guids.Count; i++) maskIds[i] = targetSet.Contains(guids[i]);
        var maskRows = new byte[ny][];
        totalCells = 0;
        for (int j = 0; j < ny; j++)
        {
            var mRow = new byte[nx];
            byte[] src = rows[j];
            for (int i = 0; i < nx; i++)
            {
                if (maskIds[src[i]]) { mRow[i] = 1; totalCells++; }
            }
            maskRows[j] = mRow;
        }
        return Cut(maskRows, x0, y0);
    }

    private static List<(ulong guid, string name, int cells, List<int[]> rects, int order)> BuildZoiRectList(
        byte[][] rows, List<ulong> guids, Dictionary<ulong, string> names,
        HashSet<ulong> targetSet, int x0, int y0)
    {
        int ny = rows.Length;
        int nx = rows[0].Length;
        var minJ = new int[guids.Count]; var maxJ = new int[guids.Count];
        var minI = new int[guids.Count]; var maxI = new int[guids.Count];
        var counts = new int[guids.Count];
        for (int k = 0; k < guids.Count; k++) { minJ[k] = ny; maxJ[k] = -1; minI[k] = nx; maxI[k] = -1; }
        for (int j = 0; j < ny; j++)
        {
            byte[] src = rows[j];
            for (int i = 0; i < nx; i++)
            {
                byte id = src[i];
                if (id == 0) continue;
                counts[id]++;
                if (j < minJ[id]) minJ[id] = j;
                if (j > maxJ[id]) maxJ[id] = j;
                if (i < minI[id]) minI[id] = i;
                if (i > maxI[id]) maxI[id] = i;
            }
        }
        var zois = new List<(ulong guid, string name, int cells, List<int[]> rects, int order)>();
        for (int idx = 1; idx < guids.Count; idx++)
        {
            ulong g = guids[idx];
            string zName;
            if (!targetSet.Contains(g) || !names.TryGetValue(g, out zName)) continue;
            int cCount = counts[idx];
            var zRects = cCount > 0
                ? CutSingleZoiBox(rows, (byte)idx, minI[idx], maxI[idx], minJ[idx], maxJ[idx], x0, y0)
                : new List<int[]>();
            zois.Add((g, zName, cCount, zRects, idx));
        }
        zois.Sort((a, b) =>
        {
            int c = b.cells.CompareTo(a.cells);
            return c != 0 ? c : a.order.CompareTo(b.order);
        });
        return zois;
    }

    private static List<int[]> CutSingleZoiBox(
        byte[][] rows, byte idx, int i0, int i1, int j0, int j1, int x0, int y0)
    {
        int subH = j1 - j0 + 1;
        int subW = i1 - i0 + 1;
        var zRows = new byte[subH][];
        for (int j = 0; j < subH; j++)
        {
            var zRow = new byte[subW];
            byte[] src = rows[j0 + j];
            for (int i = 0; i < subW; i++)
            {
                if (src[i0 + i] == idx) zRow[i] = 1;
            }
            zRows[j] = zRow;
        }
        return Cut(zRows, x0 + i0 * CellCm, y0 + j0 * CellCm);
    }

    public static RegionRectsResult Generate(
        string chunksDir,
        Dictionary<ulong, string> names,
        HashSet<ulong> riverSet,
        HashSet<ulong> allBiomesSet)
    {
        byte[][] rows;
        List<ulong> guids;
        int minCx, minCy;
        LoadWorldGrid(chunksDir, out rows, out guids, out minCx, out minCy);
        int x0 = minCx * ChunkCm;
        int y0 = minCy * ChunkCm;

        int riverCells;
        var biomeRects = CutTargetSet(rows, guids, riverSet, x0, y0, out riverCells);
        var zois = BuildZoiRectList(rows, guids, names, riverSet, x0, y0);
        int totalZoiRects = 0;
        for (int i = 0; i < zois.Count; i++) totalZoiRects += zois[i].rects.Count;

        int allCells;
        var allRects = CutTargetSet(rows, guids, allBiomesSet, x0, y0, out allCells);
        var allZois = BuildZoiRectList(rows, guids, names, allBiomesSet, x0, y0);
        int allZoiRects = 0;
        for (int i = 0; i < allZois.Count; i++) allZoiRects += allZois[i].rects.Count;

        return new RegionRectsResult
        {
            BiomeJson = RenderBiomeJson(riverCells, biomeRects),
            ZoiJson = RenderZoiJson(zois),
            BiomeRectCount = biomeRects.Count,
            RiverCellCount = riverCells,
            ZoiZoneCount = zois.Count,
            TotalZoiRectCount = totalZoiRects,
            AllBiomesJson = RenderBiomeJson(allCells, allRects),
            AllBiomesZoiJson = RenderZoiJson(allZois),
            AllBiomesMapcTsv = RenderMapcTsv(allCells, allRects),
            AllBiomesRectCount = allRects.Count,
            AllBiomesCellCount = allCells,
            AllBiomesZoiCount = allZois.Count,
            AllBiomesZoiRectCount = allZoiRects
        };
    }

    private static string RenderMapcTsv(int cells, List<int[]> rects)
    {
        var sb = new StringBuilder(rects.Count * 40 + 256);
        sb.Append("# region_rects v1: the FullVerra all-biomes footprint as disjoint rectangles, world cm, [x0,x1) x [y0,y1) (tools/mapc reads it; GENERATED by gen_region_rects.ps1, do not edit)\n");
        sb.AppendFormat(CultureInfo.InvariantCulture, "# cells\t{0}\tcell_cm\t{1}\tcount\t{2}\n", cells, CellCm, rects.Count);
        for (int i = 0; i < rects.Count; i++)
        {
            int[] q = rects[i];
            sb.AppendFormat(CultureInfo.InvariantCulture, "rect\t{0}\t{1}\t{2}\t{3}\n", q[0], q[1], q[2], q[3]);
        }
        return sb.ToString();
    }

    private static void AppendRectsJson(StringBuilder sb, List<int[]> rects)
    {
        sb.Append('[');
        for (int i = 0; i < rects.Count; i++)
        {
            if (i > 0) sb.Append(',');
            int[] r = rects[i];
            sb.AppendFormat(CultureInfo.InvariantCulture, "[{0},{1},{2},{3}]", r[0], r[1], r[2], r[3]);
        }
        sb.Append(']');
    }

    private static string EscapeJsonString(string s)
    {
        var sb = new StringBuilder(s.Length + 4);
        foreach (char c in s)
        {
            if (c == '"' || c == '\\') sb.Append('\\').Append(c);
            else sb.Append(c);
        }
        return sb.ToString();
    }

    private static string RenderBiomeJson(int cells, List<int[]> rects)
    {
        const string Note = "disjoint axis-aligned rectangles [x0,y0,x1,y1) in world cm whose union is the Riverlands surface footprint (the 1-cell hole is kept)";
        var sb = new StringBuilder(rects.Count * 36 + 256);
        sb.Append("{\"cell_cm\":512,\"reference_z_cm\":16384,");
        sb.AppendFormat(CultureInfo.InvariantCulture, "\"count\":{0},\"area_cells\":{1},\"rects_xy_cm\":", rects.Count, cells);
        AppendRectsJson(sb, rects);
        sb.AppendFormat(CultureInfo.InvariantCulture, ",\"note\":\"{0}\"}}", Note);
        return sb.ToString();
    }

    private static string RenderZoiJson(List<(ulong guid, string name, int cells, List<int[]> rects, int order)> zois)
    {
        const string Note = "disjoint [x0,y0,x1,y1) rects per Riverlands ZOI at z=16384 (the owner is the same for every z in [3072,163840))";
        var sb = new StringBuilder(65536);
        sb.AppendFormat(CultureInfo.InvariantCulture, "{{\"cell_cm\":512,\"reference_z_cm\":16384,\"note\":\"{0}\",\"zois\":[", Note);
        for (int i = 0; i < zois.Count; i++)
        {
            if (i > 0) sb.Append(',');
            var z = zois[i];
            sb.AppendFormat(CultureInfo.InvariantCulture,
                "{{\"name\":\"{0}\",\"guid\":{1},\"cells\":{2},\"rects_xy_cm\":",
                EscapeJsonString(z.name), z.guid, z.cells);
            AppendRectsJson(sb, z.rects);
            sb.Append('}');
        }
        sb.Append("]}");
        return sb.ToString();
    }
}
'@

if (-not ('AocRegionRectsGen' -as [type])) {
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
    $defaultDb = if (-not [string]::IsNullOrWhiteSpace($env:AOC_CONTENT_DB)) {
        $env:AOC_CONTENT_DB
    } else {
        Join-Path $script:RepoRoot 'dist\content\content.db'
    }
    $opts = @{
        Chunks          = $defaultChunks
        ContentDb       = $defaultDb
        BiomeOut        = (Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_rects.json')
        ZoiOut          = (Join-Path $script:RepoRoot 'build\gen\fixtures\region\riverlands_zoi_rects.json')
        AllBiomesOut    = (Join-Path $script:RepoRoot 'build\gen\fixtures\region\all_biomes_rects.json')
        AllBiomesZoiOut = (Join-Path $script:RepoRoot 'build\gen\fixtures\region\all_biomes_zoi_rects.json')
        AllBiomesTsvOut = (Join-Path $script:RepoRoot 'build\gen\nav\all_biomes_rects.tsv')
        Check           = $false
    }
    $i = 0
    $count = if ($null -eq $ArgsList) { 0 } else { $ArgsList.Count }
    while ($i -lt $count) {
        switch ($ArgsList[$i]) {
            '--chunks'         { $i++; $opts.Chunks = $ArgsList[$i] }
            '--content-db'     { $i++; $opts.ContentDb = $ArgsList[$i] }
            '--biome-out'      { $i++; $opts.BiomeOut = $ArgsList[$i] }
            '--zoi-out'        { $i++; $opts.ZoiOut = $ArgsList[$i] }
            '--all-biomes-tsv' { $i++; $opts.AllBiomesTsvOut = $ArgsList[$i] }
            '--check'          { $opts.Check = $true }
            default            { throw "Unknown argument: $($ArgsList[$i])" }
        }
        $i++
    }
    return $opts
}

function Read-ZoiDbMetadata {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$ContentDb)
    $rows = Invoke-SchemaGenSqliteQuery -DbPath $ContentDb -Query 'SELECT guid, name, biome, environment FROM city_zoi'
    $names = [System.Collections.Generic.Dictionary[uint64, string]]::new()
    $river = [System.Collections.Generic.HashSet[uint64]]::new()
    $allBiomes = [System.Collections.Generic.HashSet[uint64]]::new()
    foreach ($r in $rows) {
        $g = [uint64]$r['guid']
        $biome = [string]$r['biome']
        $envName = [string]$r['environment']
        $names[$g] = [string]$r['name']
        if ($biome -eq 'Riverlands') {
            [void]$river.Add($g)
        }
        if ($envName -ne 'Ocean' -and $biome -notin @('Default', 'cCount', '')) {
            [void]$allBiomes.Add($g)
        }
    }
    return @{ Names = $names; River = $river; AllBiomes = $allBiomes }
}

function Write-OutputOrCheck {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][hashtable]$Opts,
        [Parameter(Mandatory)][RegionRectsResult]$Result
    )
    if ($Opts.Check) {
        $pairs = @(
            @{ Path = $Opts.BiomeOut; Text = $Result.BiomeJson }
            @{ Path = $Opts.ZoiOut;   Text = $Result.ZoiJson }
        )
        foreach ($pair in $pairs) {
            $cur = if (Test-Path -LiteralPath $pair.Path) {
                [System.IO.File]::ReadAllText($pair.Path, [System.Text.Encoding]::UTF8).Replace("`r`n", "`n")
            } else {
                $null
            }
            if ($cur -ne $pair.Text) {
                [Console]::Error.WriteLine("$($pair.Path): out of date -- re-run gen_region_rects.ps1")
                exit 1
            }
        }
        return
    }
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    foreach ($p in @($Opts.BiomeOut, $Opts.ZoiOut, $Opts.AllBiomesOut, $Opts.AllBiomesZoiOut, $Opts.AllBiomesTsvOut)) {
        $parent = Split-Path -Parent $p
        if (-not [string]::IsNullOrWhiteSpace($parent) -and -not (Test-Path -LiteralPath $parent)) {
            [void](New-Item -ItemType Directory -Path $parent -Force)
        }
    }
    [System.IO.File]::WriteAllText($Opts.BiomeOut, $Result.BiomeJson, $utf8NoBom)
    [System.IO.File]::WriteAllText($Opts.ZoiOut, $Result.ZoiJson, $utf8NoBom)
    [System.IO.File]::WriteAllText($Opts.AllBiomesOut, $Result.AllBiomesJson, $utf8NoBom)
    [System.IO.File]::WriteAllText($Opts.AllBiomesZoiOut, $Result.AllBiomesZoiJson, $utf8NoBom)
    [System.IO.File]::WriteAllText($Opts.AllBiomesTsvOut, $Result.AllBiomesMapcTsv, $utf8NoBom)
    Write-Host "riverlands: $($Result.BiomeRectCount) rects covering $($Result.RiverCellCount) cells -> $($Opts.BiomeOut)"
    Write-Host "zois: $($Result.ZoiZoneCount) zones ($($Result.TotalZoiRectCount) rects total) -> $($Opts.ZoiOut)"
    Write-Host "all-biomes: $($Result.AllBiomesRectCount) rects covering $($Result.AllBiomesCellCount) cells ($($Result.AllBiomesZoiCount) zones, $($Result.AllBiomesZoiRectCount) ZOI rects) -> $($Opts.AllBiomesTsvOut)"
}

function Invoke-Main {
    [CmdletBinding()]
    param([string[]]$ArgsList)
    $opts = Parse-CliArgs -ArgsList $ArgsList
    if (-not (Test-Path -LiteralPath $opts.Chunks)) {
        throw "ZOI chunks directory not found: $($opts.Chunks)"
    }
    $meta = Read-ZoiDbMetadata -ContentDb $opts.ContentDb
    $res = [AocRegionRectsGen]::Generate($opts.Chunks, $meta.Names, $meta.River, $meta.AllBiomes)
    Write-OutputOrCheck -Opts $opts -Result $res
}

Invoke-Main -ArgsList $RemainingArgs
