// SPDX-License-Identifier: AGPL-3.0-only
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace AocExtractor.Pipeline;

public sealed record StageStampEntry(
    string StageName,
    IReadOnlyList<string> UpstreamStages,
    string InputSha256,
    string StageSha256);

public sealed record ArtifactStampEntry(
    string ArtifactId,
    string FormatMagic,
    string RelativePath,
    uint SchemaVersion,
    ulong RecordOrTileCount)
{
    public long ByteLength { get; init; }
    public string InputSha256 { get; init; } = string.Empty;
    public string OutputSha256 { get; init; } = string.Empty;
}

public sealed record ArtifactStampManifest(
    uint ManifestVersion,
    string GeneratorVersion,
    string InputFingerprintSha256,
    IReadOnlyList<StageStampEntry> Stages,
    IReadOnlyList<ArtifactStampEntry> Artifacts)
{
    public const uint CurrentManifestVersion = 1u;
    public const string CurrentGeneratorVersion = "aoc-extractor/0.7.5";
    public const string DefaultManifestFileName = "artifact_stamps.json";
    public const string ArtifactDbId = "content.db";
    public const string ArtifactVmapId = "verra.vmap";
    public const string ArtifactNavId = "verra.nav";
    public const string ArtifactPmTilesId = "verra.pmtiles";
    public const string MagicDb = "AOCDB01";
    public const string MagicVmap = "AOCVMAP1";
    public const string MagicNav = "AOCNAV02";
    public const string MagicPmTiles = "PMTiles_v3";

    public string ManifestDigestSha256 { get; init; } = string.Empty;

    public static IReadOnlyList<string> StageOrder { get; } =
    [
        "config", "curves", "townfloors", "navbuild", "database",
        "region", "headers", "fixtures", "roads", "townlayouts", "navmesh"
    ];

    private static readonly Dictionary<string, string[]> StageDeps = new(StringComparer.Ordinal)
    {
        ["config"] = [],
        ["curves"] = ["config"],
        ["townfloors"] = ["config"],
        ["navbuild"] = ["config"],
        ["database"] = ["config", "curves"],
        ["region"] = ["config", "database"],
        ["headers"] = ["config", "database", "region"],
        ["fixtures"] = ["config", "database", "region"],
        ["roads"] = ["config", "region"],
        ["townlayouts"] = ["config", "townfloors", "region"],
        ["navmesh"] = ["config", "navbuild", "region", "roads", "townlayouts"]
    };

    public static IReadOnlyList<string> GetUpstreamStages(string stageName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(stageName);
        if (!StageDeps.TryGetValue(stageName, out string[]? deps))
            throw new ArgumentException($"Unknown extraction stage '{stageName}'.", nameof(stageName));
        return deps;
    }

    public static IReadOnlyList<string> GetDownstreamClosure(string mutatedStage)
    {
        _ = GetUpstreamStages(mutatedStage);
        var affected = new HashSet<string>(StringComparer.Ordinal) { mutatedStage };
        var ordered = new List<string>();
        foreach (string stage in StageOrder)
        {
            if (affected.Contains(stage) || StageDeps[stage].Any(affected.Contains))
            {
                affected.Add(stage);
                ordered.Add(stage);
            }
        }
        return ordered;
    }

    public static string ComputeSha256Hex(ReadOnlySpan<byte> data)
    {
        Span<byte> hash = stackalloc byte[32];
        SHA256.HashData(data, hash);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public static string ComputeUtf8Sha256Hex(string text)
    {
        ArgumentNullException.ThrowIfNull(text);
        return ComputeSha256Hex(Encoding.UTF8.GetBytes(text));
    }

    public static string ComputeFileSha256Hex(string filePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        if (!File.Exists(filePath))
            throw new FileNotFoundException($"Artifact file not found for SHA-256 stamp: {filePath}", filePath);
        using FileStream fs = File.OpenRead(filePath);
        Span<byte> hash = stackalloc byte[32];
        SHA256.HashData(fs, hash);
        return Convert.ToHexString(hash).ToLowerInvariant();
    }

    public static IReadOnlyList<StageStampEntry> BuildStageDag(
        string inputFingerprintSha256,
        IReadOnlyDictionary<string, string>? stageInputs = null)
    {
        ValidateHexSha256(inputFingerprintSha256, nameof(inputFingerprintSha256));
        var resolved = new Dictionary<string, string>(StringComparer.Ordinal);
        var entries = new List<StageStampEntry>(StageOrder.Count);
        foreach (string stage in StageOrder)
        {
            string[] deps = StageDeps[stage];
            string rawSeed = stageInputs is not null && stageInputs.TryGetValue(stage, out string? s) && !string.IsNullOrEmpty(s)
                ? s : $"{inputFingerprintSha256}:{stage}";
            string inputHash = IsHexSha256(rawSeed) ? rawSeed.ToLowerInvariant() : ComputeUtf8Sha256Hex(rawSeed);
            var sb = new StringBuilder();
            sb.Append(stage).Append('|').Append(inputHash);
            foreach (string dep in deps)
                sb.Append('|').Append(dep).Append('=').Append(resolved[dep]);
            string stageHash = ComputeUtf8Sha256Hex(sb.ToString());
            resolved[stage] = stageHash;
            entries.Add(new StageStampEntry(stage, deps, inputHash, stageHash));
        }
        return entries;
    }

    public static ArtifactStampManifest Create(
        string inputFingerprintSha256,
        IReadOnlyList<ArtifactStampEntry> artifacts,
        IReadOnlyDictionary<string, string>? stageInputs = null)
    {
        ArgumentNullException.ThrowIfNull(artifacts);
        ValidateHexSha256(inputFingerprintSha256, nameof(inputFingerprintSha256));
        if (artifacts.Count == 0)
            throw new InvalidDataException("ArtifactStampManifest requires at least one artifact entry.");

        string normalizedInput = inputFingerprintSha256.ToLowerInvariant();
        IReadOnlyList<StageStampEntry> stages = BuildStageDag(normalizedInput, stageInputs);
        string digest = ComputeManifestDigest(CurrentManifestVersion, CurrentGeneratorVersion, normalizedInput, stages, artifacts);
        return new ArtifactStampManifest(CurrentManifestVersion, CurrentGeneratorVersion, normalizedInput, stages, artifacts)
        {
            ManifestDigestSha256 = digest
        };
    }

    public static ArtifactStampEntry CreateEntryFromFile(
        string baseDirectory,
        string relativePath,
        string artifactId,
        string formatMagic,
        ulong recordOrTileCount)
    {
        string fullPath = ResolveSafeRelativePath(baseDirectory, relativePath);
        if (!File.Exists(fullPath))
            throw new InvalidDataException($"Required artifact '{artifactId}' missing at '{fullPath}'.");
        var info = new FileInfo(fullPath);
        if (info.Length <= 0 || recordOrTileCount == 0UL)
            throw new InvalidDataException($"Artifact '{artifactId}' has empty byte length or zero record/tile count.");
        uint schemaVersion = artifactId switch
        {
            ArtifactDbId => 1u,
            ArtifactVmapId => 1u,
            ArtifactNavId => 2u,
            ArtifactPmTilesId => 3u,
            _ => 1u
        };
        string outputSha = ComputeFileSha256Hex(fullPath);
        string inputSha = ComputeUtf8Sha256Hex($"{artifactId}:{formatMagic}:{schemaVersion}:{recordOrTileCount}");
        return new ArtifactStampEntry(artifactId, formatMagic, relativePath, schemaVersion, recordOrTileCount)
        {
            ByteLength = info.Length,
            InputSha256 = inputSha,
            OutputSha256 = outputSha
        };
    }

    public static string SerializeJson(ArtifactStampManifest manifest)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        return JsonSerializer.Serialize(manifest, ArtifactStampJsonContext.Default.ArtifactStampManifest);
    }

    public static ArtifactStampManifest DeserializeJson(string json)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(json);
        return JsonSerializer.Deserialize(json, ArtifactStampJsonContext.Default.ArtifactStampManifest)
            ?? throw new InvalidDataException("Failed to deserialize ArtifactStampManifest JSON.");
    }

    public static void SaveToFile(ArtifactStampManifest manifest, string filePath)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        string? dir = Path.GetDirectoryName(Path.GetFullPath(filePath));
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(filePath, SerializeJson(manifest), new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
    }

    public static ArtifactStampManifest LoadFromFile(string filePath)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(filePath);
        if (!File.Exists(filePath))
            throw new InvalidDataException($"Artifact stamp manifest not found: {filePath}");
        return DeserializeJson(File.ReadAllText(filePath, Encoding.UTF8));
    }

    public static void VerifyManifest(ArtifactStampManifest manifest, string baseDirectory)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        ArgumentException.ThrowIfNullOrWhiteSpace(baseDirectory);
        if (manifest.ManifestVersion != CurrentManifestVersion)
            throw new InvalidDataException($"Unsupported manifest version {manifest.ManifestVersion}.");
        ValidateHexSha256(manifest.InputFingerprintSha256, nameof(manifest.InputFingerprintSha256));
        VerifyStageDag(manifest);
        if (manifest.Artifacts is null || manifest.Artifacts.Count < 4)
            throw new InvalidDataException("ArtifactStampManifest must contain all 4 world-data artifacts.");

        string expectedDigest = ComputeManifestDigest(
            manifest.ManifestVersion, manifest.GeneratorVersion, manifest.InputFingerprintSha256, manifest.Stages, manifest.Artifacts);
        if (!string.Equals(expectedDigest, manifest.ManifestDigestSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException("ArtifactStampManifest top-level ManifestDigestSha256 mismatch.");

        foreach (ArtifactStampEntry entry in manifest.Artifacts)
            VerifyArtifactEntry(entry, baseDirectory);
    }

    private static void VerifyStageDag(ArtifactStampManifest manifest)
    {
        if (manifest.Stages is null || manifest.Stages.Count != StageOrder.Count)
            throw new InvalidDataException($"Expected {StageOrder.Count} pipeline stages in manifest.");
        var stageInputs = new Dictionary<string, string>(StringComparer.Ordinal);
        for (int i = 0; i < StageOrder.Count; i++)
        {
            StageStampEntry s = manifest.Stages[i];
            if (!string.Equals(s.StageName, StageOrder[i], StringComparison.Ordinal))
                throw new InvalidDataException($"Stage order mismatch at index {i}: got '{s.StageName}', expected '{StageOrder[i]}'.");
            ValidateHexSha256(s.InputSha256, s.StageName);
            ValidateHexSha256(s.StageSha256, s.StageName);
            stageInputs[s.StageName] = s.InputSha256;
        }
        IReadOnlyList<StageStampEntry> recomputed = BuildStageDag(manifest.InputFingerprintSha256, stageInputs);
        for (int i = 0; i < recomputed.Count; i++)
        {
            if (!string.Equals(recomputed[i].StageSha256, manifest.Stages[i].StageSha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Stage '{manifest.Stages[i].StageName}' SHA-256 chain mismatch.");
        }
    }

    private static void VerifyArtifactEntry(ArtifactStampEntry entry, string baseDirectory)
    {
        if (string.IsNullOrWhiteSpace(entry.ArtifactId) || string.IsNullOrWhiteSpace(entry.FormatMagic) ||
            entry.ByteLength <= 0 || entry.RecordOrTileCount == 0UL)
        {
            throw new InvalidDataException($"Invalid stamp metadata for artifact '{entry.ArtifactId}'.");
        }
        ValidateHexSha256(entry.InputSha256, entry.ArtifactId);
        ValidateHexSha256(entry.OutputSha256, entry.ArtifactId);
        string fullPath = ResolveSafeRelativePath(baseDirectory, entry.RelativePath);
        if (!File.Exists(fullPath))
            throw new InvalidDataException($"Stamped artifact '{entry.ArtifactId}' is missing on disk: {fullPath}");
        var info = new FileInfo(fullPath);
        if (info.Length != entry.ByteLength)
            throw new InvalidDataException($"ByteLength mismatch for '{entry.ArtifactId}': expected {entry.ByteLength}, got {info.Length}.");
        string actualSha = ComputeFileSha256Hex(fullPath);
        if (!string.Equals(actualSha, entry.OutputSha256, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"SHA-256 digest mismatch for '{entry.ArtifactId}': expected {entry.OutputSha256}, got {actualSha}.");
    }

    private static string ComputeManifestDigest(
        uint version,
        string generator,
        string inputSha,
        IReadOnlyList<StageStampEntry> stages,
        IReadOnlyList<ArtifactStampEntry> artifacts)
    {
        var sb = new StringBuilder();
        sb.Append(version).Append('|').Append(generator).Append('|').Append(inputSha.ToLowerInvariant());
        foreach (StageStampEntry s in stages)
            sb.Append("|S:").Append(s.StageName).Append(':').Append(s.StageSha256.ToLowerInvariant());
        foreach (ArtifactStampEntry a in artifacts)
            sb.Append("|A:").Append(a.ArtifactId).Append(':').Append(a.ByteLength).Append(':').Append(a.OutputSha256.ToLowerInvariant());
        return ComputeUtf8Sha256Hex(sb.ToString());
    }

    private static string ResolveSafeRelativePath(string baseDirectory, string relativePath)
    {
        if (string.IsNullOrWhiteSpace(relativePath) || Path.IsPathRooted(relativePath))
            throw new InvalidDataException($"Artifact path '{relativePath}' must be a non-empty relative path.");
        string baseFull = Path.GetFullPath(baseDirectory);
        string combinedFull = Path.GetFullPath(Path.Combine(baseFull, relativePath));
        string prefix = baseFull.EndsWith(Path.DirectorySeparatorChar) ? baseFull : baseFull + Path.DirectorySeparatorChar;
        if (!combinedFull.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"Artifact path '{relativePath}' escapes base directory.");
        return combinedFull;
    }

    public static bool IsHexSha256(string? value)
    {
        if (value is null || value.Length != 64) return false;
        for (int i = 0; i < 64; i++)
        {
            char c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
        return true;
    }

    private static void ValidateHexSha256(string? value, string name)
    {
        if (!IsHexSha256(value))
            throw new InvalidDataException($"Invalid 64-char SHA-256 hex digest for '{name}'.");
    }

    public static void RunSelfTests()
    {
        string seed = ComputeUtf8Sha256Hex("sandalcore-07.5-seed");
        IReadOnlyList<StageStampEntry> dag1 = BuildStageDag(seed);
        IReadOnlyList<StageStampEntry> dag2 = BuildStageDag(seed);
        if (dag1.Count != 11 || !dag1.Select(static s => s.StageSha256).SequenceEqual(dag2.Select(static s => s.StageSha256)))
            throw new InvalidOperationException("ArtifactStampManifest 11-stage DAG is not deterministic.");

        IReadOnlyList<string> curvesDownstream = GetDownstreamClosure("curves");
        if (!curvesDownstream.Contains("database") || !curvesDownstream.Contains("navmesh") || curvesDownstream.Contains("townfloors"))
            throw new InvalidOperationException("ArtifactStampManifest DAG downstream closure mismatch for 'curves'.");
    }
}

[JsonSourceGenerationOptions(WriteIndented = true, PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower)]
[JsonSerializable(typeof(ArtifactStampManifest))]
[JsonSerializable(typeof(StageStampEntry))]
[JsonSerializable(typeof(ArtifactStampEntry))]
internal partial class ArtifactStampJsonContext : JsonSerializerContext
{
}
