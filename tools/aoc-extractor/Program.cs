// SPDX-License-Identifier: AGPL-3.0-only
using System.Buffers.Binary;
using System.Text;
using AocExtractor.Commands;
using AocExtractor.Pipeline;
using SandalCore.Config;
using ZstdSharp;

namespace AocExtractor;

public static class Program
{
    public const string DefaultAesKeyHex = "0x0000000000000000000000000000000000000000000000000000000000000000";

    public static async Task<int> Main(string[] args)
    {
        try
        {
            (ExtractorOptions opts, bool selfTest) = ParseArgs(args);
            if (selfTest) { await RunSelfTestsAsync().ConfigureAwait(false); return 0; }
            if (opts.Command == "extract-tiles") return ExtractTilesCommand.ExecuteExtractTiles(args);
            if (opts.Command == "verify-standin") return VerifyStandinCommand.Execute(args);
            int count = opts.Command switch
            {
                "extract-db" => await ExtractDbCommand.ExecuteAsync(opts).ConfigureAwait(false),
                "extract-vmap" => await ExtractVmapCommand.ExecuteAsync(opts).ConfigureAwait(false),
                "extract-nav" => await ExtractNavCommand.ExecuteAsync(opts).ConfigureAwait(false),
                _ => await new IoStoreExtractionPipeline().RunAsync(opts).ConfigureAwait(false)
            };
            Console.WriteLine($"[aoc-extractor] Completed {opts.Command}: {count} package(s) processed.");
            return 0;
        }
        catch (Exception ex) { Console.Error.WriteLine($"[aoc-extractor] ERROR: {ex.Message}"); return 1; }
    }

