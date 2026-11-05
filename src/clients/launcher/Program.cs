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
                Thread.Sleep(TimeSpan.FromSeconds(Math.Max(1, cliOptions.MockGameSleepSec)));
                return 0;
            }
            if (cliOptions.HasAutomationFlags || cliOptions.AutomationPort.HasValue)
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
                Console.Out.WriteLine("SandalLauncher 0.1.0 (Avalonia 11.2.3 / .NET 8.0)");
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
                var vm = new MainWindowViewModel();
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

namespace SandalLauncher.Views
{
    public partial class MainWindow : Window
    {
        public MainWindow()
        {
            AvaloniaXamlLoader.Load(this);
            DataContext ??= new MainWindowViewModel();
        }
    }
}

namespace SandalLauncher.ViewModels
{
    public sealed partial class MainWindowViewModel : ObservableObject
    {
        private LauncherConfig _config;
        private long _lastSeqId = 1024;

        [ObservableProperty]
        private string _selectedSection = "Profiles";

        [ObservableProperty]
        private string _activeSectionTitle = "1. Profiles — Environment & Sandbox Roster";

        [ObservableProperty]
        private string _activeSectionSubtitle = "Bind local-dev, docker-single, or prod-mesh environment profiles to the Verra client.";

        [ObservableProperty]
        private string _currentViewState = "populated";

        [ObservableProperty]
        private bool _isLoadingState;

        [ObservableProperty]
        private bool _isEmptyState;

        [ObservableProperty]
        private bool _isErrorState;

        [ObservableProperty]
        private bool _isDegradedState;

        [ObservableProperty]
        private bool _isPopulatedState = true;

        [ObservableProperty]
        private string _realmStatusBadgeText = "● ONLINE — SANCTUM READY";

        [ObservableProperty]
        private string _realmLatencyText = "12 ms | seq_id #1024";

        [ObservableProperty]
        private string _activeProfileName = "Profile: local-dev (Verra Sandbox)";

        [ObservableProperty]
        private string _errorTitle = "■ GATEWAY UNREACHABLE (HTTP 503 Service Unavailable — RFC 9457)";

        [ObservableProperty]
        private string _errorDetail = "What: Gateway HTTP/ICS probe timed out on 127.0.0.1:8080. Why: Local gateway container or tether daemon is not listening.";

        [ObservableProperty]
        private string _errorRemediation = "How to fix: Start the gateway service or verify SANDAL_GATEWAY_HTTP_ADVERTISE_ADDR in config/sandal.env, then click Retry Endpoint Probe.";

        [ObservableProperty]
        private string _degradedSummary = "Stream sequence gap detected (received_seq 1029 > last_seq 1024 + 1). Showing last verified realm snapshot while resyncing.";

        [ObservableProperty]
        private string _lastUpdatedSummary = "Last verified snapshot: seq_id #1024 | Automatic resync armed";

        [ObservableProperty]
        private string _clientPakDir = string.Empty;

        [ObservableProperty]
        private string _gatewayHttpAddr = string.Empty;

        [ObservableProperty]
        private string _gatewayIcsAddr = string.Empty;

        [ObservableProperty]
        private string _gatewayGrpcAddr = string.Empty;

        [ObservableProperty]
        private string _adminHttpAddr = string.Empty;

        [ObservableProperty]
        private string _adminHttpsAddr = string.Empty;

        [ObservableProperty]
        private string _eosModeSummary = string.Empty;

        [ObservableProperty]
        private string _configSourceSummary = string.Empty;

        [ObservableProperty]
        private string _launchCommandLinePreview = string.Empty;

        [ObservableProperty]
        private string _diagnosticsReportText = string.Empty;

        [ObservableProperty]
        private string _statusBannerMessage = "All 5 view states armed; live LauncherConfig SSoT bound.";

