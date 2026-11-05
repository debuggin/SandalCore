// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using SandalLauncher.Config;
using SandalLauncher.Models;
using SandalLauncher.Security;
using SandalLauncher.Services;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class EphemeralCredentialAndCliTests
{
    [Fact]
    public void LauncherCliOptions_Parse_AllSpaceSeparatedFlags_BindsExpectedValues()
    {
        string[] args =
        [
            "--username", "HeroTester@Verra.org",
            "--password", "Secr3t!Pass99",
            "--register-temp",
            "--auto-login",
            "--select-character", "Kaelen",
            "--create-character-if-missing",
            "--verify-char-select",
            "--spawn-client",
            "--terminate-after-char-select",
            "--kill-existing-client",
            "--delete-temp-account-on-exit",
            "--automation-port", "18901",
            "--timeout-sec", "90",
            "--gateway", "http://127.0.0.1:18080/",
            "--json",
            "--dry-run",
            "--help",
            "--version"
        ];

        LauncherCliOptions opts = LauncherCliOptions.Parse(args, new Dictionary<string, string?>());

        Assert.Equal("HeroTester@Verra.org", opts.Username);
        Assert.Equal("HeroTester@Verra.org", opts.Email);
        Assert.Equal("Secr3t!Pass99", opts.Password);
        Assert.True(opts.RegisterTemp);
        Assert.True(opts.AutoLogin);
        Assert.Equal("Kaelen", opts.SelectCharacter);
        Assert.True(opts.CreateCharacterIfMissing);
        Assert.True(opts.VerifyCharSelect);
        Assert.True(opts.SpawnClient);
        Assert.True(opts.TerminateAfterCharSelect);
        Assert.True(opts.KillExistingClient);
        Assert.True(opts.DeleteTempAccountOnExit);
        Assert.Equal(18901, opts.AutomationPort);
        Assert.Equal(90, opts.TimeoutSec);
        Assert.Equal("http://127.0.0.1:18080", opts.GatewayUrl);
        Assert.True(opts.JsonOutput);
        Assert.True(opts.DryRun);
        Assert.True(opts.ShowHelp);
        Assert.True(opts.ShowVersion);
        Assert.True(opts.HasAutomationFlags);
    }

    [Fact]
    public void LauncherCliOptions_Parse_InlineEqualsFlagsAndEmailAlias_NormalizesEmailAndBooleans()
    {
        string[] args =
        [
            "--email=ArchMage@Intrepid.io",
            "--password=Inline!Pass42",
            "--register-temp=false",
            "--auto-login=true",
            "--select-character=Lyra",
            "--create-character-if-missing=1",
            "--verify-char-select=yes",
            "--terminate-after-char-select=yes",
            "--kill-existing-client=1",
            "--delete-temp-account-on-exit=true",
            "--automation-port=29123",
            "--timeout-sec=120",
            "--gateway=http://localhost:9090/"
        ];

        LauncherCliOptions opts = LauncherCliOptions.Parse(args, new Dictionary<string, string?>());

        Assert.Equal("ArchMage@Intrepid.io", opts.Username);
        Assert.Equal("ArchMage@Intrepid.io", opts.Email);
        Assert.Equal("Inline!Pass42", opts.Password);
        Assert.False(opts.RegisterTemp);
        Assert.True(opts.AutoLogin);
        Assert.Equal("Lyra", opts.SelectCharacter);
        Assert.True(opts.CreateCharacterIfMissing);
        Assert.True(opts.VerifyCharSelect);
        Assert.True(opts.TerminateAfterCharSelect);
        Assert.True(opts.KillExistingClient);
        Assert.True(opts.DeleteTempAccountOnExit);
        Assert.Equal(29123, opts.AutomationPort);
        Assert.Equal(120, opts.TimeoutSec);
        Assert.Equal("http://localhost:9090", opts.GatewayUrl);
    }

    [Fact]
    public void LauncherCliOptions_Parse_EnvironmentVariables_PopulateAllSettingsWhenCliArgsOmitted()
    {
        var env = new Dictionary<string, string?>
        {
            ["AOC_GATEWAY_URL"] = "http://10.0.0.15:8080/",
            ["AOC_LAUNCHER_USERNAME"] = "env_tester",
            ["AOC_LAUNCHER_PASSWORD"] = "Env!Pass1234",
            ["AOC_LAUNCHER_REGISTER_TEMP"] = "true",
            ["AOC_LAUNCHER_AUTO_LOGIN"] = "1",
            ["AOC_LAUNCHER_CHARACTER"] = "EnvRanger",
            ["AOC_LAUNCHER_CREATE_CHAR"] = "yes",
            ["AOC_LAUNCHER_VERIFY_CHAR_SELECT"] = "true",
            ["AOC_LAUNCHER_SPAWN_CLIENT"] = "1",
            ["AOC_LAUNCHER_TERMINATE_AFTER_CHAR_SELECT"] = "true",
            ["AOC_LAUNCHER_KILL_EXISTING_CLIENT"] = "true",
            ["AOC_LAUNCHER_DELETE_TEMP_ON_EXIT"] = "1",
            ["AOC_LAUNCHER_AUTOMATION_PORT"] = "17777",
            ["AOC_LAUNCHER_TIMEOUT_SEC"] = "75"
        };

        LauncherCliOptions opts = LauncherCliOptions.Parse([], env);

        Assert.Equal("http://10.0.0.15:8080", opts.GatewayUrl);
        Assert.Equal("env_tester", opts.Username);
        Assert.Equal("env_tester@sandalcore.local", opts.Email);
        Assert.Equal("Env!Pass1234", opts.Password);
        Assert.True(opts.RegisterTemp);
        Assert.True(opts.AutoLogin);
        Assert.Equal("EnvRanger", opts.SelectCharacter);
        Assert.True(opts.CreateCharacterIfMissing);
        Assert.True(opts.VerifyCharSelect);
        Assert.True(opts.SpawnClient);
        Assert.True(opts.TerminateAfterCharSelect);
        Assert.True(opts.KillExistingClient);
        Assert.True(opts.DeleteTempAccountOnExit);
        Assert.Equal(17777, opts.AutomationPort);
        Assert.Equal(75, opts.TimeoutSec);
    }

    [Fact]
    public void LauncherCliOptions_Parse_RegisterTempDefaultsAndBoundaryGuards_GenerateEphemeralCredentials()
    {
        LauncherCliOptions opts = LauncherCliOptions.Parse(
            ["--register-temp", "--verify-char-select", "--automation-port=99999", "--timeout-sec=0"],
            new Dictionary<string, string?> { ["AOC_LAUNCHER_EMAIL"] = "" });

        Assert.True(opts.RegisterTemp);
        Assert.StartsWith("tmp_", opts.Username, StringComparison.Ordinal);
        Assert.Equal(12, opts.Username.Length);
        Assert.EndsWith("@sandalcore.local", opts.Email, StringComparison.Ordinal);
        Assert.StartsWith("Tmp!", opts.Password, StringComparison.Ordinal);
        Assert.Equal(LauncherCliOptions.DefaultHeroName, opts.SelectCharacter);
        Assert.Null(opts.AutomationPort);
        Assert.Equal(LauncherCliOptions.DefaultTimeoutSeconds, opts.TimeoutSec);
        Assert.Contains("--register-temp", LauncherCliOptions.GetUsageText(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task EphemeralCredentialGuard_RejectsPermanentAccountCredentials_NeverWritesToVault()
    {
        using var vault = new InMemoryCredentialVault();
        var guard = new EphemeralCredentialGuard(vault);

        Assert.Throws<InvalidOperationException>(() =>
            guard.SaveTemporaryCredentials("prod-user-profile", "real_player", "RealUserSecret!1", isTemporaryAccount: false));

        await Assert.ThrowsAsync<InvalidOperationException>(() =>
            guard.SaveTemporaryCredentialsAsync("prod-user-profile", "real_player", "RealUserSecret!1", isTemporaryAccount: false));

        Assert.Null(await vault.RetrieveTokenAsync("prod-user-profile"));
        Assert.Throws<ArgumentException>(() => guard.SaveTemporaryCredentials("", "u", "p", isTemporaryAccount: true));
        Assert.Throws<ArgumentException>(() => guard.SaveTemporaryCredentials("id", "", "p", isTemporaryAccount: true));
        Assert.Throws<ArgumentException>(() => guard.SaveTemporaryCredentials("id", "u", "", isTemporaryAccount: true));
    }

    [Fact]
    public async Task EphemeralCredentialGuard_SavesAndPurgesTemporaryCredentialsAndWipesTokenFile()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-guard-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string tokenFile = Path.Combine(tempDir, "ics_auth_token.txt");
            await File.WriteAllTextAsync(tokenFile, "ephemeral_session_token_value_12345");
            using var vault = new InMemoryCredentialVault();
            var guard = new EphemeralCredentialGuard(vault, tokenFile);

            await guard.SaveTemporaryCredentialsAsync("temp-tmp_1234abcd", "tmp_1234abcd", "Tmp!Pass99", isTemporaryAccount: true);
            Assert.Equal("tmp_1234abcd\nTmp!Pass99", await vault.RetrieveTokenAsync("temp-tmp_1234abcd"));

            await guard.PurgeTemporaryCredentialsAsync("temp-tmp_1234abcd", tokenFile);
            Assert.Null(await vault.RetrieveTokenAsync("temp-tmp_1234abcd"));
            Assert.False(File.Exists(tokenFile));
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    [Fact]
    public async Task ProfileStoreService_UpsertGetTemporaryAndDeleteProfile_RoundTripsCleanly()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-prof-temp-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string catalogPath = Path.Combine(tempDir, "LauncherProfiles.json");
            var store = new ProfileStoreService(catalogPath);

            var permanent = new LauncherProfile
            {
                Id = "perm-alpha",
                DisplayName = "Permanent Alpha",
                AccountUsername = "perm_user",
                IsTemporaryAccount = false
            };
            var ephemeral = new LauncherProfile
            {
                Id = "temp-tmp_abcd1234",
                DisplayName = "Temp (tmp_abcd1234)",
                AccountUsername = "tmp_abcd1234",
                IsTemporaryAccount = true,
                CreatedAtUtc = DateTimeOffset.UtcNow
            };

            await store.UpsertProfileAsync(permanent);
            await store.UpsertProfileAsync(ephemeral);

            IReadOnlyList<LauncherProfile> tempList = await store.GetTemporaryProfilesAsync();
            Assert.Single(tempList);
            Assert.Equal("temp-tmp_abcd1234", tempList[0].Id);
            Assert.True(tempList[0].IsTemporaryAccount);

            LauncherProfile updated = ephemeral with { DisplayName = "Updated Ephemeral" };
            await store.UpsertProfileAsync(updated);
            tempList = await store.GetTemporaryProfilesAsync();
            Assert.Single(tempList);
            Assert.Equal("Updated Ephemeral", tempList[0].DisplayName);

            Assert.True(await store.DeleteProfileAsync("temp-tmp_abcd1234"));
            Assert.False(await store.DeleteProfileAsync("temp-tmp_abcd1234"));
            Assert.Empty(await store.GetTemporaryProfilesAsync());
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    [Fact]
    public async Task AshesProcessManager_MatchesProcessNamesAndTerminatesTrackedDummyProcess()
    {
        Assert.True(AshesProcessManager.IsAshesProcessName("AOCClient-Win64-Shipping"));
        Assert.True(AshesProcessManager.IsAshesProcessName("AOCClient-Win64-Shipping.exe"));
        Assert.True(AshesProcessManager.IsAshesProcessName("AshesOfCreation.exe"));
        Assert.True(AshesProcessManager.IsAshesProcessName("AOCClient"));
        Assert.True(AshesProcessManager.IsAshesProcessName("AoC-Win64-Shipping"));
        Assert.False(AshesProcessManager.IsAshesProcessName("AOCGame"));
        Assert.False(AshesProcessManager.IsAshesProcessName("dotnet"));
        Assert.False(AshesProcessManager.IsAshesProcessName(""));
        Assert.False(AshesProcessManager.IsAshesProcessName(null));

        _ = AshesProcessManager.FindRunningAshesProcesses();

        var psi = new ProcessStartInfo
        {
            FileName = "pwsh",
            Arguments = "-NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 30\"",
            RedirectStandardOutput = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };
        using Process? dummy = Process.Start(psi);
        Assert.NotNull(dummy);

        var manager = new AshesProcessManager();
        AshesTerminationResult res = await manager.TerminateAshesProcessesAsync(
            trackedPid: dummy.Id,
            killAllMatching: false);

        Assert.Equal(1, res.TerminatedCount);
        Assert.Contains(dummy.Id, res.TerminatedPids);
        AshesTerminationResult none = await ClientLaunchService.TerminateAshesProcessesAsync(
            trackedPid: -1,
            killAllMatching: false);
        Assert.Equal(0, none.TerminatedCount);
    }
}
