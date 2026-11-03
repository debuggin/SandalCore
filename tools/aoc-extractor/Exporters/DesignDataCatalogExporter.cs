// SPDX-License-Identifier: AGPL-3.0-only
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AocExtractor.Exporters;

public sealed record WorldSpawnerRow(
    string SpawnerId,
    string SpawnerCategory,
    string ActorClass,
    string Sublevel,
    long NpcGuid,
    double X,
    double Y,
    double Z,
    double Radius,
    int MinNodeStage,
    int MaxNodeStage,
    int TimeOfDayMask);

public sealed record NpcAbilityRow(
    long NpcGuid,
    long AbilityGuid,
    string AbilityName,
    string Source);

public sealed record WorldInteractableRow(
    string InteractableId,
    string ActorClass,
    string InteractableType,
    string Sublevel,
    double X,
    double Y,
    double Z,
    double Radius);

public sealed record SkyClockConfigRow(
    double DayCompression,
    double NightCompression,
    double CycleLengthSec);

public sealed record WorldEventDefRow(
    long EventGuid,
    string Name,
    int MinNodeStage,
    int MaxNodeStage,
    string StateGraphJson);

public sealed record DesignDataCatalogSnapshot(
    IReadOnlyList<WorldSpawnerRow> WorldSpawners,
    IReadOnlyList<NpcAbilityRow> NpcAbilities,
    IReadOnlyList<WorldInteractableRow> WorldInteractables,
    SkyClockConfigRow SkyClockConfig,
    IReadOnlyList<WorldEventDefRow> WorldEvents);

