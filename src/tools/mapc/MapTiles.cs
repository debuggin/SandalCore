// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Text.RegularExpressions;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports.Texture;
using Newtonsoft.Json;

namespace Mapc;

/// <summary>
/// mapc pmtiles: builds dist/content/verra.pmtiles containing the 8-level Verra base minimap pyramid (z = 0..7),
/// all 130 node plug tier backdrop textures (26 levelable nodes x 5 tiers L0..L4 at z = 8),
/// and real in-game RGBA map/node POI icons at z = 9, plus embedded JSON metadata.
/// </summary>
public static class MapTiles
{
    public const string FileName = "verra.pmtiles";
    public const string Folder = "AOC/Content/UI/MinimapImageTiles/Verra_World_Master/";
    public const int SourceCm = 50000;
    public const int SourcePx = 512;
    public const int TilePx = 256;
    public const int PlugPx = 1024;
    public const int NodeSizeCm = 100000;
    public const byte MaxBaseZoom = 7;
    public const byte PlugZoom = 8;
    public const byte IconZoom = 9;

    private static readonly Regex BaseTileRx = new(@"^TUI_s50000_x(-?\d+)_y(-?\d+)$", RegexOptions.Compiled);
    private static readonly uint[] PngCrcTable = BuildPngCrcTable();

    public sealed record IconSpec(
        [property: JsonProperty("index")] int Index,
        [property: JsonProperty("name")] string Name,
        [property: JsonProperty("width")] int Width,
        [property: JsonProperty("height")] int Height);

    public sealed record GridSpec(
        int X0,
        int Y0,
        int Cols,
        int Rows,
        int BaseTileCount);

    public sealed record NodeSpec(
        int NodeIndex,
        long Guid,
        string Name,
        string DisplayName,
        string TextureStem,
        double Ox,
        double Oy,
        double Oz,
        double YawDeg);

