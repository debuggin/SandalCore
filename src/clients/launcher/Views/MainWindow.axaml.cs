// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
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
            Closing += OnWindowClosing;
        }

        private void OnWindowClosing(object? sender, WindowClosingEventArgs e)
        {
            if (DataContext is MainWindowViewModel { IsAuthenticated: true, IsTemporaryAccount: true } vm)
                vm.Logout();
        }
    }

    public static class ClientPathDetector
    {
        private static readonly Regex VdfPathRegex = new("\"path\"\\s+\"([^\"]+)\"", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        public static IReadOnlyList<DiscoveredClientInstall> DiscoverSteamInstallations(IEnumerable<string>? customCommonDirectories = null)
        {
            var results = new List<DiscoveredClientInstall>();
            var seenExes = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string commonDir in customCommonDirectories ?? EnumerateSteamCommonDirectories())
            {
                if (!Directory.Exists(commonDir)) continue;
                IEnumerable<string> candidates;
                try { candidates = Directory.EnumerateDirectories(commonDir, "Ashes*"); }
                catch (Exception ex) { Trace.WriteLine(ex.Message); continue; }
                foreach (string installDir in candidates)
                {
                    if (!ClientLaunchService.TryResolveClientExecutable(installDir, out string exePath) || !seenExes.Add(exePath))
                        continue;
                    string pakSub1 = Path.Combine(installDir, "Game", "AOC", "Content", "Paks");
                    string pakSub2 = Path.Combine(installDir, "AOC", "Content", "Paks");
                    string pakDir = Directory.Exists(pakSub1) ? pakSub1 : (Directory.Exists(pakSub2) ? pakSub2 : installDir);
                    string folderName = Path.GetFileName(installDir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
                    string label = folderName.Contains("Playtest", StringComparison.OrdinalIgnoreCase)
                        ? "Ashes of Creation Playtest"
                        : "Ashes of Creation (Retail)";
                    results.Add(new DiscoveredClientInstall(label, installDir, pakDir, exePath));
                }
            }
            return results;
        }

        public static IReadOnlyList<string> ParseLibraryFoldersVdf(string? vdfContent)
        {
            if (string.IsNullOrWhiteSpace(vdfContent)) return [];
            var paths = new List<string>();
            foreach (Match m in VdfPathRegex.Matches(vdfContent))
            {
                string libPath = m.Groups[1].Value.Replace("\\\\", "\\", StringComparison.Ordinal).Trim();
                if (!string.IsNullOrWhiteSpace(libPath))
                    paths.Add(libPath);
            }
            return paths;
        }

        public static string? TryDetectSteamClientPakDir() =>
            DiscoverSteamInstallations().FirstOrDefault()?.PakDir;

        private static IEnumerable<string> EnumerateSteamCommonDirectories()
        {
            var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            AddWindowsRegistrySteamRoots(roots);
            AddDriveSteamRoots(roots);
            var commons = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string root in roots)
            {
                commons.Add(Path.Combine(root, "steamapps", "common"));
                string vdfPath = Path.Combine(root, "steamapps", "libraryfolders.vdf");
                if (!File.Exists(vdfPath)) continue;
                try
                {
                    foreach (string libPath in ParseLibraryFoldersVdf(File.ReadAllText(vdfPath)))
                        commons.Add(Path.Combine(libPath, "steamapps", "common"));
                }
                catch (Exception ex) { Trace.WriteLine(ex.Message); }
            }
            return commons;
        }

        private static void AddWindowsRegistrySteamRoots(HashSet<string> roots)
        {
            if (!OperatingSystem.IsWindows()) return;
            try
            {
                string?[] regPaths =
                [
                    Microsoft.Win32.Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string,
                    Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", null) as string,
                    Microsoft.Win32.Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\Valve\Steam", "InstallPath", null) as string
                ];
                foreach (string? raw in regPaths)
                {
                    if (!string.IsNullOrWhiteSpace(raw))
                        roots.Add(raw.Replace('/', Path.DirectorySeparatorChar).Trim());
                }
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
        }

        private static void AddDriveSteamRoots(HashSet<string> roots)
        {
            try
            {
                if (!OperatingSystem.IsWindows())
                {
                    string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
                    if (!string.IsNullOrWhiteSpace(home))
                    {
                        roots.Add(Path.Combine(home, ".steam", "steam"));
                        roots.Add(Path.Combine(home, ".local", "share", "Steam"));
                        roots.Add(Path.Combine(home, ".var", "app", "com.valvesoftware.Steam", ".local", "share", "Steam"));
                    }
                }
                foreach (DriveInfo drive in DriveInfo.GetDrives())
                {
                    if (!drive.IsReady || drive.DriveType != DriveType.Fixed) continue;
                    string r = drive.RootDirectory.FullName;
                    roots.Add(Path.Combine(r, "Program Files (x86)", "Steam"));
                    roots.Add(Path.Combine(r, "Program Files", "Steam"));
                    roots.Add(Path.Combine(r, "Steam"));
                    roots.Add(Path.Combine(r, "SteamLibrary"));
                    roots.Add(Path.Combine(r, "Games", "Steam"));
                }
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
        }
    }
}

namespace SandalLauncher.ViewModels
{
    public sealed partial class MainWindowViewModel
    {
        private bool _suppressSavedAccountSelectionSync;

        partial void OnSelectedProfileChanged(LauncherProfile? value)
        {
            if (value is null) return;
            _config = _config with
            {
                GatewayHttpAddr = value.GatewayHttpAddr,
                GatewayIcsAddr = value.GatewayIcsAddr,
                GatewayGrpcAddr = value.GatewayGrpcAddr,
                AdminHttpAddr = value.AdminHttpAddr,
                ClientPakDir = !string.IsNullOrWhiteSpace(value.ClientPakDir) && ClientLaunchService.TryResolveClientExecutable(value.ClientPakDir, out _)
                    ? value.ClientPakDir
                    : _config.ClientPakDir,
                ClientUseEos = value.UseEos
            };
            GatewayClient.BaseUri = new Uri(LauncherCliOptions.NormalizeGatewayUrl(value.GatewayHttpAddr));
            SelectedProfileName = value.DisplayName;
            ActiveProfileName = $"Realm: {value.DisplayName}";
            ApplyConfigSnapshot(_config);
        }

        partial void OnSelectedSavedAccountChanged(SavedAccountRecord? value)
        {
            if (_suppressSavedAccountSelectionSync || value is null) return;
            if (IsAuthenticated && IsTemporaryAccount)
                Logout();
            AuthUsername = value.Username;
            AuthPassword = value.Password;
            StatusBannerMessage = $"Selected saved login '{value.Username}'. Click PLAY to launch.";
        }

        partial void OnSelectedDiscoveredInstallChanged(DiscoveredClientInstall? value)
        {
            if (value is not null)
                EditClientPakDir = value.PakDir;
        }

        private void SelectRandomBackground()
        {
            int idx = Random.Shared.Next(3);
            try
            {
                var uri = new Uri($"avares://SandalLauncher/Assets/launcher_background_{idx}.jpg");
                if (Avalonia.Platform.AssetLoader.Exists(uri))
                {
                    using Stream stream = Avalonia.Platform.AssetLoader.Open(uri);
                    BackgroundImage = new Avalonia.Media.Imaging.Bitmap(stream);
                }
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
        }

        private void SeedInitialProfiles(LauncherConfig cfg)
        {
            SelectRandomBackground();
            try
            {
                foreach (LauncherProfile p in ProfileStore.LoadServerProfiles())
                    Profiles.Add(p);
                SelectedProfile = Profiles.FirstOrDefault();
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            InitializeClientDiscovery();
            _ = PurgeAndRefreshAccountsOnStartupAsync(CancellationToken.None);
        }

        public void InitializeClientDiscovery(IEnumerable<DiscoveredClientInstall>? overrideInstalls = null)
        {
            DiscoveredClientInstalls.Clear();
            foreach (DiscoveredClientInstall inst in overrideInstalls ?? SandalLauncher.Views.ClientPathDetector.DiscoverSteamInstallations())
                DiscoveredClientInstalls.Add(inst);
            OnPropertyChanged(nameof(HasDiscoveredClientInstalls));

            bool currentValid = ClientLaunchService.TryResolveClientExecutable(_config.ClientPakDir, out _);
            if (currentValid)
            {
                SelectedDiscoveredInstall = DiscoveredClientInstalls.FirstOrDefault(i =>
                    string.Equals(i.PakDir, _config.ClientPakDir, StringComparison.OrdinalIgnoreCase)) ?? DiscoveredClientInstalls.FirstOrDefault();
                return;
            }
            if (DiscoveredClientInstalls.Count == 1)
            {
                SelectedDiscoveredInstall = DiscoveredClientInstalls[0];
                EditClientPakDir = DiscoveredClientInstalls[0].PakDir;
                _config = _config with { ClientPakDir = DiscoveredClientInstalls[0].PakDir };
                ApplyConfigSnapshot(_config);
                IsClientPathModalOpen = false;
                _ = SaveProfileAsync(CancellationToken.None);
            }
            else if (IsDesktopInteractiveMode())
            {
                SelectedDiscoveredInstall = DiscoveredClientInstalls.FirstOrDefault();
                OpenClientPathModal();
            }
        }

        private static bool IsDesktopInteractiveMode() =>
            Avalonia.Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime &&
            !(SandalLauncher.Program.CliOptions?.Headless ?? false);

        [RelayCommand] private void ToggleSettings() => IsSettingsOpen = !IsSettingsOpen;
        [RelayCommand] private void CloseClientPathModal() => IsClientPathModalOpen = false;

        [RelayCommand]
        public void OpenClientPathModal()
        {
            if (DiscoveredClientInstalls.Count > 1)
            {
                ClientPathModalTitle = "SELECT GAME CLIENT VERSION";
                ClientPathModalSubtitle = "Multiple Ashes of Creation installations were found in your Steam libraries. Choose which version to launch, or paste a custom path:";
            }
            else if (DiscoveredClientInstalls.Count == 1)
            {
                ClientPathModalTitle = "CONFIGURE GAME CLIENT PATH";
                ClientPathModalSubtitle = "Select your detected Steam installation below, or paste a custom Ashes of Creation folder path:";
            }
            else
            {
                ClientPathModalTitle = "GAME CLIENT PATH REQUIRED";
                ClientPathModalSubtitle = "Ashes of Creation was not found in any Steam library folder. Paste the path to your game folder or AOCClient-Win64-Shipping.exe below:";
            }
            IsClientPathModalOpen = true;
        }

        [RelayCommand]
        public async Task ConfirmClientPathAsync(CancellationToken ct = default)
        {
            string candidate = !string.IsNullOrWhiteSpace(EditClientPakDir)
                ? EditClientPakDir.Trim()
                : (SelectedDiscoveredInstall?.PakDir ?? string.Empty);
            if (!ClientLaunchService.TryResolveClientExecutable(candidate, out string resolvedExe))
            {
                StatusBannerMessage = "Could not find AOCClient-Win64-Shipping.exe at that path.";
                ClientStatusSummary = "Invalid path — AOCClient-Win64-Shipping.exe not found";
                return;
            }
            _config = _config with { ClientPakDir = candidate };
            ApplyConfigSnapshot(_config);
            await SaveProfileAsync(ct).ConfigureAwait(false);
            await InvokeOnUiIfDesktopAsync(() =>
            {
                IsClientPathModalOpen = false;
                StatusBannerMessage = $"Saved game client: {resolvedExe}";
            }).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task SaveProfileAsync(CancellationToken ct = default)
        {
            string gw = string.IsNullOrWhiteSpace(EditGatewayHttpAddr) ? _config.GatewayHttpAddr : EditGatewayHttpAddr.Trim();
            string pak = string.IsNullOrWhiteSpace(EditClientPakDir) ? _config.ClientPakDir : EditClientPakDir.Trim();
            string name = string.IsNullOrWhiteSpace(SelectedProfileName) ? "Local Dev (Verra Sandbox)" : SelectedProfileName.Trim();
            _config = _config with { GatewayHttpAddr = gw, ClientPakDir = pak };
            GatewayClient.BaseUri = new Uri(LauncherCliOptions.NormalizeGatewayUrl(gw));
            ApplyConfigSnapshot(_config);
            LauncherProfile updated = LauncherProfile.FromLauncherConfig(_config, SelectedProfile?.Id ?? "local-dev", name);
            try { await ProfileStore.UpsertProfileAsync(updated, ct).ConfigureAwait(false); }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            await InvokeOnUiIfDesktopAsync(() =>
            {
                IsSettingsOpen = false;
                StatusBannerMessage = $"Saved settings ({gw}).";
            }).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task PlayGameAsync(CancellationToken ct = default)
        {
            if (IsDesktopInteractiveMode() && !ClientLaunchService.TryResolveClientExecutable(_config.ClientPakDir, out _))
            {
                OpenClientPathModal();
                StatusBannerMessage = "Select or paste your Ashes of Creation game client path to play.";
                return;
            }
            string desiredUser = AuthUsername.Trim();
            if (!IsAuthenticated || (!string.IsNullOrWhiteSpace(desiredUser) && !string.Equals(desiredUser, SignedInUsername, StringComparison.OrdinalIgnoreCase)))
            {
                if (string.IsNullOrWhiteSpace(desiredUser))
                {
                    StatusBannerMessage = "Login error: enter username and password.";
                    return;
                }
                if (string.IsNullOrEmpty(AuthPassword))
                {
                    SavedAccountRecord? saved = SavedAccounts.FirstOrDefault(a => string.Equals(a.Username, desiredUser, StringComparison.OrdinalIgnoreCase));
                    AuthPassword = saved?.Password ?? await CredentialGuard.Vault.RetrieveTokenAsync(ProfileStoreService.GetSavedAccountVaultKey(desiredUser), ct).ConfigureAwait(false) ?? string.Empty;
                }
                if (string.IsNullOrEmpty(AuthPassword))
                {
                    StatusBannerMessage = "Login error: invalid username or password.";
                    return;
                }
                IsTemporaryAccount = false;
                IsRegisterMode = false;
                try
                {
                    await LoginAsync(ct).ConfigureAwait(false);
                }
                catch (System.Net.Http.HttpRequestException httpEx) when (
                    httpEx.StatusCode is System.Net.HttpStatusCode.Unauthorized or System.Net.HttpStatusCode.Forbidden or System.Net.HttpStatusCode.BadRequest)
                {
                    IsAuthenticated = false;
                    StatusBannerMessage = "Login error: invalid username or password.";
                    return;
                }
                catch (Exception ex)
                {
                    IsAuthenticated = false;
                    StatusBannerMessage = $"Login error: {ex.Message}";
                    return;
                }
            }
            if (!IsAuthenticated || string.IsNullOrWhiteSpace(AccessToken))
            {
                StatusBannerMessage = "Login error: invalid username or password.";
                return;
            }
            await EnsureCharacterAndLaunchAsync(ct).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task RegisterAccountAsync(CancellationToken ct = default)
        {
            string user = AuthUsername.Trim();
            string email = AuthEmail.Trim();
            string pass = AuthPassword;
            if (IsDesktopInteractiveMode() && !ValidateNewAccountInputs(user, email, pass, out string validationMsg))
            {
                IsRegisterMode = true;
                StatusBannerMessage = validationMsg;
                return;
            }
            if (string.IsNullOrWhiteSpace(user)) user = LauncherCliOptions.GenerateTempUsername();
            if (string.IsNullOrEmpty(pass)) pass = LauncherCliOptions.GenerateStrongPassword();
            if (IsAuthenticated && IsTemporaryAccount)
                Logout();
            AuthUsername = user;
            AuthPassword = string.Empty;
            IsTemporaryAccount = false;
            try
            {
                AuthSessionResult session = await GatewayClient.RegisterAsync(user, pass, isTemporaryAccount: false, ct, email).ConfigureAwait(false);
                IsRegisterMode = false;
                AuthEmail = string.Empty;
                await ApplyAuthenticatedSessionAsync(session, isTemp: false, ct).ConfigureAwait(false);
                if (RememberCredentials)
                    await TrySaveAccountToStoreAsync(session.Username, pass, isTemp: false, ct).ConfigureAwait(false);
                if (IsDesktopInteractiveMode())
                    await EnsureCharacterAndLaunchAsync(ct).ConfigureAwait(false);
            }
            catch (System.Net.Http.HttpRequestException httpEx) when (
                httpEx.StatusCode is System.Net.HttpStatusCode.Conflict or System.Net.HttpStatusCode.BadRequest or System.Net.HttpStatusCode.UnprocessableEntity)
            {
                IsRegisterMode = true;
                StatusBannerMessage = "Username or email is already in use (or rejected by gateway).";
            }
            catch (Exception ex)
            {
                IsRegisterMode = true;
                StatusBannerMessage = $"Account creation failed: {ex.Message}";
            }
        }

        private bool ValidateNewAccountInputs(string user, string email, string pass, out string message)
        {
            if (string.IsNullOrWhiteSpace(email) || !email.Contains('@', StringComparison.Ordinal) || !email.Contains('.', StringComparison.Ordinal))
            {
                message = "Enter a valid email address, username, and password, then click Create Account.";
                return false;
            }
            if (user.Length < 3)
            {
                message = "Username must be at least 3 characters.";
                return false;
            }
            if (SavedAccounts.Any(a => string.Equals(a.Username, user, StringComparison.OrdinalIgnoreCase)))
            {
                message = $"Username '{user}' is already in your saved logins.";
                return false;
            }
            if (pass.Length < 6)
            {
                message = "Password must be at least 6 characters.";
                return false;
            }
            message = string.Empty;
            return true;
        }

        [RelayCommand]
        public async Task ForgetSavedAccountAsync(CancellationToken ct = default)
        {
            SavedAccountRecord? acct = SelectedSavedAccount ?? SavedAccounts.FirstOrDefault();
            if (acct is null) return;
            string removedUser = acct.Username;
            try { await ProfileStore.ForgetSavedAccountAsync(CredentialGuard.Vault, removedUser, ct).ConfigureAwait(false); }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            if (string.Equals(AuthUsername, removedUser, StringComparison.OrdinalIgnoreCase))
            {
                AuthUsername = string.Empty;
                AuthPassword = string.Empty;
            }
            await RefreshSavedAccountsAsync(ct, populateInitialSelection: true).ConfigureAwait(false);
            StatusBannerMessage = $"Deleted saved login '{removedUser}'.";
        }

        private async Task EnsureCharacterAndLaunchAsync(CancellationToken ct)
        {
            try
            {
                if (IsAuthenticated && Characters.Count == 0)
                    _ = await CreateCharacterWithIdsAsync(SanitizeAutoCharacterName(SignedInUsername), 1, 1, ct).ConfigureAwait(false);
                if (IsAuthenticated && Characters.Count > 0 && string.IsNullOrWhiteSpace(LastJoinTicket))
                    _ = await SelectCharacterByNameOrIdAsync(Characters[0].CharacterId, spawnClient: false, ct).ConfigureAwait(false);
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            await InvokeOnUiIfDesktopAsync(LaunchClient).ConfigureAwait(false);
        }

        private async Task TrySaveAccountToStoreAsync(string username, string password, bool isTemp, CancellationToken ct)
        {
            try
            {
                await ProfileStore.SaveAccountCredentialsAsync(CredentialGuard.Vault, username, password, isTemp, _config, ct).ConfigureAwait(false);
                if (!isTemp)
                    await RefreshSavedAccountsAsync(ct, populateInitialSelection: false).ConfigureAwait(false);
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
        }

        private async Task PurgeAndRefreshAccountsOnStartupAsync(CancellationToken ct)
        {
            try
            {
                IReadOnlyList<LauncherProfile> staleTemps = await ProfileStore.GetTemporaryProfilesAsync(ct).ConfigureAwait(false);
                foreach (LauncherProfile p in staleTemps)
                    _ = await ProfileStore.DeleteProfileAsync(p.Id, ct).ConfigureAwait(false);
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
            await RefreshSavedAccountsAsync(ct, populateInitialSelection: true).ConfigureAwait(false);
        }

        private async Task RefreshSavedAccountsAsync(CancellationToken ct, bool populateInitialSelection = false)
        {
            try
            {
                IReadOnlyList<SavedAccountRecord> loaded = await ProfileStore.LoadSavedAccountsAsync(CredentialGuard.Vault, ct).ConfigureAwait(false);
                await InvokeOnUiIfDesktopAsync(() =>
                {
                    _suppressSavedAccountSelectionSync = !populateInitialSelection;
                    try
                    {
                        SavedAccounts.Clear();
                        foreach (SavedAccountRecord item in loaded)
                            SavedAccounts.Add(item);
                        SelectedSavedAccount = SavedAccounts.FirstOrDefault();
                        OnPropertyChanged(nameof(HasSavedAccounts));
                    }
                    finally { _suppressSavedAccountSelectionSync = false; }
                }).ConfigureAwait(false);
            }
            catch (Exception ex) { Trace.WriteLine(ex.Message); }
        }
    }
}
