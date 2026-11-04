// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.Text;
using AocExtractor;
using AocExtractor.Commands;
using AocExtractor.Exporters;
using AocExtractor.Pipeline;
using AocExtractor.PmTiles;
using Xunit;

namespace AocExtractor.Tests;

public sealed class ExtractorTests
{
    [Fact]
    public async Task ParseArgsAndPipelineContracts_ValidateFlagsAesKeysAndBoundedChannel()
    {
        string validKey = "0x" + new string('a', 64);
        string[] cli =
        [
            "extract-vmap", "--pak-dir", "./paks", "--usmap", "./config/layouts/aoc.usmap",
            "--aes-key", validKey, "--out-sqlite", "./out/content.db",
            "--out-nav", "./out/nav", "--max-zoom", "3", "--self-test"
        ];
        (ExtractorOptions opts, bool selfTest) = Program.ParseArgs(cli);
        Assert.True(selfTest);
        Assert.Equal("extract-vmap", opts.Command);
        Assert.Equal("./paks", opts.PakDir);
        Assert.Equal("./config/layouts/aoc.usmap", opts.UsmapPath);
        Assert.Equal(validKey, opts.AesKeyHex);
        Assert.Equal("./out/content.db", opts.OutSqlitePath);
        Assert.Equal("./out/nav", opts.OutNavDir);

        Assert.Throws<ArgumentException>(() => Program.ParseArgs(["--unknown-flag"]));
        Assert.Throws<ArgumentException>(() => Program.ParseArgs(["--pak-dir"]));
        Assert.Throws<ArgumentException>(() => Program.ParseArgs(["--aes-key"]));

        byte[] keyBytes = IoStoreExtractionPipeline.ValidateAesKey(validKey);
        Assert.Equal(32, keyBytes.Length);
        Assert.Throws<ArgumentException>(() => IoStoreExtractionPipeline.ValidateAesKey(""));
        Assert.Throws<ArgumentException>(() => IoStoreExtractionPipeline.ValidateAesKey("0x1234"));
        Assert.Throws<ArgumentException>(() => IoStoreExtractionPipeline.ValidateAesKey(new string('z', 64)));
        Assert.Throws<ArgumentOutOfRangeException>(() => IoStoreExtractionPipeline.CreateWorkChannel(0));

        var items = new[] { new AssetWorkItem("/Game/A.uasset", "pak0", 3), new AssetWorkItem("/Game/B.uasset", "pak0", 5) };
        int prioritySum = 0;
        int processed = await IoStoreExtractionPipeline.ProcessWorkItemsAsync(
            items, (item, _) => { Interlocked.Add(ref prioritySum, item.Priority); return ValueTask.CompletedTask; }, capacity: 64);
        Assert.Equal(2, processed);
        Assert.Equal(8, prioritySum);
    }

