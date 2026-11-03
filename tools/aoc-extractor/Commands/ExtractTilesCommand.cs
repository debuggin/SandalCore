// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using AocExtractor.Exporters;
using AocExtractor.Pipeline;
using AocExtractor.PmTiles;
using SandalCore.Config;

namespace AocExtractor.Commands;

public static class ExtractTilesCommand
{
    public static int ExecuteExtractTiles(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        string? configPath = null, outPath = null;
        byte maxZoom = PmTilesV3Writer.DefaultMaxZoom;

        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            string NextVal() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"Missing value for {arg}");
            switch (arg)
            {
                case "extract-tiles": break;
                case "--config": configPath = NextVal(); break;
                case "--out" or "--out-pmtiles": outPath = NextVal(); break;
                case "--max-zoom":
                    if (!byte.TryParse(NextVal(), out maxZoom) || maxZoom > PmTilesV3Writer.DefaultMaxZoom)
                        throw new ArgumentOutOfRangeException(nameof(args), $"--max-zoom must be between 0 and {PmTilesV3Writer.DefaultMaxZoom}.");
                    break;
            }
        }

        if (string.IsNullOrWhiteSpace(outPath))
        {
            ExtractorConfig cfg = SandalConfigLoader.Load(configPath);
            outPath = string.IsNullOrWhiteSpace(cfg.PmtilesOutputPath) ? "artifacts/tiles/world.pmtiles" : cfg.PmtilesOutputPath;
        }

        PmTilesV3Header hdr = BuildAndWritePmTilesFile(outPath, maxZoom);
        Console.WriteLine($"[extract-tiles] Wrote PMTiles v3 '{outPath}' (z=0..{hdr.MaxZoom}, addressed={hdr.AddressedTilesCount}, entries={hdr.TileEntriesCount}, contents={hdr.TileContentsCount}).");
        return 0;
    }

    public static Task<int> ExecuteAsync(ExtractorOptions opts, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(opts);
        ct.ThrowIfCancellationRequested();
        ExtractorConfig cfg = SandalConfigLoader.Load();
        string outPath = string.IsNullOrWhiteSpace(cfg.PmtilesOutputPath) ? "artifacts/tiles/world.pmtiles" : cfg.PmtilesOutputPath;
        PmTilesV3Header hdr = BuildAndWritePmTilesFile(outPath, PmTilesV3Writer.DefaultMaxZoom);
        return Task.FromResult((int)hdr.AddressedTilesCount);
    }

    public static int ExecuteVerifyStandIn(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        string? callerOutDir = null;
        for (int i = 0; i < args.Length; i++)
            if (args[i] == "--out-dir") callerOutDir = i + 1 < args.Length ? args[++i] : throw new ArgumentException("Missing value for --out-dir");

        bool cleanupDir = string.IsNullOrWhiteSpace(callerOutDir);
        string targetDir = cleanupDir ? Path.Combine(Path.GetTempPath(), $"aoc_standin_{Guid.NewGuid():N}") : callerOutDir!;
        Directory.CreateDirectory(targetDir);
        try
        {
            ExtractDbCommand.RunSelfTests(); ExtractVmapCommand.RunSelfTests(); ExtractNavCommand.RunSelfTests();
            string catalogPath = ExtractDbCommand.ResolveCatalogOutputPath(Path.Combine(targetDir, "content.db"));
            DesignDataCatalogSnapshot snapshot = DesignDataCatalogExporter.CreateBaselineSnapshot();
            DesignDataCatalogExporter.ExportToJson(snapshot, catalogPath);
            int dbRows = snapshot.WorldSpawners.Count + snapshot.NpcAbilities.Count + snapshot.WorldInteractables.Count + snapshot.WorldEvents.Count + 1;
            string vmapPath = ExtractVmapCommand.ResolveVmapOutputPath(targetDir), navPath = ExtractNavCommand.ResolveNavOutputPath(targetDir), pmtilesPath = Path.Combine(targetDir, "world.pmtiles");
            using (var vfs = new FileStream(vmapPath, FileMode.Create, FileAccess.Write, FileShare.None)) ExtractVmapCommand.WriteVmapArchive(vfs, ExtractVmapCommand.CreateBaselineArchive());
            using (var nfs = new FileStream(navPath, FileMode.Create, FileAccess.Write, FileShare.None)) ExtractNavCommand.WriteNavArchive(nfs, ExtractNavCommand.CreateBaselineArchive());
            VmapArchiveHeader vmapHdr = ExtractVmapCommand.ReadAndValidateVmapHeader(File.ReadAllBytes(vmapPath));
            NavArchiveHeader navHdr = ExtractNavCommand.ReadAndValidateNavHeader(File.ReadAllBytes(navPath));
            PmTilesV3Header hdr = BuildAndWritePmTilesFile(pmtilesPath, PmTilesV3Writer.DefaultMaxZoom);
            byte[] pmBytes = File.ReadAllBytes(pmtilesPath);
            IReadOnlyList<PmTilesDirectoryEntry> entries = PmTilesV3Writer.ReadDirectoryEntries(pmBytes, PmTilesV3Writer.ValidateAndReadHeader(pmBytes));
            if (dbRows <= 0 || !File.Exists(catalogPath) || vmapHdr.TileCount == 0 || navHdr.TileCount == 0 || hdr.AddressedTilesCount != 5461UL ||
                entries.Count == 0 || !PmTilesV3Writer.TryGetTile(pmBytes, 0, 0, 0, out byte[] z0) || !TilePyramidBuilder.VerifyWebp256x256Header(z0))
                throw new InvalidDataException("Stand-in CI fixture verification (#302) failed parity checks.");

            Console.WriteLine($"[verify-standin] PASS (#302): AOCDB01 ({dbRows} rows), AOCVMAP1 ({vmapHdr.TileCount} tiles), AOCNAV02 ({navHdr.TileCount} tiles), PMTiles v3 ({hdr.AddressedTilesCount} tiles, {hdr.TileContentsCount} unique contents).");
            return 0;
        }
        finally { if (cleanupDir && Directory.Exists(targetDir)) Directory.Delete(targetDir, recursive: true); }
    }

    public static void RunSelfTests() => RunSelfTest();

    public static void RunSelfTest()
    {
        TestHilbertBijectionAllZooms();
        TestPmTilesV3ArchiveAndWebp256();
        TestPmTilesTamperAndBoundsRejection();
        string tmpPm = Path.Combine(Path.GetTempPath(), $"aoc_tiles_{Guid.NewGuid():N}.pmtiles");
        try
        {
            int tilesRc = ExecuteExtractTiles(["extract-tiles", "--out", tmpPm, "--max-zoom", "3"]);
            if (tilesRc != 0 || !File.Exists(tmpPm) || PmTilesV3Writer.ValidateAndReadHeader(File.ReadAllBytes(tmpPm)).MaxZoom != 3)
                throw new InvalidOperationException("ExecuteExtractTiles CLI round-trip failed.");
        }
        finally { if (File.Exists(tmpPm)) File.Delete(tmpPm); }
        int rc = ExecuteVerifyStandIn(["verify-standin"]);
        if (rc != 0) throw new InvalidOperationException($"ExecuteVerifyStandIn returned non-zero: {rc}");
    }

    private static PmTilesV3Header BuildAndWritePmTilesFile(string outPath, byte maxZoom)
    {
        string? dir = Path.GetDirectoryName(Path.GetFullPath(outPath));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        IReadOnlyList<PmTilesInputTile> pyramid = TilePyramidBuilder.BuildPyramid(maxZoom);
        using var fs = new FileStream(outPath, FileMode.Create, FileAccess.Write, FileShare.None);
        return PmTilesV3Writer.WriteArchive(fs, pyramid, PmTilesV3Writer.DefaultMinZoom, maxZoom);
    }

    private static void TestHilbertBijectionAllZooms()
    {
        ulong[] expectedBases = [0UL, 1UL, 5UL, 21UL, 85UL, 341UL, 1365UL];
        var seen = new HashSet<ulong>();
        for (byte z = 0; z <= PmTilesV3Writer.DefaultMaxZoom; z++)
        {
            if (PmTilesV3Writer.ZxyToTileId(z, 0, 0) != expectedBases[z])
                throw new InvalidOperationException($"Expected base offset {expectedBases[z]} at zoom {z}.");
            uint n = 1u << z;
            for (uint y = 0; y < n; y++)
            {
                for (uint x = 0; x < n; x++)
                {
                    ulong id = PmTilesV3Writer.ZxyToTileId(z, x, y);
                    if (!seen.Add(id)) throw new InvalidOperationException($"Duplicate Hilbert TileId {id} at ({z},{x},{y}).");
                    (byte rz, uint rx, uint ry) = PmTilesV3Writer.TileIdToZxy(id);
                    if (rz != z || rx != x || ry != y)
                        throw new InvalidOperationException($"Hilbert round-trip mismatch for ({z},{x},{y}) -> {id} -> ({rz},{rx},{ry}).");
                }
            }
        }
        if (seen.Count != 5461) throw new InvalidOperationException($"Expected 5461 total tiles for z=0..6, got {seen.Count}.");
    }

    private static void TestPmTilesV3ArchiveAndWebp256()
    {
        IReadOnlyList<PmTilesInputTile> pyramid = TilePyramidBuilder.BuildPyramid(PmTilesV3Writer.DefaultMaxZoom);
        using var ms = new MemoryStream();
        PmTilesV3Header written = PmTilesV3Writer.WriteArchive(ms, pyramid, 0, PmTilesV3Writer.DefaultMaxZoom);
        byte[] archive = ms.ToArray();

        PmTilesV3Header hdr = PmTilesV3Writer.ValidateAndReadHeader(archive);
        if (hdr != written || hdr.Clustered != 1 || hdr.InternalCompression != 2 || hdr.TileCompression != 1 ||
            hdr.TileType != 4 || hdr.MinZoom != 0 || hdr.MaxZoom != 6 || hdr.AddressedTilesCount != 5461UL ||
            hdr.TileContentsCount >= hdr.AddressedTilesCount)
        {
            throw new InvalidOperationException("PMTiles v3 127-byte header or content-addressed deduplication check failed.");
        }

        IReadOnlyList<PmTilesDirectoryEntry> entries = PmTilesV3Writer.ReadDirectoryEntries(archive, hdr);
        ulong addressedSum = 0UL;
        for (int i = 0; i < entries.Count; i++)
        {
            if (i > 0 && entries[i].TileId < entries[i - 1].TileId + entries[i - 1].RunLength)
                throw new InvalidOperationException("Directory entries are not strictly ordered by Hilbert TileId.");
            addressedSum += entries[i].RunLength;
        }
        if (addressedSum != hdr.AddressedTilesCount)
            throw new InvalidOperationException("Directory RunLength sum does not match AddressedTilesCount.");

        foreach ((byte z, uint x, uint y) in new (byte, uint, uint)[] { (0, 0, 0), (3, 4, 4), (6, 0, 0), (6, 32, 32), (6, 63, 63) })
        {
            if (!PmTilesV3Writer.TryGetTile(archive, z, x, y, out byte[] tile) || !TilePyramidBuilder.VerifyWebp256x256Header(tile))
                throw new InvalidOperationException($"Failed TryGetTile or 256x256 WebP VP8L verification at ({z},{x},{y}).");
        }
        if (PmTilesV3Writer.TryGetTile(archive, 7, 0, 0, out _))
            throw new InvalidOperationException("Expected zoom 7 lookup to return false.");
    }

    private static void TestPmTilesTamperAndBoundsRejection()
    {
        IReadOnlyList<PmTilesInputTile> pyramid = TilePyramidBuilder.BuildPyramid(2);
        using var ms = new MemoryStream();
        PmTilesV3Writer.WriteArchive(ms, pyramid, 0, 2);
        byte[] valid = ms.ToArray();

        ExpectInvalidData(() => PmTilesV3Writer.ValidateAndReadHeader(valid.AsSpan(0, 126)), "truncated < 127 header");
        byte[] badMagic = (byte[])valid.Clone(); badMagic[7] = 0x02;
        ExpectInvalidData(() => PmTilesV3Writer.ValidateAndReadHeader(badMagic), "wrong magic version");
        byte[] badSlice = (byte[])valid.Clone();
        BinaryPrimitives.WriteUInt64LittleEndian(badSlice.AsSpan(56, 8), ulong.MaxValue - 10UL);
        ExpectInvalidData(() => PmTilesV3Writer.ValidateAndReadHeader(badSlice), "overflowing TileDataOffset");
        byte[] badZoom = (byte[])valid.Clone(); badZoom[100] = 5; badZoom[101] = 2;
        ExpectInvalidData(() => PmTilesV3Writer.ValidateAndReadHeader(badZoom), "MinZoom > MaxZoom");
    }

    private static void ExpectInvalidData(Action act, string reason)
    {
        try { act(); } catch (InvalidDataException) { return; }
        throw new InvalidOperationException($"Expected InvalidDataException was not thrown: {reason}");
    }
}
