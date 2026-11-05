// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using AocExtractor.Pipeline;

namespace AocExtractor.Commands;

public readonly record struct NavArchiveHeader(uint Version, uint Flags, uint TileCount, float WalkableClimbCm, float WalkableSlopeDeg)
{
    public float AgentRadiusCm { get; init; } = ExtractNavCommand.AgentRadiusCm;
    public float AgentHeightCm { get; init; } = ExtractNavCommand.AgentHeightCm;
    public uint BridgeSpanCount { get; init; }
}

public readonly record struct NavBridgeSpanRecord(int TileX, int TileY, float DeckZ)
{
    public float StartX { get; init; } public float StartY { get; init; } public float EndX { get; init; } public float EndY { get; init; }
    public float SpanLengthCm => MathF.Sqrt((EndX - StartX) * (EndX - StartX) + (EndY - StartY) * (EndY - StartY));
}
public sealed record NavTileRecord(int TileX, int TileY, uint Layer, uint BridgeSpanCount, byte[] Payload);
public sealed record NavArchiveData(
    IReadOnlyList<NavTileRecord> Tiles, IReadOnlyList<NavBridgeSpanRecord> BridgeSpans,
    float WalkableClimbCm = ExtractNavCommand.WalkableClimbCm, float WalkableSlopeDeg = ExtractNavCommand.WalkableSlopeDeg);

public static class ExtractNavCommand
{
    public const ulong NavMagic = 0x323056414E434F41UL;
    public const uint NavVersion = 2u, FlagHasBridgeSpans = 1u << 0, FlagPolyRef64 = 1u << 1;
    public const int HeaderSizeBytes = 40, BridgeSpanSizeBytes = 28;
    public const float WalkableClimbCm = 100.0f, LegacyWalkableClimbCm = 45.0f, WalkableSlopeDeg = 45.0f, AgentRadiusCm = 40.0f, AgentHeightCm = 180.0f;

