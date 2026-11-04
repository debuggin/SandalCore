// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using AocExtractor;
using AocExtractor.Commands;
using AocExtractor.Exporters;
using AocExtractor.Pipeline;
using AocExtractor.PmTiles;
using Xunit;

namespace SandalExtractor.Tests;

public sealed class WorldDataCompletenessTests
{
    [Fact]
    public void VerifyStandInArtifacts_AllFourFormatsAndManifest_PassCompletenessOracle()
    {
        string tempDir = CreateTempDir();
        try
        {
            ArtifactStampManifest generated = VerifyStandinCommand.GenerateStandInArtifacts(tempDir);
            StandInVerificationReport report = VerifyStandinCommand.VerifyStandInDirectory(tempDir);

            Assert.True(report.CatalogRowCount >= VerifyStandinCommand.MinCatalogRowCount);
            Assert.True(report.VmapTileCount >= 1u);
            Assert.True(report.NavTileCount >= 1u);
            Assert.Equal(VerifyStandinCommand.ExpectedPmTilesAddressedCount, report.PmTilesAddressedCount);
            Assert.Equal(11, report.Manifest.Stages.Count);
            Assert.Equal(4, report.Manifest.Artifacts.Count);
            Assert.Equal(generated.ManifestDigestSha256, report.Manifest.ManifestDigestSha256);

            int cliExitCode = VerifyStandinCommand.Execute(["verify-standin", "--out-dir", tempDir]);
            Assert.Equal(0, cliExitCode);
        }
        finally
        {
            CleanupTempDir(tempDir);
        }
    }

    [Fact]
    public void ArtifactStampManifest_DeterministicGeneration_IsIdempotentAcrossRuns()
    {
        string dirA = CreateTempDir();
        string dirB = CreateTempDir();
        try
        {
            ArtifactStampManifest mA = VerifyStandinCommand.GenerateStandInArtifacts(dirA, "seed-alpha");
            ArtifactStampManifest mB = VerifyStandinCommand.GenerateStandInArtifacts(dirB, "seed-alpha");

            Assert.Equal(mA.ManifestDigestSha256, mB.ManifestDigestSha256);
            Assert.Equal(
                mA.Stages.Select(static s => s.StageSha256),
                mB.Stages.Select(static s => s.StageSha256));
            Assert.Equal(
                mA.Artifacts.Select(static a => a.OutputSha256),
                mB.Artifacts.Select(static a => a.OutputSha256));
            Assert.Equal(ArtifactStampManifest.SerializeJson(mA), ArtifactStampManifest.SerializeJson(mB));

            ArtifactStampManifest mDifferentSeed = VerifyStandinCommand.GenerateStandInArtifacts(dirB, "seed-beta");
            Assert.NotEqual(mA.InputFingerprintSha256, mDifferentSeed.InputFingerprintSha256);
            Assert.NotEqual(mA.ManifestDigestSha256, mDifferentSeed.ManifestDigestSha256);
        }
        finally
        {
            CleanupTempDir(dirA);
            CleanupTempDir(dirB);
        }
    }

    [Fact]
    public void ArtifactStampManifest_ElevenStageTopologicalDag_PropagatesDownstreamInvalidation()
    {
        string seed = ArtifactStampManifest.ComputeUtf8Sha256Hex("dag-seed-01");
        IReadOnlyList<StageStampEntry> baseline = ArtifactStampManifest.BuildStageDag(seed);
        var mutatedInputs = new Dictionary<string, string>(StringComparer.Ordinal)
        {
            ["curves"] = ArtifactStampManifest.ComputeUtf8Sha256Hex("mutated-curves-v2")
        };
        IReadOnlyList<StageStampEntry> mutated = ArtifactStampManifest.BuildStageDag(seed, mutatedInputs);
        var expectedInvalidated = new HashSet<string>(ArtifactStampManifest.GetDownstreamClosure("curves"), StringComparer.Ordinal);

        Assert.Equal(11, baseline.Count);
        for (int i = 0; i < baseline.Count; i++)
        {
            string stage = baseline[i].StageName;
            bool changed = !string.Equals(baseline[i].StageSha256, mutated[i].StageSha256, StringComparison.Ordinal);
            Assert.Equal(expectedInvalidated.Contains(stage), changed);
        }
        Assert.DoesNotContain("config", expectedInvalidated);
        Assert.DoesNotContain("townfloors", expectedInvalidated);
        Assert.DoesNotContain("navbuild", expectedInvalidated);
        Assert.Contains("database", expectedInvalidated);
        Assert.Contains("navmesh", expectedInvalidated);
    }

