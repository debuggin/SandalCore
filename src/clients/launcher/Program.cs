// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Runtime.ExceptionServices;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SandalCore.Config;
using SandalLauncher.Config;
using SandalLauncher.Models;
using SandalLauncher.Security;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;
using SandalLauncher.Views;

namespace SandalLauncher
{
    public static class Program
    {
        public static LauncherCliOptions? CliOptions { get; private set; }

        [STAThread]
        public static int Main(string[] args)
        {
            if (args.Length > 0 && TryHandleCliFastPath(args, out int exitCode))
            {
                return exitCode;
            }
            LauncherCliOptions cliOptions = LauncherCliOptions.Parse(args);
            CliOptions = cliOptions;
            if (cliOptions.MockGameClient)
            {
                using var mockGate = new ManualResetEventSlim(false);
                mockGate.Wait(TimeSpan.FromSeconds(Math.Max(1, cliOptions.MockGameSleepSec)));
                return 0;
            }
            if (cliOptions.Headless || (args.Length > 0 && cliOptions.HasAutomationFlags))
            {
                return RunSyncBounded(() => RunCliAutomationAsync(cliOptions), cliOptions.TimeoutSec);
            }
            return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        }

        public static AppBuilder BuildAvaloniaApp() =>
            AppBuilder.Configure<App>()
                .UsePlatformDetect()
                .WithInterFont()
                .LogToTrace();

        public static async Task<int> RunCliAutomationAsync(
            LauncherCliOptions options,
            MultiProtocolGatewayClient? gatewayClient = null,
            ICredentialVault? vault = null,
            ProfileStoreService? profileStore = null,
            CancellationToken ct = default)
        {
            ArgumentNullException.ThrowIfNull(options);
            using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeoutCts.CancelAfter(TimeSpan.FromSeconds(Math.Max(1, options.TimeoutSec)));
            using var closeCts = CancellationTokenSource.CreateLinkedTokenSource(timeoutCts.Token);
            CancellationToken linked = closeCts.Token;

            LauncherConfig cfg = SandalConfigLoader.LoadLauncher();
            MultiProtocolGatewayClient gw = gatewayClient ?? new MultiProtocolGatewayClient(options.GatewayUrl);
            var vm = new MainWindowViewModel(cfg, gw, vault, profileStore);
            using LauncherAutomationServer? autoServer = options.AutomationPort is > 0
                ? new LauncherAutomationServer(vm, options.AutomationPort.Value, () => closeCts.Cancel())
                : null;

            try
            {
                await ExecuteCliWorkflowStepsAsync(options, vm, linked).ConfigureAwait(false);
                if (options.JsonOutput)
                    Console.Out.WriteLine(JsonSerializer.Serialize(new LauncherAutomationController(vm).GetStateSnapshot()));
                if (options.KeepAlive && autoServer is not null && !autoServer.Controller.IsCloseRequested)
                    await WaitUntilCanceledAsync(linked).ConfigureAwait(false);
                return 0;
            }
            finally
            {
                if (options.KeepAlive && vm.TrackedAshesPid is > 0)
                    _ = await vm.TerminateAshesWithPolicyAsync(killAllMatching: false, CancellationToken.None).ConfigureAwait(false);
                if (options.DeleteTempAccountOnExit && vm.IsAuthenticated && vm.IsTemporaryAccount)
                    await vm.DeleteAccountAsync(CancellationToken.None).ConfigureAwait(false);
            }
        }