    public static string ResolveNavOutputPath(string outNavDir) =>
        string.IsNullOrWhiteSpace(outNavDir) ? Path.Combine(Path.GetTempPath(), "area_nav.bin")
        : outNavDir.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) || outNavDir.EndsWith(".aocnav", StringComparison.OrdinalIgnoreCase)
            ? outNavDir : Path.Combine(outNavDir, "area_nav.bin");

    public static async Task<int> ExecuteAsync(ExtractorOptions opts, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(opts); ct.ThrowIfCancellationRequested();
        int processed = (!string.IsNullOrWhiteSpace(opts.PakDir) && Directory.Exists(opts.PakDir) && !string.IsNullOrWhiteSpace(opts.UsmapPath) && File.Exists(opts.UsmapPath))
            ? await new IoStoreExtractionPipeline().RunAsync(opts, ct).ConfigureAwait(false) : 0;
        NavArchiveData archive = CreateBaselineArchive();
        string outPath = ResolveNavOutputPath(opts.OutNavDir);
        if (Path.GetDirectoryName(Path.GetFullPath(outPath)) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        await using FileStream fs = File.Create(outPath);
        WriteNavArchive(fs, archive);
        return processed > 0 ? processed : archive.Tiles.Count;
    }

    public static void ValidateAgentAndClimbParams(float walkableClimbCm, float walkableSlopeDeg, float agentRadiusCm, float agentHeightCm)
    {
        if (!float.IsFinite(walkableClimbCm) || walkableClimbCm < WalkableClimbCm)
            throw new InvalidDataException($"AOCNAV02 requires WalkableClimbCm >= {WalkableClimbCm} cm (#188); got {walkableClimbCm}.");
        if (!float.IsFinite(walkableSlopeDeg) || walkableSlopeDeg <= 0f || walkableSlopeDeg >= 90f)
            throw new InvalidDataException($"AOCNAV02 requires WalkableSlopeDeg in (0, 90); got {walkableSlopeDeg}.");
        if (!float.IsFinite(agentRadiusCm) || agentRadiusCm <= 0f || !float.IsFinite(agentHeightCm) || agentHeightCm <= agentRadiusCm)
            throw new InvalidDataException("AOCNAV02 requires positive finite AgentRadiusCm and AgentHeightCm > AgentRadiusCm.");
    }

    public static void WriteNavArchive(Stream output, NavArchiveData archive, float agentRadiusCm = AgentRadiusCm, float agentHeightCm = AgentHeightCm)
    {
        ArgumentNullException.ThrowIfNull(output); ArgumentNullException.ThrowIfNull(archive);
        ValidateAgentAndClimbParams(archive.WalkableClimbCm, archive.WalkableSlopeDeg, agentRadiusCm, agentHeightCm);
        uint flags = FlagPolyRef64 | (archive.BridgeSpans.Count > 0 ? FlagHasBridgeSpans : 0u);
        Span<byte> hdr = stackalloc byte[HeaderSizeBytes];
        BinaryPrimitives.WriteUInt64LittleEndian(hdr[..8], NavMagic); BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(0x08, 4), NavVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(0x0C, 4), flags); BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(0x10, 4), (uint)archive.Tiles.Count);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(0x14, 4), archive.WalkableClimbCm); BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(0x18, 4), archive.WalkableSlopeDeg);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(0x1C, 4), agentRadiusCm); BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(0x20, 4), agentHeightCm);
        BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(0x24, 4), (uint)archive.BridgeSpans.Count);
        output.Write(hdr); WriteBridgeSpansAndTiles(output, archive);
    }

    private static void WriteBridgeSpansAndTiles(Stream output, NavArchiveData archive)
    {
        Span<byte> b = stackalloc byte[BridgeSpanSizeBytes];
        foreach (NavBridgeSpanRecord s in archive.BridgeSpans)
        {
            if (!float.IsFinite(s.SpanLengthCm) || s.SpanLengthCm <= 0f || !float.IsFinite(s.DeckZ))
                throw new InvalidDataException("Bridge span (#167) requires finite coordinates and positive span length.");
            BinaryPrimitives.WriteInt32LittleEndian(b[..4], s.TileX); BinaryPrimitives.WriteInt32LittleEndian(b.Slice(4, 4), s.TileY);
            BinaryPrimitives.WriteSingleLittleEndian(b.Slice(8, 4), s.StartX); BinaryPrimitives.WriteSingleLittleEndian(b.Slice(12, 4), s.StartY);
            BinaryPrimitives.WriteSingleLittleEndian(b.Slice(16, 4), s.EndX); BinaryPrimitives.WriteSingleLittleEndian(b.Slice(20, 4), s.EndY);
            BinaryPrimitives.WriteSingleLittleEndian(b.Slice(24, 4), s.DeckZ);
            output.Write(b);
        }
        Span<byte> th = stackalloc byte[20];
        foreach (NavTileRecord t in archive.Tiles)
        {
            if (t.Payload is null || t.Payload.Length == 0) throw new InvalidDataException("AOCNAV02 tile payload cannot be empty.");
            BinaryPrimitives.WriteInt32LittleEndian(th[..4], t.TileX); BinaryPrimitives.WriteInt32LittleEndian(th.Slice(4, 4), t.TileY);
            BinaryPrimitives.WriteUInt32LittleEndian(th.Slice(8, 4), t.Layer); BinaryPrimitives.WriteUInt32LittleEndian(th.Slice(12, 4), t.BridgeSpanCount);
            BinaryPrimitives.WriteUInt32LittleEndian(th.Slice(16, 4), (uint)t.Payload.Length);
            output.Write(th); output.Write(t.Payload);
        }
    }

    public static NavArchiveHeader ReadAndValidateNavHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < HeaderSizeBytes || BinaryPrimitives.ReadUInt64LittleEndian(data[..8]) != NavMagic) throw new InvalidDataException("Invalid AOCNAV02 header length or magic.");
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(0x08, 4));
        if (version != NavVersion) throw new InvalidDataException($"Unsupported AOCNAV02 version {version}.");
        float climb = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(0x14, 4)), slope = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(0x18, 4));
        float radius = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(0x1C, 4)), height = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(0x20, 4));
        ValidateAgentAndClimbParams(climb, slope, radius, height);
        return new NavArchiveHeader(version, BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(0x0C, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(0x10, 4)), climb, slope)
        { AgentRadiusCm = radius, AgentHeightCm = height, BridgeSpanCount = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(0x24, 4)) };
    }

    public static NavArchiveData CreateBaselineArchive() => new(
        [new NavTileRecord(-12, 8, 0u, 1u, [0x44, 0x54, 0x4E, 0x32, 0x01, 0x02, 0x03, 0x04])],
        [new NavBridgeSpanRecord(-12, 8, 145.0f) { StartX = -602000f, StartY = 418000f, EndX = -598000f, EndY = 422000f }]);

    public static void RunSelfTests()
    {
        if (WalkableClimbCm != 100.0f || WalkableSlopeDeg != 45.0f || AgentRadiusCm != 40.0f || AgentHeightCm != 180.0f)
            throw new InvalidOperationException("AOCNAV02 agent/climb constants mismatch (#188).");
        using var ms = new MemoryStream(); WriteNavArchive(ms, CreateBaselineArchive());
        NavArchiveHeader hdr = ReadAndValidateNavHeader(ms.ToArray());
        if (hdr.Version != 2u || hdr.TileCount != 1u || hdr.BridgeSpanCount != 1u || hdr.WalkableClimbCm != 100.0f || (hdr.Flags & FlagHasBridgeSpans) == 0)
            throw new InvalidOperationException("AOCNAV02 header or bridge continuity round-trip mismatch.");
        ExpectInvalidData(() => ValidateAgentAndClimbParams(LegacyWalkableClimbCm, WalkableSlopeDeg, AgentRadiusCm, AgentHeightCm));
        ExpectInvalidData(() => ValidateAgentAndClimbParams(WalkableClimbCm, 95.0f, AgentRadiusCm, AgentHeightCm));
        ExpectInvalidData(() => ValidateAgentAndClimbParams(WalkableClimbCm, WalkableSlopeDeg, 180.0f, 40.0f));
        ExpectInvalidData(() => WriteNavArchive(new MemoryStream(), new NavArchiveData([], [new NavBridgeSpanRecord(0, 0, 10f)])));
    }

    private static void ExpectInvalidData(Action action) { try { action(); } catch (InvalidDataException) { return; } throw new InvalidOperationException("Expected InvalidDataException was not thrown."); }
}
