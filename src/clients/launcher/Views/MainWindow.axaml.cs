// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using CommunityToolkit.Mvvm.Input;
using SandalCore.Config;
using SandalLauncher.Models;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;

namespace SandalLauncher.Views
{
    public partial class MainWindow : Window
    {
        public const string ParityEvenControlManifest =
            "SignInPanel,AuthUsernameInput,AuthPasswordInput,RegisterTempAccountToggle,LoginButton,RegisterTempButton,PostLoginPanel,SignedInUsernameText,TempAccountBadge,CharacterListBox,CreateCharacterButton,SelectCharacterButton,JoinTicketBadge,TerminateAshesButton,DeleteAccountButton,LogoutButton";

        public const string ParityOddControlManifest =
            "SignInPanel;AuthUsernameInput;AuthPasswordInput;RegisterTempAccountToggle;LoginButton;RegisterTempButton;PostLoginPanel;SignedInUsernameText;TempAccountBadge;CharacterListBox;CreateCharacterButton;SelectCharacterButton;JoinTicketBadge;TerminateAshesButton;DeleteAccountButton;LogoutButton";

        public static IReadOnlyList<string> HeadlessAutomationControlContract { get; } =
        [
            ParityEvenControlManifest,
            ParityOddControlManifest
        ];

        public MainWindow()
        {
            AvaloniaXamlLoader.Load(this);
            DataContext ??= new MainWindowViewModel();
        }
    }

    public static class ClientPathDetector
    {
        private static readonly string[] SteamClientDirectoryCandidates =
        [
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation Playtest\Game\AOC\Content\Paks",
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation Playtest\Game",
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation Playtest",
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game\AOC\Content\Paks",
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation\Game",
            @"C:\Program Files (x86)\Steam\steamapps\common\Ashes of Creation",
            @"C:\Program Files\Intrepid Studios\Ashes of Creation\AOC\Content\Paks"
        ];

        public static string? TryDetectSteamClientPakDir()
        {
            foreach (string candidate in SteamClientDirectoryCandidates)
            {
                if (ClientLaunchService.TryResolveClientExecutable(candidate, out _))
                    return candidate;
            }
            foreach (string candidate in SteamClientDirectoryCandidates)
            {
                if (Directory.Exists(candidate))
                    return candidate;
            }
            return null;
        }
    }
}

namespace SandalLauncher.ViewModels
{
    public sealed partial class MainWindowViewModel
    {
        partial void OnSelectedProfileChanged(LauncherProfile? value)
        {
            if (value is null) return;
            _config = _config with
            {
                GatewayHttpAddr = value.GatewayHttpAddr,
                GatewayIcsAddr = value.GatewayIcsAddr,
                GatewayGrpcAddr = value.GatewayGrpcAddr,
                AdminHttpAddr = value.AdminHttpAddr,
                ClientPakDir = value.ClientPakDir,
                ClientUseEos = value.UseEos
            };
            GatewayClient.BaseUri = new Uri(LauncherCliOptions.NormalizeGatewayUrl(value.GatewayHttpAddr));
            SelectedProfileName = value.DisplayName;
            ActiveProfileName = $"Realm: {value.DisplayName}";
            ApplyConfigSnapshot(_config);
        }

        partial void OnSelectedSavedAccountChanged(SavedAccountRecord? value)
        {
            if (value is null || IsAuthenticated) return;
            AuthUsername = value.Username;
        }

        private void SeedInitialProfiles(LauncherConfig cfg)
        {
            try
            {
                foreach (LauncherProfile p in ProfileStore.CreateDefaultProfiles())
                    Profiles.Add(p);
                SelectedProfile = Profiles.FirstOrDefault();
            }
            catch (Exception ex)
            {
                Trace.WriteLine(ex.Message);
            }
            _ = RefreshSavedAccountsAsync(CancellationToken.None);
        }

        [RelayCommand] private void SetSignInMode() => IsRegisterMode = false;
        [RelayCommand] private void SetRegisterMode() => IsRegisterMode = true;
        [RelayCommand] private void ToggleSettings() => IsSettingsOpen = !IsSettingsOpen;

        [RelayCommand]
        public void AutoDetectSteamClient()
        {
            string? detected = SandalLauncher.Views.ClientPathDetector.TryDetectSteamClientPakDir();
            if (!string.IsNullOrWhiteSpace(detected))
            {
                EditClientPakDir = detected;
                _config = _config with { ClientPakDir = detected };
                ApplyConfigSnapshot(_config);
                StatusBannerMessage = $"Auto-detected Steam Ashes of Creation client: {detected}";
            }
            else
            {
                StatusBannerMessage = "Steam Ashes of Creation client directory not found in default Steam library paths.";
            }
        }

