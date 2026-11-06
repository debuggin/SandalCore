using System.Buffers.Binary;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class PmTilesAndVerraMathTests
{
    private const int SourceCm = 50000;
    private const int X0 = -47;
    private const int Y0 = -17;
    private const int Cols = 51;
    private const int Rows = 46;

    private static Type GetPmTilesBuilderType()
    {
        string repoRoot = FindRepoRoot();
        string dllPath = Path.Combine(repoRoot, "build", "dotnet", "bin", "mapc", "Release", "net10.0", "mapc.dll");
        if (!File.Exists(dllPath))
            dllPath = Path.Combine(repoRoot, "build", "dotnet", "bin", "mapc", "Debug", "net10.0", "mapc.dll");
        Assert.True(File.Exists(dllPath), $"Expected compiled mapc.dll at {dllPath}");
        Assembly asm = Assembly.LoadFrom(dllPath);
        return asm.GetType("Mapc.PmTilesBuilder", throwOnError: true)!;
    }

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "AGENTS.md")) &&
                Directory.Exists(Path.Combine(dir.FullName, "src", "tools", "mapc")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        throw new DirectoryNotFoundException("Could not locate SandalCore repository root.");
    }

    public static (double U, double V) WorldToUv(double xCm, double yCm)
    {
        double u = (xCm / SourceCm) - X0;
        double v = (Y0 + Rows) - (yCm / SourceCm);
        return (u, v);
    }

    public static (double XCm, double YCm) UvToWorld(double u, double v)
    {
        double xCm = (u + X0) * SourceCm;
        double yCm = ((Y0 + Rows) - v) * SourceCm;
        return (xCm, yCm);
    }

    public static int StageToPlugTier(int stage)
    {
        if (stage < 2 || stage > 6) return -1;
        return stage - 2;
    }

    [Fact]
    public void HilbertBijection_RoundTripsAllCoords_AcrossZoom0To8()
    {
        Type builderType = GetPmTilesBuilderType();
        MethodInfo zxyToId = builderType.GetMethod("ZxyToTileId", BindingFlags.Public | BindingFlags.Static)!;
        MethodInfo idToZxy = builderType.GetMethod("TileIdToZxy", BindingFlags.Public | BindingFlags.Static)!;

        Assert.Equal(0UL, (ulong)zxyToId.Invoke(null, new object[] { 0, 0, 0 })!);

        for (int z = 0; z <= 3; z++)
        {
            int n = 1 << z;
            ulong expectedBase = z == 0 ? 0UL : ((1UL << (2 * z)) - 1UL) / 3UL;
            ulong minId = ulong.MaxValue, maxId = 0UL;
            for (int y = 0; y < n; y++)
            {
                for (int x = 0; x < n; x++)
                {
                    ulong id = (ulong)zxyToId.Invoke(null, new object[] { z, x, y })!;
                    if (id < minId) minId = id;
                    if (id > maxId) maxId = id;
                    var tuple = ((int Z, int X, int Y))idToZxy.Invoke(null, new object[] { id })!;
                    Assert.Equal((z, x, y), tuple);
                }
            }
            Assert.Equal(expectedBase, minId);
            Assert.Equal(expectedBase + (ulong)(n * n) - 1UL, maxId);
        }

        var plugIds = new HashSet<ulong>();
        for (int nodeIdx = 0; nodeIdx < 26; nodeIdx++)
        {
            for (int tier = 0; tier < 5; tier++)
            {
                ulong id = (ulong)zxyToId.Invoke(null, new object[] { 8, nodeIdx, tier })!;
                Assert.True(plugIds.Add(id), $"Duplicate tile_id {id} for z=8, x={nodeIdx}, y={tier}");
                var roundTrip = ((int Z, int X, int Y))idToZxy.Invoke(null, new object[] { id })!;
                Assert.Equal((8, nodeIdx, tier), roundTrip);
            }
        }
        Assert.Equal(130, plugIds.Count);
    }

    [Fact]
    public void PmTilesBuilder_WritesValidHeaderDeduplicatedRunsAndGzipMetadata()
    {
        Type builderType = GetPmTilesBuilderType();
        MethodInfo zxyToId = builderType.GetMethod("ZxyToTileId", BindingFlags.Public | BindingFlags.Static)!;
        MethodInfo addRgba = builderType.GetMethod("AddRgbaTile", BindingFlags.Public | BindingFlags.Instance)!;
        MethodInfo writeToFile = builderType.GetMethod("WriteToFile", BindingFlags.Public | BindingFlags.Instance)!;

        string tmpPath = Path.Combine(Path.GetTempPath(), $"pmtiles_unit_{Guid.NewGuid():N}.pmtiles");
        try
        {
            using var builder = (IDisposable)Activator.CreateInstance(builderType)!;
            byte[] tileA = CreateSolidRgba(16, 40, 80, 160, 255);
            byte[] tileB = CreateSolidRgba(16, 200, 120, 40, 255);

            ulong id0 = (ulong)zxyToId.Invoke(null, new object[] { 1, 0, 0 })!;
            ulong id1 = (ulong)zxyToId.Invoke(null, new object[] { 1, 0, 1 })!;
            Assert.Equal(1L, Math.Abs((long)id1 - (long)id0));

            Assert.True((bool)addRgba.Invoke(builder, new object[] { 1, 0, 0, tileA, 16 })!);
            Assert.True((bool)addRgba.Invoke(builder, new object[] { 1, 0, 1, tileA, 16 })!);
            Assert.True((bool)addRgba.Invoke(builder, new object[] { 8, 25, 4, tileB, 16 })!);

            string metaJson = "{\"name\":\"verra-unit\",\"plug_zoom\":8,\"nodes\":26}";
            long written = (long)writeToFile.Invoke(builder, new object[] { tmpPath, metaJson, (byte)0, (byte)8 })!;
            Assert.True(written > 127);

            byte[] bytes = File.ReadAllBytes(tmpPath);
            Assert.Equal("PMTiles", Encoding.ASCII.GetString(bytes, 0, 7));
            Assert.Equal(3, bytes[7]);
            Assert.Equal(127UL, BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(8, 8)));

            ulong metaOff = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(24, 8));
            ulong metaLen = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(32, 8));
            ulong addressed = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(72, 8));
            ulong entries = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(80, 8));
            ulong contents = BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(88, 8));

            Assert.Equal(3UL, addressed);
            Assert.Equal(2UL, entries);
            Assert.Equal(2UL, contents);
            Assert.Equal(0, bytes[100]);
            Assert.Equal(8, bytes[101]);

            string roundTripMeta = DecompressGzipUtf8(bytes, (int)metaOff, (int)metaLen);
            Assert.Equal(metaJson, roundTripMeta);
        }
        finally
        {
            if (File.Exists(tmpPath)) File.Delete(tmpPath);
        }
    }

    [Fact]
    public void VerraCoordinateMath_AndNodePlugStageMapping_HoldInvariants()
    {
        var (uWinstead, vWinstead) = WorldToUv(-641389.0, 377367.1);
        Assert.InRange(uWinstead, 0.0, Cols);
        Assert.InRange(vWinstead, 0.0, Rows);
        var (rtX, rtY) = UvToWorld(uWinstead, vWinstead);
        Assert.Equal(-641389.0, rtX, precision: 3);
        Assert.Equal(377367.1, rtY, precision: 3);

        var (uOriginTile, vOriginTile) = WorldToUv(X0 * SourceCm, (Y0 + Rows) * SourceCm);
        Assert.Equal(0.0, uOriginTile, precision: 6);
        Assert.Equal(0.0, vOriginTile, precision: 6);

        Assert.Equal(-1, StageToPlugTier(0));
        Assert.Equal(-1, StageToPlugTier(1));
        Assert.Equal(0, StageToPlugTier(2));
        Assert.Equal(1, StageToPlugTier(3));
        Assert.Equal(2, StageToPlugTier(4));
        Assert.Equal(3, StageToPlugTier(5));
        Assert.Equal(4, StageToPlugTier(6));
        Assert.Equal(-1, StageToPlugTier(7));
    }

    [Fact]
    public void DistVerraPmTiles_ContainsBasePyramidAnd130NodePlugTiles()
    {
        string repoRoot = FindRepoRoot();
        string pmtilesPath = Path.Combine(repoRoot, "dist", "content", "verra.pmtiles");
        if (!File.Exists(pmtilesPath)) return;

        using var fs = File.OpenRead(pmtilesPath);
        Span<byte> hdr = stackalloc byte[127];
        Assert.Equal(127, fs.Read(hdr));
        Assert.Equal("PMTiles", Encoding.ASCII.GetString(hdr.Slice(0, 7)));
        Assert.Equal(3, hdr[7]);

        ulong metaOff = BinaryPrimitives.ReadUInt64LittleEndian(hdr.Slice(24, 8));
        ulong metaLen = BinaryPrimitives.ReadUInt64LittleEndian(hdr.Slice(32, 8));
        ulong addressed = BinaryPrimitives.ReadUInt64LittleEndian(hdr.Slice(72, 8));
        Assert.Equal(9600UL, addressed);
        Assert.Equal(0, hdr[100]);
        Assert.Equal(8, hdr[101]);

        byte[] metaGz = new byte[metaLen];
        fs.Seek((long)metaOff, SeekOrigin.Begin);
        fs.ReadExactly(metaGz, 0, metaGz.Length);
        string json = DecompressGzipUtf8(metaGz, 0, metaGz.Length);

        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement root = doc.RootElement;
        Assert.Equal(-47, root.GetProperty("x0").GetInt32());
        Assert.Equal(-17, root.GetProperty("y0").GetInt32());
        Assert.Equal(51, root.GetProperty("cols").GetInt32());
        Assert.Equal(46, root.GetProperty("rows").GetInt32());
        Assert.Equal(7, root.GetProperty("max_zoom").GetInt32());
        Assert.Equal(8, root.GetProperty("plug_zoom").GetInt32());
        Assert.Equal(1765, root.GetProperty("tiles").GetInt32());
        Assert.Equal(130, root.GetProperty("plug_tiles").GetInt32());
        Assert.Equal(26, root.GetProperty("nodes").GetArrayLength());
    }

    private static byte[] CreateSolidRgba(int size, byte r, byte g, byte b, byte a)
    {
        byte[] buf = new byte[size * size * 4];
        for (int i = 0; i < buf.Length; i += 4)
        {
            buf[i] = r;
            buf[i + 1] = g;
            buf[i + 2] = b;
            buf[i + 3] = a;
        }
        return buf;
    }

    private static string DecompressGzipUtf8(byte[] buffer, int offset, int count)
    {
        using var ms = new MemoryStream(buffer, offset, count);
        using var gz = new GZipStream(ms, CompressionMode.Decompress);
        using var sr = new StreamReader(gz, Encoding.UTF8);
        return sr.ReadToEnd();
    }
}