    // The 26 levelable city nodes in CityNodeRecordBase GUID order, with exact CityNodeRecordBase locations and rotations.
    public static readonly NodeSpec[] LevelableNodes =
    {
        new(0, 108646108518008L, "Verra_RVR_Winstead", "Winstead", "Winstead", -644519.11, 376773.24, 12012.23, 319.20846),
        new(1, 108646121231993L, "Verra_RVR_Joeva", "Joeva", "Joeva", -1094396.04, 521336.34, 11836.45, 292.625506),
        new(2, 108646608033403L, "Verra_RVR_NewAela", "New Aela", "New_Aela", -1134183.5439466983, 853387.2772475089, 11761.82381661977, -90.21495968964565),
        new(3, 108646610851452L, "Verra_RVR_Halcyon", "Halcyon", "Halcyon", -891024.50, 296919.66, 11657.05, 180.0),
        new(4, 108646615701117L, "Verra_RVR_Miraleth", "Miraleth", "Miraleth", -459889.634548, 599317.092515, 11000.972636, -20.0),
        new(5, 6064632225073725498L, "Verra_TUR_Windansea", "Windansea", "Windansea", -1100624.184211, 246466.992911, 6674.000004, 55.41214),
        new(6, 6064632734251679746L, "Verra_SAN_Aithanahr", "Aithanahr", "Aithanahr", -232617.63, 762558.56, 6679.26, 304.121675),
        new(7, 6064632734253907971L, "Verra_SAN_Azmaran", "Azmaran", "Azmaran", -278767.94, 987391.03, 7227.0, 351.156897),
        new(8, 6064632734268915716L, "Verra_SAN_Sunhaven", "Sunhaven", "Sunhaven", -619803.283992, 873514.222232, 12073.59434, -127.073107),
        new(9, 6064633097318170624L, "Verra_SAN_Djinna", "Djinna", "Djinna", 63370.908474, 861198.365739, 5066.0, -187.137862),
        new(10, 6064633097371058177L, "Verra_SAN_SquallsEnd", "Squall's End", "Squall_s_End", -37370.0, 1133335.0, 16385.348318, -217.228668),
        new(11, 6064633334652600320L, "Verra_TUR_Korrin", "Korrin", "Korrin", -1764281.284871, 114557.650316, 5066.0, 90.707388),
        new(12, 6064633334741794817L, "Verra_TUR_Brinebarrel", "Brinebarrel", "Brinebarrel", -1860980.218817, -171832.947158, 7472.764935, -46.566062),
        new(13, 6064633407845105664L, "Verra_TUR_Seahook", "Seahook", "Seahook", -1764263.887544, 539283.279654, 5066.0, 14.228906),
        new(14, 6064633407846088705L, "Verra_TUR_Shorefoot", "Shorefoot", "Shorefoot", -1372642.268119, 57374.666533, 5066.0, -66.900158),
        new(15, 6064633425419042816L, "Verra_JUN_Wildport", "Wildport", "Wildport", -1783982.0, 809682.0, 9777.0, 16.0),
        new(16, 6064633425517019141L, "Verra_JUN_Hecribba", "Hecribba", "Hecribba", -1708500.0, 1053019.0, 9620.0, 246.0),
        new(17, 6064633425555816457L, "Verra_JUN_Vinebreach", "Vinebreach", "Vinebreach", -1476480.0, 1178456.0, 7013.0, 200.0),
        new(18, 6064633429404090381L, "Verra_JUN_Arisalon", "Arisalon", "Arisalon", -1250506.0, 1062377.0, 5379.0, -35.0),
        new(19, 6064633429423882257L, "Verra_JUN_TangledPost", "Tangled Post", "Tangled_Post", -1031419.0, 1098848.0, 7264.0, -3.5),
        new(20, 6064633429437251605L, "Verra_JUN_Mythbreak", "Mythbreak", "Mythbreak", -742705.0, 1087738.0, 5066.0, -159.058146),
        new(21, 6064633651969589265L, "Verra_ANV_Vexhelm", "Vexhelm", "Vexhelm", -1463243.0, -341306.0, 25074.0, 60.0),
        new(22, 6064633651985711129L, "Verra_ANV_Duunhold", "Duunhold", "Duunhold", -1069784.0, -150970.0, 24891.0, 0.0),
        new(23, 6064633652009173023L, "Verra_ANV_Dhurgrum", "Dhurgrum", "Dhurgrum", -937374.0, -411205.0, 24983.0, -15.0),
        new(24, 6064633652023394340L, "Verra_ANV_Vhalgadim", "Vhalgadim", "Vhalgadim", -762862.0, -563881.0, 17256.0, 349.999997),
        new(25, 6064633652044300331L, "Verra_ANV_KalTorhum", "Kal Torhum", "Kal_Torhum", -1271772.0, -462821.0, 5066.0, 5.0),
    };

    public static long BuildToFile(Game game, string outputPath, Action<string> log)
    {
        var sw = Stopwatch.StartNew();
        var baseTiles = CollectBaseTileKeys(game);
        int x0 = baseTiles.Min(t => t.X);
        int y0 = baseTiles.Min(t => t.Y);
        int cols = baseTiles.Max(t => t.X) - x0 + 1;
        int rows = baseTiles.Max(t => t.Y) - y0 + 1;
        int widthPx = cols * SourcePx;
        int heightPx = rows * SourcePx;

        log($"pmtiles: {baseTiles.Count} base tiles, x {x0}..{x0 + cols - 1}, y {y0}..{y0 + rows - 1} ({widthPx}x{heightPx} px)");
        using var builder = new PmTilesBuilder();
        var z6Tiles = BuildBaseAndLevel6(game, baseTiles, x0, y0, builder);
        BuildCoarsePyramid(z6Tiles, builder);
        z6Tiles.Clear();
        int plugCount = BuildNodePlugTiles(game, builder);
        var icons = BuildMapIconTiles(game, builder);
        var grid = new GridSpec(x0, y0, cols, rows, baseTiles.Count);
        string metaJson = BuildMetadataJson(grid, icons);
        long bytesWritten = builder.WriteToFile(outputPath, metaJson, minZoom: 0, maxZoom: IconZoom);
        log($"pmtiles: encoded {builder.Count} total tiles ({plugCount} node plug tiles at z={PlugZoom}, {icons.Count} icon tiles at z={IconZoom}) into {bytesWritten} bytes in {sw.Elapsed.TotalSeconds:F1} s");
        return bytesWritten;
    }