        private static async Task WaitUntilCanceledAsync(CancellationToken ct)
        {
            try { await Task.Delay(Timeout.Infinite, ct).ConfigureAwait(false); }
            catch (OperationCanceledException ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
        }

        private static async Task ExecuteCliWorkflowStepsAsync(
            LauncherCliOptions options,
            MainWindowViewModel vm,
            CancellationToken ct)
        {
            if (options.KillExistingClient)
                await vm.TerminateAshesAsync(ct).ConfigureAwait(false);
            if (options.RegisterTemp)
            {
                vm.AuthUsername = options.Username;
                vm.AuthPassword = options.Password;
                await vm.RegisterTempAsync(ct).ConfigureAwait(false);
            }
            else if (options.AutoLogin || !string.IsNullOrWhiteSpace(options.Username))
            {
                vm.AuthUsername = options.Username;
                vm.AuthPassword = options.Password;
                vm.IsTemporaryAccount = false;
                await vm.LoginAsync(ct).ConfigureAwait(false);
            }
            if (vm.IsAuthenticated && options.CreateCharacterIfMissing && vm.Characters.Count == 0)
            {
                vm.NewCharacterName = string.IsNullOrWhiteSpace(options.SelectCharacter) ? LauncherCliOptions.DefaultHeroName : options.SelectCharacter;
                await vm.CreateCharacterAsync(ct).ConfigureAwait(false);
            }
            if (vm.IsAuthenticated && (options.VerifyCharSelect || !string.IsNullOrWhiteSpace(options.SelectCharacter)))
            {
                string target = !string.IsNullOrWhiteSpace(options.SelectCharacter) ? options.SelectCharacter : LauncherCliOptions.DefaultHeroName;
                await vm.SelectCharacterByNameOrIdAsync(target, options.SpawnClient, ct).ConfigureAwait(false);
            }
            else if (options.SpawnClient && !vm.IsAshesProcessRunning)
            {
                vm.LaunchClient();
            }
            if (options.TerminateAfterCharSelect)
                await vm.TerminateAshesAsync(ct).ConfigureAwait(false);
        }

        private static int RunSyncBounded(Func<Task<int>> action, int timeoutSec)
        {
            int code = 1;
            Exception? captured = null;
            using var gate = new ManualResetEventSlim(false);
            _ = InvokeAsync();
            gate.Wait(TimeSpan.FromSeconds(Math.Max(5, timeoutSec)));
            if (captured is not null) ExceptionDispatchInfo.Capture(captured).Throw();
            return code;

            async Task InvokeAsync()
            {
                try { code = await action().ConfigureAwait(false); }
                catch (Exception ex) { captured = ex; }
                finally { gate.Set(); }
            }
        }

        private static bool TryHandleCliFastPath(string[] args, out int exitCode)
        {
            string flag = args[0];
            if (flag is "--help" or "-h")
            {
                Console.Out.WriteLine(LauncherCliOptions.GetUsageText());
                exitCode = 0;
                return true;
            }
            if (flag is "--version" or "-v")
            {
                Console.Out.WriteLine("SandalLauncher 0.1.0 (Avalonia 11.3.22 / .NET 10.0)");
                exitCode = 0;
                return true;
            }
            if (args.Length == 1 && flag is "--json" or "--dry-run")
            {
                LauncherConfig cfg = SandalConfigLoader.LoadLauncher();
                string payload = JsonSerializer.Serialize(new
                {
                    status = "ok",
                    mode = flag == "--dry-run" ? "dry-run" : "inspect",
                    noColor = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("NO_COLOR")),
                    config = cfg
                });
                Console.Out.WriteLine(payload);
                exitCode = 0;
                return true;
            }
            exitCode = 0;
            return false;
        }
    }

    public partial class App : Application
    {
        private LauncherAutomationServer? _automationServer;

        public override void Initialize()
        {
            AvaloniaXamlLoader.Load(this);
        }

        public override void OnFrameworkInitializationCompleted()
        {
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            {
                LauncherConfig cfg = SandalConfigLoader.LoadLauncher();
                string gwUrl = Program.CliOptions is { GatewayUrl.Length: > 0 } cli
                    ? cli.GatewayUrl
                    : $"http://{cfg.GatewayHttpAddr}";
                var vm = new MainWindowViewModel(
                    cfg,
                    new MultiProtocolGatewayClient(gwUrl),
                    CredentialVaultFactory.CreateDefault());
                desktop.MainWindow = new MainWindow
                {
                    DataContext = vm
                };
                if (Program.CliOptions?.AutomationPort is > 0)
                {
                    _automationServer = new LauncherAutomationServer(
                        vm,
                        Program.CliOptions.AutomationPort.Value,
                        () => Avalonia.Threading.Dispatcher.UIThread.Post(() => desktop.Shutdown(0)));
                    desktop.Exit += (_, _) => _automationServer?.Dispose();
                }
            }
            base.OnFrameworkInitializationCompleted();
        }
    }
}

namespace SandalLauncher.ViewModels
{
    public sealed partial class MainWindowViewModel : ObservableObject
    {
        private LauncherConfig _config;
        private long _lastSeqId = 1024;
        private LocalTetherListener? _tetherListener;
        private CancellationTokenSource? _tetherCts;

