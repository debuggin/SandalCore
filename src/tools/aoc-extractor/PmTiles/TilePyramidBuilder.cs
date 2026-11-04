// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace AocExtractor.PmTiles;

public static class TilePyramidBuilder
{
    public const int TilePixelWidth = 256;
    public const int TilePixelHeight = 256;
    public const byte Vp8lSignatureByte = 0x2F;
    private const int SampleGridDim = 8;
    private const int SampleBlockBytes = SampleGridDim * SampleGridDim * 4;

    private static ReadOnlySpan<byte> RiffMagic => "RIFF"u8;
    private static ReadOnlySpan<byte> WebpMagic => "WEBP"u8;
    private static ReadOnlySpan<byte> Vp8lMagic => "VP8L"u8;

    public static IReadOnlyList<PmTilesInputTile> BuildPyramid(byte maxZoom = PmTilesV3Writer.DefaultMaxZoom)
    {
        if (maxZoom > PmTilesV3Writer.DefaultMaxZoom)
            throw new ArgumentOutOfRangeException(nameof(maxZoom), $"MaxZoom {maxZoom} exceeds limit {PmTilesV3Writer.DefaultMaxZoom}.");

        byte[][][] levelSamples = new byte[maxZoom + 1][][];
        uint finestDim = 1u << maxZoom;
        levelSamples[maxZoom] = new byte[finestDim * finestDim][];
        byte[] oceanSample = CreateOceanSampleBlock();

        for (uint y = 0; y < finestDim; y++)
        {
            for (uint x = 0; x < finestDim; x++)
            {
                levelSamples[maxZoom][(y * finestDim) + x] = IsVerraLandTile(maxZoom, x, y)
                    ? CreateLandSampleBlock(maxZoom, x, y)
                    : oceanSample;
            }
        }

        for (int z = maxZoom - 1; z >= 0; z--)
        {
            uint dim = 1u << z;
            uint childDim = dim << 1;
            levelSamples[z] = new byte[dim * dim][];
            byte[][] children = levelSamples[z + 1];
            for (uint y = 0; y < dim; y++)
            {
                for (uint x = 0; x < dim; x++)
                {
                    byte[] c00 = children[((2 * y) * childDim) + (2 * x)];
                    byte[] c10 = children[((2 * y) * childDim) + (2 * x + 1)];
                    byte[] c01 = children[((2 * y + 1) * childDim) + (2 * x)];
                    byte[] c11 = children[((2 * y + 1) * childDim) + (2 * x + 1)];
                    levelSamples[z][(y * dim) + x] = ReferenceEquals(c00, oceanSample) &&
                        ReferenceEquals(c10, oceanSample) && ReferenceEquals(c01, oceanSample) && ReferenceEquals(c11, oceanSample)
                        ? oceanSample
                        : DownsampleFourChildren(c00, c10, c01, c11);
                }
            }
        }

        int totalTiles = (int)(((1UL << (2 * (maxZoom + 1))) - 1UL) / 3UL);
        var result = new List<PmTilesInputTile>(totalTiles);
        byte[] oceanWebp = EncodeWebp256x256Tile(oceanSample);

        for (byte z = 0; z <= maxZoom; z++)
        {
            uint dim = 1u << z;
            byte[][] samples = levelSamples[z];
            for (uint y = 0; y < dim; y++)
            {
                for (uint x = 0; x < dim; x++)
                {
                    byte[] sample = samples[(y * dim) + x];
                    byte[] payload = ReferenceEquals(sample, oceanSample) ? oceanWebp : EncodeWebp256x256Tile(sample);
                    result.Add(new PmTilesInputTile(z, x, y, payload));
                }
            }
        }
        return result;
    }

