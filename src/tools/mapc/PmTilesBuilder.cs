// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Buffers.Binary;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

namespace Mapc;

/// <summary>
/// Standards-compliant PMTiles v3 streaming archive builder and PNG tile encoder for Verra map pyramids and node plug tiers.
/// </summary>
public sealed class PmTilesBuilder : IDisposable
{
    public const int HeaderBytes = 127;
    public const int MaxRootDirBytes = 16384;
    public const byte CompressionNone = 1;
    public const byte CompressionGzip = 2;
    public const byte TileTypePng = 2;

    public readonly record struct DirEntry(ulong TileId, ulong Offset, uint Length, uint RunLength);

    public readonly record struct HeaderInfo(
        ulong RootOffset,
        ulong RootBytes,
        ulong MetadataOffset,
        ulong MetadataBytes,
        ulong LeafOffset,
        ulong LeafBytes,
        ulong DataOffset,
        ulong DataBytes,
        ulong AddressedTiles,
        ulong TileEntries,
        ulong TileContents,
        byte MinZoom,
        byte MaxZoom);

    private static readonly uint[] Crc32Table = BuildCrc32Table();
    private readonly List<(ulong TileId, ulong TempOffset, uint Length, string Hash)> _staged = new();
    private readonly string _tempDataPath;
    private readonly FileStream _tempDataStream;
    private readonly object _gate = new();

    public PmTilesBuilder()
    {
        _tempDataPath = Path.Combine(Path.GetTempPath(), $"pmtiles_stage_{Environment.ProcessId}_{Guid.NewGuid():N}.tmp");
        _tempDataStream = new FileStream(_tempDataPath, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None, 65536);
    }

    public int Count => _staged.Count;

    public void Dispose()
    {
        try { _tempDataStream.Dispose(); } catch { }
        try { if (File.Exists(_tempDataPath)) File.Delete(_tempDataPath); } catch { }
    }

    public static ulong ZxyToTileId(int z, int x, int y)
    {
        if (z < 0 || z > 26) throw new ArgumentOutOfRangeException(nameof(z));
        if (z == 0) return 0UL;
        ulong acc = ((1UL << (2 * z)) - 1UL) / 3UL;
        uint tx = (uint)x;
        uint ty = (uint)y;
        ulong d = 0UL;
        for (uint s = 1u << (z - 1); s > 0; s >>= 1)
        {
            uint rx = (tx & s) != 0 ? 1u : 0u;
            uint ry = (ty & s) != 0 ? 1u : 0u;
            d += (ulong)s * s * ((3u * rx) ^ ry);
            RotateHilbert(s, ref tx, ref ty, rx, ry);
        }
        return acc + d;
    }

    public static (int Z, int X, int Y) TileIdToZxy(ulong tileId)
    {
        ulong acc = 0UL;
        for (int z = 0; z <= 26; z++)
        {
            ulong count = 1UL << (2 * z);
            if (tileId < acc + count)
            {
                ulong t = tileId - acc;
                uint tx = 0u, ty = 0u;
                uint n = 1u << z;
                for (uint s = 1u; s < n; s <<= 1)
                {
                    uint rx = (uint)(1UL & (t >> 1));
                    uint ry = (uint)(1UL & (t ^ rx));
                    RotateHilbert(s, ref tx, ref ty, rx, ry);
                    tx += s * rx;
                    ty += s * ry;
                    t >>= 2;
                }
                return (z, (int)tx, (int)ty);
            }
            acc += count;
        }
        throw new ArgumentOutOfRangeException(nameof(tileId));
    }

    private static void RotateHilbert(uint n, ref uint x, ref uint y, uint rx, uint ry)
    {
        if (ry != 0) return;
        if (rx == 1)
        {
            x = n - 1u - x;
            y = n - 1u - y;
        }
        (x, y) = (y, x);
    }