[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
[JsonSerializable(typeof(DesignDataCatalogSnapshot))]
internal sealed partial class DesignDataCatalogJsonContext : JsonSerializerContext;

public static class DesignDataCatalogExporter
{
    public const string SpawnerCategoryHumanoidSpot = "HUMANOID_SPOT";
    public const string SpawnerCategoryHostile = "HOSTILE";
    public const double DefaultDayCompression = 9.0;
    public const double DefaultNightCompression = 6.0;
    public const double DefaultCycleLengthSec = 7200.0;

    public static string ClassifySpawnerCategory(string actorClass, string packagePath)
    {
        string cls = actorClass?.Trim() ?? string.Empty;
        string pkg = (packagePath?.Trim() ?? string.Empty).Replace('\\', '/');
        if (cls.Equals("AAoCPopulationSpot", StringComparison.OrdinalIgnoreCase) ||
            cls.Contains("PopulationSpot", StringComparison.OrdinalIgnoreCase) ||
            cls.StartsWith("BP_Population_", StringComparison.OrdinalIgnoreCase) ||
            cls.Contains("/BP_Population_", StringComparison.OrdinalIgnoreCase) ||
            pkg.Contains("/BP_Population_", StringComparison.OrdinalIgnoreCase))
        {
            return SpawnerCategoryHumanoidSpot;
        }
        if (pkg.Contains("/Sublevels/Encounters/", StringComparison.OrdinalIgnoreCase) ||
            pkg.Contains("/Population/", StringComparison.OrdinalIgnoreCase) ||
            cls.Contains("EncounterSpawner", StringComparison.OrdinalIgnoreCase) ||
            cls.Contains("HostileSpawner", StringComparison.OrdinalIgnoreCase))
        {
            return SpawnerCategoryHostile;
        }
        return string.Empty;
    }

    public static void VerifySnapshotInvariants(DesignDataCatalogSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        if (snapshot.WorldSpawners is null || snapshot.NpcAbilities is null ||
            snapshot.WorldInteractables is null || snapshot.SkyClockConfig is null || snapshot.WorldEvents is null)
            throw new InvalidDataException("Catalog snapshot collections and sky clock configuration must be non-null.");

        foreach (WorldSpawnerRow spawner in snapshot.WorldSpawners)
        {
            if (string.IsNullOrWhiteSpace(spawner.SpawnerId) || string.IsNullOrWhiteSpace(spawner.ActorClass))
                throw new InvalidDataException("WorldSpawnerRow requires non-empty SpawnerId and ActorClass.");
            if (spawner.SpawnerCategory is not (SpawnerCategoryHumanoidSpot or SpawnerCategoryHostile))
                throw new InvalidDataException($"Invalid SpawnerCategory '{spawner.SpawnerCategory}' on '{spawner.SpawnerId}'.");
            if (!IsFinite(spawner.X) || !IsFinite(spawner.Y) || !IsFinite(spawner.Z) || !IsFinite(spawner.Radius) || spawner.Radius < 0.0)
                throw new InvalidDataException($"Non-finite coordinate or negative radius on spawner '{spawner.SpawnerId}'.");
            if (spawner.MinNodeStage < 0 || spawner.MinNodeStage > spawner.MaxNodeStage)
                throw new InvalidDataException($"Invalid node stage bounds [{spawner.MinNodeStage}, {spawner.MaxNodeStage}] on '{spawner.SpawnerId}'.");
        }
        foreach (NpcAbilityRow ability in snapshot.NpcAbilities)
        {
            if (ability.NpcGuid <= 0 || ability.AbilityGuid <= 0 || string.IsNullOrWhiteSpace(ability.AbilityName) || string.IsNullOrWhiteSpace(ability.Source))
                throw new InvalidDataException("NpcAbilityRow requires positive GUIDs and non-empty AbilityName/Source.");
        }
        foreach (WorldInteractableRow inter in snapshot.WorldInteractables)
        {
            if (string.IsNullOrWhiteSpace(inter.InteractableId) || string.IsNullOrWhiteSpace(inter.InteractableType))
                throw new InvalidDataException("WorldInteractableRow requires non-empty InteractableId and InteractableType.");
            if (!IsFinite(inter.X) || !IsFinite(inter.Y) || !IsFinite(inter.Z) || !IsFinite(inter.Radius) || inter.Radius < 0.0)
                throw new InvalidDataException($"Non-finite coordinate or negative radius on interactable '{inter.InteractableId}'.");
        }
        VerifySkyClockAndEvents(snapshot.SkyClockConfig, snapshot.WorldEvents);
    }

    public static string SerializeJson(DesignDataCatalogSnapshot snapshot)
    {
        VerifySnapshotInvariants(snapshot);
        return JsonSerializer.Serialize(snapshot, DesignDataCatalogJsonContext.Default.DesignDataCatalogSnapshot);
    }

    public static DesignDataCatalogSnapshot DeserializeJson(string json)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(json);
        DesignDataCatalogSnapshot? snapshot = JsonSerializer.Deserialize(json, DesignDataCatalogJsonContext.Default.DesignDataCatalogSnapshot)
            ?? throw new InvalidDataException("Deserialized DesignDataCatalogSnapshot was null.");
        VerifySnapshotInvariants(snapshot);
        return snapshot;
    }

    public static void ExportToJson(DesignDataCatalogSnapshot snapshot, string outputPath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputPath);
        string json = SerializeJson(snapshot);
        string? dir = Path.GetDirectoryName(Path.GetFullPath(outputPath));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(outputPath, json, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    public static DesignDataCatalogSnapshot CreateBaselineSnapshot() => new(
        [
            new WorldSpawnerRow("spawner.humanoid.lionhold_guard_01", ClassifySpawnerCategory("BP_Population_Guard", "/Game/Maps/Sublevels/Population/Lionhold"), "BP_Population_Guard", "/Game/Maps/Sublevels/Population/Lionhold", 100001L, 12450.5, -8320.25, 412.0, 250.0, 1, 5, 3),
            new WorldSpawnerRow("spawner.hostile.darkspawn_01", ClassifySpawnerCategory("BP_EncounterSpawner_Goblin", "/Game/Maps/Sublevels/Encounters/DarkSpawn_01"), "BP_EncounterSpawner_Goblin", "/Game/Maps/Sublevels/Encounters/DarkSpawn_01", 200001L, 18920.0, -14200.75, 620.5, 600.0, 0, 5, 2)
        ],
        [
            new NpcAbilityRow(100001L, 500101L, "Guard_ShieldBash", "Montage"),
            new NpcAbilityRow(200001L, 500201L, "Goblin_Cleave", "BehaviorTree")
        ],
        [
            new WorldInteractableRow("inter.foliage.oak_01", "UInstancedFoliageActor", "RESOURCE_NODE", "/Game/Maps/Sublevels/Foliage/Riverlands", 10120.0, -4500.0, 310.0, 120.0),
            new WorldInteractableRow("inter.station.forge_01", "BP_Interactive_Forge", "CRAFTING_STATION", "/Game/Maps/Sublevels/Lionhold_Bld", 12500.0, -8200.0, 415.0, 180.0),
            new WorldInteractableRow("inter.chest.sanctum_01", "BP_Chest_Ancient", "LOOT_CONTAINER", "/Game/Maps/Sublevels/Dungeons/Sanctum_01", 22100.0, -19400.0, 110.0, 150.0),
            new WorldInteractableRow("inter.door.sanctum_gate_01", "BP_Door_SanctumGate", "DOOR", "/Game/Maps/Sublevels/Dungeons/Sanctum_01", 22050.0, -19350.0, 110.0, 200.0),
            new WorldInteractableRow("inter.altar.totem_01", "BP_Totem_Shrine", "SHRINE", "/Game/Maps/Sublevels/Encounters/DarkSpawn_01", 18950.0, -14250.0, 622.0, 220.0)
        ],
        new SkyClockConfigRow(DefaultDayCompression, DefaultNightCompression, DefaultCycleLengthSec),
        [
            new WorldEventDefRow(700001L, "Event_Riverlands_CaravanAmbush", 1, 5, "{\"initial\":\"Idle\",\"states\":[\"Idle\",\"Active\",\"Resolved\"]}"),
            new WorldEventDefRow(700002L, "Event_NodeSiege_StageTransition", 3, 5, "{\"initial\":\"Prep\",\"states\":[\"Prep\",\"Siege\",\"Rebuild\"]}"),
            new WorldEventDefRow(700003L, "Event_RiftIncursion_Corrupted", 2, 5, "{\"initial\":\"Dormant\",\"states\":[\"Dormant\",\"RiftOpen\",\"BossPhase\",\"Cleared\"]}")
        ]);

    private static void VerifySkyClockAndEvents(SkyClockConfigRow sky, IReadOnlyList<WorldEventDefRow> events)
    {
        if (!IsFinite(sky.DayCompression) || !IsFinite(sky.NightCompression) || !IsFinite(sky.CycleLengthSec) ||
            sky.DayCompression <= 0.0 || sky.NightCompression <= 0.0 || sky.CycleLengthSec <= 0.0)
            throw new InvalidDataException("SkyClockConfig compression ratios and cycle length must be finite and positive.");

        foreach (WorldEventDefRow ev in events)
        {
            if (ev.EventGuid <= 0 || string.IsNullOrWhiteSpace(ev.Name) || string.IsNullOrWhiteSpace(ev.StateGraphJson))
                throw new InvalidDataException("WorldEventDefRow requires positive EventGuid, Name, and StateGraphJson.");
            if (ev.MinNodeStage < 0 || ev.MinNodeStage > ev.MaxNodeStage)
                throw new InvalidDataException($"Invalid node stage bounds [{ev.MinNodeStage}, {ev.MaxNodeStage}] on event '{ev.Name}'.");
        }
    }

    private static bool IsFinite(double value) => !double.IsNaN(value) && !double.IsInfinity(value);
}