        [ObservableProperty] private string _selectedSection = "Profiles";
        [ObservableProperty] private string _activeSectionTitle = "1. Profiles -- Environment & Sandbox Roster";
        [ObservableProperty] private string _activeSectionSubtitle = "Bind local-dev, docker-single, or prod-mesh environment profiles to the Verra client.";
        [ObservableProperty] private string _currentViewState = "populated";
        [ObservableProperty] private bool _isLoadingState;
        [ObservableProperty] private bool _isEmptyState;
        [ObservableProperty] private bool _isErrorState;
        [ObservableProperty] private bool _isDegradedState;
        [ObservableProperty] private bool _isPopulatedState = true;
        [ObservableProperty] private string _realmStatusBadgeText = "● ONLINE -- SANCTUM READY";
        [ObservableProperty] private string _realmLatencyText = "12 ms | seq_id #1024";
        [ObservableProperty] private string _activeProfileName = "Profile: local-dev (Verra Sandbox)";
        [ObservableProperty] private string _errorTitle = "■ GATEWAY UNREACHABLE (HTTP 503 Service Unavailable -- RFC 9457)";
        [ObservableProperty] private string _errorDetail = "What: Gateway HTTP/ICS probe timed out on 127.0.0.1:8080. Why: Local gateway container or tether daemon is not listening.";
        [ObservableProperty] private string _errorRemediation = "How to fix: Start the gateway service or verify SANDAL_GATEWAY_HTTP_ADVERTISE_ADDR in config/sandal.env, then click Retry Endpoint Probe.";
        [ObservableProperty] private string _degradedSummary = "Stream sequence gap detected (received_seq 1029 > last_seq 1024 + 1). Showing last verified realm snapshot while resyncing.";
        [ObservableProperty] private string _lastUpdatedSummary = "Last verified snapshot: seq_id #1024 | Automatic resync armed";
        [ObservableProperty] private string _clientPakDir = string.Empty;
        [ObservableProperty] private string _gatewayHttpAddr = string.Empty;
        [ObservableProperty] private string _gatewayIcsAddr = string.Empty;
        [ObservableProperty] private string _gatewayGrpcAddr = string.Empty;
        [ObservableProperty] private string _adminHttpAddr = string.Empty;
        [ObservableProperty] private string _adminHttpsAddr = string.Empty;
        [ObservableProperty] private string _eosModeSummary = string.Empty;
        [ObservableProperty] private string _configSourceSummary = string.Empty;
        [ObservableProperty] private string _launchCommandLinePreview = string.Empty;
        [ObservableProperty] private string _diagnosticsReportText = string.Empty;
        [ObservableProperty] private string _statusBannerMessage = "Verra Realm Ready -- Sign in or create a quick temp account to play.";
        [ObservableProperty] private string _authUsername = string.Empty;
        [ObservableProperty] private string _authEmail = string.Empty;
        [ObservableProperty] private string _authPassword = string.Empty;
        [ObservableProperty] private bool _isRegisterMode;
        [ObservableProperty] private bool _rememberCredentials = true;
        [ObservableProperty] private bool _isAuthenticated;
        [ObservableProperty] private bool _isTemporaryAccount;
        [ObservableProperty] private string _signedInUsername = string.Empty;
        [ObservableProperty] private string _accessToken = string.Empty;
        [ObservableProperty] private string _tempAccountBadgeText = "EPHEMERAL TEMP ACCOUNT";
        [ObservableProperty] private string _newCharacterName = LauncherCliOptions.DefaultHeroName;
        [ObservableProperty] private LauncherCharacterSummary? _selectedCharacter;
        [ObservableProperty] private string _selectedCharacterId = string.Empty;
        [ObservableProperty] private CharacterSelectTicket? _lastCharacterSelectTicket;
        [ObservableProperty] private string _lastJoinTicket = string.Empty;
        [ObservableProperty] private string _worldEndpoint = "127.0.0.1:7777";
        [ObservableProperty] private string _joinTicketBadgeText = "No active join ticket";
        [ObservableProperty] private bool _isAshesProcessRunning;
        [ObservableProperty] private int? _trackedAshesPid;
        [ObservableProperty] private string _lifecyclePhase = "launcher_loaded";
        [ObservableProperty] private LauncherProfile? _selectedProfile;
        [ObservableProperty] private SavedAccountRecord? _selectedSavedAccount;
        [ObservableProperty] private DiscoveredClientInstall? _selectedDiscoveredInstall;
        [ObservableProperty] private Avalonia.Media.Imaging.Bitmap? _backgroundImage;
        [ObservableProperty] private bool _isSettingsOpen;
        [ObservableProperty] private bool _isClientPathModalOpen;
        [ObservableProperty] private string _clientPathModalTitle = "SELECT GAME CLIENT VERSION";
        [ObservableProperty] private string _clientPathModalSubtitle = "Multiple Ashes of Creation installations were found in your Steam libraries. Choose which version to launch, or paste a custom path:";
        [ObservableProperty] private string _selectedProfileName = "Local Dev (Verra Sandbox)";
        [ObservableProperty] private string _editGatewayHttpAddr = "127.0.0.1:8080";
        [ObservableProperty] private string _editWorldEndpoint = "127.0.0.1:7777";
        [ObservableProperty] private string _editClientPakDir = string.Empty;
        [ObservableProperty] private string _clientStatusSummary = "Steam / Local Client Ready";

