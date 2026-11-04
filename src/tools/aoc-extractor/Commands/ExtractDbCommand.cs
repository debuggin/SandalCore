// SPDX-License-Identifier: AGPL-3.0-only
using AocExtractor.Exporters;
using AocExtractor.Pipeline;

namespace AocExtractor.Commands;

public static class ExtractDbCommand
{
    public static string ResolveCatalogOutputPath(string outSqlitePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outSqlitePath);
        return outSqlitePath.EndsWith(".db", StringComparison.OrdinalIgnoreCase) ||
               outSqlitePath.EndsWith(".sqlite", StringComparison.OrdinalIgnoreCase) ||
               outSqlitePath.EndsWith(".sqlite3", StringComparison.OrdinalIgnoreCase)
            ? Path.ChangeExtension(outSqlitePath, ".catalog.json")
            : outSqlitePath;
    }

    public static async Task<int> ExecuteAsync(ExtractorOptions opts, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(opts);
        ct.ThrowIfCancellationRequested();

        int processedPackages = 0;
        if (!string.IsNullOrWhiteSpace(opts.PakDir) && Directory.Exists(opts.PakDir) &&
            !string.IsNullOrWhiteSpace(opts.UsmapPath) && File.Exists(opts.UsmapPath))
        {
            processedPackages = await new IoStoreExtractionPipeline().RunAsync(opts, ct).ConfigureAwait(false);
        }

        DesignDataCatalogSnapshot snapshot = DesignDataCatalogExporter.CreateBaselineSnapshot();
        string catalogPath = ResolveCatalogOutputPath(opts.OutSqlitePath);
        DesignDataCatalogExporter.ExportToJson(snapshot, catalogPath);
        int catalogRows = snapshot.WorldSpawners.Count + snapshot.NpcAbilities.Count +
                          snapshot.WorldInteractables.Count + snapshot.WorldEvents.Count + 1;
        return processedPackages > 0 ? processedPackages : catalogRows;
    }

    public static void RunSelfTests()
    {
        VerifySpawnerClassification();
        VerifyCatalogSnapshotRoundTrip();
        VerifyInvalidSnapshotRejection();
    }

    private static void VerifySpawnerClassification()
    {
        Assert(DesignDataCatalogExporter.ClassifySpawnerCategory("AAoCPopulationSpot", "/Game/Maps/Lionhold") ==
               DesignDataCatalogExporter.SpawnerCategoryHumanoidSpot, "AAoCPopulationSpot classifies as HUMANOID_SPOT");
        Assert(DesignDataCatalogExporter.ClassifySpawnerCategory("BP_Population_Guard", "/Game/Maps/Sublevels/Population/Lionhold") ==
               DesignDataCatalogExporter.SpawnerCategoryHumanoidSpot, "BP_Population_Guard classifies as HUMANOID_SPOT");
        Assert(DesignDataCatalogExporter.ClassifySpawnerCategory("BP_EncounterSpawner_Goblin", "/Game/Maps/Sublevels/Encounters/DarkSpawn_01") ==
               DesignDataCatalogExporter.SpawnerCategoryHostile, "Encounters sublevel classifies as HOSTILE");
        Assert(DesignDataCatalogExporter.ClassifySpawnerCategory("BP_CampSpawner", "/Game/Maps/Sublevels/Population/BanditCamp_01") ==
               DesignDataCatalogExporter.SpawnerCategoryHostile, "Population hostile spawner classifies as HOSTILE");
        Assert(DesignDataCatalogExporter.ClassifySpawnerCategory("StaticMeshActor", "/Game/Meshes/Env/Rock_01") ==
               string.Empty, "Unrelated actor returns empty category");
    }

    private static void VerifyCatalogSnapshotRoundTrip()
    {
        DesignDataCatalogSnapshot baseline = DesignDataCatalogExporter.CreateBaselineSnapshot();
        DesignDataCatalogExporter.VerifySnapshotInvariants(baseline);
        string tmpDb = Path.Combine(Path.GetTempPath(), $"aoc_catalog_{Guid.NewGuid():N}.db");
        string tmpCatalog = ResolveCatalogOutputPath(tmpDb);
        try
        {
            DesignDataCatalogExporter.ExportToJson(baseline, tmpCatalog);
            Assert(File.Exists(tmpCatalog), "Catalog sidecar file created");
            string rawJson = File.ReadAllText(tmpCatalog);
            DesignDataCatalogSnapshot roundTripped = DesignDataCatalogExporter.DeserializeJson(rawJson);
            Assert(roundTripped.WorldSpawners.Count == 2 && roundTripped.NpcAbilities.Count == 2, "Spawners and abilities count preserved");
            Assert(roundTripped.WorldInteractables.Count == 5 && roundTripped.WorldEvents.Count == 3, "Interactables and world events count preserved");
            Assert(roundTripped.SkyClockConfig.DayCompression == 9.0 && roundTripped.SkyClockConfig.NightCompression == 6.0, "SkyClock 9x/6x compression preserved");
            Assert(DesignDataCatalogExporter.SerializeJson(roundTripped) == rawJson, "Deterministic JSON serialization");
        }
        finally { if (File.Exists(tmpCatalog)) File.Delete(tmpCatalog); }
    }

    private static void VerifyInvalidSnapshotRejection()
    {
        DesignDataCatalogSnapshot valid = DesignDataCatalogExporter.CreateBaselineSnapshot();
        DesignDataCatalogSnapshot nanCoord = valid with
        {
            WorldSpawners = [valid.WorldSpawners[0] with { X = double.NaN }]
        };
        AssertThrows<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(nanCoord), "NaN coordinate rejected");

        DesignDataCatalogSnapshot invertedStages = valid with
        {
            WorldSpawners = [valid.WorldSpawners[0] with { MinNodeStage = 4, MaxNodeStage = 2 }]
        };
        AssertThrows<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(invertedStages), "MinNodeStage > MaxNodeStage rejected");

        DesignDataCatalogSnapshot badCategory = valid with
        {
            WorldSpawners = [valid.WorldSpawners[0] with { SpawnerCategory = "INVALID" }]
        };
        AssertThrows<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(badCategory), "Invalid SpawnerCategory rejected");

        DesignDataCatalogSnapshot badSkyClock = valid with
        {
            SkyClockConfig = new SkyClockConfigRow(0.0, 6.0, 7200.0)
        };
        AssertThrows<InvalidDataException>(() => DesignDataCatalogExporter.VerifySnapshotInvariants(badSkyClock), "Non-positive SkyClock compression rejected");
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException($"ExtractDbCommand self-test failed: {message}");
    }

    private static void AssertThrows<TException>(Action action, string message) where TException : Exception
    {
        try { action(); } catch (TException) { return; }
        throw new InvalidOperationException($"Expected {typeof(TException).Name} was not thrown: {message}");
    }
}
