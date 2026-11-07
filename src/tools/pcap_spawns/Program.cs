// SPDX-License-Identifier: Apache-2.0
using System.Diagnostics;
using System.Globalization;

namespace PcapSpawns;

public static class Program
{
    public static int Main(string[] args)
    {
        string repoRoot = FindRepoRoot();
        string pcapDir = @"D:\GitHub\PCAPRepo";
        string outPath = Path.Combine(repoRoot, "dist", "content", "pcap_resource_spawners.tsv");
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--repo-root" && i + 1 < args.Length) repoRoot = args[++i];
            else if (args[i] == "--pcap-dir" && i + 1 < args.Length) pcapDir = args[++i];
            else if (args[i] == "--out" && i + 1 < args.Length) outPath = args[++i];
        }

        string dbPath = Path.Combine(repoRoot, "dist", "content", "content.db");
        string zoiDir = Path.Combine(repoRoot, "dist", "content", "zoi_chunks");
        string areaSpawnsPath = Path.Combine(repoRoot, "src", "server", "world", "spawns", "area_spawns.h");
        var sw = Stopwatch.StartNew();
        var catalog = ContentDbCatalog.Load(dbPath, zoiDir);
        Console.WriteLine(
            $"[pcap_spawns] Loaded {catalog.GatherDefs.Count} gather_def rows and {catalog.Zois.Count} city_zoi rows ({sw.ElapsedMilliseconds} ms).");

        var rawPoints = AreaSpawnsParser.ParseFile(areaSpawnsPath, catalog.ValidGatherGuids);
        int fromHeader = rawPoints.Count;
        var pcapPoints = PcapFileScanner.ScanDirectory(
            pcapDir, catalog.ValidGatherGuids, out int filesScanned, out int pktsScanned);
        rawPoints.AddRange(pcapPoints);
        Console.WriteLine(
            $"[pcap_spawns] Scanned {filesScanned} PCAP files ({pktsScanned} packets): {pcapPoints.Count} raw gatherable opens + {fromHeader} from area_spawns.h.");

        string foliagePath = Path.Combine(repoRoot, "dist", "content", "region_foliage.tsv");
        var foliageIndex = ClusterBuilder.LoadPaintedFoliageIndex(foliagePath);
        var rows = ClusterBuilder.BuildClusters(rawPoints, catalog, foliageIndex, out int strippedByFoliage);
        Console.WriteLine(
            $"[pcap_spawns] Indexed {foliageIndex.SectionCount} painted foliage sections ({foliageIndex.InstanceCount} instances); stripped {strippedByFoliage} PCAP points inside painted foliage sections of the same def_name.");
        ClusterBuilder.WriteTsv(outPath, rows);
        SyncToDeploy(repoRoot, outPath);
        PrintSpawnerSummary(rows);
        PrintFoliageSummary(foliagePath, catalog);
        return 0;
    }

    private static string FindRepoRoot()
    {
        string dir = AppContext.BaseDirectory;
        for (int i = 0; i < 8; i++)
        {
            if (File.Exists(Path.Combine(dir, "AGENTS.md"))) return dir;
            string? parent = Path.GetDirectoryName(dir);
            if (string.IsNullOrEmpty(parent)) break;
            dir = parent;
        }
        return Directory.GetCurrentDirectory();
    }

    private static void SyncToDeploy(string repoRoot, string spawnersOutPath)
    {
        string deployDir = Path.Combine(repoRoot, "deploy", "content");
        if (!Directory.Exists(deployDir)) return;
        string deploySpawners = Path.Combine(deployDir, "pcap_resource_spawners.tsv");
        if (File.Exists(deploySpawners)) File.Delete(deploySpawners);
        File.Copy(spawnersOutPath, deploySpawners);
    }

    private static void PrintSpawnerSummary(IReadOnlyList<ResourceSpawnerRow> rows)
    {
        int totalInstances = rows.Sum(r => r.Points.Count);
        double maxRadius = rows.Count > 0 ? rows.Max(r => r.RadiusCm) : 0.0;
        Console.WriteLine(
            $"[pcap_spawns] Wrote {rows.Count} cluster spawners ({totalInstances} unique instances, max radius {maxRadius:F1} cm <= 3500.0 cm):");
        foreach (var grp in rows.GroupBy(r => r.Zoi.Biome).OrderByDescending(g => g.Count()))
        {
            int inst = grp.Sum(x => x.Points.Count);
            Console.WriteLine($"  spawners biome={grp.Key,-20} spawners={grp.Count(),5} instances={inst,6}");
        }
        foreach (var grp in rows.GroupBy(r => r.Def.Profession).OrderByDescending(g => g.Count()))
        {
            int inst = grp.Sum(x => x.Points.Count);
            Console.WriteLine($"  spawners profession={grp.Key,-15} spawners={grp.Count(),5} instances={inst,6}");
        }
    }

    private static void PrintFoliageSummary(string foliageTsvPath, ContentDbCatalog catalog)
    {
        if (!File.Exists(foliageTsvPath)) return;
        var countsByBiome = new Dictionary<string, int>(StringComparer.Ordinal);
        int total = 0;
        using var sr = new StreamReader(foliageTsvPath);
        string? line;
        while ((line = sr.ReadLine()) != null)
        {
            if (line.Length == 0 || line[0] == '#') continue;
            string[] cols = line.Split('\t');
            if (cols.Length < 8) continue;
            if (!double.TryParse(cols[6], NumberStyles.Float, CultureInfo.InvariantCulture, out double x)) continue;
            if (!double.TryParse(cols[7], NumberStyles.Float, CultureInfo.InvariantCulture, out double y)) continue;
            total++;
            string biome = catalog.TryResolveZoi(x, y, out _, out var zoi) ? zoi.Biome : "Unmapped";
            countsByBiome[biome] = countsByBiome.GetValueOrDefault(biome) + 1;
        }
        Console.WriteLine($"[pcap_spawns] region_foliage.tsv biome breakdown ({total} total instances):");
        foreach (var kv in countsByBiome.OrderByDescending(k => k.Value))
        {
            Console.WriteLine($"  foliage  biome={kv.Key,-20} instances={kv.Value,7}");
        }
    }
}
