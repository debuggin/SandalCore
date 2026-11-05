// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SandalLauncher.Models;
using SandalLauncher.Security;

namespace SandalLauncher.Services;

public sealed class ClientLaunchService
{
    public const string ShippingExeName = "AOCClient-Win64-Shipping.exe";
    public const string BootstrapExeName = "AOCClient.exe";
    public const string DefaultIcsTokenRelativePath = "data/ics_auth_token.txt";

    private static readonly string[] RelativeCandidates =
    [
        ShippingExeName, BootstrapExeName,
        Path.Combine("Binaries", "Win64", ShippingExeName), Path.Combine("Binaries", "Win64", BootstrapExeName),
        Path.Combine("AOC", "Binaries", "Win64", ShippingExeName), Path.Combine("AOC", "Binaries", "Win64", BootstrapExeName),
        Path.Combine("Game", "AOC", "Binaries", "Win64", ShippingExeName), Path.Combine("Game", "AOC", "Binaries", "Win64", BootstrapExeName)
    ];

    public static string ResolveClientExecutable(string clientPathOrDirectory)
    {
        if (string.IsNullOrWhiteSpace(clientPathOrDirectory)) throw new ArgumentException("Client path cannot be empty.", nameof(clientPathOrDirectory));
        string canonical = Path.GetFullPath(clientPathOrDirectory.Trim());
        if (File.Exists(canonical))
        {
            if (string.Equals(Path.GetFileName(canonical), BootstrapExeName, StringComparison.OrdinalIgnoreCase) &&
                Path.GetDirectoryName(canonical) is { Length: > 0 } dir && Path.Combine(dir, ShippingExeName) is var ship && File.Exists(ship))
                return Path.GetFullPath(ship);
            return canonical;
        }
        if (Directory.Exists(canonical))
        {
            string? p1 = Directory.GetParent(canonical)?.FullName, p2 = p1 is not null ? Directory.GetParent(p1)?.FullName : null;
            foreach (string? root in new[] { canonical, p1, p2 })
                if (!string.IsNullOrEmpty(root))
                    foreach (string rel in RelativeCandidates)
                        if (Path.Combine(root, rel) is var candidate && File.Exists(candidate))
                            return Path.GetFullPath(candidate);
        }
        throw new FileNotFoundException($"UE5 client executable not found under '{canonical}'.", canonical);
    }

    public static ProcessStartInfo BuildLaunchStartInfo(LauncherProfile profile, int tetherPort, string authTokenPipePath)
    {
        ArgumentNullException.ThrowIfNull(profile);
        return BuildLaunchStartInfo(profile.ClientPakDir, tetherPort, authTokenPipePath, profile.UseEos);
    }

    public static ProcessStartInfo BuildLaunchStartInfo(string clientPath, int tetherPort, string authTokenPipePath, bool useEos = false)
    {
        if (string.IsNullOrWhiteSpace(clientPath)) throw new ArgumentException("Client path cannot be empty.", nameof(clientPath));
        if (tetherPort is <= 0 or > 65535) throw new ArgumentOutOfRangeException(nameof(tetherPort));
        if (string.IsNullOrWhiteSpace(authTokenPipePath)) throw new ArgumentException("Pipe path cannot be empty.", nameof(authTokenPipePath));
        string trimmedPipe = authTokenPipePath.Trim();
        string pipePath = trimmedPipe.StartsWith(@"\\.\pipe\", StringComparison.OrdinalIgnoreCase) ? trimmedPipe : $@"\\.\pipe\{trimmedPipe}";
        if (!pipePath.StartsWith(@"\\.\pipe\SandalAuth_", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("AuthTokenPipe must reference an ephemeral SandalAuth_ named pipe.", nameof(authTokenPipePath));
        string full = Path.GetFullPath(clientPath.Trim());
        string exe = File.Exists(full) || Directory.Exists(full) ? ResolveClientExecutable(full) : full;
        var psi = new ProcessStartInfo(exe) { WorkingDirectory = Path.GetDirectoryName(exe) ?? string.Empty, UseShellExecute = false };
        foreach (string arg in new[] { $"-LauncherTetherPort={tetherPort}", $"-AuthTokenPipe={pipePath}", $"-UseEOS={(useEos ? "true" : "false")}", "-NoEAC" })
            psi.ArgumentList.Add(arg);
        return psi;
    }

    public static async Task<string> WriteIcsAuthTokenFileAsync(
        string accessToken,
        string? tokenFilePath = null,
        CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if (string.IsNullOrWhiteSpace(accessToken) || !TokenPipeServer.IsValidPrintableAsciiToken(accessToken))
            throw new ArgumentException("Access token must be non-empty printable ASCII.", nameof(accessToken));
        string targetPath = Path.GetFullPath(string.IsNullOrWhiteSpace(tokenFilePath) ? DefaultIcsTokenRelativePath : tokenFilePath.Trim());
        if (Path.GetDirectoryName(targetPath) is { Length: > 0 } dir)
            Directory.CreateDirectory(dir);
        string tempPath = $"{targetPath}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        byte[] utf8 = Encoding.UTF8.GetBytes(accessToken.Trim());
        try
        {
            await using (var fs = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await fs.WriteAsync(utf8, ct).ConfigureAwait(false);
                await fs.FlushAsync(ct).ConfigureAwait(false);
                fs.Flush(flushToDisk: true);
            }
            if (!OperatingSystem.IsWindows())
            {
                try { File.SetUnixFileMode(tempPath, UnixFileMode.UserRead | UnixFileMode.UserWrite); }
                catch (PlatformNotSupportedException) { }
            }
            File.Move(tempPath, targetPath, overwrite: true);
            return targetPath;
        }
        finally
        {
            CryptographicOperations.ZeroMemory(utf8);
            try { if (File.Exists(tempPath)) File.Delete(tempPath); } catch (IOException) { }
        }
    }

    public static IReadOnlyList<AshesProcessInfo> FindRunningAshesProcesses() =>
        AshesProcessManager.FindRunningAshesProcesses();

    public static Task<AshesTerminationResult> TerminateAshesProcessesAsync(
        int? trackedPid = null,
        bool killAllMatching = true,
        CancellationToken ct = default) =>
        AshesProcessManager.TerminateAshesProcessesAsync(trackedPid, killAllMatching, ct);
}
