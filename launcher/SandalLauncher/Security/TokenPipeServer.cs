// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.IO.Pipes;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Security;

public sealed class TokenPipeServer : IAsyncDisposable, IDisposable
{
    public const int MaxTokenBytes = 4096;
    private readonly NamedPipeServerStream _pipe;

    public TokenPipeServer(string? pipeName = null)
    {
        PipeName = string.IsNullOrWhiteSpace(pipeName) ? GeneratePipeName() : pipeName.Trim();
        _pipe = new NamedPipeServerStream(PipeName, PipeDirection.Out, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
    }

    public string PipeName { get; }
    public string PipePath => $@"\\.\pipe\{PipeName}";
    public string CliArgument => $"-AuthTokenPipe={PipePath}";
    public static string GeneratePipeName() => $"SandalAuth_{Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant()}";

    public static bool IsValidPrintableAsciiToken(ReadOnlySpan<char> token)
    {
        ReadOnlySpan<char> trimmed = token.Trim();
        if (trimmed.IsEmpty || trimmed.Length > MaxTokenBytes) return false;
        foreach (char c in trimmed) if (c is < (char)0x21 or > (char)0x7E) return false;
        return true;
    }

    public async Task<bool> ServeTokenOnceAsync(string token, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(token) || !IsValidPrintableAsciiToken(token))
            throw new ArgumentException("Token must be non-empty printable ASCII within 4096 bytes.", nameof(token));
        byte[] utf8 = Encoding.UTF8.GetBytes(token.Trim());
        try
        {
            await _pipe.WaitForConnectionAsync(cancellationToken).ConfigureAwait(false);
            await _pipe.WriteAsync(utf8, cancellationToken).ConfigureAwait(false);
            await _pipe.FlushAsync(cancellationToken).ConfigureAwait(false);
            return true;
        }
        finally { CryptographicOperations.ZeroMemory(utf8); }
    }

    public static bool WriteOwnerOnlyTokenFile(string path, string token)
    {
        if (string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(token) || !IsValidPrintableAsciiToken(token)) return false;
        string fullPath = Path.GetFullPath(path), tmpPath = $"{fullPath}.tmp";
        if (Path.GetDirectoryName(fullPath) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        byte[] utf8 = Encoding.UTF8.GetBytes(token.Trim());
        try
        {
            using (var fs = new FileStream(tmpPath, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
            { fs.Write(utf8, 0, utf8.Length); fs.Flush(flushToDisk: true); }
            if (!OperatingSystem.IsWindows()) File.SetUnixFileMode(tmpPath, UnixFileMode.UserRead | UnixFileMode.UserWrite);
            File.Move(tmpPath, fullPath, overwrite: true);
            return true;
        }
        catch (IOException)
        {
            if (File.Exists(tmpPath)) File.Delete(tmpPath);
            return false;
        }
        finally { CryptographicOperations.ZeroMemory(utf8); }
    }

    public static string ReadOwnerOnlyTokenFile(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return string.Empty;
        byte[] raw = File.ReadAllBytes(Path.GetFullPath(path));
        if (raw.Length > MaxTokenBytes) return string.Empty;
        string trimmed = Encoding.UTF8.GetString(raw).TrimStart('\uFEFF').Trim();
        return IsValidPrintableAsciiToken(trimmed) ? trimmed : string.Empty;
    }

    public void Dispose() => _pipe.Dispose();
    public ValueTask DisposeAsync() => _pipe.DisposeAsync();
}