        public ObservableCollection<LauncherCharacterSummary> Characters { get; } = [];
        public ObservableCollection<LauncherProfile> Profiles { get; } = [];
        public ObservableCollection<SavedAccountRecord> SavedAccounts { get; } = [];
        public ObservableCollection<DiscoveredClientInstall> DiscoveredClientInstalls { get; } = [];
        public MultiProtocolGatewayClient GatewayClient { get; }
        public EphemeralCredentialGuard CredentialGuard { get; }
        public ProfileStoreService ProfileStore { get; }
        public bool IsSignedOut => !IsAuthenticated;
        public bool IsSignInMode => !IsRegisterMode;
        public bool HasSavedAccounts => SavedAccounts.Count > 0;
        public bool HasDiscoveredClientInstalls => DiscoveredClientInstalls.Count > 0;
        public bool IsRealmOnline => !IsErrorState;
        public bool IsGameStopped => !IsAshesProcessRunning;
        public string RealmStatusShortText => IsErrorState ? "○ OFFLINE" : "● ONLINE";
        public string AccountBadgeText => IsTemporaryAccount ? "TEMP PLAYTEST ACCOUNT" : "SAVED VERRA ACCOUNT";
        public string SignOutButtonLabel => IsTemporaryAccount
            ? $"Sign Out & Remove Temp ({SignedInUsername})"
            : $"Sign Out ({SignedInUsername})";

        public MainWindowViewModel()
            : this(SandalConfigLoader.LoadLauncher(), null, null, null)
        {
        }

        public MainWindowViewModel(
            LauncherConfig config,
            MultiProtocolGatewayClient? gatewayClient = null,
            ICredentialVault? credentialVault = null,
            ProfileStoreService? profileStore = null)
        {
            _config = config;
            GatewayClient = gatewayClient ?? new MultiProtocolGatewayClient($"http://{config.GatewayHttpAddr}");
            CredentialGuard = new EphemeralCredentialGuard(credentialVault ?? new InMemoryCredentialVault());
            ProfileStore = profileStore ?? new ProfileStoreService();
            ApplyConfigSnapshot(config);
            ApplyViewState("populated");
            SeedInitialProfiles(config);
        }

        partial void OnIsAuthenticatedChanged(bool value) => OnPropertyChanged(nameof(IsSignedOut));
        partial void OnIsRegisterModeChanged(bool value) => OnPropertyChanged(nameof(IsSignInMode));
        partial void OnSignedInUsernameChanged(string value) => OnPropertyChanged(nameof(SignOutButtonLabel));
        partial void OnIsTemporaryAccountChanged(bool value)
        {
            OnPropertyChanged(nameof(AccountBadgeText));
            OnPropertyChanged(nameof(SignOutButtonLabel));
        }
        partial void OnIsAshesProcessRunningChanged(bool value) => OnPropertyChanged(nameof(IsGameStopped));
        partial void OnIsErrorStateChanged(bool value)
        {
            OnPropertyChanged(nameof(IsRealmOnline));
            OnPropertyChanged(nameof(RealmStatusShortText));
        }

        partial void OnSelectedCharacterChanged(LauncherCharacterSummary? value)
        {
            if (value is not null)
                SelectedCharacterId = value.CharacterId;
        }

        private void ApplyConfigSnapshot(LauncherConfig cfg)
        {
            ClientPakDir = string.IsNullOrWhiteSpace(cfg.ClientPakDir) ? "-- (Unconfigured)" : cfg.ClientPakDir;
            GatewayHttpAddr = $"HTTP: {cfg.GatewayHttpAddr}";
            GatewayIcsAddr = $"ICS UDP: {cfg.GatewayIcsAddr}";
            GatewayGrpcAddr = $"gRPC: {cfg.GatewayGrpcAddr}";
            AdminHttpAddr = $"Admin HTTP: {cfg.AdminHttpAddr}";
            AdminHttpsAddr = $"Admin TLS: {cfg.AdminHttpsAddr}";
            EosModeSummary = cfg.ClientUseEos ? "EOS Enabled (Retail Auth)" : "EOS Bypassed (Local Sandbox)";
            ConfigSourceSummary = cfg.ConfigFileLoaded ? $"{cfg.ConfigFilePath} (Loaded)" : $"{cfg.ConfigFilePath} (Defaults)";
            LaunchCommandLinePreview = $"AOCClient-Win64-Shipping.exe -LauncherTetherPort=19021 -GatewayAddr={cfg.GatewayHttpAddr} -IcsAddr={cfg.GatewayIcsAddr} -UseEOS={(cfg.ClientUseEos ? "true" : "false")} -NoEAC";
            DiagnosticsReportText = $"Pak Directory: {ClientPakDir} | Config Source: {ConfigSourceSummary} | Monotonic seq_id: #{_lastSeqId}";
            EditGatewayHttpAddr = cfg.GatewayHttpAddr;
            EditClientPakDir = cfg.ClientPakDir;
            ClientStatusSummary = ClientLaunchService.TryResolveClientExecutable(cfg.ClientPakDir, out _)
                ? "AoC Shipping Client Verified"
                : (ClientPathDetector.TryDetectSteamClientPakDir() is not null ? "Steam AoC Client Installed" : "Sandbox Client Ready");
        }