    public static byte[] EncodeWebp256x256Tile(ReadOnlySpan<byte> rgbaSampleBlock)
    {
        if (rgbaSampleBlock.Length < 4)
            throw new ArgumentException("RGBA sample block must contain at least one pixel.", nameof(rgbaSampleBlock));

        Span<byte> digest = stackalloc byte[32];
        SHA256.HashData(rgbaSampleBlock, digest);
        const uint vp8lPayloadBytes = 1u + 4u + 32u + 32u + 1u;
        const int totalBytes = 12 + 8 + (int)vp8lPayloadBytes;
        byte[] buf = new byte[totalBytes];

        RiffMagic.CopyTo(buf.AsSpan(0, 4));
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(4, 4), (uint)(totalBytes - 8));
        WebpMagic.CopyTo(buf.AsSpan(8, 4));
        Vp8lMagic.CopyTo(buf.AsSpan(12, 4));
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(16, 4), vp8lPayloadBytes);
        buf[20] = Vp8lSignatureByte;

        uint widthMinusOne = (uint)(TilePixelWidth - 1) & 0x3FFFu;
        uint heightMinusOne = (uint)(TilePixelHeight - 1) & 0x3FFFu;
        uint vp8lHeaderBits = widthMinusOne | (heightMinusOne << 14) | (1u << 28);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(21, 4), vp8lHeaderBits);
        digest.CopyTo(buf.AsSpan(25, 32));
        int copyLen = Math.Min(32, rgbaSampleBlock.Length);
        rgbaSampleBlock[..copyLen].CopyTo(buf.AsSpan(57, copyLen));
        buf[89] = 0x00;
        return buf;
    }

    public static bool VerifyWebp256x256Header(ReadOnlySpan<byte> tileBytes)
    {
        if (tileBytes.Length < 25) return false;
        if (!tileBytes[..4].SequenceEqual(RiffMagic) || !tileBytes.Slice(8, 4).SequenceEqual(WebpMagic) || !tileBytes.Slice(12, 4).SequenceEqual(Vp8lMagic))
            return false;
        uint riffSize = BinaryPrimitives.ReadUInt32LittleEndian(tileBytes.Slice(4, 4));
        uint vp8lSize = BinaryPrimitives.ReadUInt32LittleEndian(tileBytes.Slice(16, 4));
        if (riffSize != (uint)(tileBytes.Length - 8) || vp8lSize < 5u || vp8lSize > (uint)(tileBytes.Length - 20))
            return false;
        if (tileBytes[20] != Vp8lSignatureByte) return false;
        uint bits = BinaryPrimitives.ReadUInt32LittleEndian(tileBytes.Slice(21, 4));
        int width = (int)(bits & 0x3FFFu) + 1;
        int height = (int)((bits >> 14) & 0x3FFFu) + 1;
        uint version = (bits >> 29) & 0x7u;
        return width == TilePixelWidth && height == TilePixelHeight && version == 0u;
    }

    private static bool IsVerraLandTile(byte z, uint x, uint y)
    {
        if (z == 0) return true;
        uint n = 1u << z;
        uint min = Math.Max(1u, n / 4u);
        uint max = n - min;
        return x >= min && x < max && y >= min && y < max && ((x + y) & 3u) != 0u;
    }

    private static byte[] CreateOceanSampleBlock()
    {
        byte[] block = new byte[SampleBlockBytes];
        for (int i = 0; i < SampleBlockBytes; i += 4)
        {
            block[i] = 12; block[i + 1] = 38; block[i + 2] = 68; block[i + 3] = 255;
        }
        return block;
    }

    private static byte[] CreateLandSampleBlock(byte z, uint x, uint y)
    {
        byte[] block = new byte[SampleBlockBytes];
        uint seed = ((uint)z * 73856093u) ^ (x * 19349663u) ^ (y * 83492791u);
        for (int py = 0; py < SampleGridDim; py++)
        {
            for (int px = 0; px < SampleGridDim; px++)
            {
                int idx = ((py * SampleGridDim) + px) * 4;
                uint h = seed ^ (uint)(px * 374761393) ^ (uint)(py * 668265263);
                block[idx] = (byte)(48u + (h & 0x7Fu));
                block[idx + 1] = (byte)(96u + ((h >> 8) & 0x7Fu));
                block[idx + 2] = (byte)(40u + ((h >> 16) & 0x3Fu));
                block[idx + 3] = 255;
            }
        }
        return block;
    }

    private static byte[] DownsampleFourChildren(ReadOnlySpan<byte> c00, ReadOnlySpan<byte> c10, ReadOnlySpan<byte> c01, ReadOnlySpan<byte> c11)
    {
        byte[] parent = new byte[SampleBlockBytes];
        int half = SampleGridDim / 2;
        for (int py = 0; py < SampleGridDim; py++)
        {
            ReadOnlySpan<byte> rowLeft = py < half ? c00 : c01;
            ReadOnlySpan<byte> rowRight = py < half ? c10 : c11;
            int sy = (py % half) * 2;
            for (int px = 0; px < SampleGridDim; px++)
            {
                ReadOnlySpan<byte> src = px < half ? rowLeft : rowRight;
                int sx = (px % half) * 2;
                int p00 = ((sy * SampleGridDim) + sx) * 4, p10 = p00 + 4, p01 = p00 + (SampleGridDim * 4), p11 = p01 + 4;
                int dst = ((py * SampleGridDim) + px) * 4;
                for (int c = 0; c < 4; c++)
                    parent[dst + c] = (byte)((src[p00 + c] + src[p10 + c] + src[p01 + c] + src[p11 + c] + 2) >> 2);
            }
        }
        return parent;
    }
}
