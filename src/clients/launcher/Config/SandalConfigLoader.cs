// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text.Json;

namespace SandalCore.Config;

public sealed record LauncherConfig(string ClientPakDir, string GatewayHttpAddr, string GatewayIcsAddr, string GatewayGrpcAddr, string AdminHttpAddr, string AdminHttpsAddr, bool ClientUseEos, string ConfigFilePath, bool ConfigFileLoaded);

public static class SandalConfigLoader
{
    private static readonly string[] ConfigRelPaths =
    [
        Path.Combine("src", "config", "server.local.json"),
        Path.Combine("dist", "config", "server.local.json"),
        Path.Combine("config", "server.local.json"),
        Path.Combine("src", "config", "server.json"),
        Path.Combine("dist", "config", "server.json"),
        Path.Combine("config", "server.json")
    ];

    private static readonly string[] CertRelPaths =
    [
        Path.Combine("dist", "certs", "server.pem"),
        Path.Combine("deploy", "certs", "server.pem"),
        Path.Combine("certs", "server.pem")
    ];
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

    private static readonly HashSet<string> PlaceholderHosts = new(StringComparer.OrdinalIgnoreCase)
    {
        "0.0.0.0",
        "127.0.0.1",
        "::1",
        "localhost",
        "gateway",
        "aoc-gateway",
        "sandalcore-gateway",
        "host.docker.internal",
        "host.containers.internal",
        "dev.localhost",
        "dev.internal",
        "local",
        "internal"
    };

    public static bool IsLoopbackOrPlaceholderHost(string? host)
    {
        if (string.IsNullOrWhiteSpace(host)) return true;
        string h = host.Trim().TrimEnd('.');
        return h.StartsWith("*.", StringComparison.Ordinal) ||
               h.Contains('*', StringComparison.Ordinal) ||
               PlaceholderHosts.Contains(h);
    }

    public static string? NormalizeCertDnsCandidate(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return null;
        string h = raw.Trim().TrimEnd('.');
        if (h.StartsWith("*.", StringComparison.Ordinal))
        {
            string suffix = h[2..].Trim();
            if (string.IsNullOrEmpty(suffix) || PlaceholderHosts.Contains(suffix) || IPAddress.TryParse(suffix, out _))
                return null;
            return $"localhost.{suffix}";
        }
        return IsLoopbackOrPlaceholderHost(h) ? null : h;
    }

    public static string ResolveLocalIcsHost(string? repoRoot = null)
    {
        List<string> roots = GetSearchRoots(repoRoot);
        foreach (string root in roots)
        {
            string? fromConfig = TryResolveHostFromConfigs(root);
            if (!string.IsNullOrEmpty(fromConfig)) return fromConfig;
        }
        foreach (string root in roots)
        {
            string? fromCert = TryResolveHostFromCerts(root);
            if (!string.IsNullOrEmpty(fromCert)) return fromCert;
        }
        return "127.0.0.1";
    }

    private static List<string> GetSearchRoots(string? repoRoot)
    {
        var roots = new List<string>(8);
        if (!string.IsNullOrWhiteSpace(repoRoot))
        {
            roots.Add(Path.GetFullPath(repoRoot.Trim()));
            return roots;
        }
        AddAncestorRoots(roots, Directory.GetCurrentDirectory());
        AddAncestorRoots(roots, AppContext.BaseDirectory);
        return roots;
    }

    private static void AddAncestorRoots(List<string> roots, string? startDir)
    {
        string? current = string.IsNullOrWhiteSpace(startDir) ? null : Path.GetFullPath(startDir);
        for (int depth = 0; depth < 5 && !string.IsNullOrEmpty(current); depth++)
        {
            if (!roots.Contains(current, StringComparer.OrdinalIgnoreCase))
                roots.Add(current);
            current = Directory.GetParent(current)?.FullName;
        }
    }

    private static string? TryResolveHostFromConfigs(string root)
    {
        foreach (string rel in ConfigRelPaths)
        {
            string? host = TryReadIcsHostFromJson(Path.Combine(root, rel));
            if (!string.IsNullOrEmpty(host)) return host;
        }
        return null;
    }

