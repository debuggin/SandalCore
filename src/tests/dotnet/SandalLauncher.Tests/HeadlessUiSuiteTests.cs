// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;
using SandalLauncher.Models;
using SandalLauncher.Security;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;
using SandalLauncher.Views;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class HeadlessUiSuiteTests
{
    [Fact]
    public void RealmStatusPillViewModel_CoversAllFiveViewStatesWcagContrastAndNonColorGlyphs()
    {
        var pill = new RealmStatusPillViewModel();
        Assert.Equal(RealmPillState.Empty, pill.ViewState);
        Assert.Equal("○", pill.StatusIcon);
        Assert.StartsWith("○ ", pill.StatusLabel);
        Assert.True(ComputeWcagContrastRatio(pill.BadgeForegroundHex, RealmStatusPillViewModel.DarkSurfaceHex) >= 4.5);
        pill.SetCheckingState();
        Assert.Equal(RealmPillState.Loading, pill.ViewState);
        Assert.Equal("◌", pill.StatusIcon);
        Assert.StartsWith("◌ ", pill.StatusLabel);
        Assert.True(ComputeWcagContrastRatio(pill.BadgeForegroundHex, RealmStatusPillViewModel.DarkSurfaceHex) >= 4.5);
        pill.ApplyProbeResult(new GatewayProbeResult(false, 401, 18, 0, 0, "HttpJson", "Unauthorized"), hashMatched: true, authFailed: true);
        Assert.Equal(RealmPillState.Error, pill.ViewState);
        Assert.Equal("■", pill.StatusIcon);
        Assert.StartsWith("■ ", pill.StatusLabel);
        Assert.True(ComputeWcagContrastRatio(pill.BadgeForegroundHex, RealmStatusPillViewModel.DarkSurfaceHex) >= 4.5);
        pill.ApplyProbeResult(new GatewayProbeResult(true, 200, 11, 24, 500, "HttpJson", null), hashMatched: false);
        Assert.Equal(RealmPillState.Degraded, pill.ViewState);
        Assert.Equal("▲", pill.StatusIcon);
        Assert.StartsWith("▲ ", pill.StatusLabel);
        Assert.True(ComputeWcagContrastRatio(pill.BadgeForegroundHex, RealmStatusPillViewModel.DarkSurfaceHex) >= 4.5);
        pill.ApplyProbeResult(new GatewayProbeResult(true, 200, 9, 128, 500, "GrpcWeb", null), hashMatched: true);
        Assert.Equal(RealmPillState.Populated, pill.ViewState);
        Assert.Equal("●", pill.StatusIcon);
        Assert.StartsWith("● ", pill.StatusLabel);
        Assert.Equal("9 ms", pill.LatencyLabel);
        Assert.Equal("128 / 500", pill.PopulationLabel);
        Assert.True(ComputeWcagContrastRatio(pill.BadgeForegroundHex, RealmStatusPillViewModel.DarkSurfaceHex) >= 4.5);
    }

    [Fact]
    public void LaunchPresetsViewModel_CoversAllFourPresetsAndServerStackLifecycle()
    {
        var vm = new LaunchPresetsViewModel();
        Assert.Equal(4, vm.AvailablePresets.Count);

        vm.SelectedPreset = LaunchPresetKind.RetailParity;
        Assert.True(vm.UseEos);
        Assert.False(vm.EnableDebugLog);
        Assert.False(vm.WindowedMode);

        vm.SelectedPreset = LaunchPresetKind.LocalSandbox;
        Assert.False(vm.UseEos);
        Assert.False(vm.EnableDebugLog);
        Assert.True(vm.WindowedMode);

        vm.SelectedPreset = LaunchPresetKind.GmArchitectDebug;
        Assert.False(vm.UseEos);
        Assert.True(vm.EnableDebugLog);
        Assert.True(vm.WindowedMode);
        string[] gmArgs = vm.BuildEffectiveLaunchArguments(29000, "SandalAuth_1234");
        Assert.Contains("-Log", gmArgs);
        Assert.Contains("-Windowed", gmArgs);
        Assert.Contains("-NoEAC", gmArgs);

        vm.SelectedPreset = LaunchPresetKind.Custom;
        vm.UseEos = true;
        Assert.Contains("-UseEOS=true", vm.EffectiveArgumentsPreview);

        Assert.Equal(ServerStackState.Stopped, vm.StackState);
        Assert.True(vm.CanStartStack);
        vm.StartStackCommand.Execute(null);
        Assert.Equal(ServerStackState.Starting, vm.StackState);
        Assert.True(vm.CanStopStack);
        vm.TransitionStackState("ready");
        Assert.Equal(ServerStackState.Running, vm.StackState);
        Assert.True(vm.CanRestartStack);
        vm.TransitionStackState("fault");
        Assert.Equal(ServerStackState.Faulted, vm.StackState);
        vm.StopStackCommand.Execute(null);
        Assert.Equal(ServerStackState.Stopped, vm.StackState);

        string hex = new('a', 64);
        vm.ApplyBinaryHashResult(new BinaryHashResult(true, hex, hex, 1024));
        Assert.True(vm.IsBinaryHashVerified);
        Assert.StartsWith("● SHA-256 VERIFIED", vm.BinaryHashBadgeText);
    }

    [Fact]
    public async Task ProfileSwitcherViewModel_SupportsCrudVaultTokensAndZeroPlaintextPolicy()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-ui-prof-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string storePath = Path.Combine(tempDir, "LauncherProfiles.json");
            var store = new ProfileStoreService(storePath);
            using var vault = new InMemoryCredentialVault();
            var vm = new ProfileSwitcherViewModel(store, vault);
            await vm.LoadProfilesAsync();
            Assert.Equal("populated", vm.CurrentViewState);
            Assert.NotEmpty(vm.Profiles);

            vm.PrepareNewProfile();
            vm.EditId = "staging-mesh";
            vm.EditDisplayName = "Staging Verra Mesh";
            await vm.SaveProfileAsync();
            vm.SessionTokenInput = "ephemeral_vault_session_99";
            await vm.StoreSessionTokenAsync();
            Assert.Equal(string.Empty, vm.SessionTokenInput);
            Assert.True(vm.HasVaultToken);
            Assert.Equal("ephemeral_vault_session_99", await vm.RetrieveSessionTokenAsync());

            string diskJson = await File.ReadAllTextAsync(storePath);
            Assert.DoesNotContain("ephemeral_vault_session_99", diskJson);

            vm.ArmDeleteProfile();
            Assert.True(vm.IsDeleteConfirmationArmed);
            vm.CancelDeleteProfile();
            Assert.False(vm.IsDeleteConfirmationArmed);
            vm.ArmDeleteProfile();
            await vm.DeleteSelectedProfileAsync();
            Assert.Null(await vault.RetrieveTokenAsync("staging-mesh"));
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public void HeadlessViews_InstantiateServerControlAndProfileEditorWithCompiledBindings()
    {
        var serverView = new ServerControlView();
        var serverVm = Assert.IsType<LaunchPresetsViewModel>(serverView.DataContext);
        Assert.Equal(LaunchPresetKind.LocalSandbox, serverVm.SelectedPreset);
        Assert.NotNull(serverView.Content);

        string tempStore = Path.Combine(Path.GetTempPath(), $"sandal-view-{Guid.NewGuid():N}", "LauncherProfiles.json");
        try
        {
            var customProfileVm = new ProfileSwitcherViewModel(new ProfileStoreService(tempStore), new InMemoryCredentialVault());
            var profileView = new ProfileEditorView { DataContext = customProfileVm };
            Assert.Same(customProfileVm, profileView.DataContext);
            Assert.True(customProfileVm.IsPopulatedState);
            Assert.NotNull(profileView.Content);
        }
        finally
        {
            string? dir = Path.GetDirectoryName(tempStore);
            if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir)) Directory.Delete(dir, recursive: true);
        }
    }

    private static double ComputeWcagContrastRatio(string hexA, string hexB)
    {
        double l1 = ComputeRelativeLuminance(hexA), l2 = ComputeRelativeLuminance(hexB);
        return (Math.Max(l1, l2) + 0.05) / (Math.Min(l1, l2) + 0.05);
    }

    private static double ComputeRelativeLuminance(string hex)
    {
        string clean = hex.Trim().TrimStart('#');
        double r = LinearizeChannel(int.Parse(clean[0..2], NumberStyles.HexNumber, CultureInfo.InvariantCulture) / 255.0);
        double g = LinearizeChannel(int.Parse(clean[2..4], NumberStyles.HexNumber, CultureInfo.InvariantCulture) / 255.0);
        double b = LinearizeChannel(int.Parse(clean[4..6], NumberStyles.HexNumber, CultureInfo.InvariantCulture) / 255.0);
        return (0.2126 * r) + (0.7152 * g) + (0.0722 * b);
    }

    private static double LinearizeChannel(double c) => c <= 0.03928 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);
}