    public void AddTile(int z, int x, int y, byte[] data)
    {
        if (data == null || data.Length == 0) return;
        ulong tileId = ZxyToTileId(z, x, y);
        string hash = Convert.ToHexString(SHA256.HashData(data));
        lock (_gate)
        {
            ulong offset = (ulong)_tempDataStream.Position;
            _tempDataStream.Write(data, 0, data.Length);
            _staged.Add((tileId, offset, (uint)data.Length, hash));
        }
    }

    public bool AddRgbaTile(int z, int x, int y, byte[] rgba, int size)
    {
        byte[]? png = EncodePngRgba(rgba, size, size);
        if (png == null) return false;
        AddTile(z, x, y, png);
        return true;
    }

    public long WriteToFile(string outputPath, string metadataJson, byte minZoom = 0, byte maxZoom = 8)
    {
        string orderedPath = _tempDataPath + ".ordered";
        try
        {
            var sorted = _staged.OrderBy(t => t.TileId).ToList();
            ulong uniqueContents;
            ulong totalDataBytes;
            List<DirEntry> entries;
            using (var orderedStream = new FileStream(orderedPath, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 65536))
            {
                entries = WriteClusteredTileData(sorted, orderedStream, out uniqueContents);
                totalDataBytes = (ulong)orderedStream.Length;
            }

            BuildDirectories(entries, out byte[] rootBytes, out byte[] leafBytes);
            byte[] metaBytes = CompressGzip(Encoding.UTF8.GetBytes(metadataJson));

            ulong rootOffset = HeaderBytes;
            ulong metaOffset = rootOffset + (ulong)rootBytes.Length;
            ulong leafOffset = metaOffset + (ulong)metaBytes.Length;
            ulong dataOffset = leafOffset + (ulong)leafBytes.Length;

            var info = new HeaderInfo(
                rootOffset, (ulong)rootBytes.Length,
                metaOffset, (ulong)metaBytes.Length,
                leafOffset, (ulong)leafBytes.Length,
                dataOffset, totalDataBytes,
                (ulong)sorted.Count, (ulong)entries.Count, uniqueContents,
                minZoom, maxZoom);

            Span<byte> header = stackalloc byte[HeaderBytes];
            WriteHeader(header, in info);

            using var outFs = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 65536);
            outFs.Write(header);
            outFs.Write(rootBytes, 0, rootBytes.Length);
            outFs.Write(metaBytes, 0, metaBytes.Length);
            if (leafBytes.Length > 0)
                outFs.Write(leafBytes, 0, leafBytes.Length);
            using (var orderedRead = new FileStream(orderedPath, FileMode.Open, FileAccess.Read, FileShare.Read, 65536))
            {
                orderedRead.CopyTo(outFs, 65536);
            }
            outFs.Flush();
            return outFs.Length;
        }
        finally
        {
            try { if (File.Exists(orderedPath)) File.Delete(orderedPath); } catch { }
        }
    }

    private List<DirEntry> WriteClusteredTileData(
        List<(ulong TileId, ulong TempOffset, uint Length, string Hash)> sorted,
        FileStream orderedStream,
        out ulong uniqueContents)
    {
        var entries = new List<DirEntry>(sorted.Count);
        var seenByHash = new Dictionary<string, (ulong Offset, uint Length)>(StringComparer.Ordinal);
        uniqueContents = 0UL;
        byte[] copyBuf = new byte[1 << 20];

        foreach (var (tileId, tempOffset, length, hash) in sorted)
        {
            if (!seenByHash.TryGetValue(hash, out var loc))
            {
                ulong destOffset = (ulong)orderedStream.Position;
                _tempDataStream.Seek((long)tempOffset, SeekOrigin.Begin);
                if (copyBuf.Length < length) copyBuf = new byte[length];
                _tempDataStream.ReadExactly(copyBuf, 0, (int)length);
                orderedStream.Write(copyBuf, 0, (int)length);
                loc = (destOffset, length);
                seenByHash[hash] = loc;
                uniqueContents++;
            }
            if (entries.Count > 0)
            {
                var prev = entries[^1];
                if (prev.TileId + prev.RunLength == tileId && prev.Offset == loc.Offset && prev.Length == loc.Length)
                {
                    entries[^1] = new DirEntry(prev.TileId, prev.Offset, prev.Length, prev.RunLength + 1u);
                    continue;
                }
            }
            entries.Add(new DirEntry(tileId, loc.Offset, loc.Length, 1u));
        }
        return entries;
    }

    private static void BuildDirectories(List<DirEntry> entries, out byte[] rootDir, out byte[] leafDirs)
    {
        ReadOnlySpan<DirEntry> span = entries.ToArray();
        byte[] directRoot = SerializeDirectoryGzip(span);
        if (directRoot.Length <= MaxRootDirBytes)
        {
            rootDir = directRoot;
            leafDirs = Array.Empty<byte>();
            return;
        }

        const int LeafBatchSize = 2048;
        using var leafStream = new MemoryStream();
        var rootPointers = new List<DirEntry>((entries.Count + LeafBatchSize - 1) / LeafBatchSize);
        for (int i = 0; i < entries.Count; i += LeafBatchSize)
        {
            int count = Math.Min(LeafBatchSize, entries.Count - i);
            byte[] leafBytes = SerializeDirectoryGzip(span.Slice(i, count));
            ulong leafOffset = (ulong)leafStream.Position;
            leafStream.Write(leafBytes, 0, leafBytes.Length);
            rootPointers.Add(new DirEntry(entries[i].TileId, leafOffset, (uint)leafBytes.Length, 0u));
        }
        rootDir = SerializeDirectoryGzip(rootPointers.ToArray());
        leafDirs = leafStream.ToArray();
    }

    public static byte[] SerializeDirectoryGzip(ReadOnlySpan<DirEntry> entries)
    {
        using var raw = new MemoryStream();
        WriteVarint(raw, (ulong)entries.Length);
        ulong lastId = 0UL;
        for (int i = 0; i < entries.Length; i++)
        {
            WriteVarint(raw, entries[i].TileId - lastId);
            lastId = entries[i].TileId;
        }
        for (int i = 0; i < entries.Length; i++)
            WriteVarint(raw, entries[i].RunLength);
        for (int i = 0; i < entries.Length; i++)
            WriteVarint(raw, entries[i].Length);
        for (int i = 0; i < entries.Length; i++)
        {
            if (i > 0 && entries[i].Offset == entries[i - 1].Offset + entries[i - 1].Length)
                WriteVarint(raw, 0UL);
            else
                WriteVarint(raw, entries[i].Offset + 1UL);
        }
        return CompressGzip(raw.ToArray());
    }

    private static void WriteHeader(Span<byte> h, in HeaderInfo info)
    {
        h.Clear();
        "PMTiles"u8.CopyTo(h);
        h[7] = 3;
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(8, 8), info.RootOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(16, 8), info.RootBytes);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(24, 8), info.MetadataOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(32, 8), info.MetadataBytes);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(40, 8), info.LeafOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(48, 8), info.LeafBytes);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(56, 8), info.DataOffset);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(64, 8), info.DataBytes);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(72, 8), info.AddressedTiles);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(80, 8), info.TileEntries);
        BinaryPrimitives.WriteUInt64LittleEndian(h.Slice(88, 8), info.TileContents);
        h[96] = 1;
        h[97] = CompressionGzip;
        h[98] = CompressionNone;
        h[99] = TileTypePng;
        h[100] = info.MinZoom;
        h[101] = info.MaxZoom;
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(102, 4), -1800000000);
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(106, 4), -850511287);
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(110, 4), 1800000000);
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(114, 4), 850511287);
        h[118] = 3;
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(119, 4), 0);
        BinaryPrimitives.WriteInt32LittleEndian(h.Slice(123, 4), 0);
    }

    private static byte[] CompressGzip(ReadOnlySpan<byte> raw)
    {
        using var ms = new MemoryStream();
        using (var gz = new GZipStream(ms, CompressionLevel.Optimal, leaveOpen: true))
        {
            gz.Write(raw);
        }
        return ms.ToArray();
    }

    private static void WriteVarint(Stream s, ulong value)
    {
        while (value >= 0x80UL)
        {
            s.WriteByte((byte)((value & 0x7FUL) | 0x80UL));
            value >>= 7;
        }
        s.WriteByte((byte)value);
    }

    public static byte[]? EncodePngRgba(byte[] rgba, int width, int height)
    {
        byte minA = 255, maxA = 0;
        for (int i = 3; i < rgba.Length; i += 4)
        {
            byte a = rgba[i];
            if (a < minA) minA = a;
            if (a > maxA) maxA = a;
        }
        if (maxA == 0) return null;

        bool opaque = minA == 255;
        using var png = new MemoryStream(width * height + 128);
        png.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });

        Span<byte> ihdr = stackalloc byte[13];
        BinaryPrimitives.WriteUInt32BigEndian(ihdr.Slice(0, 4), (uint)width);
        BinaryPrimitives.WriteUInt32BigEndian(ihdr.Slice(4, 4), (uint)height);
        ihdr[8] = 8;
        ihdr[9] = opaque ? (byte)2 : (byte)6;
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        WritePngChunk(png, "IHDR"u8, ihdr);

        byte[] scanlines = BuildFilteredScanlines(rgba, width, height, opaque);
        using var idatCompressed = new MemoryStream(width * height);
        using (var zlib = new ZLibStream(idatCompressed, CompressionLevel.Optimal, leaveOpen: true))
        {
            zlib.Write(scanlines, 0, scanlines.Length);
        }
        WritePngChunk(png, "IDAT"u8, idatCompressed.ToArray());
        WritePngChunk(png, "IEND"u8, ReadOnlySpan<byte>.Empty);
        return png.ToArray();
    }

    private static byte[] BuildFilteredScanlines(byte[] rgba, int width, int height, bool opaque)
    {
        int bpp = opaque ? 3 : 4;
        int stride = 1 + width * bpp;
        byte[] output = new byte[height * stride];
        byte[] rawRow = new byte[width * bpp];

        for (int y = 0; y < height; y++)
        {
            int srcRow = y * width * 4;
            if (opaque)
            {
                for (int x = 0; x < width; x++)
                {
                    int s = srcRow + x * 4;
                    int d = x * 3;
                    rawRow[d] = (byte)(rgba[s] & 0xF8);
                    rawRow[d + 1] = (byte)(rgba[s + 1] & 0xF8);
                    rawRow[d + 2] = (byte)(rgba[s + 2] & 0xF8);
                }
            }
            else
            {
                for (int x = 0; x < width; x++)
                {
                    int s = srcRow + x * 4;
                    int d = x * 4;
                    rawRow[d] = (byte)(rgba[s] & 0xF8);
                    rawRow[d + 1] = (byte)(rgba[s + 1] & 0xF8);
                    rawRow[d + 2] = (byte)(rgba[s + 2] & 0xF8);
                    rawRow[d + 3] = rgba[s + 3];
                }
            }

            int dstRow = y * stride;
            output[dstRow] = 1;
            for (int i = 0; i < bpp; i++)
                output[dstRow + 1 + i] = rawRow[i];
            for (int i = bpp; i < rawRow.Length; i++)
                output[dstRow + 1 + i] = (byte)(rawRow[i] - rawRow[i - bpp]);
        }
        return output;
    }

    private static void WritePngChunk(Stream dst, ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        Span<byte> buf = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(buf, (uint)data.Length);
        dst.Write(buf);
        dst.Write(type);
        if (data.Length > 0) dst.Write(data);
        BinaryPrimitives.WriteUInt32BigEndian(buf, ComputeCrc32(type, data));
        dst.Write(buf);
    }

    private static uint ComputeCrc32(ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < type.Length; i++)
            crc = Crc32Table[(crc ^ type[i]) & 0xFF] ^ (crc >> 8);
        for (int i = 0; i < data.Length; i++)
            crc = Crc32Table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }

    private static uint[] BuildCrc32Table()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++)
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }
}