    private static List<(string Key, int X, int Y)> CollectBaseTileKeys(Game game)
    {
        var tiles = new List<(string Key, int X, int Y)>();
        foreach (var key in game.Provider.Files.Keys)
        {
            if (!key.StartsWith(Folder, StringComparison.OrdinalIgnoreCase) || !key.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase))
                continue;
            if (key.IndexOf('/', Folder.Length) >= 0)
                continue;
            var m = BaseTileRx.Match(Path.GetFileNameWithoutExtension(key));
            if (m.Success)
            {
                int x = int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
                int y = int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                tiles.Add((key, x, y));
            }
        }
        if (tiles.Count == 0)
            throw new InvalidOperationException($"no base minimap tiles found under {Folder}");
        return tiles.OrderBy(t => t.Y).ThenBy(t => t.X).ToList();
    }

    private static Dictionary<(int Tx, int Ty), byte[]> BuildBaseAndLevel6(
        Game game,
        List<(string Key, int X, int Y)> baseTiles,
        int x0,
        int y0,
        PmTilesBuilder builder)
    {
        var z6Tiles = new Dictionary<(int Tx, int Ty), byte[]>(baseTiles.Count);
        foreach (var (key, x, y) in baseTiles)
        {
            byte[] rawBgra = ReadTextureBgra(game, key, SourcePx);
            byte[] rotRgba = Rotate90ClockwiseBgraToRgba(rawBgra, SourcePx);
            int gx = x - x0;
            int gy = y - y0;

            for (int dy = 0; dy < 2; dy++)
            {
                for (int dx = 0; dx < 2; dx++)
                {
                    byte[] quad = CropQuadrant(rotRgba, SourcePx, dx * TilePx, dy * TilePx, TilePx);
                    builder.AddRgbaTile(MaxBaseZoom, gx * 2 + dx, gy * 2 + dy, quad, TilePx);
                }
            }

            byte[] half = Downsample512To256(rotRgba);
            z6Tiles[(gx, gy)] = half;
            builder.AddRgbaTile(MaxBaseZoom - 1, gx, gy, half, TilePx);
        }
        return z6Tiles;
    }

    private static void BuildCoarsePyramid(Dictionary<(int Tx, int Ty), byte[]> z6Tiles, PmTilesBuilder builder)
    {
        var prev = z6Tiles;
        for (int z = MaxBaseZoom - 2; z >= 0; z--)
        {
            var parentCoords = prev.Keys
                .Select(k => (Tx: k.Tx / 2, Ty: k.Ty / 2))
                .Distinct()
                .OrderBy(k => k.Ty)
                .ThenBy(k => k.Tx)
                .ToList();

            var cur = new Dictionary<(int Tx, int Ty), byte[]>(parentCoords.Count);
            foreach (var (tx, ty) in parentCoords)
            {
                byte[] canvas = new byte[SourcePx * SourcePx * 4];
                for (int dy = 0; dy < 2; dy++)
                {
                    for (int dx = 0; dx < 2; dx++)
                    {
                        if (prev.TryGetValue((tx * 2 + dx, ty * 2 + dy), out var child))
                            PasteQuadrant(canvas, SourcePx, child, TilePx, dx * TilePx, dy * TilePx);
                    }
                }
                byte[] down = Downsample512To256(canvas);
                cur[(tx, ty)] = down;
                builder.AddRgbaTile(z, tx, ty, down, TilePx);
            }
            prev = cur;
        }
    }

    private static int BuildNodePlugTiles(Game game, PmTilesBuilder builder)
    {
        int written = 0;
        foreach (var node in LevelableNodes)
        {
            for (int tier = 0; tier < 5; tier++)
            {
                string key = $"{Folder}TUI_{node.TextureStem}_L{tier}.uasset";
                byte[] rawBgra = ReadTextureBgra(game, key, PlugPx);
                byte[] rotRgba = Rotate90ClockwiseBgraToRgba(rawBgra, PlugPx);
                if (!builder.AddRgbaTile(PlugZoom, node.NodeIndex, tier, rotRgba, PlugPx))
                    throw new InvalidOperationException($"node plug tile {key} produced an empty image");
                written++;
            }
        }
        return written;
    }

    private static byte[] ReadTextureBgra(Game game, string key, int expectedSize)
    {
        var pkg = game.Load(key) ?? throw new InvalidOperationException($"package {key} could not be loaded");
        var tex = pkg.GetExports().OfType<UTexture2D>().FirstOrDefault()
            ?? throw new InvalidOperationException($"package {key} contains no UTexture2D");
        if (tex.PlatformData.PixelFormat != "PF_B8G8R8A8")
            throw new InvalidOperationException($"{key}: unexpected pixel format {tex.PlatformData.PixelFormat}");
        var mip = tex.GetFirstMip();
        var data = mip?.BulkData?.Data ?? throw new InvalidOperationException($"{key}: missing mip bulk data");
        if (mip.SizeX != expectedSize || mip.SizeY != expectedSize || data.Length != expectedSize * expectedSize * 4)
            throw new InvalidOperationException($"{key}: expected {expectedSize}x{expectedSize} BGRA, got {mip.SizeX}x{mip.SizeY} ({data.Length} bytes)");
        game.Forget(key);
        return data;
    }

    public static byte[] Rotate90ClockwiseBgraToRgba(byte[] srcBgra, int size)
    {
        var dstRgba = new byte[size * size * 4];
        for (int dstY = 0; dstY < size; dstY++)
        {
            int srcX = dstY;
            int dstRow = dstY * size;
            for (int dstX = 0; dstX < size; dstX++)
            {
                int srcY = size - 1 - dstX;
                int s = (srcY * size + srcX) * 4;
                int d = (dstRow + dstX) * 4;
                dstRgba[d] = srcBgra[s + 2];
                dstRgba[d + 1] = srcBgra[s + 1];
                dstRgba[d + 2] = srcBgra[s];
                dstRgba[d + 3] = 255;
            }
        }
        return dstRgba;
    }

    private static byte[] CropQuadrant(byte[] src, int srcSize, int x0, int y0, int quadSize)
    {
        var dst = new byte[quadSize * quadSize * 4];
        int rowBytes = quadSize * 4;
        for (int y = 0; y < quadSize; y++)
        {
            int srcOffset = ((y0 + y) * srcSize + x0) * 4;
            Buffer.BlockCopy(src, srcOffset, dst, y * rowBytes, rowBytes);
        }
        return dst;
    }

    private static void PasteQuadrant(byte[] dst, int dstSize, byte[] src, int quadSize, int x0, int y0)
    {
        int rowBytes = quadSize * 4;
        for (int y = 0; y < quadSize; y++)
        {
            int dstOffset = ((y0 + y) * dstSize + x0) * 4;
            Buffer.BlockCopy(src, y * rowBytes, dst, dstOffset, rowBytes);
        }
    }

    private static byte[] Downsample512To256(byte[] src512)
    {
        var dst256 = new byte[TilePx * TilePx * 4];
        for (int y = 0; y < TilePx; y++)
        {
            int row0 = (y * 2) * SourcePx;
            int row1 = (y * 2 + 1) * SourcePx;
            for (int x = 0; x < TilePx; x++)
            {
                int p00 = (row0 + x * 2) * 4;
                int p10 = (row0 + x * 2 + 1) * 4;
                int p01 = (row1 + x * 2) * 4;
                int p11 = (row1 + x * 2 + 1) * 4;

                int a0 = src512[p00 + 3], a1 = src512[p10 + 3], a2 = src512[p01 + 3], a3 = src512[p11 + 3];
                int sumA = a0 + a1 + a2 + a3;
                int d = (y * TilePx + x) * 4;
                if (sumA == 0) continue;

                dst256[d] = (byte)((src512[p00] * a0 + src512[p10] * a1 + src512[p01] * a2 + src512[p11] * a3 + (sumA >> 1)) / sumA);
                dst256[d + 1] = (byte)((src512[p00 + 1] * a0 + src512[p10 + 1] * a1 + src512[p01 + 1] * a2 + src512[p11 + 1] * a3 + (sumA >> 1)) / sumA);
                dst256[d + 2] = (byte)((src512[p00 + 2] * a0 + src512[p10 + 2] * a1 + src512[p01 + 2] * a2 + src512[p11 + 2] * a3 + (sumA >> 1)) / sumA);
                dst256[d + 3] = (byte)((sumA + 2) >> 2);
            }
        }
        return dst256;
    }

    public static List<(string ShortName, string Key)> CollectMapIconKeys(IEnumerable<string> fileKeys)
    {
        var byName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var key in fileKeys.OrderBy(k => k, StringComparer.OrdinalIgnoreCase))
        {
            if (!key.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase)) continue;
            bool inMapIcons = key.IndexOf("UI/Icons/Map/", StringComparison.OrdinalIgnoreCase) >= 0;
            bool inNodeIcons = key.IndexOf("UI/Widgets/Nodes/", StringComparison.OrdinalIgnoreCase) >= 0;
            if (!inMapIcons && !inNodeIcons) continue;
            string shortName = Path.GetFileNameWithoutExtension(key);
            if (!shortName.StartsWith("TUI_Icon_", StringComparison.OrdinalIgnoreCase)) continue;
            if (!byName.ContainsKey(shortName))
                byName[shortName] = key;
        }
        return byName
            .OrderBy(kv => kv.Key, StringComparer.OrdinalIgnoreCase)
            .Select(kv => (kv.Key, kv.Value))
            .ToList();
    }

    private static List<IconSpec> BuildMapIconTiles(Game game, PmTilesBuilder builder)
    {
        var iconKeys = CollectMapIconKeys(game.Provider.Files.Keys);
        var icons = new List<IconSpec>(iconKeys.Count);
        for (int i = 0; i < iconKeys.Count; i++)
        {
            var (shortName, key) = iconKeys[i];
            var (rgba, width, height) = ReadIconTextureRgba(game, key);
            byte[] png = EncodeRgbaPng(rgba, width, height);
            builder.AddTile(IconZoom, i, 0, png);
            icons.Add(new IconSpec(i, shortName, width, height));
        }
        return icons;
    }

    private static (byte[] Rgba, int Width, int Height) ReadIconTextureRgba(Game game, string key)
    {
        var pkg = game.Load(key) ?? throw new InvalidOperationException($"package {key} could not be loaded");
        var tex = pkg.GetExports().OfType<UTexture2D>().FirstOrDefault()
            ?? throw new InvalidOperationException($"package {key} contains no UTexture2D");
        if (tex.PlatformData.PixelFormat != "PF_B8G8R8A8")
            throw new InvalidOperationException($"{key}: unexpected pixel format {tex.PlatformData.PixelFormat}");
        var mip = tex.GetFirstMip();
        var data = mip?.BulkData?.Data ?? throw new InvalidOperationException($"{key}: missing mip bulk data");
        int w = mip.SizeX, h = mip.SizeY;
        if (w <= 0 || h <= 0 || data.Length != w * h * 4)
            throw new InvalidOperationException($"{key}: expected {w}x{h} BGRA, got {data.Length} bytes");
        byte[] rgba = ConvertBgraToRgbaUnrotated(data, w, h);
        game.Forget(key);
        return (rgba, w, h);
    }

    public static byte[] ConvertBgraToRgbaUnrotated(byte[] srcBgra, int width, int height)
    {
        if (srcBgra == null || width <= 0 || height <= 0 || srcBgra.Length != width * height * 4)
            throw new ArgumentException("Invalid BGRA buffer dimensions.");
        var dstRgba = new byte[width * height * 4];
        for (int i = 0; i < dstRgba.Length; i += 4)
        {
            dstRgba[i] = srcBgra[i + 2];
            dstRgba[i + 1] = srcBgra[i + 1];
            dstRgba[i + 2] = srcBgra[i];
            dstRgba[i + 3] = srcBgra[i + 3];
        }
        return dstRgba;
    }

    public static byte[] EncodeRgbaPng(byte[] rgba, int width, int height)
    {
        if (rgba == null || width <= 0 || height <= 0 || rgba.Length != width * height * 4)
            throw new ArgumentException("rgba buffer length must equal width * height * 4.");
        int stride = width * 4;
        byte[] rawScanlines = new byte[height * (1 + stride)];
        int dst = 0;
        for (int y = 0; y < height; y++)
        {
            rawScanlines[dst++] = 0;
            Buffer.BlockCopy(rgba, y * stride, rawScanlines, dst, stride);
            dst += stride;
        }
        byte[] idatPayload;
        using (var zlibMs = new MemoryStream())
        {
            using (var zlib = new ZLibStream(zlibMs, CompressionLevel.Optimal, leaveOpen: true))
                zlib.Write(rawScanlines, 0, rawScanlines.Length);
            idatPayload = zlibMs.ToArray();
        }
        using var pngMs = new MemoryStream(64 + idatPayload.Length);
        pngMs.Write(new byte[] { 137, 80, 78, 71, 13, 10, 26, 10 });
        Span<byte> ihdr = stackalloc byte[13];
        BinaryPrimitives.WriteUInt32BigEndian(ihdr.Slice(0, 4), (uint)width);
        BinaryPrimitives.WriteUInt32BigEndian(ihdr.Slice(4, 4), (uint)height);
        ihdr[8] = 8;
        ihdr[9] = 6;
        WritePngChunk(pngMs, "IHDR"u8, ihdr);
        WritePngChunk(pngMs, "IDAT"u8, idatPayload);
        WritePngChunk(pngMs, "IEND"u8, ReadOnlySpan<byte>.Empty);
        return pngMs.ToArray();
    }

    private static uint[] BuildPngCrcTable()
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

    private static void WritePngChunk(Stream s, ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        Span<byte> lenBuf = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(lenBuf, (uint)data.Length);
        s.Write(lenBuf);
        s.Write(type);
        if (!data.IsEmpty) s.Write(data);
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < type.Length; i++)
            crc = PngCrcTable[(crc ^ type[i]) & 0xFF] ^ (crc >> 8);
        for (int i = 0; i < data.Length; i++)
            crc = PngCrcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        Span<byte> crcBuf = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(crcBuf, crc ^ 0xFFFFFFFFu);
        s.Write(crcBuf);
    }

    public static string BuildMetadataJson(int x0, int y0, int cols, int rows, int baseTileCount) =>
        BuildMetadataJson(new GridSpec(x0, y0, cols, rows, baseTileCount), Array.Empty<IconSpec>());

    public static string BuildMetadataJson(GridSpec grid, IReadOnlyList<IconSpec> icons)
    {
        var nodes = LevelableNodes.Select(n => new
        {
            node_index = n.NodeIndex,
            guid = n.Guid,
            name = n.Name,
            display_name = n.DisplayName,
            texture_stem = n.TextureStem,
            ox = n.Ox,
            oy = n.Oy,
            oz = n.Oz,
            yaw_deg = n.YawDeg,
            size_cm = NodeSizeCm,
            tiers = new[] { 0, 1, 2, 3, 4 },
        }).ToArray();

        var meta = new
        {
            name = "verra",
            version = 1,
            format = "png",
            ext = "png",
            x0 = grid.X0,
            y0 = grid.Y0,
            cols = grid.Cols,
            rows = grid.Rows,
            source_cm = SourceCm,
            source_px = SourcePx,
            tile_px = TilePx,
            plug_px = PlugPx,
            width_px = grid.Cols * SourcePx,
            height_px = grid.Rows * SourcePx,
            levels = MaxBaseZoom + 1,
            min_zoom = 0,
            max_zoom = (int)MaxBaseZoom,
            plug_zoom = (int)PlugZoom,
            icon_zoom = (int)IconZoom,
            tiles = grid.BaseTileCount,
            plug_tiles = LevelableNodes.Length * 5,
            icon_tiles = icons.Count,
            nodes,
            icons,
        };
        return JsonConvert.SerializeObject(meta, Formatting.None);
    }
}