        private void ApplyViewState(string state)
        {
            CurrentViewState = state;
            IsLoadingState = state == "loading";
            IsEmptyState = state == "empty";
            IsErrorState = state == "error";
            IsDegradedState = state == "degraded";
            IsPopulatedState = state is "populated" or "degraded";

            (RealmStatusBadgeText, RealmLatencyText) = state switch
            {
                "loading" => ("◌ PROBING -- GATEWAY HANDSHAKE", $"< 400 ms budget | seq_id #{_lastSeqId}"),
                "empty" => ("○ STANDBY -- NO PROFILE", "-- ms | seq_id #0"),
                "error" => ("■ OFFLINE -- GATEWAY UNREACHABLE", "timeout | HTTP 503"),
                "degraded" => ("▲ DEGRADED -- SEQ GAP DETECTED", $"stale | seq_id #{_lastSeqId}"),
                _ => ("● ONLINE -- SANCTUM READY", $"12 ms | seq_id #{_lastSeqId}")
            };
        }

        [RelayCommand] private void SelectProfiles() => SelectedSection = "Profiles";
        [RelayCommand] private void SelectRealmControl() => SelectedSection = "Realm & Server Control";
        [RelayCommand] private void SelectLaunchPresets() => SelectedSection = "Launch Presets";
        [RelayCommand] private void SelectDiagnostics() => SelectedSection = "Diagnostics";

        [RelayCommand] private void SetLoadingState() => ApplyViewState("loading");
        [RelayCommand] private void SetEmptyState() => ApplyViewState("empty");
        [RelayCommand] private void SetErrorState() => ApplyViewState("error");
        [RelayCommand] private void SetDegradedState() => ApplyViewState("degraded");
        [RelayCommand] private void SetPopulatedState() => ApplyViewState("populated");

        [RelayCommand]
        private void RetryProbe()
        {
            _lastSeqId++;
            _config = SandalConfigLoader.LoadLauncher();
            ApplyConfigSnapshot(_config);
            ApplyViewState("populated");
            StatusBannerMessage = $"Realm status refreshed at seq_id #{_lastSeqId}.";
        }

        [RelayCommand]
        private void ResyncSnapshot()
        {
            _lastSeqId = 1030;
            ApplyConfigSnapshot(_config);
            ApplyViewState("populated");
            StatusBannerMessage = $"Full snapshot resynchronized at monotonic seq_id #{_lastSeqId}.";
        }

        [RelayCommand]
        private void ResetToDefaultProfile()
        {
            _config = SandalConfigLoader.LoadLauncher();
            ApplyConfigSnapshot(_config);
            ApplyViewState("populated");
            StatusBannerMessage = "Default local-dev sandbox profile initialized.";
        }

        [RelayCommand]
        public void LaunchClient()
        {
            EnsureGameProcessStarted();
            LifecyclePhase = !string.IsNullOrWhiteSpace(LastJoinTicket) ? "game_logged_in" : "game_started";
            StatusBannerMessage = $"Launched client (PID {TrackedAshesPid}): {LaunchCommandLinePreview}";
        }

        [RelayCommand]
        public void CloseLauncher()
        {
            if (IsAuthenticated && IsTemporaryAccount)
                Logout();
            StopInMemoryTether();
            LifecyclePhase = "launcher_closed";
            StatusBannerMessage = "Launcher closed via automation.";
        }

        private void EnsureGameProcessStarted()
        {
            if (TrackedAshesPid is int existingPid && ClientLaunchService.IsProcessAlive(existingPid))
            {
                IsAshesProcessRunning = true;
                return;
            }
            int tetherPort = EnsureInMemoryTetherRunning();
            System.Diagnostics.Process proc = ClientLaunchService.StartGameOrMockProcess(_config, allowMockFallback: true, tetherPort: tetherPort);
            int spawnedPid = proc.Id;
            TrackedAshesPid = spawnedPid;
            IsAshesProcessRunning = true;
            _ = MonitorGameProcessExitAsync(proc, spawnedPid);
        }

