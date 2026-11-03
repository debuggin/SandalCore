// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.Security.Cryptography;
using AocExtractor.Pipeline;

namespace AocExtractor.Commands;

public readonly record struct VmapArchiveHeader(
    uint Version,
    uint TileCount,
    float WorldBminX,
    float WorldBminY,
    float WorldBminZ)
{
    public float WorldBmaxX { get; init; }
    public float WorldBmaxY { get; init; }
    public float WorldBmaxZ { get; init; }
    public byte[] ContentSha256 { get; init; } = new byte[32];
}

public readonly record struct VmapTileTocEntry(
    short TileX,
    short TileY,
    uint NodeCount,
    uint TriCount,
    ulong ByteOffset = 0UL)
{
    public ulong ByteLen { get; init; }
    public uint Crc32 { get; init; }
    public uint Reserved => Crc32;
}

public readonly record struct BvhNode32(
    short BminX,
    short BminY,
    short BminZ,
    short BmaxX,
    short BmaxY)
{
    public short BmaxZ { get; init; }
    public uint LeftOrFirstTri { get; init; }
    public ushort TriCount { get; init; }
    public ushort MaterialFlags { get; init; }
}

public readonly record struct VmapTriangle(
    float V0X,
    float V0Y,
    float V0Z,
    ushort MaterialFlags = ExtractVmapCommand.MatOpaqueLos)
{
    public float V1X { get; init; }
    public float V1Y { get; init; }
    public float V1Z { get; init; }
    public float V2X { get; init; }
    public float V2Y { get; init; }
    public float V2Z { get; init; }
    public short TileX { get; init; }
    public short TileY { get; init; }
}

public readonly record struct VmapVolumeRecord(
    float SurfaceZ,
    float DepthCm,
    byte VolumeKind,
    ushort MaterialFlags = ExtractVmapCommand.MatWater)
{
    public float MinX { get; init; }
    public float MinY { get; init; }
    public float MaxX { get; init; }
    public float MaxY { get; init; }
}

public sealed record VmapArchiveData(
    IReadOnlyList<VmapTileTocEntry> Tiles,
    IReadOnlyList<BvhNode32> Nodes,
    IReadOnlyList<VmapTriangle> Triangles,
    IReadOnlyList<VmapVolumeRecord> Volumes)
{
    public float WorldBminX { get; init; } = -1000.0f;
    public float WorldBminY { get; init; } = -1000.0f;
    public float WorldBminZ { get; init; } = -500.0f;
    public float WorldBmaxX { get; init; } = 1000.0f;
    public float WorldBmaxY { get; init; } = 1000.0f;
    public float WorldBmaxZ { get; init; } = 500.0f;
}

public static class ExtractVmapCommand
{
    public const ulong VmapMagic = 0x3150414D56434F41UL;
    public const uint VmapVersion = 1u, MaxTileCount = 1048576u, Crc32Polynomial = 0xEDB88320u;
    public const int HeaderSizeBytes = 72, TocEntrySizeBytes = 32, BvhNodeSizeBytes = 32, TriangleSizeBytes = 44, VolumeRecordSizeBytes = 28;
    public const ushort MatOpaqueLos = 1 << 0, MatWater = 1 << 1, MatLava = 1 << 2, MatBridge = 1 << 3, MatFoliageTransparent = 1 << 4;
    public const byte VolumeKindWater = 1, VolumeKindLava = 2;

    public static string ResolveVmapOutputPath(string outNavDir) =>
        string.IsNullOrWhiteSpace(outNavDir) ? Path.Combine(Path.GetTempPath(), "area_vmap.bin")
        : outNavDir.EndsWith(".bin", StringComparison.OrdinalIgnoreCase) ? outNavDir : Path.Combine(outNavDir, "area_vmap.bin");

