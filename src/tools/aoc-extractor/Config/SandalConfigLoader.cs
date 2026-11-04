// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.IO;

namespace SandalCore.Config;

public sealed record ExtractorConfig(string ClientPakDir, string UsmapFilePath, string ExtractedOutDir, string ContentDbPath, string PmtilesOutputPath, string NavmeshDir, string VmapOutputPath, int MaxConcurrency, string ConfigFilePath, bool ConfigFileLoaded);

public static class SandalConfigLoader
{
    public static Dictionary<string, string> ReadOptionalEnvFile(string path)
    {
        var vars = new Dictionary<string, string>(StringComparer.Ordinal);
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return vars;
        foreach (string raw in File.ReadLines(path))
        {
            ReadOnlySpan<char> line = raw.AsSpan().Trim();
            if (!line.IsEmpty && line[0] != '#' && (line.StartsWith("export ", StringComparison.Ordinal) ? line[7..].Trim() : line) is var s && s.IndexOf('=') is > 0 and int eq && s[(eq + 1)..].Trim() is var v)
                vars[s[..eq].Trim().ToString()] = (v.Length >= 2 && ((v[0] == '"' && v[^1] == '"') || (v[0] == '\'' && v[^1] == '\'')) ? v[1..^1] : v).ToString();
        }
        return vars;
    }

    public static string RedactDsn(string dsn)
    {
        if (string.IsNullOrEmpty(dsn)) return dsn;
        if (dsn.IndexOf("://", StringComparison.Ordinal) is >= 0 and int s && dsn.IndexOf('@', s + 3) is int at && at > s + 3 &&
            (dsn.IndexOfAny(['/', '?'], s + 3) is int sl && (sl < 0 || at < sl)) && dsn.IndexOf(':', s + 3, at - s - 3) is >= 0 and int c && c + 1 < at)
            dsn = string.Concat(dsn.AsSpan(0, c + 1), "***REDACTED***", dsn.AsSpan(at));
        if (dsn.IndexOf("password=", StringComparison.Ordinal) is >= 0 and int pw && pw + 9 is int p &&
            (dsn.IndexOfAny([' ', '\t', '&', ';'], p) is >= 0 and int e ? e : dsn.Length) is int end && end > p)
            dsn = string.Concat(dsn.AsSpan(0, p), "***REDACTED***", dsn.AsSpan(end));
        return dsn;
    }

    private static string Resolve(IReadOnlyDictionary<string, string> envFile, string suffix, string fallback) =>
        Environment.GetEnvironmentVariable("SANDAL_" + suffix) is { Length: > 0 } v1 ? v1 :
        Environment.GetEnvironmentVariable("AOC_" + suffix) is { Length: > 0 } v2 ? v2 :
        envFile.TryGetValue("SANDAL_" + suffix, out string? v3) && !string.IsNullOrEmpty(v3) ? v3 :
        envFile.TryGetValue("AOC_" + suffix, out string? v4) && !string.IsNullOrEmpty(v4) ? v4 : fallback;

    public static ExtractorConfig Load(string? explicitEnvFilePath = null)
    {
        string envPath = string.IsNullOrWhiteSpace(explicitEnvFilePath) ? Resolve(new Dictionary<string, string>(), "CONFIG_FILE", Path.Combine("config", "sandal.env")) : explicitEnvFilePath;
        var vars = ReadOptionalEnvFile(envPath);
        int threads = int.TryParse(Resolve(vars, "EXTRACTOR_THREADS", "0"), out int n) && n > 0 ? n : Environment.ProcessorCount;
        return new ExtractorConfig(
            Resolve(vars, "CLIENT_PAK_DIR", @"C:\Program Files\Intrepid Studios\Ashes of Creation\AOC\Content\Paks"),
            Resolve(vars, "USMAP_PATH", Path.Combine("data", "mappings", "<mappings>.usmap")),
            Resolve(vars, "EXTRACTED_DIR", Path.Combine("dist", "extracted")),
            Resolve(vars, "CONTENT_DB_PATH", Resolve(vars, "CONTENT_DB", Path.Combine("dist", "content", "content.db"))),
            Resolve(vars, "PMTILES_PATH", Path.Combine("dist", "content", "verra.pmtiles")),
            Resolve(vars, "NAVMESH_DIR", Path.Combine("dist", "nav")),
            Resolve(vars, "VMAP_PATH", Path.Combine("dist", "nav", "area_vmap.bin")), threads, envPath, File.Exists(envPath));
    }

    public static ExtractorConfig LoadExtractor(string? explicitEnvFilePath = null) => Load(explicitEnvFilePath);
}