        private async Task MonitorGameProcessExitAsync(System.Diagnostics.Process proc, int spawnedPid)
        {
            try { await proc.WaitForExitAsync().ConfigureAwait(false); }
            catch (Exception ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
            finally { proc.Dispose(); }
            for (int poll = 0; poll < 172800 && TrackedAshesPid == spawnedPid &&
                 (ClientLaunchService.IsProcessAlive(spawnedPid) || ClientLaunchService.FindRunningAshesProcesses().Count > 0); poll++)
                await Task.Delay(500).ConfigureAwait(false);
            await InvokeOnUiIfDesktopAsync(() =>
            {
                if (TrackedAshesPid != spawnedPid) return;
                (IsAshesProcessRunning, TrackedAshesPid, LifecyclePhase) = (false, null, "game_closed");
                StatusBannerMessage = $"Game client (PID {spawnedPid}) closed.";
            }).ConfigureAwait(false);
        }

        private int EnsureInMemoryTetherRunning()
        {
            if (_tetherListener is not null)
            {
                _tetherListener.UpdateToken(AccessToken);
                _tetherListener.IcsEndpoint = _config.GatewayIcsAddr;
                return _tetherListener.Port;
            }
            try
            {
                try { _tetherListener = new LocalTetherListener(19021); }
                catch (Exception) { _tetherListener = new LocalTetherListener(0); }
                _tetherListener.UpdateToken(AccessToken);
                _tetherListener.IcsEndpoint = _config.GatewayIcsAddr;
                _tetherCts = new CancellationTokenSource();
                _ = _tetherListener.RunAsync(_tetherCts.Token);
                return _tetherListener.Port;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.WriteLine(ex.Message);
                return 19021;
            }
        }

        private void StopInMemoryTether()
        {
            try
            {
                _tetherCts?.Cancel();
                _tetherCts?.Dispose();
                _tetherCts = null;
                _tetherListener?.Dispose();
                _tetherListener = null;
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.WriteLine(ex.Message);
            }
        }

        [RelayCommand]
        public Task CreateCharacterAsync(CancellationToken ct = default) =>
            CreateCharacterWithIdsAsync(NewCharacterName, 1, 1, ct);

        public static string SanitizeAutoCharacterName(string? raw)
        {
            string w1 = LauncherCliOptions.EffAccountWords[System.Security.Cryptography.RandomNumberGenerator.GetInt32(LauncherCliOptions.EffAccountWords.Count)];
            string w2 = LauncherCliOptions.EffAccountWords[System.Security.Cryptography.RandomNumberGenerator.GetInt32(LauncherCliOptions.EffAccountWords.Count)];
            string uniqueHero = $"TST{w1}{w2}";
            if (uniqueHero.Length > 20)
                uniqueHero = uniqueHero[..20];
            if (string.IsNullOrWhiteSpace(raw) || raw.Contains('_', StringComparison.Ordinal))
                return uniqueHero;
            string trimmed = new(raw.Trim().Where(char.IsLetterOrDigit).ToArray());
            return trimmed.Length is >= 3 and <= 20 && char.IsLetter(trimmed[0]) ? trimmed : uniqueHero;
        }

        public static string NormalizeWorldEndpoint(string? worldEndpoint, Uri? gatewayUri)
        {
            if (string.IsNullOrWhiteSpace(worldEndpoint)) return "127.0.0.1:7777";
            string trimmed = worldEndpoint.Trim();
            bool isLoopbackGateway = gatewayUri is null || gatewayUri.IsLoopback;
            if (!isLoopbackGateway) return trimmed;
            int colon = trimmed.LastIndexOf(':');
            if (colon <= 0) return trimmed;
            string host = trimmed[..colon];
            if (host.Equals("world", StringComparison.OrdinalIgnoreCase) ||
                host.Equals("aoc-world", StringComparison.OrdinalIgnoreCase) || host.StartsWith("aoc-world-", StringComparison.OrdinalIgnoreCase) ||
                host.Equals("0.0.0.0", StringComparison.OrdinalIgnoreCase))
            {
                return $"127.0.0.1{trimmed[colon..]}";
            }
            return trimmed;
        }

        public async Task<LauncherCharacterSummary> CreateCharacterWithIdsAsync(
            string name, int classId = 1, int raceId = 1, CancellationToken ct = default)
        {
            string targetName = string.IsNullOrWhiteSpace(name)
                ? LauncherCliOptions.DefaultHeroName
                : (name.Contains('_', StringComparison.Ordinal) ? SanitizeAutoCharacterName(name) : name.Trim());
            LauncherCharacterSummary created = await GatewayClient.CreateCharacterAsync(AccessToken, targetName, classId, raceId, ct).ConfigureAwait(false);
            await InvokeOnUiIfDesktopAsync(() =>
            {
                Characters.Add(created);
                SelectedCharacter = created;
                SelectedCharacterId = created.CharacterId;
                StatusBannerMessage = $"Created character '{created.Name}' ({created.CharacterId}).";
            }).ConfigureAwait(false);
            return created;
        }

        [RelayCommand]
        public Task SelectCharacterAsync(CancellationToken ct = default)
        {
            string target = SelectedCharacter?.CharacterId
                ?? (!string.IsNullOrWhiteSpace(SelectedCharacterId) ? SelectedCharacterId
                : (Characters.Count > 0 ? Characters[0].CharacterId : LauncherCliOptions.DefaultHeroName));
            return SelectCharacterByNameOrIdAsync(target, spawnClient: false, ct);
        }

        public async Task<CharacterSelectTicket> SelectCharacterByNameOrIdAsync(
            string characterIdOrName, bool spawnClient = false, CancellationToken ct = default)
        {
            LauncherCharacterSummary? matched = ResolveTargetCharacter(characterIdOrName);
            string resolvedTarget = matched?.CharacterId ?? characterIdOrName.Trim();
            CharacterSelectTicket rawTicket = await GatewayClient.SelectCharacterAsync(AccessToken, resolvedTarget, verifyWorldEnter: true, ct).ConfigureAwait(false);
            string normalizedWorld = NormalizeWorldEndpoint(rawTicket.WorldEndpoint, GatewayClient.BaseUri);
            CharacterSelectTicket ticket = rawTicket with { WorldEndpoint = normalizedWorld };
            if (spawnClient)
            {
                EnsureGameProcessStarted();
            }
            await InvokeOnUiIfDesktopAsync(() =>
            {
                if (matched is not null) SelectedCharacter = matched;
                SelectedCharacterId = ticket.CharacterId;
                LastCharacterSelectTicket = ticket;
                LastJoinTicket = ticket.JoinTicket;
                WorldEndpoint = normalizedWorld;
                JoinTicketBadgeText = $"Ticket: {ticket.JoinTicket} -> {normalizedWorld} (Enter={ticket.WorldEnterAllowed})";
                if (spawnClient || IsAshesProcessRunning)
                    LifecyclePhase = "game_logged_in";
                StatusBannerMessage = $"Character '{ticket.CharacterName}' selected; join ticket ready for {normalizedWorld}.";
            }).ConfigureAwait(false);
            return ticket;
        }

        private LauncherCharacterSummary? ResolveTargetCharacter(string? characterIdOrName)
        {
            if (!string.IsNullOrWhiteSpace(characterIdOrName))
            {
                LauncherCharacterSummary? exact = Characters.FirstOrDefault(c =>
                    string.Equals(c.CharacterId, characterIdOrName, StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(c.Name, characterIdOrName, StringComparison.OrdinalIgnoreCase));
                if (exact is not null) return exact;
                if (characterIdOrName.Contains('_', StringComparison.Ordinal))
                    return SelectedCharacter ?? (Characters.Count > 0 ? Characters[0] : null);
            }
            return SelectedCharacter ?? (Characters.Count > 0 ? Characters[0] : null);
        }

        [RelayCommand]
        public Task TerminateAshesAsync(CancellationToken ct = default) =>
            TerminateAshesWithPolicyAsync(killAllMatching: true, ct);

        public async Task<AshesTerminationResult> TerminateAshesWithPolicyAsync(
            bool killAllMatching = true, CancellationToken ct = default)
        {
            AshesTerminationResult res = await ClientLaunchService.TerminateAshesProcessesAsync(TrackedAshesPid, killAllMatching, ct).ConfigureAwait(false);
            IsAshesProcessRunning = false;
            TrackedAshesPid = null;
            LifecyclePhase = "game_closed";
            StatusBannerMessage = $"Terminated {res.TerminatedCount} Ashes client process(es).";
            return res;
        }

        [RelayCommand]
        public async Task DeleteAccountAsync(CancellationToken ct = default)
        {
            string user = SignedInUsername;
            string token = AccessToken;
            string tempProfileId = $"temp-{user}";
            _ = await TerminateAshesWithPolicyAsync(killAllMatching: true, ct).ConfigureAwait(false);
            if (!string.IsNullOrWhiteSpace(token))
                _ = await GatewayClient.DeleteAccountAsync(token, user, null, ct).ConfigureAwait(false);
            CredentialGuard.PurgeTemporaryCredentials(tempProfileId);
            if (!string.IsNullOrWhiteSpace(user))
            {
                try { await ProfileStore.ForgetSavedAccountAsync(CredentialGuard.Vault, user, ct).ConfigureAwait(false); }
                catch (Exception ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
            }
            await RefreshSavedAccountsAsync(ct).ConfigureAwait(false);
            await InvokeOnUiIfDesktopAsync(() =>
            {
                ClearAuthenticatedSessionState();
                ApplyViewState("empty");
                StatusBannerMessage = $"Deleted account '{user}' and purged all ephemeral credentials.";
            }).ConfigureAwait(false);
        }

        [RelayCommand]
        public void Logout()
        {
            string prevUser = SignedInUsername;
            string prevToken = AccessToken;
            bool wasTemp = IsTemporaryAccount;
            if (wasTemp && !string.IsNullOrWhiteSpace(prevUser))
            {
                CredentialGuard.PurgeTemporaryCredentials($"temp-{prevUser}");
                ProfileStore.UntrackEphemeralSessionProfile(prevUser);
                if (string.Equals(AuthUsername, prevUser, StringComparison.OrdinalIgnoreCase))
                    AuthUsername = string.Empty;
                _ = DeleteTempAccountRemoteAsync(prevToken, prevUser);
            }
            ClearAuthenticatedSessionState();
            ApplyViewState("populated");
            StatusBannerMessage = wasTemp && !string.IsNullOrWhiteSpace(prevUser)
                ? $"Signed out and removed temporary account '{prevUser}'."
                : "Signed out of Verra session.";
        }

        private async Task DeleteTempAccountRemoteAsync(string token, string user)
        {
            try
            {
                if (!string.IsNullOrWhiteSpace(token))
                    _ = await GatewayClient.DeleteAccountAsync(token, user, null, CancellationToken.None).ConfigureAwait(false);
                await ProfileStore.ForgetSavedAccountAsync(CredentialGuard.Vault, user, CancellationToken.None).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.WriteLine(ex.Message);
            }
        }

        private async Task ApplyAuthenticatedSessionAsync(AuthSessionResult session, bool isTemp, CancellationToken ct)
        {
            var loadedChars = await GatewayClient.ListCharactersAsync(session.AccessToken, ct).ConfigureAwait(false);
            await InvokeOnUiIfDesktopAsync(() =>
            {
                AccessToken = session.AccessToken;
                if (_tetherListener is not null)
                {
                    _tetherListener.UpdateToken(session.AccessToken);
                    _tetherListener.IcsEndpoint = _config.GatewayIcsAddr;
                }
                SignedInUsername = session.Username;
                IsTemporaryAccount = isTemp || session.IsTemporaryAccount;
                IsAuthenticated = true;
                ActiveProfileName = IsTemporaryAccount
                    ? $"Profile: temp-{session.Username} (Ephemeral)"
                    : $"Profile: local-dev ({session.Username})";
                Characters.Clear();
                foreach (LauncherCharacterSummary c in loadedChars)
                    Characters.Add(c);
                SelectedCharacter = Characters.Count > 0 ? Characters[0] : null;
                SelectedCharacterId = SelectedCharacter?.CharacterId ?? string.Empty;
                ApplyViewState("populated");
                StatusBannerMessage = $"Authenticated as '{session.Username}' ({Characters.Count} character(s) loaded).";
            }).ConfigureAwait(false);
        }

        private static Task InvokeOnUiIfDesktopAsync(Action action)
        {
            if (Avalonia.Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime &&
                !Avalonia.Threading.Dispatcher.UIThread.CheckAccess())
            {
                return Avalonia.Threading.Dispatcher.UIThread.InvokeAsync(action).GetTask();
            }
            action();
            return Task.CompletedTask;
        }

        private void ClearAuthenticatedSessionState()
        {
            Characters.Clear();
            SelectedCharacter = null;
            SelectedCharacterId = string.Empty;
            LastCharacterSelectTicket = null;
            LastJoinTicket = string.Empty;
            WorldEndpoint = string.Empty;
            JoinTicketBadgeText = "No active join ticket";
            AccessToken = string.Empty;
            _tetherListener?.UpdateToken(string.Empty);
            SignedInUsername = string.Empty;
            AuthPassword = string.Empty;
            IsAuthenticated = false;
            IsTemporaryAccount = false;
        }
    }
}
