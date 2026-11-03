// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Diagnostics;
using System.IO;
using SandalLauncher.Models;

namespace SandalLauncher.Services;

public sealed class ClientLaunchService
{
    public const string ShippingExeName = "AOCClient-Win64-Shipping.exe";
    public const string BootstrapExeName = "AOCClient.exe";

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
}
