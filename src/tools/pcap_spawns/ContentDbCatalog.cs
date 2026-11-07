// SPDX-License-Identifier: Apache-2.0
using System.Globalization;
using Microsoft.Data.Sqlite;

namespace PcapSpawns;

public sealed record GatherDefEntry(
    long Guid,
    string Name,
    string Profession,
    string ResourceType,
    int Tier,
    string TierName);

public sealed record ZoiEntry(
    ulong Guid,
    string Name,
    string Biome,
    string Environment);

public sealed class ContentDbCatalog
{
    private const int Cells = 64;
    private const int CellCm = 512;
    private const int ChunkCm = Cells * CellCm;
    private const int Total = Cells * Cells * Cells;
    private const int SurfaceIz = Cells / 2;

    private static readonly int[] Spread = BuildSpread();
    private static readonly long[] Slab = BuildSlab();

    private readonly Dictionary<long, GatherDefEntry> _gatherDefs = new();
    private readonly HashSet<long> _validGatherGuids = new();
    private readonly Dictionary<ulong, ZoiEntry> _zois = new();
    private readonly Dictionary<(int cx, int cy), (ulong[] pal, byte[] surf)> _chunks = new();

    public IReadOnlyDictionary<long, GatherDefEntry> GatherDefs => _gatherDefs;
    public HashSet<long> ValidGatherGuids => _validGatherGuids;
    public IReadOnlyDictionary<ulong, ZoiEntry> Zois => _zois;

    public static ContentDbCatalog Load(string contentDbPath, string zoiChunksDir)
    {
        var cat = new ContentDbCatalog();
        cat.LoadSqlite(contentDbPath);
        cat.LoadZoiChunks(zoiChunksDir);
        return cat;
    }

    private void LoadSqlite(string contentDbPath)
    {
        string fullDb = Path.GetFullPath(contentDbPath);
        using var conn = new SqliteConnection($"Data Source={fullDb};Mode=ReadOnly");
        conn.Open();
        LoadGatherDefs(conn);
        LoadCityZois(conn);
    }

    private void LoadGatherDefs(SqliteConnection conn)
    {
        using var cmd = conn.CreateCommand();
        cmd.CommandText = @"
            SELECT
                gd.guid,
                gd.name,
                COALESCE(p.name, '') AS profession,
                COALESCE(rt.name, frt.name, '') AS resource_type,
                gd.cert_tier AS tier,
                COALESCE(cl.name, '') AS tier_name
            FROM gather_def gd
            LEFT JOIN profession p ON p.guid = gd.profession_guid
            LEFT JOIN record rt ON rt.guid = gd.resource_type_guid
            LEFT JOIN foliage_def fd ON fd.guid = gd.foliage_guid
            LEFT JOIN record frt ON frt.guid = fd.resource_type_guid
            LEFT JOIN certification_level cl ON cl.guid = gd.cert_guid";
        using var r = cmd.ExecuteReader();
        while (r.Read())
        {
            long guid = r.GetInt64(0);
            string name = r.GetString(1);
            string prof = r.GetString(2);
            string resType = r.GetString(3);
            int tier = r.GetInt32(4);
            string tierName = r.GetString(5);
            if (string.IsNullOrEmpty(resType)) resType = InferResourceTypeFromName(name);
            if (string.IsNullOrEmpty(prof)) prof = InferProfession(resType, name);
            if (string.IsNullOrEmpty(tierName)) tierName = FallbackTierName(tier);
            var entry = new GatherDefEntry(guid, name, prof, resType, tier, tierName);
            _gatherDefs[guid] = entry;
            _validGatherGuids.Add(guid);
        }
    }

    private static string InferResourceTypeFromName(string name)
    {
        string[] parts = name.Split('_', StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length >= 2) return parts[0] + "_" + parts[1];
        return name;
    }

    private static string InferProfession(string resourceType, string name)
    {
        string key = !string.IsNullOrEmpty(resourceType) ? resourceType : name;
        if (key.StartsWith("Stone_", StringComparison.OrdinalIgnoreCase) ||
            key.StartsWith("Metal_", StringComparison.OrdinalIgnoreCase) ||
            key.StartsWith("Gem_", StringComparison.OrdinalIgnoreCase)) return "Mining";
        if (key.StartsWith("Tree_", StringComparison.OrdinalIgnoreCase)) return "Lumberjacking";
        if (key.StartsWith("Plant_", StringComparison.OrdinalIgnoreCase)) return "Herbalism";
        if (key.StartsWith("Fish_", StringComparison.OrdinalIgnoreCase)) return "Fishing";
        return "Hunting";
    }

    private static string FallbackTierName(int tier) => tier switch
    {
        0 => "Novice",
        1 => "Apprentice",
        2 => "Journeyman",
        3 => "Master",
        4 => "GrandMaster",
        _ => "Novice"
    };

    private void LoadCityZois(SqliteConnection conn)
    {
        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT guid, name, biome, environment FROM city_zoi";
        using var r = cmd.ExecuteReader();
        while (r.Read())
        {
            ulong guid = unchecked((ulong)r.GetInt64(0));
            string name = r.GetString(1);
            string biome = r.GetString(2);
            string env = r.GetString(3);
            _zois[guid] = new ZoiEntry(guid, name, biome, env);
        }
    }

    private void LoadZoiChunks(string zoiChunksDir)
    {
        foreach (string f in Directory.EnumerateFiles(zoiChunksDir, "C_*_*_0.uasset", SearchOption.AllDirectories))
        {
            string[] parts = Path.GetFileNameWithoutExtension(f).Split('_');
            if (parts.Length != 4 || parts[0] != "C" || parts[3] != "0") continue;
            int cx = int.Parse(parts[1], CultureInfo.InvariantCulture);
            int cy = int.Parse(parts[2], CultureInfo.InvariantCulture);
            DecodeChunk(f, out ulong[] pal, out byte[] surf);
            _chunks[(cx, cy)] = (pal, surf);
        }
    }

    public bool TryResolveZoi(double xCm, double yCm, out ulong zoiGuid, out ZoiEntry zoi)
    {
        zoiGuid = 0UL;
        zoi = default!;
        int cx = (int)Math.Floor(xCm / ChunkCm);
        int cy = (int)Math.Floor(yCm / ChunkCm);
        if (!_chunks.TryGetValue((cx, cy), out var chunk)) return false;
        int ix = Math.Clamp((int)Math.Floor((xCm - (double)cx * ChunkCm) / CellCm), 0, Cells - 1);
        int iy = Math.Clamp((int)Math.Floor((yCm - (double)cy * ChunkCm) / CellCm), 0, Cells - 1);
        byte palIdx = chunk.surf[iy * Cells + ix];
        if (palIdx >= chunk.pal.Length) return false;
        zoiGuid = chunk.pal[palIdx];
        if (zoiGuid == 0UL) return false;
        if (!_zois.TryGetValue(zoiGuid, out var found)) return false;
        if (string.Equals(found.Environment, "Ocean", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(found.Biome, "Default", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }
        zoi = found;
        return true;
    }

    private static int[] BuildSpread()
    {
        var s = new int[Cells];
        for (int n = 0; n < Cells; n++)
        {
            int v = 0;
            for (int k = 0; k < 6; k++) v |= ((n >> k) & 1) << (3 * k);
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
        for (int k = 0; k < p; k++) pal[k] = BitConverter.ToUInt64(d, palOff + 8 * k);
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
        if (pos != Total || i != d.Length) throw new InvalidDataException(path + ": cell count mismatch");
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
}