    public static uint ComputeCrc32Ieee(ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < data.Length; i++)
        {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
            {
                uint mask = 0u - (crc & 1u);
                crc = (crc >> 1) ^ (Crc32Polynomial & mask);
            }
        }
        return ~crc;
    }

    public static async Task<int> ExecuteAsync(ExtractorOptions opts, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(opts);
        ct.ThrowIfCancellationRequested();
        int processed = (!string.IsNullOrWhiteSpace(opts.PakDir) && Directory.Exists(opts.PakDir) && !string.IsNullOrWhiteSpace(opts.UsmapPath) && File.Exists(opts.UsmapPath))
            ? await new IoStoreExtractionPipeline().RunAsync(opts, ct).ConfigureAwait(false) : 0;
        VmapArchiveData archive = CreateBaselineArchive();
        string outPath = ResolveVmapOutputPath(opts.OutNavDir);
        if (Path.GetDirectoryName(Path.GetFullPath(outPath)) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        await using FileStream fs = File.Create(outPath);
        WriteVmapArchive(fs, archive);
        return processed > 0 ? processed : archive.Tiles.Count;
    }

    public static int ExecuteExtractVmap(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        string outDir = string.Empty;
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] is "--out" or "--out-nav") outDir = args[i + 1];
        WriteSyntheticVmap(ResolveVmapOutputPath(outDir));
        return 0;
    }

    public static void WriteSyntheticVmap(string outPath)
    {
        if (Path.GetDirectoryName(Path.GetFullPath(outPath)) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        using FileStream fs = File.Create(outPath);
        WriteVmapArchive(fs, CreateBaselineArchive());
    }

    public static VmapArchiveHeader ValidateVmapFile(string path) => ReadAndValidateVmapHeader(File.ReadAllBytes(path));

    public static void WriteVmapArchive(Stream output, VmapArchiveData archive)
    {
        ArgumentNullException.ThrowIfNull(output);
        ArgumentNullException.ThrowIfNull(archive);
        ValidateWorldBounds(archive);
        byte[] payload = BuildTilePayloadAndToc(archive, out VmapTileTocEntry[] resolvedToc);
        byte[] sha256 = SHA256.HashData(payload);

        Span<byte> hdr = stackalloc byte[HeaderSizeBytes];
        hdr.Clear();
        BinaryPrimitives.WriteUInt64LittleEndian(hdr[..8], VmapMagic);
        BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(8, 4), VmapVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(hdr.Slice(12, 4), (uint)resolvedToc.Length);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(16, 4), archive.WorldBminX);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(20, 4), archive.WorldBminY);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(24, 4), archive.WorldBminZ);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(28, 4), archive.WorldBmaxX);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(32, 4), archive.WorldBmaxY);
        BinaryPrimitives.WriteSingleLittleEndian(hdr.Slice(36, 4), archive.WorldBmaxZ);
        sha256.AsSpan(0, 32).CopyTo(hdr.Slice(40, 32));
        output.Write(hdr);

        Span<byte> tocBuf = stackalloc byte[TocEntrySizeBytes];
        foreach (VmapTileTocEntry e in resolvedToc)
        {
            WriteTocEntry(tocBuf, e);
            output.Write(tocBuf);
        }
        output.Write(payload);
    }

    private static void ValidateWorldBounds(VmapArchiveData archive)
    {
        if (!float.IsFinite(archive.WorldBminX) || !float.IsFinite(archive.WorldBminY) || !float.IsFinite(archive.WorldBminZ) ||
            !float.IsFinite(archive.WorldBmaxX) || !float.IsFinite(archive.WorldBmaxY) || !float.IsFinite(archive.WorldBmaxZ) ||
            archive.WorldBminX > archive.WorldBmaxX || archive.WorldBminY > archive.WorldBmaxY || archive.WorldBminZ > archive.WorldBmaxZ)
        {
            throw new InvalidDataException("vmap world bounds are not finite or inverted.");
        }
    }

    private static byte[] BuildTilePayloadAndToc(VmapArchiveData archive, out VmapTileTocEntry[] toc)
    {
        toc = archive.Tiles.Count > 0
            ? [.. archive.Tiles]
            : (archive.Nodes.Count > 0 || archive.Triangles.Count > 0)
                ? [new VmapTileTocEntry(0, 0, (uint)archive.Nodes.Count, (uint)archive.Triangles.Count)]
                : [];

        ulong totalNodes = 0, totalTris = 0;
        foreach (VmapTileTocEntry e in toc) { totalNodes += e.NodeCount; totalTris += e.TriCount; }
        if (totalNodes != (ulong)archive.Nodes.Count || totalTris != (ulong)archive.Triangles.Count)
            throw new InvalidDataException("vmap TOC counts do not match payload.");

        using var ms = new MemoryStream();
        ulong offset = (ulong)HeaderSizeBytes + (ulong)toc.Length * TocEntrySizeBytes;
        int nodeCursor = 0, triCursor = 0;
        Span<byte> scratch = stackalloc byte[TriangleSizeBytes];
        for (int i = 0; i < toc.Length; i++)
        {
            VmapTileTocEntry e = toc[i];
            int startPos = (int)ms.Position;
            for (int n = 0; n < e.NodeCount; n++) { WriteBvhNode(scratch[..BvhNodeSizeBytes], archive.Nodes[nodeCursor++]); ms.Write(scratch[..BvhNodeSizeBytes]); }
            for (int t = 0; t < e.TriCount; t++) { WriteTriangle(scratch, archive.Triangles[triCursor++]); ms.Write(scratch); }
            byte[] currentBuf = ms.GetBuffer();
            int len = (int)ms.Position - startPos;
            uint crc = ComputeCrc32Ieee(currentBuf.AsSpan(startPos, len));
            toc[i] = e with { ByteOffset = offset, ByteLen = (ulong)len, Crc32 = crc };
            offset += (ulong)len;
        }
        return ms.ToArray();
    }

    private static void WriteTocEntry(Span<byte> dst, VmapTileTocEntry e)
    {
        dst.Clear();
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(0, 2), e.TileX);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(2, 2), e.TileY);
        BinaryPrimitives.WriteUInt32LittleEndian(dst.Slice(4, 4), e.NodeCount);
        BinaryPrimitives.WriteUInt32LittleEndian(dst.Slice(8, 4), e.TriCount);
        BinaryPrimitives.WriteUInt64LittleEndian(dst.Slice(12, 8), e.ByteOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(dst.Slice(20, 8), e.ByteLen);
        BinaryPrimitives.WriteUInt32LittleEndian(dst.Slice(28, 4), e.Crc32);
    }

    private static void WriteBvhNode(Span<byte> dst, BvhNode32 n)
    {
        dst.Clear();
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(0, 2), n.BminX);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(2, 2), n.BminY);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(4, 2), n.BminZ);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(6, 2), n.BmaxX);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(8, 2), n.BmaxY);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(10, 2), n.BmaxZ);
        BinaryPrimitives.WriteUInt32LittleEndian(dst.Slice(12, 4), n.LeftOrFirstTri);
        BinaryPrimitives.WriteUInt16LittleEndian(dst.Slice(16, 2), n.TriCount);
        BinaryPrimitives.WriteUInt16LittleEndian(dst.Slice(18, 2), n.MaterialFlags);
    }

    private static void WriteTriangle(Span<byte> dst, VmapTriangle tri)
    {
        if (!float.IsFinite(tri.V0X) || !float.IsFinite(tri.V0Y) || !float.IsFinite(tri.V0Z) ||
            !float.IsFinite(tri.V1X) || !float.IsFinite(tri.V1Y) || !float.IsFinite(tri.V1Z) ||
            !float.IsFinite(tri.V2X) || !float.IsFinite(tri.V2Y) || !float.IsFinite(tri.V2Z))
        {
            throw new InvalidDataException("vmap triangle has non-finite vertex.");
        }
        dst.Clear();
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(0, 4), tri.V0X);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(4, 4), tri.V0Y);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(8, 4), tri.V0Z);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(12, 4), tri.V1X);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(16, 4), tri.V1Y);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(20, 4), tri.V1Z);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(24, 4), tri.V2X);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(28, 4), tri.V2Y);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(32, 4), tri.V2Z);
        BinaryPrimitives.WriteUInt16LittleEndian(dst.Slice(36, 2), tri.MaterialFlags);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(38, 2), tri.TileX);
        BinaryPrimitives.WriteInt16LittleEndian(dst.Slice(40, 2), tri.TileY);
    }

    public static void WriteVolumeRecord(Span<byte> dst, VmapVolumeRecord v)
    {
        if (v.VolumeKind is not (VolumeKindWater or VolumeKindLava))
            throw new InvalidDataException($"Unsupported VolumeKind {v.VolumeKind}.");
        dst.Clear();
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(0, 4), v.MinX);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(4, 4), v.MinY);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(8, 4), v.MaxX);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(12, 4), v.MaxY);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(16, 4), v.SurfaceZ);
        BinaryPrimitives.WriteSingleLittleEndian(dst.Slice(20, 4), v.DepthCm);
        dst[24] = v.VolumeKind;
        BinaryPrimitives.WriteUInt16LittleEndian(dst.Slice(26, 2), v.MaterialFlags);
    }

    public static VmapArchiveHeader ReadAndValidateVmapHeader(ReadOnlySpan<byte> data)
    {
        if (data.Length < HeaderSizeBytes || BinaryPrimitives.ReadUInt64LittleEndian(data[..8]) != VmapMagic)
            throw new InvalidDataException("Invalid AOCVMAP1 header length or magic.");
        uint version = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(8, 4));
        uint tileCount = BinaryPrimitives.ReadUInt32LittleEndian(data.Slice(12, 4));
        if (version != VmapVersion || tileCount > MaxTileCount)
            throw new InvalidDataException("Unsupported AOCVMAP1 version or tile_count exceeds limit.");

        float bminX = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(16, 4));
        float bminY = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(20, 4));
        float bminZ = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(24, 4));
        float bmaxX = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(28, 4));
        float bmaxY = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(32, 4));
        float bmaxZ = BinaryPrimitives.ReadSingleLittleEndian(data.Slice(36, 4));
        if (!float.IsFinite(bminX) || !float.IsFinite(bminY) || !float.IsFinite(bminZ) ||
            !float.IsFinite(bmaxX) || !float.IsFinite(bmaxY) || !float.IsFinite(bmaxZ) ||
            bminX > bmaxX || bminY > bmaxY || bminZ > bmaxZ)
        {
            throw new InvalidDataException("Invalid AOCVMAP1 world bounds.");
        }

        ValidateTocAndTilePayloads(data, tileCount);
        return new VmapArchiveHeader(version, tileCount, bminX, bminY, bminZ)
        {
            WorldBmaxX = bmaxX,
            WorldBmaxY = bmaxY,
            WorldBmaxZ = bmaxZ,
            ContentSha256 = data.Slice(40, 32).ToArray()
        };
    }

    private static void ValidateTocAndTilePayloads(ReadOnlySpan<byte> data, uint tileCount)
    {
        ulong tocBytes = (ulong)tileCount * TocEntrySizeBytes;
        ulong payloadMin = (ulong)HeaderSizeBytes + tocBytes;
        if (payloadMin > (ulong)data.Length)
            throw new InvalidDataException("vmap file truncated in TOC.");

        for (uint i = 0; i < tileCount; i++)
        {
            ReadOnlySpan<byte> e = data.Slice(HeaderSizeBytes + (int)(i * TocEntrySizeBytes), TocEntrySizeBytes);
            uint nodeCount = BinaryPrimitives.ReadUInt32LittleEndian(e.Slice(4, 4));
            uint triCount = BinaryPrimitives.ReadUInt32LittleEndian(e.Slice(8, 4));
            ulong byteOffset = BinaryPrimitives.ReadUInt64LittleEndian(e.Slice(12, 8));
            ulong byteLen = BinaryPrimitives.ReadUInt64LittleEndian(e.Slice(20, 8));
            uint crc32 = BinaryPrimitives.ReadUInt32LittleEndian(e.Slice(28, 4));
            ulong expectedLen = (ulong)nodeCount * BvhNodeSizeBytes + (ulong)triCount * TriangleSizeBytes;
            if (byteOffset < payloadMin || byteOffset > (ulong)data.Length || byteLen > (ulong)data.Length - byteOffset || byteLen != expectedLen)
                throw new InvalidDataException("vmap tile TOC offset/length out of bounds.");
            ReadOnlySpan<byte> tileSlice = data.Slice((int)byteOffset, (int)byteLen);
            if (ComputeCrc32Ieee(tileSlice) != crc32)
                throw new InvalidDataException("vmap tile CRC32 mismatch.");
            ValidateTileTriangles(tileSlice.Slice((int)(nodeCount * BvhNodeSizeBytes)), triCount);
        }
    }

    private static void ValidateTileTriangles(ReadOnlySpan<byte> trisBytes, uint triCount)
    {
        for (uint t = 0; t < triCount; t++)
        {
            ReadOnlySpan<byte> tri = trisBytes.Slice((int)(t * TriangleSizeBytes), TriangleSizeBytes);
            for (int f = 0; f < 9; f++)
            {
                if (!float.IsFinite(BinaryPrimitives.ReadSingleLittleEndian(tri.Slice(f * 4, 4))))
                    throw new InvalidDataException("vmap triangle has non-finite vertex.");
            }
        }
    }

    public static VmapArchiveData CreateBaselineArchive() => new(
        [new VmapTileTocEntry(3, -2, 2, 2)],
        [
            new BvhNode32(-200, -200, -100, 200, 200) { BmaxZ = 0, LeftOrFirstTri = 0, TriCount = 1, MaterialFlags = MatWater },
            new BvhNode32(300, 300, -50, 500, 500) { BmaxZ = 50, LeftOrFirstTri = 1, TriCount = 1, MaterialFlags = (ushort)(MatLava | MatBridge) }
        ],
        [
            new VmapTriangle(0.0f, -100.0f, -100.0f, MatOpaqueLos) { V1X = 0.0f, V1Y = 100.0f, V1Z = -100.0f, V2X = 0.0f, V2Y = 0.0f, V2Z = 200.0f, TileX = 3, TileY = -2 },
            new VmapTriangle(50.0f, -100.0f, 300.0f, MatFoliageTransparent) { V1X = 50.0f, V1Y = 100.0f, V1Z = 300.0f, V2X = 50.0f, V2Y = 0.0f, V2Z = 500.0f, TileX = 3, TileY = -2 }
        ],
        [
            new VmapVolumeRecord(115.0f, 350.0f, VolumeKindWater, MatWater) { MinX = -200f, MinY = -200f, MaxX = 200f, MaxY = 200f },
            new VmapVolumeRecord(95.0f, 220.0f, VolumeKindLava, MatLava) { MinX = 300f, MinY = 300f, MaxX = 500f, MaxY = 500f }
        ]);

    public static void RunSelfTest() => RunSelfTests();

    public static void RunSelfTests()
    {
        using var ms = new MemoryStream();
        VmapArchiveData baseline = CreateBaselineArchive();
        WriteVmapArchive(ms, baseline);
        byte[] bytes = ms.ToArray();
        VmapArchiveHeader hdr = ReadAndValidateVmapHeader(bytes);
        int expectedSize = HeaderSizeBytes + TocEntrySizeBytes + 2 * BvhNodeSizeBytes + 2 * TriangleSizeBytes;
        if (bytes.Length != expectedSize || hdr.Version != VmapVersion || hdr.TileCount != 1u || hdr.ContentSha256.All(static b => b == 0))
            throw new InvalidOperationException("AOCVMAP1 round-trip layout or SHA-256 mismatch.");

        Span<byte> volBuf = stackalloc byte[VolumeRecordSizeBytes];
        WriteVolumeRecord(volBuf, baseline.Volumes[0]);
        if (volBuf[24] != VolumeKindWater || BinaryPrimitives.ReadUInt16LittleEndian(volBuf.Slice(26, 2)) != MatWater)
            throw new InvalidOperationException("AOCVMAP1 volume record encoding mismatch.");

        byte[] badMagic = [.. bytes]; badMagic[0] = (byte)'X'; ExpectInvalidData(() => ReadAndValidateVmapHeader(badMagic));
        byte[] badVer = [.. bytes]; BinaryPrimitives.WriteUInt32LittleEndian(badVer.AsSpan(8, 4), 2u); ExpectInvalidData(() => ReadAndValidateVmapHeader(badVer));
        byte[] badCrc = [.. bytes]; badCrc[^1] ^= 0xFF; ExpectInvalidData(() => ReadAndValidateVmapHeader(badCrc));
        byte[] badBounds = [.. bytes]; BinaryPrimitives.WriteSingleLittleEndian(badBounds.AsSpan(16, 4), float.NaN); ExpectInvalidData(() => ReadAndValidateVmapHeader(badBounds));
    }

    private static void ExpectInvalidData(Action action)
    {
        try { action(); } catch (InvalidDataException) { return; }
        throw new InvalidOperationException("Expected InvalidDataException was not thrown.");
    }
}