    public static (ExtractorOptions Options, bool SelfTest) ParseArgs(string[] args)
    {
        ArgumentNullException.ThrowIfNull(args);
        string? configPath = null;
        for (int i = 0; i < args.Length - 1; i++)
            if (args[i] == "--config") configPath = args[i + 1];

        ExtractorConfig cfg = SandalConfigLoader.Load(configPath);
        string pakDir = cfg.ClientPakDir, usmap = cfg.UsmapFilePath, aesKey = DefaultAesKeyHex;
        string outSqlite = cfg.ContentDbPath, outNav = cfg.NavmeshDir, cmd = "extract-all";
        bool selfTest = false;
        for (int i = 0; i < args.Length; i++)
        {
            string arg = args[i];
            string NextVal() => i + 1 < args.Length ? args[++i] : throw new ArgumentException($"Missing value for {arg}");
            switch (arg)
            {
                case "--self-test": selfTest = true; break;
                case "--config": _ = NextVal(); break;
                case "--pak-dir" or "--paks": pakDir = NextVal(); break;
                case "--usmap": usmap = NextVal(); break;
                case "--aes-key": aesKey = NextVal(); break;
                case "--out-sqlite": outSqlite = NextVal(); break;
                case "--out-nav": outNav = NextVal(); break;
                case "--out" or "--out-pmtiles" or "--max-zoom" or "--out-dir": _ = NextVal(); break;
                default:
                    if (arg.StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException($"Unknown option: {arg}");
                    cmd = arg;
                    break;
            }
        }
        return (new ExtractorOptions(pakDir, usmap, aesKey, outSqlite, outNav, cmd), selfTest);
    }

    public static Task RunAllSelfTests(CancellationToken ct = default) => RunSelfTestsAsync(ct);

    public static async Task RunSelfTestsAsync(CancellationToken ct = default)
    {
        TestUsmapDowngradeUncompressedAndZstd();
        TestUsmapPassthroughOpenAndNegatives();
        await TestPipelineChannelAndAesValidationAsync(ct).ConfigureAwait(false);
        TestCliAndConfigRoundTrip();
        ExtractDbCommand.RunSelfTests();
        ExtractVmapCommand.RunSelfTests();
        ExtractNavCommand.RunSelfTests();
        ExtractTilesCommand.RunSelfTest();
        ArtifactStampManifest.RunSelfTests();
        VerifyStandinCommand.RunSelfTests();
        string tmpOut = Path.Combine(Path.GetTempPath(), $"aoc_extract_db_{Guid.NewGuid():N}.db");
        string tmpNavDir = Path.Combine(Path.GetTempPath(), $"aoc_extract_nav_{Guid.NewGuid():N}");
        string sidecar = ExtractDbCommand.ResolveCatalogOutputPath(tmpOut);
        try
        {
            int rows = await ExtractDbCommand.ExecuteAsync(new ExtractorOptions("", "", DefaultAesKeyHex, tmpOut, "", "extract-db"), ct).ConfigureAwait(false);
            int vmapTiles = await ExtractVmapCommand.ExecuteAsync(new ExtractorOptions("", "", DefaultAesKeyHex, "", tmpNavDir, "extract-vmap"), ct).ConfigureAwait(false);
            int navTiles = await ExtractNavCommand.ExecuteAsync(new ExtractorOptions("", "", DefaultAesKeyHex, "", tmpNavDir, "extract-nav"), ct).ConfigureAwait(false);
            Assert(rows > 0 && File.Exists(sidecar) && vmapTiles > 0 && navTiles > 0, "ExtractDb/Vmap/Nav ExecuteAsync exports");
        }
        finally
        {
            if (File.Exists(sidecar)) File.Delete(sidecar);
            if (Directory.Exists(tmpNavDir)) Directory.Delete(tmpNavDir, recursive: true);
        }
        Console.WriteLine("[SELF-TEST] PASS: all 16 semantic test suites succeeded.");
    }

    private static byte[] BuildSyntheticPayload(bool includeV4EnumHeader)
    {
        using var ms = new MemoryStream();
        using var bw = new BinaryWriter(ms, Encoding.UTF8, leaveOpen: true);
        bw.Write(1u); bw.Write((ushort)4); bw.Write("Test"u8); bw.Write(2u);
        if (includeV4EnumHeader) { bw.Write((byte)1); bw.Write((byte)0); }
        bw.Write(0xCAFEBABEu); bw.Write(0xDEADBEEFu); bw.Flush();
        return ms.ToArray();
    }

    private static byte[] BuildSyntheticUsmap(byte version, byte hasVersion, byte method, ReadOnlySpan<byte> rawPayload)
    {
        byte[] compPayload = method == 3 ? new Compressor().Wrap(rawPayload).ToArray() : rawPayload.ToArray();
        int hdrLen = hasVersion == 0 ? UsmapCompat.V3HeaderLength : UsmapCompat.V4VersionedHeaderLength;
        byte[] buf = new byte[hdrLen + compPayload.Length];
        BinaryPrimitives.WriteUInt16LittleEndian(buf.AsSpan(0, 2), UsmapCompat.Magic);
        buf[2] = version; buf[3] = hasVersion;
        int meta = hasVersion == 0 ? 4 : 12;
        buf[meta] = method;
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(meta + 1, 4), (uint)compPayload.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(buf.AsSpan(meta + 5, 4), (uint)rawPayload.Length);
        compPayload.CopyTo(buf.AsSpan(hdrLen));
        return buf;
    }

    private static void TestUsmapDowngradeUncompressedAndZstd()
    {
        byte[] v4Payload = BuildSyntheticPayload(true), expectedV3Payload = BuildSyntheticPayload(false);
        foreach (byte hasVer in new byte[] { 0, 1 })
        {
            byte[] outV3 = UsmapCompat.DowngradeToV3(BuildSyntheticUsmap(4, hasVer, 0, v4Payload));
            Assert(BinaryPrimitives.ReadUInt16LittleEndian(outV3.AsSpan(0, 2)) == UsmapCompat.Magic, "v3 magic");
            Assert(outV3[2] == 3 && outV3[3] == 0 && outV3[4] == 0, "v3 header fields");
            Assert(outV3.AsSpan(UsmapCompat.V3HeaderLength).SequenceEqual(expectedV3Payload), "v3 stripped payload");
        }
        byte[] outZstdV3 = UsmapCompat.DowngradeToV3(BuildSyntheticUsmap(4, 1, 3, v4Payload));
        Assert(outZstdV3[2] == 3 && outZstdV3[4] == 0 && outZstdV3.AsSpan(UsmapCompat.V3HeaderLength).SequenceEqual(expectedV3Payload), "zstd v3");
    }

    private static void TestUsmapPassthroughOpenAndNegatives()
    {
        byte[] v3Payload = BuildSyntheticPayload(false), v3Input = BuildSyntheticUsmap(3, 0, 0, v3Payload);
        Assert(ReferenceEquals(v3Input, UsmapCompat.DowngradeToV3(v3Input)), "v3 passthrough reference identity");
        string tmp = Path.Combine(Path.GetTempPath(), $"usmap_test_{Guid.NewGuid():N}.usmap");
        try
        {
            File.WriteAllBytes(tmp, BuildSyntheticUsmap(4, 0, 0, BuildSyntheticPayload(true)));
            using MemoryStream ms = UsmapCompat.Open(tmp);
            Assert(ms.Position == 0 && ms.Length == UsmapCompat.V3HeaderLength + v3Payload.Length, "UsmapCompat.Open stream");
        }
        finally { if (File.Exists(tmp)) File.Delete(tmp); }

        AssertThrows<InvalidDataException>(() => UsmapCompat.DowngradeToV3(new byte[10]), "too short < 15");
        byte[] badMagic = BuildSyntheticUsmap(4, 0, 0, v3Payload); badMagic[0] = 0xFF;
        AssertThrows<InvalidDataException>(() => UsmapCompat.DowngradeToV3(badMagic), "bad magic");
        AssertThrows<InvalidDataException>(() => UsmapCompat.DowngradeToV3(BuildSyntheticUsmap(5, 0, 0, v3Payload)), "version 5");
        AssertThrows<InvalidDataException>(() => UsmapCompat.DowngradeToV3(BuildSyntheticUsmap(4, 0, 0, [1, 0, 0, 0, 2, 0])), "truncated payload");
    }

    private static async Task TestPipelineChannelAndAesValidationAsync(CancellationToken ct)
    {
        var ch = IoStoreExtractionPipeline.CreateWorkChannel(2);
        Assert(ch.Writer.TryWrite(new AssetWorkItem("a", "a")) && ch.Writer.TryWrite(new AssetWorkItem("b", "b")), "writes 1..2");
        Assert(!ch.Writer.TryWrite(new AssetWorkItem("c", "c")), "bounded capacity FullMode.Wait backpressure");
        Assert(IoStoreExtractionPipeline.CreateWorkChannel().Writer.TryWrite(new AssetWorkItem("x", "x")), "default 4096 channel");

        var items = Enumerable.Range(0, 32).Select(static i => new AssetWorkItem($"/Game/Item_{i}.uasset", $"Item_{i}", i)).ToList();
        var received = new List<int>();
        int count = await IoStoreExtractionPipeline.ProcessWorkItemsAsync(items, (item, _) =>
        {
            lock (received) received.Add(item.Priority);
            return ValueTask.CompletedTask;
        }, IoStoreExtractionPipeline.DefaultChannelCapacity, ct).ConfigureAwait(false);
        Assert(count == 32 && received.SequenceEqual(Enumerable.Range(0, 32)), "ProcessWorkItemsAsync count and ordering");

        byte[] key = IoStoreExtractionPipeline.ValidateAesKey(DefaultAesKeyHex);
        Assert(key.Length == 32 && key.All(static b => b == 0), "valid 0x 64-hex AES key");
        AssertThrows<ArgumentException>(() => IoStoreExtractionPipeline.ValidateAesKey("0x1234"), "short AES key");
        AssertThrows<ArgumentException>(() => IoStoreExtractionPipeline.ValidateAesKey(new string('Z', 64)), "non-hex AES key");
    }

    private static void TestCliAndConfigRoundTrip()
    {
        string tmpEnv = Path.Combine(Path.GetTempPath(), $"sandal_env_{Guid.NewGuid():N}.env");
        try
        {
            File.WriteAllText(tmpEnv, "SANDAL_CLIENT_PAK_DIR=/tmp/paks\nSANDAL_USMAP_PATH=/tmp/test.usmap\n");
            ExtractorConfig cfg = SandalConfigLoader.Load(tmpEnv);
            Assert(cfg.ConfigFileLoaded && cfg.ClientPakDir == "/tmp/paks" && cfg.UsmapFilePath == "/tmp/test.usmap", "SandalConfigLoader round-trip");
            Assert(SandalConfigLoader.RedactDsn("postgres://u:secret@localhost/db").Contains("***REDACTED***", StringComparison.Ordinal), "RedactDsn");

            (ExtractorOptions opts, bool selfTest) = ParseArgs([
                "--config", tmpEnv, "--pak-dir", "/custom/paks", "--usmap", "/custom/map.usmap",
                "--aes-key", DefaultAesKeyHex, "--out-sqlite", "/out/content.db", "--out-nav", "/out/nav", "--self-test"
            ]);
            Assert(selfTest && opts.PakDir == "/custom/paks" && opts.UsmapPath == "/custom/map.usmap", "ParseArgs overrides");
            Assert(opts.OutSqlitePath == "/out/content.db" && opts.OutNavDir == "/out/nav", "ParseArgs output paths");
        }
        finally { if (File.Exists(tmpEnv)) File.Delete(tmpEnv); }
    }

    private static void Assert(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException($"Self-test assertion failed: {message}");
    }

    private static void AssertThrows<TException>(Action action, string message) where TException : Exception
    {
        try { action(); } catch (TException) { return; }
        throw new InvalidOperationException($"Expected {typeof(TException).Name} was not thrown: {message}");
    }
}
