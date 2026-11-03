// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.Security.Cryptography;
using AocExtractor.Exporters;
using AocExtractor.Pipeline;
using AocExtractor.PmTiles;

namespace AocExtractor.Commands;

public sealed record StandInVerificationReport(
    int CatalogRowCount,
    uint VmapTileCount,
    uint NavTileCount,
    ulong PmTilesAddressedCount,
    ArtifactStampManifest Manifest);

public static class VerifyStandinCommand
{
    public const int MinCatalogRowCount = 13;
    public const ulong ExpectedPmTilesAddressedCount = 5461UL;
    public const string DefaultInputSeed = "sandalcore-standin-v0.7.5";
    public const string CatalogFileName = "content.catalog.json";
    public const string VmapFileName = "area_vmap.bin";
    public const string NavFileName = "area_nav.bin";
    public const string PmTilesFileName = "world.pmtiles";

    public static int ExecuteVerifyStandIn(string[] args) => Execute(args);

    public static int Execute(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        string? callerOutDir = null;
        for (int i = 0; i < args.Length; i++)
        {
            if (args[i] == "--out-dir")
                callerOutDir = i + 1 < args.Length ? args[++i] : throw new ArgumentException("Missing value for --out-dir");
        }

        bool cleanupDir = string.IsNullOrWhiteSpace(callerOutDir);
        string targetDir = cleanupDir ? Path.Combine(Path.GetTempPath(), $"aoc_standin_{Guid.NewGuid():N}") : callerOutDir!;
        Directory.CreateDirectory(targetDir);
        try
        {
            ExtractDbCommand.RunSelfTests();
            ExtractVmapCommand.RunSelfTests();
            ExtractNavCommand.RunSelfTests();
            _ = GenerateStandInArtifacts(targetDir, DefaultInputSeed);
            StandInVerificationReport report = VerifyStandInDirectory(targetDir);
            Console.WriteLine(
                $"[verify-standin] PASS (#302): AOCDB01 ({report.CatalogRowCount} rows), " +
                $"AOCVMAP1 ({report.VmapTileCount} tiles), AOCNAV02 ({report.NavTileCount} tiles), " +
                $"PMTiles v3 ({report.PmTilesAddressedCount} tiles), Stamp ({report.Manifest.ManifestDigestSha256[..16]}...).");
            return 0;
        }
        finally
        {
            if (cleanupDir && Directory.Exists(targetDir))
                Directory.Delete(targetDir, recursive: true);
        }
    }

    public static ArtifactStampManifest GenerateStandInArtifacts(string targetDir, string inputSeed = DefaultInputSeed)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(targetDir);
        ArgumentException.ThrowIfNullOrWhiteSpace(inputSeed);
        Directory.CreateDirectory(targetDir);

        string catalogPath = Path.Combine(targetDir, CatalogFileName);
        DesignDataCatalogSnapshot snapshot = DesignDataCatalogExporter.CreateBaselineSnapshot();
        DesignDataCatalogExporter.ExportToJson(snapshot, catalogPath);
        int dbRows = CountCatalogRows(snapshot);

        string vmapPath = Path.Combine(targetDir, VmapFileName);
        VmapArchiveData vmapData = ExtractVmapCommand.CreateBaselineArchive();
        using (var vfs = new FileStream(vmapPath, FileMode.Create, FileAccess.Write, FileShare.None))
            ExtractVmapCommand.WriteVmapArchive(vfs, vmapData);

        string navPath = Path.Combine(targetDir, NavFileName);
        NavArchiveData navData = ExtractNavCommand.CreateBaselineArchive();
        using (var nfs = new FileStream(navPath, FileMode.Create, FileAccess.Write, FileShare.None))
            ExtractNavCommand.WriteNavArchive(nfs, navData);

        string pmTilesPath = Path.Combine(targetDir, PmTilesFileName);
        IReadOnlyList<PmTilesInputTile> pyramid = TilePyramidBuilder.BuildPyramid(PmTilesV3Writer.DefaultMaxZoom);
        PmTilesV3Header pmHdr;
        using (var pfs = new FileStream(pmTilesPath, FileMode.Create, FileAccess.Write, FileShare.None))
            pmHdr = PmTilesV3Writer.WriteArchive(pfs, pyramid, PmTilesV3Writer.DefaultMinZoom, PmTilesV3Writer.DefaultMaxZoom);

