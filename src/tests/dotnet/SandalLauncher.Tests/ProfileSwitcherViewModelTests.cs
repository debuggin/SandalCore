// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using SandalCore.Config;
using SandalLauncher.Security;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class ProfileSwitcherViewModelTests
{
    [Fact]
    public async Task MainWindowViewModel_LaunchClient_AutomaticallyDetectsExternalGameProcessExitWithinTwoSeconds()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-exit-detect-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        int? spawnedPid = null;
        MainWindowViewModel? vm = null;
        try
        {
            using var vault = new InMemoryCredentialVault();
            var store = new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json"));
            vm = new MainWindowViewModel(SandalConfigLoader.LoadLauncher(), null, vault, store);

            vm.LaunchClient();
            Assert.True(vm.IsAshesProcessRunning);
            Assert.False(vm.IsGameStopped);
            Assert.Equal("game_started", vm.LifecyclePhase);
            Assert.NotNull(vm.TrackedAshesPid);
            spawnedPid = vm.TrackedAshesPid!.Value;
            Assert.True(ClientLaunchService.IsProcessAlive(spawnedPid));

            KillProcessExternally(spawnedPid.Value);
            await WaitForExitStateResetAsync(vm, TimeSpan.FromSeconds(2));

            Assert.False(vm.IsAshesProcessRunning);
            Assert.True(vm.IsGameStopped);
            Assert.Null(vm.TrackedAshesPid);
            Assert.Equal("game_closed", vm.LifecyclePhase);
        }
        finally
        {
            if (spawnedPid is > 0 && ClientLaunchService.IsProcessAlive(spawnedPid))
                _ = await ClientLaunchService.TerminateAshesProcessesAsync(spawnedPid, killAllMatching: false);
            vm?.CloseLauncher();
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    private static void KillProcessExternally(int pid)
    {
        using Process proc = Process.GetProcessById(pid);
        proc.Kill(entireProcessTree: true);
        proc.WaitForExit(2000);
    }

    private static async Task WaitForExitStateResetAsync(MainWindowViewModel vm, TimeSpan timeout)
    {
        var sw = Stopwatch.StartNew();
        // Wait briefly in a bounded polling loop while the Process.Exited event callback updates ViewModel state
        while (sw.Elapsed < timeout && (vm.IsAshesProcessRunning || vm.TrackedAshesPid is not null || vm.LifecyclePhase != "game_closed"))
        {
            await Task.Delay(25);
        }
    }
}
