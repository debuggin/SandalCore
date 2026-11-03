// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SandalLauncher.Services;

namespace SandalLauncher.ViewModels
{
    public enum LaunchPresetKind { RetailParity = 0, LocalSandbox = 1, GmArchitectDebug = 2, Custom = 3 }
    public enum ServerStackState { Stopped = 0, Starting = 1, Running = 2, Stopping = 3, Faulted = 4 }

    public sealed partial class LaunchPresetsViewModel : ObservableObject
    {
        [ObservableProperty] private LaunchPresetKind _selectedPreset = LaunchPresetKind.LocalSandbox;
        [ObservableProperty] private ServerStackState _stackState = ServerStackState.Stopped;
        [ObservableProperty] private bool _useEos;
        [ObservableProperty] private bool _enableDebugLog;
        [ObservableProperty] private bool _windowedMode = true;
        [ObservableProperty] private int _resolutionX = 1920;
        [ObservableProperty] private int _resolutionY = 1080;
        [ObservableProperty] private string _stackStatusText = "○ STOPPED — Local Verra server stack halted.";
        [ObservableProperty] private string _binaryHashBadgeText = "● SHA-256 VERIFIED — AOCClient-Win64-Shipping.exe";
        [ObservableProperty] private bool _isBinaryHashVerified = true;
        [ObservableProperty] private string _effectiveArgumentsPreview = string.Empty;

        public LaunchPresetsViewModel()
        {
            RealmPill = new RealmStatusPillViewModel();
            AvailablePresets = Enum.GetValues<LaunchPresetKind>();
            ApplyPresetDefaults(LaunchPresetKind.LocalSandbox);
        }

        public RealmStatusPillViewModel RealmPill { get; }
        public IReadOnlyList<LaunchPresetKind> AvailablePresets { get; }
        public bool CanStartStack => StackState is ServerStackState.Stopped or ServerStackState.Faulted;
        public bool CanStopStack => StackState is ServerStackState.Starting or ServerStackState.Running;
        public bool CanRestartStack => StackState is ServerStackState.Running or ServerStackState.Faulted;

        partial void OnSelectedPresetChanged(LaunchPresetKind value) => ApplyPresetDefaults(value);
        partial void OnUseEosChanged(bool value) => _ = BuildEffectiveLaunchArguments(27900, @"\\.\pipe\SandalAuth_Preview");
        partial void OnEnableDebugLogChanged(bool value) => _ = BuildEffectiveLaunchArguments(27900, @"\\.\pipe\SandalAuth_Preview");
        partial void OnWindowedModeChanged(bool value) => _ = BuildEffectiveLaunchArguments(27900, @"\\.\pipe\SandalAuth_Preview");

        public string[] BuildEffectiveLaunchArguments(int tetherPort, string pipePath)
        {
            if (tetherPort is <= 0 or > 65535) throw new ArgumentOutOfRangeException(nameof(tetherPort));
            if (string.IsNullOrWhiteSpace(pipePath)) throw new ArgumentException("Pipe path cannot be empty.", nameof(pipePath));
            string trimmed = pipePath.Trim();
            string fullPipe = trimmed.StartsWith(@"\\.\pipe\", StringComparison.OrdinalIgnoreCase) ? trimmed : $@"\\.\pipe\{trimmed}";
            var args = new List<string>(8) { $"-LauncherTetherPort={tetherPort}", $"-AuthTokenPipe={fullPipe}", $"-UseEOS={(UseEos ? "true" : "false")}", "-NoEAC" };
            if (EnableDebugLog) args.Add("-Log");
            if (WindowedMode) { args.Add("-Windowed"); args.Add($"-ResX={Math.Max(640, ResolutionX)}"); args.Add($"-ResY={Math.Max(480, ResolutionY)}"); }
            EffectiveArgumentsPreview = string.Join(' ', args);
            return args.ToArray();
        }

        public void TransitionStackState(string action)
        {
            if (string.IsNullOrWhiteSpace(action)) throw new ArgumentException("Stack action cannot be empty.", nameof(action));
            (StackState, StackStatusText) = action.Trim().ToLowerInvariant() switch
            {
                "start" => (ServerStackState.Starting, "◌ STARTING — Launching local Gateway + World daemon..."),
                "ready" => (ServerStackState.Running, "● RUNNING — Local Verra stack online (127.0.0.1:8080)."),
                "stop" => (ServerStackState.Stopped, "○ STOPPED — Local Verra server stack halted."),
                "restart" => (ServerStackState.Starting, "◌ RESTARTING — Cycling local Gateway + World stack..."),
                "fault" => (ServerStackState.Faulted, "■ FAULTED — Local stack exited unexpectedly."),
                _ => throw new ArgumentException($"Unsupported stack transition action '{action}'.", nameof(action))
            };
            OnPropertyChanged(nameof(CanStartStack)); OnPropertyChanged(nameof(CanStopStack)); OnPropertyChanged(nameof(CanRestartStack));
        }

        public void ApplyBinaryHashResult(BinaryHashResult result)
        {
            ArgumentNullException.ThrowIfNull(result); IsBinaryHashVerified = result.IsMatch;
            BinaryHashBadgeText = result.IsMatch ? $"● SHA-256 VERIFIED ({result.ActualHex[..12]}…)" : $"▲ SHA-256 MISMATCH (expected {result.ExpectedHex[..12]}…)";
        }

        [RelayCommand] private void StartStack() => TransitionStackState("start");
        [RelayCommand] private void StopStack() => TransitionStackState("stop");
        [RelayCommand] private void RestartStack() => TransitionStackState("restart");

        private void ApplyPresetDefaults(LaunchPresetKind preset)
        {
            if (preset == LaunchPresetKind.RetailParity) { UseEos = true; EnableDebugLog = false; WindowedMode = false; }
            else if (preset == LaunchPresetKind.LocalSandbox) { UseEos = false; EnableDebugLog = false; WindowedMode = true; }
            else if (preset == LaunchPresetKind.GmArchitectDebug) { UseEos = false; EnableDebugLog = true; WindowedMode = true; }
            _ = BuildEffectiveLaunchArguments(27900, @"\\.\pipe\SandalAuth_Preview");
        }
    }
}
namespace SandalLauncher.Views { public partial class ServerControlView : UserControl { public ServerControlView() { AvaloniaXamlLoader.Load(this); DataContext ??= new ViewModels.LaunchPresetsViewModel(); } } }
