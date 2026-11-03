// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using ZstdSharp;

namespace AocExtractor;

public static class UsmapCompat
{
    public const ushort Magic = 0x30C4;
    public const int V3HeaderLength = 15;
    public const int V4VersionedHeaderLength = 23;

    public static MemoryStream Open(string path)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        byte[] downgraded = DowngradeToV3(File.ReadAllBytes(path));
        return new MemoryStream(downgraded, 0, downgraded.Length, writable: false, publiclyVisible: true);
    }

    public static byte[] DowngradeToV3(byte[] src)
    {
        if (src is null || src.Length < V3HeaderLength || BinaryPrimitives.ReadUInt16LittleEndian(src.AsSpan(0, 2)) != Magic)
            throw new InvalidDataException("Invalid .usmap header or magic.");
        byte version = src[2];
        if (version <= 3) return src;
        if (version > 4) throw new InvalidDataException($"Unsupported .usmap version {version}.");

        byte hasVersion = src[3];
        int hdrLen = hasVersion == 0 ? V3HeaderLength : V4VersionedHeaderLength;
        if (src.Length < hdrLen) throw new InvalidDataException("Truncated versioned .usmap header.");

        int metaOffset = hasVersion == 0 ? 4 : 12;
        byte method = src[metaOffset];
        uint compSize = BinaryPrimitives.ReadUInt32LittleEndian(src.AsSpan(metaOffset + 1, 4));
        uint decompSize = BinaryPrimitives.ReadUInt32LittleEndian(src.AsSpan(metaOffset + 5, 4));
        if (compSize > (uint)(src.Length - hdrLen) || decompSize == 0 || decompSize > 256u * 1024u * 1024u)
            throw new InvalidDataException("Invalid .usmap payload lengths.");

        ReadOnlySpan<byte> compSpan = src.AsSpan(hdrLen, (int)compSize);
        byte[] payload = method switch
        {
            0 => compSize == decompSize ? compSpan.ToArray() : throw new InvalidDataException("Uncompressed size mismatch."),
            3 => DecompressZstd(compSpan, (int)decompSize),
            _ => throw new InvalidDataException($"Unsupported .usmap compression method {method}.")
        };

        int enumEnd = FindEnumsCountEndOffset(payload);
        if (enumEnd + 2 > payload.Length) throw new InvalidDataException("Truncated v4 enum header after enums_count.");

        int v3PayloadLen = payload.Length - 2;
        byte[] dst = new byte[V3HeaderLength + v3PayloadLen];
        BinaryPrimitives.WriteUInt16LittleEndian(dst.AsSpan(0, 2), Magic);
        dst[2] = 3; dst[3] = 0; dst[4] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(dst.AsSpan(5, 4), (uint)v3PayloadLen);
        BinaryPrimitives.WriteUInt32LittleEndian(dst.AsSpan(9, 4), (uint)v3PayloadLen);
        payload.AsSpan(0, enumEnd).CopyTo(dst.AsSpan(V3HeaderLength));
        payload.AsSpan(enumEnd + 2).CopyTo(dst.AsSpan(V3HeaderLength + enumEnd));
        return dst;
    }

    private static byte[] DecompressZstd(ReadOnlySpan<byte> compressed, int decompSize)
    {
        try
        {
            using var decompressor = new Decompressor();
            Span<byte> unwrapped = decompressor.Unwrap(compressed, decompSize);
            return unwrapped.Length == decompSize ? unwrapped.ToArray() : throw new InvalidDataException("Zstd size mismatch.");
        }
        catch (Exception ex) when (ex is not InvalidDataException) { throw new InvalidDataException("Failed to decompress Zstd .usmap.", ex); }
    }

    private static int FindEnumsCountEndOffset(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 4) throw new InvalidDataException("Truncated .usmap payload before names_count.");
        uint namesCount = BinaryPrimitives.ReadUInt32LittleEndian(payload[..4]);
        int offset = 4;
        for (uint i = 0; i < namesCount; i++)
        {
            if (offset + 2 > payload.Length) throw new InvalidDataException("Truncated .usmap name length.");
            offset += 2 + BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(offset, 2));
            if (offset > payload.Length) throw new InvalidDataException("Truncated .usmap name bytes.");
        }
        return offset + 4 <= payload.Length ? offset + 4 : throw new InvalidDataException("Truncated .usmap payload before enums_count.");
    }
}
