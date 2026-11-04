// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace AocExtractor.PmTiles;

public readonly record struct PmTilesV3Header(ulong RootDirOffset, ulong RootDirBytes, ulong JsonMetadataOffset, ulong JsonMetadataBytes, ulong TileDataOffset)
{
    public ulong LeafDirsOffset { get; init; } public ulong LeafDirsBytes { get; init; } public ulong TileDataBytes { get; init; }
    public ulong AddressedTilesCount { get; init; } public ulong TileEntriesCount { get; init; } public ulong TileContentsCount { get; init; }
    public byte Clustered { get; init; } = PmTilesV3Writer.ClusteredByHilbert; public byte InternalCompression { get; init; } = PmTilesV3Writer.CompressionGzip;
    public byte TileCompression { get; init; } = PmTilesV3Writer.CompressionNone; public byte TileType { get; init; } = PmTilesV3Writer.TileTypeWebp;
    public byte MinZoom { get; init; } public byte MaxZoom { get; init; } = PmTilesV3Writer.DefaultMaxZoom;
    public int MinLonE7 { get; init; } = -1800000000; public int MinLatE7 { get; init; } = -850511287;
    public int MaxLonE7 { get; init; } = 1800000000; public int MaxLatE7 { get; init; } = 850511287;
    public byte CenterZoom { get; init; } = 3; public int CenterLonE7 { get; init; } public int CenterLatE7 { get; init; }
}

public readonly record struct PmTilesDirectoryEntry(ulong TileId, ulong Offset, uint Length, uint RunLength = 1u);
public sealed record PmTilesInputTile(byte Z, uint X, uint Y, byte[] Payload);

public static class PmTilesV3Writer
{
    public const int HeaderSize = 127, MaxDirectoryEntries = 262_144, MaxDecompressedDirBytes = 16 * 1024 * 1024;
    public const byte Version = 3, ClusteredByHilbert = 1, CompressionNone = 1, CompressionGzip = 2, TileTypeWebp = 4;
    public const byte DefaultMinZoom = 0, DefaultMaxZoom = 6, MaxSupportedHilbertZoom = 15;
    private static ReadOnlySpan<byte> MagicV3 => "PMTiles\x03"u8;

    public static ulong ZxyToTileId(byte z, uint x, uint y)
    {
        if (z > MaxSupportedHilbertZoom) throw new ArgumentOutOfRangeException(nameof(z));
        uint n = 1u << z, tx = x, ty = y;
        if (x >= n || y >= n) throw new ArgumentOutOfRangeException(nameof(x));
        ulong baseId = ((1UL << (2 * z)) - 1UL) / 3UL, d = 0UL;
        for (uint s = n >> 1; s > 0; s >>= 1)
        {
            uint rx = (tx & s) != 0 ? 1u : 0u, ry = (ty & s) != 0 ? 1u : 0u;
            d += (ulong)s * s * ((3u * rx) ^ ry); tx &= s - 1u; ty &= s - 1u; Rotate(s, ref tx, ref ty, rx, ry);
        }
        return baseId + d;
    }

    public static (byte Z, uint X, uint Y) TileIdToZxy(ulong tileId)
    {
        for (byte z = 0; z <= MaxSupportedHilbertZoom; z++)
        {
            ulong count = 1UL << (2 * z), baseId = (count - 1UL) / 3UL;
            if (tileId < baseId || tileId - baseId >= count) continue;
            ulong t = tileId - baseId; uint n = 1u << z, x = 0u, y = 0u;
            for (uint s = 1u; s < n; s <<= 1)
            {
                uint rx = (uint)(1UL & (t >> 1)), ry = (uint)(1UL & (t ^ rx));
                Rotate(s, ref x, ref y, rx, ry); x += s * rx; y += s * ry; t >>= 2;
            }
            return (z, x, y);
        }
        throw new ArgumentOutOfRangeException(nameof(tileId));
    }

