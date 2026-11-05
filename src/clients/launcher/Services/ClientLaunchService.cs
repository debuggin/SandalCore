// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SandalCore.Config;
using SandalLauncher.Models;
using SandalLauncher.Security;

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
            string? p1 = Directory.GetParent(canonical)?.FullName;
            string? p2 = p1 is not null ? Directory.GetParent(p1)?.FullName : null;
            string? p3 = p2 is not null ? Directory.GetParent(p2)?.FullName : null;
            foreach (string? root in new[] { canonical, p1, p2, p3 })
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

    public static string? ResolveRootCaCertificatePath()
    {
        string[] roots = [Directory.GetCurrentDirectory(), AppContext.BaseDirectory];
        string[] rels =
        [
            Path.Combine("dist", "certs", "ca.crt"),
            Path.Combine("deploy", "certs", "ca.crt"),
            Path.Combine("certs", "ca.crt"),
            Path.Combine("..", "certs", "ca.crt"),
            Path.Combine("..", "dist", "certs", "ca.crt")
        ];
        foreach (string r in roots)
        {
            if (string.IsNullOrWhiteSpace(r)) continue;
            foreach (string rel in rels)
            {
                string full = Path.GetFullPath(Path.Combine(r, rel));
                if (File.Exists(full)) return full;
            }
        }
        return null;
    }

    public static void ConfigureTlsEnvironment(ProcessStartInfo psi, string? explicitCaCertPath = null)
    {
        ArgumentNullException.ThrowIfNull(psi);
        string? caPath = !string.IsNullOrWhiteSpace(explicitCaCertPath) && File.Exists(explicitCaCertPath)
            ? Path.GetFullPath(explicitCaCertPath)
            : ResolveRootCaCertificatePath();
        if (!string.IsNullOrWhiteSpace(caPath))
        {
            psi.Environment["SSL_CERT_FILE"] = caPath;
            psi.Environment["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = caPath;
        }
    }

    public static bool TryResolveClientExecutable(string? clientPathOrDirectory, out string resolvedExePath)
    {
        resolvedExePath = string.Empty;
        if (string.IsNullOrWhiteSpace(clientPathOrDirectory)) return false;
        try
        {
            resolvedExePath = ResolveClientExecutable(clientPathOrDirectory);
            return File.Exists(resolvedExePath);
        }
        catch (Exception ex) when (ex is ArgumentException or IOException or UnauthorizedAccessException or NotSupportedException)
        {
            return false;
        }
    }

    public static Process StartGameOrMockProcess(
        LauncherConfig? config,
        bool allowMockFallback = true,
        int mockSleepSeconds = 300,
        int tetherPort = 19021)
    {
        string? candidateDir = config?.ClientPakDir;
        if (!TryResolveClientExecutable(candidateDir, out string realExe) &&
            Avalonia.Application.Current?.ApplicationLifetime is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)
        {
            candidateDir = SandalLauncher.Views.ClientPathDetector.TryDetectSteamClientPakDir();
            _ = TryResolveClientExecutable(candidateDir, out realExe);
        }
        if (config is not null && !string.IsNullOrEmpty(realExe) && File.Exists(realExe))
        {
            try
            {
                var realPsi = new ProcessStartInfo(realExe)
                {
                    WorkingDirectory = Path.GetDirectoryName(realExe) ?? string.Empty,
                    UseShellExecute = false,
                    CreateNoWindow = false
                };
                realPsi.ArgumentList.Add($"-GatewayAddr={config.GatewayHttpAddr}");
                realPsi.ArgumentList.Add($"-IcsAddr={config.GatewayIcsAddr}");
                realPsi.ArgumentList.Add($"-LauncherTetherPort={(tetherPort > 0 ? tetherPort : 19021)}");
                realPsi.ArgumentList.Add($"-UseEOS={(config.ClientUseEos ? "true" : "false")}");
                realPsi.ArgumentList.Add("-NoEAC");
                ConfigureTlsEnvironment(realPsi);
                if (Process.Start(realPsi) is { } startedReal)
                    return startedReal;
            }
            catch (Exception ex) when (allowMockFallback)
            {
                Trace.WriteLine(ex.Message);
            }
        }
        if (!allowMockFallback)
            throw new FileNotFoundException("Configured Ashes of Creation client executable was not found.");
        ProcessStartInfo mockPsi = BuildMockGameStartInfo(mockSleepSeconds);
        return Process.Start(mockPsi) ?? throw new InvalidOperationException("Failed to start mock game client process.");
    }

    public static ProcessStartInfo BuildMockGameStartInfo(int sleepSeconds = 300)
    {
        string asmPath = typeof(ClientLaunchService).Assembly.Location;
        string? candidateExe = !string.IsNullOrWhiteSpace(asmPath)
            ? Path.ChangeExtension(asmPath, OperatingSystem.IsWindows() ? ".exe" : null)
            : null;
        ProcessStartInfo psi;
        if (!string.IsNullOrWhiteSpace(candidateExe) && File.Exists(candidateExe))
        {
            psi = new ProcessStartInfo(candidateExe);
        }
        else if (!string.IsNullOrWhiteSpace(asmPath) && File.Exists(asmPath))
        {
            psi = new ProcessStartInfo("dotnet");
            psi.ArgumentList.Add(asmPath);
        }
        else
        {
            psi = new ProcessStartInfo(Environment.ProcessPath ?? "dotnet");
        }
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.ArgumentList.Add("--mock-game-client");
        psi.ArgumentList.Add(Math.Max(1, sleepSeconds).ToString(System.Globalization.CultureInfo.InvariantCulture));
        return psi;
    }

    public static bool IsProcessAlive(int? pid)
    {
        if (pid is not > 0) return false;
        try
        {
            using Process proc = Process.GetProcessById(pid.Value);
            return !proc.HasExited;
        }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            return false;
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