    [Fact]
    public void DesignDataCatalogExporter_DeterministicJsonSerializationAndInvariantValidation()
    {
        DesignDataCatalogSnapshot snapshot = DesignDataCatalogExporter.CreateBaselineSnapshot();
        DesignDataCatalogExporter.VerifySnapshotInvariants(snapshot);

        string json1 = DesignDataCatalogExporter.SerializeJson(snapshot);
        Assert.Equal(json1, DesignDataCatalogExporter.SerializeJson(snapshot));

        DesignDataCatalogSnapshot roundTrip = DesignDataCatalogExporter.DeserializeJson(json1);
        Assert.Equal(snapshot.WorldSpawners.Count, roundTrip.WorldSpawners.Count);
        Assert.Equal(snapshot.NpcAbilities.Count, roundTrip.NpcAbilities.Count);
        Assert.Equal(snapshot.WorldEvents.Count, roundTrip.WorldEvents.Count);
        Assert.Equal(DesignDataCatalogExporter.SpawnerCategoryHumanoidSpot, DesignDataCatalogExporter.ClassifySpawnerCategory("AAoCPopulationSpot", "/Game/Population"));
        Assert.Equal(DesignDataCatalogExporter.SpawnerCategoryHostile, DesignDataCatalogExporter.ClassifySpawnerCategory("BP_EncounterSpawner", "/Game/Sublevels/Encounters/Test"));

        var badClock = snapshot with { SkyClockConfig = new SkyClockConfigRow(0.0, 6.0, 7200.0) };
        Assert.Throws<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(badClock));
        var badSpawners = snapshot with { WorldSpawners = [new WorldSpawnerRow("", "HOSTILE", "BP", "Sub", 1, 0, 0, 0, 10, 0, 5, 1)] };
        Assert.Throws<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(badSpawners));
    }

    [Fact]
    public void ArtifactStampManifest_CalculatesDeterministicSha256StampsAndVerifiesDag()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-ext-stamp-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            foreach (string name in new[] { "content.db", "verra.vmap", "verra.nav", "verra.pmtiles" })
                File.WriteAllText(Path.Combine(tempDir, name), $"payload-{name}");

            string hexDb = ArtifactStampManifest.ComputeFileSha256Hex(Path.Combine(tempDir, "content.db"));
            Assert.Equal(64, hexDb.Length);
            Assert.Equal(hexDb, ArtifactStampManifest.ComputeUtf8Sha256Hex("payload-content.db"));
            Assert.Equal(["config", "curves"], ArtifactStampManifest.GetUpstreamStages("database"));
            Assert.Contains("navmesh", ArtifactStampManifest.GetDownstreamClosure("database"));
            Assert.Throws<ArgumentException>(() => ArtifactStampManifest.GetUpstreamStages("invalid-stage"));

            ArtifactStampEntry[] entries =
            [
                ArtifactStampManifest.CreateEntryFromFile(tempDir, "content.db", ArtifactStampManifest.ArtifactDbId, ArtifactStampManifest.MagicDb, 10UL),
                ArtifactStampManifest.CreateEntryFromFile(tempDir, "verra.vmap", ArtifactStampManifest.ArtifactVmapId, ArtifactStampManifest.MagicVmap, 4UL),
                ArtifactStampManifest.CreateEntryFromFile(tempDir, "verra.nav", ArtifactStampManifest.ArtifactNavId, ArtifactStampManifest.MagicNav, 4UL),
                ArtifactStampManifest.CreateEntryFromFile(tempDir, "verra.pmtiles", ArtifactStampManifest.ArtifactPmTilesId, ArtifactStampManifest.MagicPmTiles, 85UL)
            ];
            ArtifactStampManifest manifest = ArtifactStampManifest.Create(hexDb, entries);
            ArtifactStampManifest loaded = ArtifactStampManifest.DeserializeJson(ArtifactStampManifest.SerializeJson(manifest));
            ArtifactStampManifest.VerifyManifest(loaded, tempDir);

            File.WriteAllText(Path.Combine(tempDir, "content.db"), "tampered-payload");
            Assert.Throws<InvalidDataException>(() => ArtifactStampManifest.VerifyManifest(loaded, tempDir));
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public void UsmapCompat_DowngradesV4ToV3AndRejectsTruncatedOrCorruptHeaders()
    {
        byte[] payload = BuildMinimalV4UncompressedPayload();
        byte[] v4 = new byte[UsmapCompat.V4VersionedHeaderLength + payload.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(v4.AsSpan(0, 2), UsmapCompat.Magic);
        v4[2] = 4; v4[3] = 1; v4[12] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(v4.AsSpan(13, 4), (uint)payload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(v4.AsSpan(17, 4), (uint)payload.Length);
        payload.CopyTo(v4.AsSpan(UsmapCompat.V4VersionedHeaderLength));

        byte[] v3 = UsmapCompat.DowngradeToV3(v4);
        Assert.Equal(3, v3[2]);
        Assert.Equal(UsmapCompat.DowngradeToV3(v4), v3);
        Assert.Throws<InvalidDataException>(() => UsmapCompat.DowngradeToV3(new byte[8]));

        byte[] badMagic = (byte[])v4.Clone(); badMagic[0] = 0x00;
        Assert.Throws<InvalidDataException>(() => UsmapCompat.DowngradeToV3(badMagic));
        byte[] badVer = (byte[])v4.Clone(); badVer[2] = 9;
        Assert.Throws<InvalidDataException>(() => UsmapCompat.DowngradeToV3(badVer));
        byte[] badComp = (byte[])v4.Clone(); badComp[12] = 77;
        Assert.Throws<InvalidDataException>(() => UsmapCompat.DowngradeToV3(badComp));
    }

    [Fact]
    public void VmapAndNavBinaryArchives_EnforceHeaderMagicCrc32BoundsAndRejectCorruption()
    {
        using var vmapStream = new MemoryStream();
        ExtractVmapCommand.WriteVmapArchive(vmapStream, ExtractVmapCommand.CreateBaselineArchive());
        byte[] vmapBytes = vmapStream.ToArray();
        VmapArchiveHeader vmapHdr = ExtractVmapCommand.ReadAndValidateVmapHeader(vmapBytes);
        Assert.Equal(ExtractVmapCommand.VmapVersion, vmapHdr.Version);
        Assert.Throws<InvalidDataException>(() => ExtractVmapCommand.ReadAndValidateVmapHeader(vmapBytes.AsSpan(0, 32)));
        byte[] badVmap = (byte[])vmapBytes.Clone();
        badVmap[0] ^= 0xFF;
        Assert.Throws<InvalidDataException>(() => ExtractVmapCommand.ReadAndValidateVmapHeader(badVmap));

        using var navStream = new MemoryStream();
        ExtractNavCommand.WriteNavArchive(navStream, ExtractNavCommand.CreateBaselineArchive());
        byte[] navBytes = navStream.ToArray();
        NavArchiveHeader navHdr = ExtractNavCommand.ReadAndValidateNavHeader(navBytes);
        Assert.Equal(ExtractNavCommand.WalkableClimbCm, navHdr.WalkableClimbCm);
        Assert.Throws<InvalidDataException>(() => ExtractNavCommand.ReadAndValidateNavHeader(navBytes.AsSpan(0, 16)));
        Assert.Throws<InvalidDataException>(() =>
            ExtractNavCommand.ValidateAgentAndClimbParams(ExtractNavCommand.LegacyWalkableClimbCm, 34f, 180f, 45f));

        byte[] tiles = TilePyramidBuilder.EncodeWebp256x256Tile(new byte[256]);
        Assert.True(TilePyramidBuilder.VerifyWebp256x256Header(tiles));
        Assert.False(TilePyramidBuilder.VerifyWebp256x256Header(tiles.AsSpan(0, 12)));
        Assert.Throws<InvalidDataException>(() => PmTilesV3Writer.ValidateAndReadHeader(new byte[64]));
    }

    private static byte[] BuildMinimalV4UncompressedPayload()
    {
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true);
        bw.Write(1u);
        bw.Write((ushort)4);
        bw.Write("Item"u8);
        bw.Write(1u);
        bw.Write(0u);
        bw.Write((ushort)1);
        bw.Write(0u);
        bw.Write(0u);
        bw.Flush();
        return ms.ToArray();
    }
}
