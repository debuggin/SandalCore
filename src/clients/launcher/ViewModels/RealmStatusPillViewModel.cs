// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Globalization;
using CommunityToolkit.Mvvm.ComponentModel;
using SandalLauncher.Services;

namespace SandalLauncher.ViewModels;

public enum RealmPillState { Loading = 0, Empty = 1, Error = 2, Degraded = 3, Populated = 4 }

public sealed partial class RealmStatusPillViewModel : ObservableObject
{
    public const string DarkSurfaceHex = "#0D1117";
    public const string OfflineForegroundHex = "#8B949E";
    public const string CheckingForegroundHex = "#D29922";
    public const string OnlineForegroundHex = "#3FB950";
    public const string AuthErrorForegroundHex = "#F85149";
    public const string VersionMismatchForegroundHex = "#DB6D28";

    [ObservableProperty] private RealmPillState _viewState = RealmPillState.Empty;
    [ObservableProperty] private string _semanticStatus = "Offline";
    [ObservableProperty] private string _statusIcon = "○";
    [ObservableProperty] private string _statusLabel = "○ OFFLINE";
    [ObservableProperty] private string _latencyLabel = "— ms";
    [ObservableProperty] private string _populationLabel = "0 / 0";
    [ObservableProperty] private string _badgeForegroundHex = OfflineForegroundHex;
    [ObservableProperty] private string _badgeBackgroundHex = DarkSurfaceHex;
    [ObservableProperty] private string _diagnosticSummary = "Gateway probe idle. Click Probe or Start Stack to check realm health.";

    private readonly record struct PillVisualSpec(
        RealmPillState State, string Semantic, string Icon, string Label, string ForegroundHex);

    private static readonly PillVisualSpec CheckingSpec = new(RealmPillState.Loading, "Checking", "◌", "◌ CHECKING", CheckingForegroundHex);
    private static readonly PillVisualSpec AuthErrorSpec = new(RealmPillState.Error, "AuthError", "■", "■ AUTH ERROR", AuthErrorForegroundHex);
    private static readonly PillVisualSpec OfflineSpec = new(RealmPillState.Empty, "Offline", "○", "○ OFFLINE", OfflineForegroundHex);
    private static readonly PillVisualSpec MismatchSpec = new(RealmPillState.Degraded, "VersionMismatch", "▲", "▲ VERSION MISMATCH", VersionMismatchForegroundHex);
    private static readonly PillVisualSpec OnlineSpec = new(RealmPillState.Populated, "Online", "●", "● ONLINE", OnlineForegroundHex);

    public void SetCheckingState() =>
        ApplyState(CheckingSpec, "— ms", "— / —", "Probing multi-protocol gateway endpoint (<= 2500 ms timeout)...");

    public void ApplyProbeResult(GatewayProbeResult probe, bool hashMatched = true, bool authFailed = false)
    {
        ArgumentNullException.ThrowIfNull(probe);
        if (authFailed || probe.StatusCode is 401 or 403)
        {
            ApplyState(AuthErrorSpec, FormatLatency(probe.RttMs), "— / —",
                probe.ErrorMessage ?? "Authentication handshake rejected by gateway (401/403).");
            return;
        }
        if (!probe.IsReachable)
        {
            ApplyState(OfflineSpec, "— ms", "0 / 0", probe.ErrorMessage ?? "Realm gateway is unreachable.");
            return;
        }
        string rttText = FormatLatency(probe.RttMs);
        string popText = string.Create(CultureInfo.InvariantCulture, $"{Math.Max(0, probe.OnlinePlayers)} / {Math.Max(0, probe.MaxPlayers)}");
        if (!hashMatched)
        {
            ApplyState(MismatchSpec, rttText, popText,
                "Gateway reachable, but AOCClient-Win64-Shipping.exe SHA-256 does not match pinned manifest.");
            return;
        }
        ApplyState(OnlineSpec, rttText, popText, $"Connected via {probe.ProtocolUsed} ({rttText}, {popText} players).");
    }

    private static string FormatLatency(long rttMs) =>
        rttMs >= 0 ? string.Create(CultureInfo.InvariantCulture, $"{rttMs} ms") : "— ms";

    private void ApplyState(PillVisualSpec spec, string latency, string population, string summary)
    {
        ViewState = spec.State; SemanticStatus = spec.Semantic; StatusIcon = spec.Icon; StatusLabel = spec.Label;
        BadgeForegroundHex = spec.ForegroundHex; LatencyLabel = latency; PopulationLabel = population; DiagnosticSummary = summary;
    }
}
