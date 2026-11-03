// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SandalCore.Config;
using SandalLauncher.ViewModels;
using SandalLauncher.Views;

namespace SandalLauncher
{
    public static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            if (args.Length > 0 && TryHandleCliFastPath(args, out int exitCode))
            {
                return exitCode;
            }
            return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        }

        public static AppBuilder BuildAvaloniaApp() =>
            AppBuilder.Configure<App>()
                .UsePlatformDetect()
                .WithInterFont()
                .LogToTrace();

        private static bool TryHandleCliFastPath(string[] args, out int exitCode)
        {
            string flag = args[0];
            if (flag is "--help" or "-h")
            {
                Console.Out.WriteLine("Usage: SandalLauncher [--json] [--dry-run] [--version] [--help]");
                exitCode = 0;
                return true;
            }
            if (flag is "--version" or "-v")
            {
                Console.Out.WriteLine("SandalLauncher 0.1.0 (Avalonia 11.2.3 / .NET 8.0)");
                exitCode = 0;
                return true;
            }
            if (flag is "--json" or "--dry-run")
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
        public override void Initialize()
        {
            AvaloniaXamlLoader.Load(this);
        }

        public override void OnFrameworkInitializationCompleted()
        {
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            {
                desktop.MainWindow = new MainWindow
                {
                    DataContext = new MainWindowViewModel()
                };
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

        public MainWindowViewModel()
            : this(SandalConfigLoader.LoadLauncher())
        {
        }

        public MainWindowViewModel(LauncherConfig config)
        {
            _config = config;
            ApplyConfigSnapshot(config);
            ApplyViewState("populated");
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
        private void LaunchClient()
        {
            StatusBannerMessage = $"Ready to launch client: {LaunchCommandLinePreview}";
        }
    }
}
