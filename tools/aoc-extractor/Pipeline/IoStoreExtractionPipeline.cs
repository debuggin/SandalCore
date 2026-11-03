// SPDX-License-Identifier: AGPL-3.0-only
using System.Threading.Channels;
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;

namespace AocExtractor.Pipeline;

public sealed record AssetWorkItem(string VirtualPath, string PackagePath, int Priority = 0);

public sealed record ExtractorOptions(string PakDir, string UsmapPath, string AesKeyHex, string OutSqlitePath, string OutNavDir, string Command = "extract-all");

public sealed class IoStoreExtractionPipeline
{
    public const int DefaultChannelCapacity = 4096;

    public static Channel<AssetWorkItem> CreateWorkChannel(int capacity = DefaultChannelCapacity)
    {
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(capacity, 0);
        return Channel.CreateBounded<AssetWorkItem>(new BoundedChannelOptions(capacity)
        {
            FullMode = BoundedChannelFullMode.Wait,
            SingleWriter = false,
            SingleReader = false
        });
    }

    public static byte[] ValidateAesKey(string hex)
    {
        if (string.IsNullOrWhiteSpace(hex)) throw new ArgumentException("AES key hex string cannot be empty.", nameof(hex));
        ReadOnlySpan<char> span = hex.AsSpan().Trim();
        if (span.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) span = span[2..];
        if (span.Length != 64) throw new ArgumentException("AES-256 key must be 64 hex chars (32 bytes).", nameof(hex));
        for (int i = 0; i < span.Length; i++)
            if (!Uri.IsHexDigit(span[i])) throw new ArgumentException($"Invalid hex char '{span[i]}' at index {i}.", nameof(hex));
        return Convert.FromHexString(span);
    }

    public static async Task<int> ProcessWorkItemsAsync(
        IEnumerable<AssetWorkItem> items,
        Func<AssetWorkItem, CancellationToken, ValueTask> consumer,
        int capacity = DefaultChannelCapacity,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(items);
        ArgumentNullException.ThrowIfNull(consumer);
        Channel<AssetWorkItem> channel = CreateWorkChannel(capacity);
        Task producerTask = Task.Run(async () =>
        {
            try
            {
                foreach (AssetWorkItem item in items) await channel.Writer.WriteAsync(item, ct).ConfigureAwait(false);
                channel.Writer.TryComplete();
            }
            catch (Exception ex) { channel.Writer.TryComplete(ex); throw; }
        }, ct);

        int processed = 0;
        await foreach (AssetWorkItem item in channel.Reader.ReadAllAsync(ct).ConfigureAwait(false))
        {
            await consumer(item, ct).ConfigureAwait(false);
            Interlocked.Increment(ref processed);
        }
        await producerTask.ConfigureAwait(false);
        return processed;
    }

    public async Task<int> RunAsync(ExtractorOptions opts, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(opts);
        byte[] keyBytes = ValidateAesKey(opts.AesKeyHex);
        if (!Directory.Exists(opts.PakDir)) throw new DirectoryNotFoundException($"Pak directory not found: {opts.PakDir}");
        if (!File.Exists(opts.UsmapPath)) throw new FileNotFoundException($"Usmap file not found: {opts.UsmapPath}", opts.UsmapPath);

        string tmpUsmap = Path.Combine(Path.GetTempPath(), $"aoc_extractor_{Guid.NewGuid():N}.usmap");
        try
        {
            await File.WriteAllBytesAsync(tmpUsmap, UsmapCompat.DowngradeToV3(await File.ReadAllBytesAsync(opts.UsmapPath, ct).ConfigureAwait(false)), ct).ConfigureAwait(false);
            using var provider = new DefaultFileProvider(opts.PakDir, SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_6), StringComparer.OrdinalIgnoreCase);
            provider.MappingsContainer = new FileUsmapTypeMappingsProvider(tmpUsmap);
            provider.Initialize();
            provider.SubmitKey(new FGuid(), new FAesKey(keyBytes));
            var workItems = provider.Files.Keys
                .Where(static p => p.EndsWith(".uasset", StringComparison.OrdinalIgnoreCase) || p.EndsWith(".umap", StringComparison.OrdinalIgnoreCase))
                .Select(static p => new AssetWorkItem(p, p, 0));
            return await ProcessWorkItemsAsync(workItems, static (_, _) => ValueTask.CompletedTask, DefaultChannelCapacity, ct).ConfigureAwait(false);
        }
        finally { if (File.Exists(tmpUsmap)) File.Delete(tmpUsmap); }
    }
}