    private static void Rotate(uint n, ref uint x, ref uint y, uint rx, uint ry)
    {
        if (ry != 0) return;
        if (rx == 1u) { x = n - 1u - x; y = n - 1u - y; }
        (x, y) = (y, x);
    }
    public static PmTilesV3Header WriteArchive(Stream output, IReadOnlyList<PmTilesInputTile> tiles, byte minZoom = DefaultMinZoom, byte maxZoom = DefaultMaxZoom, string jsonMetadata = "{\"name\":\"verra\",\"format\":\"webp\"}")
    {
        ArgumentNullException.ThrowIfNull(output); ArgumentNullException.ThrowIfNull(tiles);
        if (tiles.Count == 0 || minZoom > maxZoom || maxZoom > DefaultMaxZoom) throw new InvalidDataException("Invalid PMTiles v3 zoom bounds or empty tile pyramid.");
        (List<PmTilesDirectoryEntry> entries, byte[] tileData, ulong contentsCount) = BuildDeduplicatedData(tiles, minZoom, maxZoom);
        byte[] rootDirGz = SerializeDirectory(entries), jsonMetaGz = GzipCompress(Encoding.UTF8.GetBytes(jsonMetadata ?? "{}"));
        ulong rootOff = HeaderSize, rootLen = (ulong)rootDirGz.Length, metaOff = rootOff + rootLen, metaLen = (ulong)jsonMetaGz.Length, dataOff = metaOff + metaLen, dataLen = (ulong)tileData.Length;
        var header = new PmTilesV3Header(rootOff, rootLen, metaOff, metaLen, dataOff)
        {
            LeafDirsOffset = dataOff, LeafDirsBytes = 0UL, TileDataBytes = dataLen, AddressedTilesCount = (ulong)tiles.Count,
            TileEntriesCount = (ulong)entries.Count, TileContentsCount = contentsCount, MinZoom = minZoom, MaxZoom = maxZoom, CenterZoom = (byte)((minZoom + maxZoom) / 2)
        };
        Span<byte> hdrBytes = stackalloc byte[HeaderSize]; WriteHeaderBytes(hdrBytes, header);
        output.Write(hdrBytes); output.Write(rootDirGz); output.Write(jsonMetaGz); output.Write(tileData);
        return header;
    }
    private static (List<PmTilesDirectoryEntry> Entries, byte[] TileData, ulong ContentsCount) BuildDeduplicatedData(IReadOnlyList<PmTilesInputTile> tiles, byte minZoom, byte maxZoom)
    {
        var sorted = new (ulong TileId, byte[] Payload)[tiles.Count];
        for (int i = 0; i < tiles.Count; i++)
        {
            PmTilesInputTile t = tiles[i];
            if (t.Z < minZoom || t.Z > maxZoom || t.Payload is null || t.Payload.Length == 0) throw new InvalidDataException("Invalid tile entry.");
            sorted[i] = (ZxyToTileId(t.Z, t.X, t.Y), t.Payload);
        }
        Array.Sort(sorted, static (a, b) => a.TileId.CompareTo(b.TileId));
        var dedup = new Dictionary<string, (ulong Offset, uint Length)>(StringComparer.Ordinal);
        var entries = new List<PmTilesDirectoryEntry>(sorted.Length); using var dataStream = new MemoryStream();
        for (int i = 0; i < sorted.Length; i++)
        {
            if (i > 0 && sorted[i].TileId == sorted[i - 1].TileId) throw new InvalidDataException("Duplicate Hilbert TileId.");
            string hash = Convert.ToHexString(SHA256.HashData(sorted[i].Payload));
            if (!dedup.TryGetValue(hash, out (ulong Offset, uint Length) slice))
            {
                slice = ((ulong)dataStream.Position, (uint)sorted[i].Payload.Length); dataStream.Write(sorted[i].Payload); dedup[hash] = slice;
            }
            if (entries.Count > 0 && entries[^1] is var prev && prev.TileId + prev.RunLength == sorted[i].TileId && prev.Offset == slice.Offset && prev.Length == slice.Length)
                entries[^1] = prev with { RunLength = prev.RunLength + 1u };
            else entries.Add(new PmTilesDirectoryEntry(sorted[i].TileId, slice.Offset, slice.Length, 1u));
        }
        return (entries, dataStream.ToArray(), (ulong)dedup.Count);
    }
    private static void WriteHeaderBytes(Span<byte> dst, PmTilesV3Header h)
    {
        dst.Clear(); MagicV3.CopyTo(dst[..8]);
        ulong[] u64 = [h.RootDirOffset, h.RootDirBytes, h.JsonMetadataOffset, h.JsonMetadataBytes, h.LeafDirsOffset, h.LeafDirsBytes, h.TileDataOffset, h.TileDataBytes, h.AddressedTilesCount, h.TileEntriesCount, h.TileContentsCount];
        for (int i = 0; i < u64.Length; i++) BinaryPrimitives.WriteUInt64LittleEndian(dst.Slice(8 + i * 8, 8), u64[i]);
        dst[96] = h.Clustered; dst[97] = h.InternalCompression; dst[98] = h.TileCompression; dst[99] = h.TileType; dst[100] = h.MinZoom; dst[101] = h.MaxZoom;
        BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(102, 4), h.MinLonE7); BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(106, 4), h.MinLatE7);
        BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(110, 4), h.MaxLonE7); BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(114, 4), h.MaxLatE7);
        dst[118] = h.CenterZoom; BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(119, 4), h.CenterLonE7); BinaryPrimitives.WriteInt32LittleEndian(dst.Slice(123, 4), h.CenterLatE7);
    }

    public static byte[] SerializeDirectory(IReadOnlyList<PmTilesDirectoryEntry> entries)
    {
        ArgumentNullException.ThrowIfNull(entries);
        if (entries.Count == 0 || entries.Count > MaxDirectoryEntries) throw new InvalidDataException("Invalid PMTiles v3 directory entry count.");
        using var raw = new MemoryStream(); WriteVarint(raw, (ulong)entries.Count); ulong lastId = 0UL;
        for (int i = 0; i < entries.Count; i++)
        {
            if (entries[i].RunLength == 0 || entries[i].Length == 0 || (i > 0 && entries[i].TileId < lastId)) throw new InvalidDataException("Unclustered directory entry.");
            WriteVarint(raw, entries[i].TileId - (i == 0 ? 0UL : entries[i - 1].TileId)); lastId = entries[i].TileId + entries[i].RunLength;
        }
        foreach (PmTilesDirectoryEntry e in entries) WriteVarint(raw, e.RunLength);
        foreach (PmTilesDirectoryEntry e in entries) WriteVarint(raw, e.Length);
        for (int i = 0; i < entries.Count; i++)
            WriteVarint(raw, i > 0 && entries[i].Offset == entries[i - 1].Offset + entries[i - 1].Length ? 0UL : entries[i].Offset + 1UL);
        return GzipCompress(raw.ToArray());
    }

    public static IReadOnlyList<PmTilesDirectoryEntry> ReadDirectoryEntries(ReadOnlySpan<byte> archive, PmTilesV3Header header)
    {
        CheckSliceBounds(header.RootDirOffset, header.RootDirBytes, (ulong)archive.Length, HeaderSize);
        byte[] raw = GzipDecompress(archive.Slice((int)header.RootDirOffset, (int)header.RootDirBytes));
        int pos = 0; ulong count = ReadVarint(raw, ref pos);
        if (count == 0 || count > MaxDirectoryEntries || count != header.TileEntriesCount) throw new InvalidDataException("Directory entry count mismatch.");
        int n = (int)count; ulong[] tileIds = new ulong[n], offsets = new ulong[n]; uint[] runLengths = new uint[n], lengths = new uint[n];
        for (int i = 0; i < n; i++)
        {
            ulong delta = ReadVarint(raw, ref pos);
            if (i > 0 && delta == 0) throw new InvalidDataException("Non-ascending TileId delta.");
            tileIds[i] = i == 0 ? delta : checked(tileIds[i - 1] + delta);
        }
        for (int i = 0; i < n; i++) runLengths[i] = checked((uint)ReadVarint(raw, ref pos));
        for (int i = 0; i < n; i++) lengths[i] = checked((uint)ReadVarint(raw, ref pos));
        var entries = new PmTilesDirectoryEntry[n];
        for (int i = 0; i < n; i++)
        {
            ulong rawOff = ReadVarint(raw, ref pos);
            if (rawOff == 0 && i == 0) throw new InvalidDataException("Invalid continuation offset 0.");
            offsets[i] = rawOff == 0 ? checked(offsets[i - 1] + lengths[i - 1]) : rawOff - 1UL;
            if (runLengths[i] == 0 || lengths[i] == 0 || (i > 0 && tileIds[i] < tileIds[i - 1] + runLengths[i - 1])) throw new InvalidDataException("Overlapping directory run.");
            CheckSliceBounds(offsets[i], lengths[i], header.TileDataBytes, 0UL);
            entries[i] = new PmTilesDirectoryEntry(tileIds[i], offsets[i], lengths[i], runLengths[i]);
        }
        return entries;
    }

    public static PmTilesV3Header ValidateAndReadHeader(ReadOnlySpan<byte> archive)
    {
        if (archive.Length < HeaderSize || !archive[..8].SequenceEqual(MagicV3)) throw new InvalidDataException("Invalid PMTiles v3 header length or magic.");
        ReadOnlySpan<byte> a = archive;
        static ulong U64(ReadOnlySpan<byte> s, int o) => BinaryPrimitives.ReadUInt64LittleEndian(s.Slice(o, 8));
        static int I32(ReadOnlySpan<byte> s, int o) => BinaryPrimitives.ReadInt32LittleEndian(s.Slice(o, 4));
        var h = new PmTilesV3Header(U64(a, 8), U64(a, 16), U64(a, 24), U64(a, 32), U64(a, 56))
        {
            LeafDirsOffset = U64(a, 40), LeafDirsBytes = U64(a, 48), TileDataBytes = U64(a, 64), AddressedTilesCount = U64(a, 72), TileEntriesCount = U64(a, 80), TileContentsCount = U64(a, 88),
            Clustered = a[96], InternalCompression = a[97], TileCompression = a[98], TileType = a[99], MinZoom = a[100], MaxZoom = a[101],
            MinLonE7 = I32(a, 102), MinLatE7 = I32(a, 106), MaxLonE7 = I32(a, 110), MaxLatE7 = I32(a, 114), CenterZoom = a[118], CenterLonE7 = I32(a, 119), CenterLatE7 = I32(a, 123)
        };
        if (h.Clustered != ClusteredByHilbert || h.InternalCompression != CompressionGzip || h.TileCompression != CompressionNone || h.TileType != TileTypeWebp ||
            h.MinZoom > h.MaxZoom || h.MaxZoom > DefaultMaxZoom || h.TileContentsCount == 0 || h.TileEntriesCount == 0 || h.TileContentsCount > h.AddressedTilesCount)
            throw new InvalidDataException("Invalid PMTiles v3 compression, tile type, zoom, or count invariants.");
        ulong total = (ulong)a.Length;
        CheckSliceBounds(h.RootDirOffset, h.RootDirBytes, total, HeaderSize); CheckSliceBounds(h.JsonMetadataOffset, h.JsonMetadataBytes, total, HeaderSize);
        if (h.LeafDirsBytes > 0) CheckSliceBounds(h.LeafDirsOffset, h.LeafDirsBytes, total, HeaderSize);
        CheckSliceBounds(h.TileDataOffset, h.TileDataBytes, total, HeaderSize);
        return h;
    }

    public static bool TryGetTile(ReadOnlySpan<byte> archive, byte z, uint x, uint y, out byte[] payload)
    {
        payload = []; PmTilesV3Header hdr = ValidateAndReadHeader(archive);
        if (z < hdr.MinZoom || z > hdr.MaxZoom) return false;
        IReadOnlyList<PmTilesDirectoryEntry> entries = ReadDirectoryEntries(archive, hdr);
        ulong targetId = ZxyToTileId(z, x, y);
        for (int lo = 0, hi = entries.Count - 1; lo <= hi;)
        {
            int mid = lo + ((hi - lo) >> 1); PmTilesDirectoryEntry e = entries[mid];
            if (targetId < e.TileId) hi = mid - 1;
            else if (targetId - e.TileId >= e.RunLength) lo = mid + 1;
            else
            {
                CheckSliceBounds(e.Offset, e.Length, hdr.TileDataBytes, 0UL);
                ulong absOff = hdr.TileDataOffset + e.Offset; CheckSliceBounds(absOff, e.Length, (ulong)archive.Length, HeaderSize);
                payload = archive.Slice((int)absOff, (int)e.Length).ToArray(); return true;
            }
        }
        return false;
    }

    private static void CheckSliceBounds(ulong offset, ulong length, ulong totalLen, ulong minOffset)
    { if (offset < minOffset || offset > totalLen || length > totalLen - offset) throw new InvalidDataException("PMTiles v3 slice out of bounds."); }
    private static void WriteVarint(Stream s, ulong value)
    { while (value >= 0x80UL) { s.WriteByte((byte)((value & 0x7FUL) | 0x80UL)); value >>= 7; } s.WriteByte((byte)value); }
    private static ulong ReadVarint(ReadOnlySpan<byte> buf, ref int pos)
    {
        ulong result = 0UL;
        for (int shift = 0; shift < 64; shift += 7)
        {
            if ((uint)pos >= (uint)buf.Length) throw new InvalidDataException("Truncated LEB128 varint.");
            byte b = buf[pos++]; result |= (ulong)(b & 0x7F) << shift; if ((b & 0x80) == 0) return result;
        }
        throw new InvalidDataException("Overlong LEB128 varint.");
    }

    private static byte[] GzipCompress(ReadOnlySpan<byte> data)
    { using var ms = new MemoryStream(); using (var gz = new GZipStream(ms, CompressionLevel.SmallestSize, leaveOpen: true)) gz.Write(data); return ms.ToArray(); }

    private static byte[] GzipDecompress(ReadOnlySpan<byte> gzData)
    {
        using var src = new MemoryStream(gzData.ToArray()); using var gz = new GZipStream(src, CompressionMode.Decompress); using var dst = new MemoryStream();
        byte[] buf = new byte[4096]; int read;
        while ((read = gz.Read(buf, 0, buf.Length)) > 0)
        {
            if (dst.Length > MaxDecompressedDirBytes - read) throw new InvalidDataException("Decompression ceiling exceeded.");
            dst.Write(buf, 0, read);
        }
        return dst.ToArray();
    }
}