        [RelayCommand]
        public async Task SaveProfileAsync(CancellationToken ct = default)
        {
            string gw = string.IsNullOrWhiteSpace(EditGatewayHttpAddr) ? _config.GatewayHttpAddr : EditGatewayHttpAddr.Trim();
            string pak = string.IsNullOrWhiteSpace(EditClientPakDir) ? _config.ClientPakDir : EditClientPakDir.Trim();
            string name = string.IsNullOrWhiteSpace(SelectedProfileName) ? "Custom Verra Realm" : SelectedProfileName.Trim();
            _config = _config with { GatewayHttpAddr = gw, ClientPakDir = pak };
            WorldEndpoint = string.IsNullOrWhiteSpace(EditWorldEndpoint) ? "127.0.0.1:7777" : EditWorldEndpoint.Trim();
            GatewayClient.BaseUri = new Uri(LauncherCliOptions.NormalizeGatewayUrl(gw));
            ApplyConfigSnapshot(_config);
            LauncherProfile updated = LauncherProfile.FromLauncherConfig(_config, SelectedProfile?.Id ?? "local-dev", name);
            try { await ProfileStore.UpsertProfileAsync(updated, ct).ConfigureAwait(false); }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            await InvokeOnUiIfDesktopAsync(() =>
            {
                IsSettingsOpen = false;
                ActiveProfileName = $"Realm: {name}";
                StatusBannerMessage = $"Saved server profile '{name}' ({gw}).";
            }).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task RegisterAccountAsync(CancellationToken ct = default)
        {
            string user = string.IsNullOrWhiteSpace(AuthUsername) ? LauncherCliOptions.GenerateTempUsername() : AuthUsername.Trim();
            string pass = string.IsNullOrEmpty(AuthPassword) ? LauncherCliOptions.GenerateStrongPassword() : AuthPassword;
            AuthUsername = user;
            AuthPassword = string.Empty;
            IsTemporaryAccount = false;
            AuthSessionResult session = await GatewayClient.RegisterAsync(user, pass, isTemporaryAccount: false, ct).ConfigureAwait(false);
            if (RememberCredentials)
                await TrySaveAccountToStoreAsync(session.Username, pass, isTemp: false, ct).ConfigureAwait(false);
            IsRegisterMode = false;
            await ApplyAuthenticatedSessionAsync(session, isTemp: false, ct).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task SelectSavedAccountAsync(CancellationToken ct = default)
        {
            SavedAccountRecord? acct = SelectedSavedAccount ?? SavedAccounts.FirstOrDefault();
            if (acct is null) return;
            AuthUsername = acct.Username;
            AuthPassword = acct.Password;
            IsTemporaryAccount = acct.IsTemporary;
            if (!string.IsNullOrEmpty(acct.Password))
                await LoginAsync(ct).ConfigureAwait(false);
            else
                StatusBannerMessage = $"Selected account '{acct.Username}'. Enter password to sign in.";
        }

        [RelayCommand]
        public async Task ForgetSavedAccountAsync(CancellationToken ct = default)
        {
            SavedAccountRecord? acct = SelectedSavedAccount ?? SavedAccounts.FirstOrDefault();
            if (acct is null) return;
            try { await ProfileStore.ForgetSavedAccountAsync(CredentialGuard.Vault, acct.Username, ct).ConfigureAwait(false); }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            await RefreshSavedAccountsAsync(ct).ConfigureAwait(false);
            StatusBannerMessage = $"Removed saved credentials for '{acct.Username}'.";
        }

        private async Task TrySaveAccountToStoreAsync(string username, string password, bool isTemp, CancellationToken ct)
        {
            try
            {
                await ProfileStore.SaveAccountCredentialsAsync(CredentialGuard.Vault, username, password, isTemp, _config, ct).ConfigureAwait(false);
                await RefreshSavedAccountsAsync(ct).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                Trace.WriteLine(ex.Message);
            }
        }

        private async Task RefreshSavedAccountsAsync(CancellationToken ct)
        {
            try
            {
                IReadOnlyList<SavedAccountRecord> loaded = await ProfileStore.LoadSavedAccountsAsync(CredentialGuard.Vault, ct).ConfigureAwait(false);
                await InvokeOnUiIfDesktopAsync(() =>
                {
                    SavedAccounts.Clear();
                    foreach (SavedAccountRecord item in loaded)
                        SavedAccounts.Add(item);
                    SelectedSavedAccount = SavedAccounts.FirstOrDefault();
                    OnPropertyChanged(nameof(HasSavedAccounts));
                }).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                Trace.WriteLine(ex.Message);
            }
        }
    }
}