        IReadOnlyList<ArtifactStampEntry> entries =
        [
            ArtifactStampManifest.CreateEntryFromFile(targetDir, CatalogFileName, ArtifactStampManifest.ArtifactDbId, ArtifactStampManifest.MagicDb, (ulong)dbRows),
            ArtifactStampManifest.CreateEntryFromFile(targetDir, VmapFileName, ArtifactStampManifest.ArtifactVmapId, ArtifactStampManifest.MagicVmap, (ulong)vmapData.Tiles.Count),
            ArtifactStampManifest.CreateEntryFromFile(targetDir, NavFileName, ArtifactStampManifest.ArtifactNavId, ArtifactStampManifest.MagicNav, (ulong)navData.Tiles.Count),
            ArtifactStampManifest.CreateEntryFromFile(targetDir, PmTilesFileName, ArtifactStampManifest.ArtifactPmTilesId, ArtifactStampManifest.MagicPmTiles, pmHdr.AddressedTilesCount)
        ];

        string inputSha = ArtifactStampManifest.IsHexSha256(inputSeed)
            ? inputSeed.ToLowerInvariant()
            : ArtifactStampManifest.ComputeUtf8Sha256Hex(inputSeed);
        ArtifactStampManifest manifest = ArtifactStampManifest.Create(inputSha, entries);
        ArtifactStampManifest.SaveToFile(manifest, Path.Combine(targetDir, ArtifactStampManifest.DefaultManifestFileName));
        return manifest;
    }

    public static StandInVerificationReport VerifyStandInDirectory(string targetDir)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(targetDir);
        if (!Directory.Exists(targetDir))
            throw new InvalidDataException($"Stand-in directory does not exist: {targetDir}");

        string catalogPath = Path.Combine(targetDir, CatalogFileName);
        string vmapPath = Path.Combine(targetDir, VmapFileName);
        string navPath = Path.Combine(targetDir, NavFileName);
        string pmTilesPath = Path.Combine(targetDir, PmTilesFileName);
        string manifestPath = Path.Combine(targetDir, ArtifactStampManifest.DefaultManifestFileName);

        int dbRows = VerifyCatalogArtifact(catalogPath);
        VmapArchiveHeader vmapHdr = VerifyVmapArtifact(File.ReadAllBytes(vmapPath));
        NavArchiveHeader navHdr = VerifyNavArtifact(File.ReadAllBytes(navPath));
        PmTilesV3Header pmHdr = VerifyPmTilesArtifact(File.ReadAllBytes(pmTilesPath));

        ArtifactStampManifest manifest = ArtifactStampManifest.LoadFromFile(manifestPath);
        ArtifactStampManifest.VerifyManifest(manifest, targetDir);
        return new StandInVerificationReport(dbRows, vmapHdr.TileCount, navHdr.TileCount, pmHdr.AddressedTilesCount, manifest);
    }

    public static int VerifyCatalogArtifact(string catalogPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(catalogPath);
        if (!File.Exists(catalogPath))
            throw new InvalidDataException($"AOCDB01 catalog artifact missing: {catalogPath}");

        DesignDataCatalogSnapshot snapshot = DesignDataCatalogExporter.DeserializeJson(File.ReadAllText(catalogPath));
        DesignDataCatalogExporter.VerifySnapshotInvariants(snapshot);
        int rows = CountCatalogRows(snapshot);
        if (rows < MinCatalogRowCount)
            throw new InvalidDataException($"AOCDB01 catalog requires >= {MinCatalogRowCount} rows; got {rows}.");

        bool hasHumanoid = snapshot.WorldSpawners.Any(static s => s.SpawnerCategory == DesignDataCatalogExporter.SpawnerCategoryHumanoidSpot);
        bool hasHostile = snapshot.WorldSpawners.Any(static s => s.SpawnerCategory == DesignDataCatalogExporter.SpawnerCategoryHostile);
        if (!hasHumanoid || !hasHostile)
            throw new InvalidDataException("AOCDB01 catalog missing required HUMANOID_SPOT or HOSTILE spawner categories.");
        return rows;
    }

    public static VmapArchiveHeader VerifyVmapArtifact(ReadOnlySpan<byte> vmapBytes)
    {
        VmapArchiveHeader hdr = ExtractVmapCommand.ReadAndValidateVmapHeader(vmapBytes);
        if (hdr.TileCount == 0u)
            throw new InvalidDataException("AOCVMAP1 archive must contain at least 1 tile.");

        int payloadOffset = ExtractVmapCommand.HeaderSizeBytes + (int)(hdr.TileCount * ExtractVmapCommand.TocEntrySizeBytes);
        Span<byte> computedSha = stackalloc byte[32];
        SHA256.HashData(vmapBytes[payloadOffset..], computedSha);
        if (!computedSha.SequenceEqual(hdr.ContentSha256))
            throw new InvalidDataException("AOCVMAP1 32-byte header ContentSha256 does not match tile payload digest.");
        return hdr;
    }

    public static NavArchiveHeader VerifyNavArtifact(ReadOnlySpan<byte> navBytes)
    {
        NavArchiveHeader hdr = ExtractNavCommand.ReadAndValidateNavHeader(navBytes);
        if ((hdr.Flags & ExtractNavCommand.FlagPolyRef64) == 0u || (hdr.Flags & ExtractNavCommand.FlagHasBridgeSpans) == 0u)
            throw new InvalidDataException("AOCNAV02 header missing mandatory FlagPolyRef64 or FlagHasBridgeSpans.");
        if (hdr.TileCount == 0u || hdr.BridgeSpanCount == 0u)
            throw new InvalidDataException("AOCNAV02 archive requires non-zero TileCount and BridgeSpanCount (#167).");

        ValidateNavBodyBounds(navBytes, hdr);
        return hdr;
    }

    private static void ValidateNavBodyBounds(ReadOnlySpan<byte> navBytes, NavArchiveHeader hdr)
    {
        ulong spanBytes = (ulong)hdr.BridgeSpanCount * ExtractNavCommand.BridgeSpanSizeBytes;
        ulong cursor = (ulong)ExtractNavCommand.HeaderSizeBytes;
        if (cursor + spanBytes > (ulong)navBytes.Length)
            throw new InvalidDataException("AOCNAV02 truncated inside bridge span records.");

        for (uint i = 0; i < hdr.BridgeSpanCount; i++)
        {
            ReadOnlySpan<byte> s = navBytes.Slice((int)cursor, ExtractNavCommand.BridgeSpanSizeBytes);
            float sx = BinaryPrimitives.ReadSingleLittleEndian(s.Slice(8, 4)), sy = BinaryPrimitives.ReadSingleLittleEndian(s.Slice(12, 4));
            float ex = BinaryPrimitives.ReadSingleLittleEndian(s.Slice(16, 4)), ey = BinaryPrimitives.ReadSingleLittleEndian(s.Slice(20, 4));
            float dz = BinaryPrimitives.ReadSingleLittleEndian(s.Slice(24, 4));
            float dx = ex - sx, dy = ey - sy;
            if (!float.IsFinite(sx) || !float.IsFinite(sy) || !float.IsFinite(ex) || !float.IsFinite(ey) || !float.IsFinite(dz) || (dx * dx + dy * dy) <= 0f)
                throw new InvalidDataException("AOCNAV02 bridge span contains non-finite coordinates or zero length.");
            cursor += ExtractNavCommand.BridgeSpanSizeBytes;
        }

        for (uint t = 0; t < hdr.TileCount; t++)
        {
            if (cursor + 20UL > (ulong)navBytes.Length)
                throw new InvalidDataException("AOCNAV02 truncated inside tile record header.");
            uint payloadLen = BinaryPrimitives.ReadUInt32LittleEndian(navBytes.Slice((int)cursor + 16, 4));
            cursor += 20UL;
            if (payloadLen == 0u || cursor + payloadLen > (ulong)navBytes.Length)
                throw new InvalidDataException("AOCNAV02 tile payload is empty or exceeds archive bounds.");
            cursor += payloadLen;
        }
        if (cursor != (ulong)navBytes.Length)
            throw new InvalidDataException("AOCNAV02 archive has unparsed trailing bytes.");
    }

    public static PmTilesV3Header VerifyPmTilesArtifact(ReadOnlySpan<byte> pmTilesBytes)
    {
        PmTilesV3Header hdr = PmTilesV3Writer.ValidateAndReadHeader(pmTilesBytes);
        if (hdr.Clustered != 1 || hdr.InternalCompression != 2 || hdr.TileCompression != 1 || hdr.TileType != 4 ||
            hdr.MinZoom != 0 || hdr.MaxZoom != 6 || hdr.AddressedTilesCount != ExpectedPmTilesAddressedCount ||
            hdr.TileContentsCount == 0UL || hdr.TileContentsCount >= hdr.AddressedTilesCount)
        {
            throw new InvalidDataException("PMTiles v3 header invariants (z=0..6, 5461 Hilbert tiles, WebP) failed.");
        }

        IReadOnlyList<PmTilesDirectoryEntry> entries = PmTilesV3Writer.ReadDirectoryEntries(pmTilesBytes, hdr);
        if (entries.Count == 0)
            throw new InvalidDataException("PMTiles v3 root directory is empty.");
        ulong sum = 0UL;
        for (int i = 0; i < entries.Count; i++)
        {
            if (i > 0 && entries[i].TileId < entries[i - 1].TileId + entries[i - 1].RunLength)
                throw new InvalidDataException("PMTiles v3 directory entries violate Hilbert TileId ordering.");
            sum += entries[i].RunLength;
        }
        if (sum != ExpectedPmTilesAddressedCount)
            throw new InvalidDataException($"PMTiles v3 directory RunLength sum {sum} != {ExpectedPmTilesAddressedCount}.");

        foreach ((byte z, uint x, uint y) in new (byte, uint, uint)[] { (0, 0, 0), (3, 4, 4), (6, 0, 0), (6, 32, 32), (6, 63, 63) })
        {
            if (!PmTilesV3Writer.TryGetTile(pmTilesBytes, z, x, y, out byte[] tile) || !TilePyramidBuilder.VerifyWebp256x256Header(tile))
                throw new InvalidDataException($"PMTiles v3 WebP 256x256 VP8L tile check failed at ({z},{x},{y}).");
        }
        return hdr;
    }

    public static void RunSelfTests()
    {
        string tmpDir = Path.Combine(Path.GetTempPath(), $"aoc_verify_standin_{Guid.NewGuid():N}");
        try
        {
            ArtifactStampManifest m1 = GenerateStandInArtifacts(tmpDir, DefaultInputSeed);
            StandInVerificationReport r1 = VerifyStandInDirectory(tmpDir);
            ArtifactStampManifest m2 = GenerateStandInArtifacts(tmpDir, DefaultInputSeed);
            if (r1.CatalogRowCount < MinCatalogRowCount || r1.PmTilesAddressedCount != ExpectedPmTilesAddressedCount ||
                m1.ManifestDigestSha256 != m2.ManifestDigestSha256)
            {
                throw new InvalidOperationException("VerifyStandinCommand completeness or idempotency check failed.");
            }

            string vmapPath = Path.Combine(tmpDir, VmapFileName);
            byte[] vmapBytes = File.ReadAllBytes(vmapPath);
            byte[] badSha = [.. vmapBytes];
            badSha[40] ^= 0xFF;
            ExpectInvalidData(() => VerifyVmapArtifact(badSha));

            File.WriteAllBytes(vmapPath, badSha);
            ExpectInvalidData(() => VerifyStandInDirectory(tmpDir));
        }
        finally
        {
            if (Directory.Exists(tmpDir))
                Directory.Delete(tmpDir, recursive: true);
        }
    }

    private static int CountCatalogRows(DesignDataCatalogSnapshot s) =>
        s.WorldSpawners.Count + s.NpcAbilities.Count + s.WorldInteractables.Count + s.WorldEvents.Count + 1;

    private static void ExpectInvalidData(Action action)
    {
        try { action(); } catch (InvalidDataException) { return; }
        throw new InvalidOperationException("Expected InvalidDataException was not thrown.");
    }
}