    private static string? TryReadIcsHostFromJson(string filePath)
    {
        if (!File.Exists(filePath)) return null;
        try
        {
            using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(filePath));
            if (doc.RootElement.ValueKind == JsonValueKind.Object &&
                doc.RootElement.TryGetProperty("ics_host", out JsonElement prop) &&
                prop.ValueKind == JsonValueKind.String &&
                NormalizeCertDnsCandidate(prop.GetString()) is { Length: > 0 } val)
            {
                return val;
            }
        }
        catch (Exception ex) when (ex is IOException or JsonException or UnauthorizedAccessException) { }
        return null;
    }

    private static string? TryResolveHostFromCerts(string root)
    {
        foreach (string rel in CertRelPaths)
        {
            string? host = TryExtractCertDnsHost(Path.Combine(root, rel));
            if (!string.IsNullOrEmpty(host)) return host;
        }
        return null;
    }

    public static string? TryExtractCertDnsHost(string certPath)
    {
        if (string.IsNullOrWhiteSpace(certPath) || !File.Exists(certPath)) return null;
        try
        {
            using X509Certificate2 cert = X509CertificateLoader.LoadCertificateFromFile(certPath);
            string? sanHost = TryExtractSanDnsHost(cert);
            if (!string.IsNullOrEmpty(sanHost)) return sanHost;
            string? altDns = NormalizeCertDnsCandidate(cert.GetNameInfo(X509NameType.DnsFromAlternativeName, forIssuer: false));
            if (!string.IsNullOrEmpty(altDns)) return altDns;
            string? simple = NormalizeCertDnsCandidate(cert.GetNameInfo(X509NameType.SimpleName, forIssuer: false));
            if (!string.IsNullOrEmpty(simple)) return simple;
        }
        catch (Exception ex) when (ex is CryptographicException or IOException or UnauthorizedAccessException) { }
        return null;
    }

    private static string? TryExtractSanDnsHost(X509Certificate2 cert)
    {
        foreach (X509Extension ext in cert.Extensions)
        {
            if (ext is not X509SubjectAlternativeNameExtension san) continue;
            foreach (string dns in san.EnumerateDnsNames())
            {
                string? normalized = NormalizeCertDnsCandidate(dns);
                if (!string.IsNullOrEmpty(normalized)) return normalized;
            }
        }
        return null;
    }

    public static string NormalizeIcsEndpoint(string? raw, string? repoRoot = null)
    {
        string defaultHost = ResolveLocalIcsHost(repoRoot);
        if (string.IsNullOrWhiteSpace(raw)) return $"{defaultHost}:50061";
        string trimmed = raw.Trim();
        if (trimmed.Equals("127.0.0.1:8444", StringComparison.OrdinalIgnoreCase) ||
            trimmed.Equals("localhost:8444", StringComparison.OrdinalIgnoreCase) ||
            trimmed.Equals("127.0.0.1:50061", StringComparison.OrdinalIgnoreCase) ||
            trimmed.Equals("localhost:50061", StringComparison.OrdinalIgnoreCase))
            return $"{defaultHost}:50061";
        int colon = trimmed.LastIndexOf(':');
        if (colon > 0)
        {
            string host = trimmed[..colon];
            string port = trimmed[(colon + 1)..];
            if (port == "8444") port = "50061";
            if (IsLoopbackOrPlaceholderHost(host))
                return $"{defaultHost}:{port}";
        }
        return trimmed;
    }

    public static LauncherConfig Load(string? explicitEnvFilePath = null)
    {
        string envPath = string.IsNullOrWhiteSpace(explicitEnvFilePath) ? Resolve(new Dictionary<string, string>(), "CONFIG_FILE", Path.Combine("config", "sandal.env")) : explicitEnvFilePath;
        var vars = ReadOptionalEnvFile(envPath);
        string eos = Resolve(vars, "CLIENT_USE_EOS", "false");
        string defaultIcs = $"{ResolveLocalIcsHost()}:50061";
        return new LauncherConfig(
            Resolve(vars, "CLIENT_PAK_DIR", @"C:\Program Files\Intrepid Studios\Ashes of Creation\AOC\Content\Paks"),
            Resolve(vars, "GATEWAY_HTTP_ADVERTISE_ADDR", "127.0.0.1:8080"),
            NormalizeIcsEndpoint(Resolve(vars, "GATEWAY_ICS_ADVERTISE_ADDR", defaultIcs)),
            Resolve(vars, "GATEWAY_GRPC_ADVERTISE_ADDR", "127.0.0.1:50050"),
            Resolve(vars, "ADMIN_HTTP_ADVERTISE_ADDR", "127.0.0.1:8090"),
            Resolve(vars, "ADMIN_HTTPS_ADVERTISE_ADDR", "127.0.0.1:8443"),
            eos == "1" || string.Equals(eos, "true", StringComparison.OrdinalIgnoreCase), envPath, File.Exists(envPath));
    }

    public static LauncherConfig LoadLauncher(string? explicitEnvFilePath = null) => Load(explicitEnvFilePath);
}
