// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider;
using CUE4Parse.UE4.Assets;
using CUE4Parse.UE4.Assets.Exports;
using CUE4Parse.UE4.Objects.Core.Misc;
using CUE4Parse.UE4.Versions;
using Serilog;

namespace Mapc;

/// <summary>The game's containers opened read-only through CUE4Parse, with the usmap's mappings.</summary>
public sealed class Game
{
    public static readonly string DefaultPaks = Path.Combine(Environment.GetEnvironmentVariable("GAME_ROOT") ?? @"C:\Ashes of Creation\Game", "AOC", "Content", "Paks");   // $GAME_ROOT\AOC\Content\Paks
    public static readonly string DefaultUsmap = ResolveDefaultUsmap();   // $HOME/.aoc/aoc.usmap if present, else dist/content/aoc.usmap
    public const string MasterPath = "AOC/Content/Levels/Verra_World_Master/Verra_World_Master";

    private static string ResolveDefaultUsmap()
    {
        try
        {
            string? homeDir = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            string? userUsmap = !string.IsNullOrWhiteSpace(homeDir) ? Path.Combine(homeDir, ".aoc", "aoc.usmap") : null;
            foreach (string? candidate in new[]
            {
                Environment.GetEnvironmentVariable("SANDAL_USMAP_PATH"),
                Environment.GetEnvironmentVariable("AOC_USMAP"),
                Environment.GetEnvironmentVariable("SANDAL_USMAP_OVERRIDE_DIR"),
                Environment.GetEnvironmentVariable("AOC_USMAP_OVERRIDE_DIR"),
                userUsmap
            })
            {
                if (TryResolveUsmapCandidate(candidate, out string resolved))
                    return resolved;
            }
            if (!string.IsNullOrWhiteSpace(userUsmap) && File.Exists(userUsmap) && new FileInfo(userUsmap).Length > 0)
                return userUsmap;
            string distUsmap = Path.Combine("dist", "content", "aoc.usmap");
            if (File.Exists(distUsmap) && new FileInfo(distUsmap).Length > 0)
                return distUsmap;
            string repoUsmap = Path.Combine("src", "config", "layouts", "aoc.usmap");
            if (File.Exists(repoUsmap) && new FileInfo(repoUsmap).Length > 0)
                return repoUsmap;
        }
        catch (Exception ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
        return Path.Combine("dist", "content", "aoc.usmap");
    }

    private static bool TryResolveUsmapCandidate(string? candidate, out string resolved)
    {
        resolved = string.Empty;
        if (string.IsNullOrWhiteSpace(candidate)) return false;
        string trimmed = candidate.Trim();
        if (File.Exists(trimmed) && new FileInfo(trimmed).Length > 0)
        {
            resolved = trimmed;
            return true;
        }
        if (!Directory.Exists(trimmed)) return false;
        string canonical = Path.Combine(trimmed, "aoc.usmap");
        if (File.Exists(canonical) && new FileInfo(canonical).Length > 0)
        {
            resolved = canonical;
            return true;
        }
        FileInfo? newest = new DirectoryInfo(trimmed)
            .GetFiles("*.usmap")
            .Where(f => f.Length > 0)
            .OrderByDescending(f => f.LastWriteTimeUtc)
            .FirstOrDefault();
        if (newest is null) return false;
        resolved = newest.FullName;
        return true;
    }

    public readonly DefaultFileProvider Provider;
    readonly Dictionary<string, IPackage?> _cache = new();

    public Game(string paks, string usmap)
    {
        Log.Logger = new LoggerConfiguration().MinimumLevel.Fatal().WriteTo.Console().CreateLogger();
        // CUE4Parse 1.2.2 reads usmap versions up to 3; the game's is 4 (see UsmapCompat)
        var tmp = Path.Combine(Path.GetTempPath(), $"mapc_{Guid.NewGuid():N}.usmap");
        try
        {
            File.WriteAllBytes(tmp, UsmapCompat.DowngradeToV3(File.ReadAllBytes(usmap)));
            Provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, new VersionContainer(EGame.GAME_UE5_6), StringComparer.OrdinalIgnoreCase);
            Provider.MappingsContainer = new FileUsmapTypeMappingsProvider(tmp);
            Provider.Initialize();
            Provider.SubmitKey(new FGuid(), new FAesKey(new byte[32]));   // the containers are not encrypted: the all-zero key mounts them
        }
        finally { File.Delete(tmp); }
    }

    public IPackage? Load(string path)
    {
        if (_cache.TryGetValue(path, out var p)) return p;
        try { p = Provider.LoadPackage(path); } catch (Exception) { p = null; }
        _cache[path] = p;
        return p;
    }

    public void Forget(string path) => _cache.Remove(path);
}