    [Theory]
    [InlineData(VerifyStandinCommand.CatalogFileName)]
    [InlineData(VerifyStandinCommand.VmapFileName)]
    [InlineData(VerifyStandinCommand.NavFileName)]
    [InlineData(VerifyStandinCommand.PmTilesFileName)]
    public void ArtifactStampManifest_ByteMutationOrTruncation_RejectsTamperedArtifacts(string fileName)
    {
        string tempDir = CreateTempDir();
        try
        {
            ArtifactStampManifest manifest = VerifyStandinCommand.GenerateStandInArtifacts(tempDir);
            string targetFile = Path.Combine(tempDir, fileName);
            byte[] original = File.ReadAllBytes(targetFile);

            byte[] mutated = [.. original];
            mutated[mutated.Length / 2] ^= 0x5A;
            File.WriteAllBytes(targetFile, mutated);
            Assert.Throws<InvalidDataException>(() => ArtifactStampManifest.VerifyManifest(manifest, tempDir));
            Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyStandInDirectory(tempDir));

            File.WriteAllBytes(targetFile, original.AsSpan(0, original.Length - 1).ToArray());
            Assert.Throws<InvalidDataException>(() => ArtifactStampManifest.VerifyManifest(manifest, tempDir));
        }
        finally
        {
            CleanupTempDir(tempDir);
        }
    }

    [Fact]
    public void VmapAndNavOracles_CorruptMagicHeaderDigestOrClimb_ThrowInvalidDataException()
    {
        using var vmapMs = new MemoryStream();
        ExtractVmapCommand.WriteVmapArchive(vmapMs, ExtractVmapCommand.CreateBaselineArchive());
        byte[] validVmap = vmapMs.ToArray();

        byte[] badVmapMagic = [.. validVmap];
        badVmapMagic[0] = 0x00;
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyVmapArtifact(badVmapMagic));

        byte[] badVmapSha = [.. validVmap];
        badVmapSha[40] ^= 0xFF;
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyVmapArtifact(badVmapSha));

        byte[] badVmapCrc = [.. validVmap];
        badVmapCrc[^1] ^= 0xFF;
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyVmapArtifact(badVmapCrc));

        using var navMs = new MemoryStream();
        ExtractNavCommand.WriteNavArchive(navMs, ExtractNavCommand.CreateBaselineArchive());
        byte[] validNav = navMs.ToArray();

        byte[] badNavClimb = [.. validNav];
        BinaryPrimitives.WriteSingleLittleEndian(badNavClimb.AsSpan(0x14, 4), ExtractNavCommand.LegacyWalkableClimbCm);
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyNavArtifact(badNavClimb));

        byte[] badNavFlags = [.. validNav];
        BinaryPrimitives.WriteUInt32LittleEndian(badNavFlags.AsSpan(0x0C, 4), ExtractNavCommand.FlagHasBridgeSpans);
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyNavArtifact(badNavFlags));

        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyNavArtifact(validNav.AsSpan(0, validNav.Length - 2)));
    }

    [Fact]
    public void PmTilesAndCatalogOracles_InvalidTileCountOrCorruptCatalog_ThrowInvalidDataException()
    {
        IReadOnlyList<PmTilesInputTile> smallPyramid = TilePyramidBuilder.BuildPyramid(maxZoom: 2);
        using var pmMs = new MemoryStream();
        PmTilesV3Writer.WriteArchive(pmMs, smallPyramid, minZoom: 0, maxZoom: 2);
        byte[] smallPmTiles = pmMs.ToArray();
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyPmTilesArtifact(smallPmTiles));

        byte[] badPmMagic = [.. smallPmTiles];
        badPmMagic[0] = (byte)'X';
        Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyPmTilesArtifact(badPmMagic));

        string tempDir = CreateTempDir();
        try
        {
            string catalogPath = Path.Combine(tempDir, VerifyStandinCommand.CatalogFileName);
            DesignDataCatalogSnapshot baseline = DesignDataCatalogExporter.CreateBaselineSnapshot();
            DesignDataCatalogSnapshot missingHostile = baseline with
            {
                WorldSpawners = [baseline.WorldSpawners[0], baseline.WorldSpawners[0] with { SpawnerId = "dup_humanoid" }]
            };
            DesignDataCatalogExporter.ExportToJson(missingHostile, catalogPath);
            Assert.Throws<InvalidDataException>(() => VerifyStandinCommand.VerifyCatalogArtifact(catalogPath));
        }
        finally
        {
            CleanupTempDir(tempDir);
        }
    }

    [Fact]
    public async Task ExtractorProgramSelfTests_AllSixteenSuites_PassAsync()
    {
        await Program.RunAllSelfTests();
    }

    private static string CreateTempDir()
    {
        string path = Path.Combine(Path.GetTempPath(), $"sandal_w075_test_{Guid.NewGuid():N}");
        Directory.CreateDirectory(path);
        return path;
    }

    private static void CleanupTempDir(string path)
    {
        if (Directory.Exists(path))
            Directory.Delete(path, recursive: true);
    }
}