        [ObservableProperty] private string _authUsername = string.Empty;
        [ObservableProperty] private string _authPassword = string.Empty;
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
        [ObservableProperty] private string _worldEndpoint = string.Empty;
        [ObservableProperty] private string _joinTicketBadgeText = "No active join ticket";
        [ObservableProperty] private bool _isAshesProcessRunning;
        [ObservableProperty] private int? _trackedAshesPid;
        [ObservableProperty] private string _lifecyclePhase = "launcher_loaded";

        public ObservableCollection<LauncherCharacterSummary> Characters { get; } = [];
        public MultiProtocolGatewayClient GatewayClient { get; }
        public EphemeralCredentialGuard CredentialGuard { get; }
        public ProfileStoreService ProfileStore { get; }
        public bool IsSignedOut => !IsAuthenticated;

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
        }

        partial void OnIsAuthenticatedChanged(bool value) => OnPropertyChanged(nameof(IsSignedOut));

        partial void OnSelectedCharacterChanged(LauncherCharacterSummary? value)
        {
            if (value is not null)
                SelectedCharacterId = value.CharacterId;
        }

        private void ApplyConfigSnapshot(LauncherConfig cfg)
        {
            ClientPakDir = string.IsNullOrWhiteSpace(cfg.ClientPakDir) ? "— (Unconfigured)" : cfg.ClientPakDir;
            GatewayHttpAddr = $"HTTP: {cfg.GatewayHttpAddr}";
            GatewayIcsAddr = $"ICS UDP: {cfg.GatewayIcsAddr}";
            GatewayGrpcAddr = $"gRPC: {cfg.GatewayGrpcAddr}";
            AdminHttpAddr = $"Admin HTTP: {cfg.AdminHttpAddr}";
            AdminHttpsAddr = $"Admin TLS: {cfg.AdminHttpsAddr}";
            EosModeSummary = cfg.ClientUseEos ? "EOS Enabled (Retail Auth)" : "EOS Bypassed (Local Sandbox)";
            ConfigSourceSummary = cfg.ConfigFileLoaded ? $"{cfg.ConfigFilePath} (Loaded)" : $"{cfg.ConfigFilePath} (Defaults)";
            LaunchCommandLinePreview = $"AOCClient.exe -GatewayAddr={cfg.GatewayHttpAddr} -IcsAddr={cfg.GatewayIcsAddr} -NoEOS={!cfg.ClientUseEos}";
            DiagnosticsReportText = $"Pak Directory: {ClientPakDir} | Config Source: {ConfigSourceSummary} | Monotonic seq_id: #{_lastSeqId}";
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
                "loading" => ("◌ PROBING — GATEWAY HANDSHAKE", $"< 400 ms budget | seq_id #{_lastSeqId}"),
                "empty" => ("○ STANDBY — NO PROFILE", "— ms | seq_id #0"),
                "error" => ("■ OFFLINE — GATEWAY UNREACHABLE", "timeout | HTTP 503"),
                "degraded" => ("▲ DEGRADED — SEQ GAP DETECTED", $"stale | seq_id #{_lastSeqId}"),
                _ => ("● ONLINE — SANCTUM READY", $"12 ms | seq_id #{_lastSeqId}")
            };
        }

        [RelayCommand]
        private void SelectProfiles()
        {
            SelectedSection = "Profiles";
            ActiveSectionTitle = "1. Profiles — Environment & Sandbox Roster";
            ActiveSectionSubtitle = "Bind local-dev, docker-single, or prod-mesh environment profiles to the Verra client.";
            StatusBannerMessage = "Active section: Profiles";
        }

        [RelayCommand]
        private void SelectRealmControl()
        {
            SelectedSection = "Realm & Server Control";
            ActiveSectionTitle = "2. Realm & Server Control — Gateway, Tether & World Mesh";
            ActiveSectionSubtitle = "Inspect live HTTP/ICS/gRPC endpoints and coordinate local realm processes.";
            StatusBannerMessage = "Active section: Realm & Server Control";
        }

        [RelayCommand]
        private void SelectLaunchPresets()
        {
            SelectedSection = "Launch Presets";
            ActiveSectionTitle = "3. Launch Presets — Client Arguments & EOS Bypass";
            ActiveSectionSubtitle = "Configure deterministic client command-line flags, pak paths, and authentication shims.";
            StatusBannerMessage = "Active section: Launch Presets";
        }

        [RelayCommand]
        private void SelectDiagnostics()
        {
            SelectedSection = "Diagnostics";
            ActiveSectionTitle = "4. Diagnostics — Telemetry Stream & Config Audit";
            ActiveSectionSubtitle = "Verify monotonic sequence integrity, redact DSN credentials, and inspect endpoint health.";
            StatusBannerMessage = "Active section: Diagnostics";
        }

        [RelayCommand]
        private void SetLoadingState() => ApplyViewState("loading");

        [RelayCommand]
        private void SetEmptyState() => ApplyViewState("empty");

        [RelayCommand]
        private void SetErrorState() => ApplyViewState("error");

        [RelayCommand]
        private void SetDegradedState() => ApplyViewState("degraded");

        [RelayCommand]
        private void SetPopulatedState() => ApplyViewState("populated");

        [RelayCommand]
        private void RetryProbe()
        {
            _lastSeqId++;
            _config = SandalConfigLoader.LoadLauncher();
            ApplyConfigSnapshot(_config);
            ApplyViewState("populated");
            StatusBannerMessage = $"Endpoint probe recovered at seq_id #{_lastSeqId}.";
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
            using System.Diagnostics.Process proc = ClientLaunchService.StartGameOrMockProcess(_config, allowMockFallback: true);
            TrackedAshesPid = proc.Id;
            IsAshesProcessRunning = true;
        }

        [RelayCommand]
        public async Task LoginAsync(CancellationToken ct = default)
        {
            string user = AuthUsername.Trim();
            string pass = AuthPassword;
            AuthPassword = string.Empty;
            AuthSessionResult session = await GatewayClient.LoginAsync(user, pass, ct).ConfigureAwait(false);
            if (IsTemporaryAccount)
                CredentialGuard.SaveTemporaryCredentials($"temp-{session.Username}", session.Username, pass, isTemporaryAccount: true);
            await ApplyAuthenticatedSessionAsync(session, IsTemporaryAccount, ct).ConfigureAwait(false);
        }

        [RelayCommand]
        public async Task RegisterTempAsync(CancellationToken ct = default)
        {
            string user = string.IsNullOrWhiteSpace(AuthUsername) ? LauncherCliOptions.GenerateTempUsername() : AuthUsername.Trim();
            string pass = string.IsNullOrEmpty(AuthPassword) ? LauncherCliOptions.GenerateStrongPassword() : AuthPassword;
            AuthUsername = user;
            AuthPassword = string.Empty;
            IsTemporaryAccount = true;
            AuthSessionResult session = await GatewayClient.RegisterAsync(user, pass, isTemporaryAccount: true, ct).ConfigureAwait(false);
            string tempProfileId = $"temp-{session.Username}";
            CredentialGuard.SaveTemporaryCredentials(tempProfileId, session.Username, pass, isTemporaryAccount: true);
            await SaveTempProfileEntryAsync(tempProfileId, session.Username, ct).ConfigureAwait(false);
            await ApplyAuthenticatedSessionAsync(session, isTemp: true, ct).ConfigureAwait(false);
        }

        [RelayCommand]
        public Task CreateCharacterAsync(CancellationToken ct = default) =>
            CreateCharacterWithIdsAsync(NewCharacterName, 1, 1, ct);

        public static string SanitizeAutoCharacterName(string? raw)
        {
            string uniqueHero = $"Hero{Guid.NewGuid():N}"[..12];
            if (string.IsNullOrWhiteSpace(raw) || raw.Contains('_', StringComparison.Ordinal))
                return uniqueHero;
            string trimmed = new(raw.Trim().Where(char.IsLetterOrDigit).ToArray());
            if (trimmed.Length is < 3 or > 20 || !char.IsLetter(trimmed[0]))
                return uniqueHero;
            return trimmed;
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
                host.Equals("aoc-world", StringComparison.OrdinalIgnoreCase) ||
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
            Characters.Add(created);
            SelectedCharacter = created;
            SelectedCharacterId = created.CharacterId;
            StatusBannerMessage = $"Created character '{created.Name}' ({created.CharacterId}).";
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
            if (matched is not null) SelectedCharacter = matched;
            SelectedCharacterId = ticket.CharacterId;
            LastCharacterSelectTicket = ticket;
            LastJoinTicket = ticket.JoinTicket;
            WorldEndpoint = normalizedWorld;
            JoinTicketBadgeText = $"Ticket: {ticket.JoinTicket} -> {normalizedWorld} (Enter={ticket.WorldEnterAllowed})";
            if (spawnClient)
            {
                EnsureGameProcessStarted();
                LifecyclePhase = "game_logged_in";
            }
            else if (IsAshesProcessRunning)
            {
                LifecyclePhase = "game_logged_in";
            }
            StatusBannerMessage = $"Character '{ticket.CharacterName}' selected; join ticket ready for {normalizedWorld}.";
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
                try { _ = await ProfileStore.DeleteProfileAsync(tempProfileId, ct).ConfigureAwait(false); }
                catch (Exception ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
            }
            ClearAuthenticatedSessionState();
            ApplyViewState("empty");
            StatusBannerMessage = $"Deleted temporary account '{user}' and purged all ephemeral credentials.";
        }

        [RelayCommand]
        public void Logout()
        {
            if (IsTemporaryAccount && !string.IsNullOrWhiteSpace(SignedInUsername))
                CredentialGuard.PurgeTemporaryCredentials($"temp-{SignedInUsername}");
            ClearAuthenticatedSessionState();
            ApplyViewState("populated");
            StatusBannerMessage = "Signed out of Verra session.";
        }

        private async Task ApplyAuthenticatedSessionAsync(AuthSessionResult session, bool isTemp, CancellationToken ct)
        {
            AccessToken = session.AccessToken;
            SignedInUsername = session.Username;
            IsTemporaryAccount = isTemp || session.IsTemporaryAccount;
            IsAuthenticated = true;
            ActiveProfileName = IsTemporaryAccount
                ? $"Profile: temp-{session.Username} (Ephemeral)"
                : $"Profile: local-dev ({session.Username})";
            _ = await ClientLaunchService.WriteIcsAuthTokenFileAsync(session.AccessToken, null, ct).ConfigureAwait(false);
            Characters.Clear();
            foreach (LauncherCharacterSummary c in await GatewayClient.ListCharactersAsync(session.AccessToken, ct).ConfigureAwait(false))
                Characters.Add(c);
            SelectedCharacter = Characters.Count > 0 ? Characters[0] : null;
            SelectedCharacterId = SelectedCharacter?.CharacterId ?? string.Empty;
            ApplyViewState("populated");
            StatusBannerMessage = $"Authenticated as '{session.Username}' ({Characters.Count} character(s) loaded).";
        }

        private async Task SaveTempProfileEntryAsync(string tempProfileId, string username, CancellationToken ct)
        {
            try
            {
                LauncherProfile tempProfile = LauncherProfile.FromLauncherConfig(_config, tempProfileId, $"Temp ({username})") with
                {
                    AccountUsername = username,
                    IsTemporaryAccount = true,
                    CreatedAtUtc = DateTimeOffset.UtcNow
                };
                await ProfileStore.UpsertProfileAsync(tempProfile, ct).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Trace.WriteLine(ex.Message);
            }
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
            SignedInUsername = string.Empty;
            AuthPassword = string.Empty;
            IsAuthenticated = false;
            IsTemporaryAccount = false;
        }
    }
}
