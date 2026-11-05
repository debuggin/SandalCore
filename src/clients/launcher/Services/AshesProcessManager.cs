// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Services;

public sealed record AshesProcessInfo(int ProcessId, string ProcessName)
{
    public int Pid => ProcessId;
}

public sealed record AshesTerminationResult(
    int TerminatedCount,
    IReadOnlyList<int> TerminatedPids,
    IReadOnlyList<string> Errors)
{
    public bool AllTerminated => Errors.Count == 0;
}

public sealed class AshesProcessManager
{
    public static readonly IReadOnlyList<string> TargetProcessNames =
    [
        "AOCClient-Win64-Shipping",
        "AoC-Win64-Shipping",
        "AOCClient",
        "AshesOfCreation"
    ];

    public static bool IsAshesProcessName(string? processName)
    {
        if (string.IsNullOrWhiteSpace(processName)) return false;
        ReadOnlySpan<char> span = processName.AsSpan().Trim();
        if (span.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
            span = span[..^4];
        foreach (string target in TargetProcessNames)
        {
            if (span.Equals(target.AsSpan(), StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    public static IReadOnlyList<AshesProcessInfo> FindRunningAshesProcesses()
    {
        var seen = new Dictionary<int, AshesProcessInfo>();
        foreach (string name in TargetProcessNames)
        {
            try
            {
                foreach (Process proc in Process.GetProcessesByName(name))
                {
                    using (proc)
                    {
                        try
                        {
                            if (!proc.HasExited)
                                seen[proc.Id] = new AshesProcessInfo(proc.Id, proc.ProcessName);
                        }
                        catch (Exception ex) when (ex is InvalidOperationException or Win32Exception) { }
                    }
                }
            }
            catch (Exception ex) when (ex is InvalidOperationException or PlatformNotSupportedException) { }
        }
        return seen.Values.OrderBy(p => p.ProcessId).ToArray();
    }

    public static async Task<AshesTerminationResult> TerminateAshesProcessesAsync(
        int? trackedPid = null,
        bool killAllMatching = true,
        CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        var targetPids = new SortedSet<int>();
        if (trackedPid is > 0)
            targetPids.Add(trackedPid.Value);
        if (killAllMatching)
        {
            foreach (AshesProcessInfo info in FindRunningAshesProcesses())
                targetPids.Add(info.ProcessId);
        }

        var terminated = new List<int>(targetPids.Count);
        var errors = new List<string>();
        foreach (int pid in targetPids)
        {
            ct.ThrowIfCancellationRequested();
            if (await TryTerminateSingleProcessAsync(pid, errors, ct).ConfigureAwait(false))
                terminated.Add(pid);
        }
        return new AshesTerminationResult(terminated.Count, terminated, errors);
    }

    private static async Task<bool> TryTerminateSingleProcessAsync(
        int pid,
        List<string> errors,
        CancellationToken ct)
    {
        Process? proc = null;
        try
        {
            proc = Process.GetProcessById(pid);
            if (proc.HasExited) return true;
            try { _ = proc.CloseMainWindow(); }
            catch (Exception ex) when (ex is InvalidOperationException or PlatformNotSupportedException) { }

            proc.Kill(entireProcessTree: true);
            using var waitCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            waitCts.CancelAfter(TimeSpan.FromMilliseconds(2000));
            try { await proc.WaitForExitAsync(waitCts.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested) { }
            return true;
        }
        catch (ArgumentException)
        {
            return true;
        }
        catch (InvalidOperationException)
        {
            return true;
        }
        catch (Win32Exception ex)
        {
            errors.Add($"PID {pid}: {ex.Message}");
            return false;
        }
        finally
        {
            proc?.Dispose();
        }
    }
}

public static class AshesProcessManagerExtensions
{
    public static IReadOnlyList<AshesProcessInfo> FindRunningAshesProcesses(this AshesProcessManager manager)
    {
        ArgumentNullException.ThrowIfNull(manager);
        return AshesProcessManager.FindRunningAshesProcesses();
    }

    public static Task<AshesTerminationResult> TerminateAshesProcessesAsync(
        this AshesProcessManager manager,
        int? trackedPid = null,
        bool killAllMatching = true,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(manager);
        return AshesProcessManager.TerminateAshesProcessesAsync(trackedPid, killAllMatching, ct);
    }
}
