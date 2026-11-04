// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Services;

public static class DiagnosticBundleExporter
{
    public const int MaxEntryBytes = 1024 * 1024;
    public const string DoctorReportEntryName = "doctor_report.json";
    public const string RedactedConfigEntryName = "redacted_config.json";
    public const string RedactedTailLogEntryName = "redacted_tail.log";

    private const string SecretKeyGroup = "ics_token|session_ticket|session_token|sessiontoken|access_token|accesstoken|refresh_token|refreshtoken|token|password|passwd|pwd|secret|client_secret|api_key|apikey|private_key|privatekey|credential|aoc_master_key|postgres_password|aoc_service_token|sandal_secret_key";
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static readonly Regex PemBlockRegex = new(@"-----BEGIN [A-Z0-9 ]*KEY-----[\s\S]*?-----END [A-Z0-9 ]*KEY-----", RegexOptions.Compiled);
    private static readonly Regex PemHeaderRegex = new(@"-----BEGIN [A-Z0-9 ]*KEY-----", RegexOptions.Compiled);
    private static readonly Regex AuthSchemeRegex = new(@"(?i)\b(Bearer|Basic)\s+[A-Za-z0-9\-._~+/]+=*", RegexOptions.Compiled);
    private static readonly Regex JsonSecretRegex = new($@"(?i)(""(?:{SecretKeyGroup})""\s*:\s*)""[^""]*""", RegexOptions.Compiled);
    private static readonly Regex KvSecretRegex = new($@"(?i)\b({SecretKeyGroup})(\s*[:=]\s*)(?:""[^""]*""|'[^']*'|[^\s,;\r\n}}""']+)", RegexOptions.Compiled);
    private static readonly Regex DsnSecretRegex = new(@"(?i)(\b[a-z][a-z0-9+.\-]*://[^:\s/@""']+:)([^@\s""']+)(@)", RegexOptions.Compiled);
    private static readonly Regex JwtRegex = new(@"\beyJ[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+\.[A-Za-z0-9_\-]+\b", RegexOptions.Compiled);
    private static readonly Regex HexOrBase64Regex = new(@"(?:\b(?:0x)?[0-9a-fA-F]{32,128}\b|\b[A-Za-z0-9+/]{24,}={1,2})", RegexOptions.Compiled);
    private static readonly Regex WinUserBackslashRegex = new(@"(?i)\b([A-Za-z]:\\Users\\)([^\\/\r\n""'\s]+)", RegexOptions.Compiled);
    private static readonly Regex WinUserSlashRegex = new(@"(?i)\b([A-Za-z]:/Users/)([^\\/\r\n""'\s]+)", RegexOptions.Compiled);

    public static string RedactSensitiveText(string? input)
    {
        if (string.IsNullOrEmpty(input)) return string.Empty;
        string s = PemBlockRegex.Replace(input, "[REDACTED]");
        s = PemHeaderRegex.Replace(s, "[REDACTED]");
        s = AuthSchemeRegex.Replace(s, "$1 [REDACTED]");
        s = JsonSecretRegex.Replace(s, "$1\"[REDACTED]\"");
        s = KvSecretRegex.Replace(s, "$1$2[REDACTED]");
        s = DsnSecretRegex.Replace(s, "$1[REDACTED]$3");
        s = JwtRegex.Replace(s, "[REDACTED]");
        s = HexOrBase64Regex.Replace(s, "[REDACTED]");
        s = WinUserBackslashRegex.Replace(s, "$1[REDACTED_USER]");
        return WinUserSlashRegex.Replace(s, "$1[REDACTED_USER]");
    }

    public static string SanitizeZipEntryName(string? entryName)
    {
        if (string.IsNullOrWhiteSpace(entryName)) throw new InvalidDataException("Archive entry name cannot be empty.");
        string trimmed = entryName.Trim();
        if (Path.IsPathRooted(trimmed) || trimmed.StartsWith('/') || trimmed.StartsWith('\\') || trimmed.Contains(':'))
            throw new InvalidDataException($"Rooted archive entry path '{trimmed}' is forbidden (SEC-10).");
        string[] segments = trimmed.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
        if (segments.Length == 0 || segments.Any(seg => seg is "." or ".."))
            throw new InvalidDataException($"Path traversal archive entry '{trimmed}' is forbidden (SEC-10).");
        return string.Join('/', segments);
    }

    public static void WriteRedactedEntry(ZipArchive archive, string entryName, string? rawContent)
    {
        ArgumentNullException.ThrowIfNull(archive);
        string safeName = SanitizeZipEntryName(entryName);
        if ((rawContent?.Length ?? 0) > MaxEntryBytes) throw new InvalidDataException($"Archive entry '{safeName}' exceeds 1 MiB limit ({MaxEntryBytes} bytes).");
        byte[] utf8 = Encoding.UTF8.GetBytes(RedactSensitiveText(rawContent));
        if (utf8.Length > MaxEntryBytes) throw new InvalidDataException($"Archive entry '{safeName}' ({utf8.Length} bytes) exceeds 1 MiB limit ({MaxEntryBytes} bytes).");
        using Stream entryStream = archive.CreateEntry(safeName, CompressionLevel.Optimal).Open();
        entryStream.Write(utf8, 0, utf8.Length);
    }

    public static void ExportBundleToStream(Stream destination, DoctorReport report, string? rawConfigJson, string? rawTailLog)
    {
        ArgumentNullException.ThrowIfNull(destination);
        ArgumentNullException.ThrowIfNull(report);
        using var archive = new ZipArchive(destination, ZipArchiveMode.Create, leaveOpen: true);
        WriteRedactedEntry(archive, DoctorReportEntryName, JsonSerializer.Serialize(report, JsonOptions));
        WriteRedactedEntry(archive, RedactedConfigEntryName, rawConfigJson ?? "{}");
        WriteRedactedEntry(archive, RedactedTailLogEntryName, rawTailLog ?? string.Empty);
    }

    public static async Task ExportBundleAsync(string destinationZipPath, DoctorReport report, string? rawConfigJson, string? rawTailLog, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(destinationZipPath)) throw new ArgumentException("Destination zip path cannot be empty.", nameof(destinationZipPath));
        string fullPath = Path.GetFullPath(destinationZipPath.Trim());
        if (Path.GetDirectoryName(fullPath) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        using var buffer = new MemoryStream();
        ExportBundleToStream(buffer, report, rawConfigJson, rawTailLog);
        await using var fs = new FileStream(fullPath, FileMode.Create, FileAccess.Write, FileShare.None, 4096, useAsync: true);
        await fs.WriteAsync(buffer.ToArray(), ct).ConfigureAwait(false);
        await fs.FlushAsync(ct).ConfigureAwait(false);
    }
}
