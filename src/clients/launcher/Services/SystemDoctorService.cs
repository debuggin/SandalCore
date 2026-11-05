// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;

namespace SandalLauncher.Services;

public enum DoctorCheckSeverity { Pass = 0, Warning = 1, Fail = 2 }
public sealed record DoctorCheckItem(string CheckId, string Title, DoctorCheckSeverity Severity, string StatusBadge, string Summary, string Remediation);

public sealed record DoctorProbeInput
{
    public bool WorldUdp7777Ready { get; init; } = true;
    public bool GatewayHttp8080Ready { get; init; } = true;
    public bool PostgresTcp5432Ready { get; init; } = true;
    public bool NatsTcp4222Ready { get; init; } = true;
    public bool TetherTcp29000Ready { get; init; } = true;
    public bool WfpLoopbackHealthy { get; init; } = true;
    public int WfpLoopbackDropCount { get; init; }
    public bool ClientBinaryPresent { get; init; } = true;
    public bool ClientBinarySha256Matched { get; init; } = true;
    public string ClientBinaryActualHex { get; init; } = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    public bool NamedPipeActive { get; init; } = true;
    public bool NamedPipeCurrentUserOnlyDacl { get; init; } = true;
    public DateTimeOffset? TimestampUtc { get; init; }
}

public sealed record DoctorReport(DoctorCheckSeverity OverallSeverity, string OverallBadge, IReadOnlyList<DoctorCheckItem> Checks, DateTimeOffset GeneratedAtUtc);

public static class SystemDoctorService
{
    public const string PassBadge = "● PASS", WarnBadge = "▲ WARN", FailBadge = "■ FAIL";
    public static string GetStatusBadge(DoctorCheckSeverity severity) =>
        severity == DoctorCheckSeverity.Pass ? PassBadge : (severity == DoctorCheckSeverity.Warning ? WarnBadge : FailBadge);

    public static DoctorCheckItem EvaluatePortReadiness(int port, string protocol, string serviceName, bool isReady, bool required = true)
    {
        string proto = string.IsNullOrWhiteSpace(protocol) ? "TCP" : protocol.Trim().ToUpperInvariant();
        string svc = string.IsNullOrWhiteSpace(serviceName) ? "service" : serviceName.Trim();
        DoctorCheckSeverity sev = isReady ? DoctorCheckSeverity.Pass : (required ? DoctorCheckSeverity.Fail : DoctorCheckSeverity.Warning);
        string summary = isReady ? $"{svc} endpoint listening on 127.0.0.1:{port} ({proto})." : $"{svc} endpoint unreachable on 127.0.0.1:{port} ({proto}).";
        string fix = isReady ? "No action required." : $"Start the {svc} process or release conflicting {proto} port {port} bindings.";
        return new DoctorCheckItem($"port.{proto.ToLowerInvariant()}.{port}", $"{svc} ({proto} {port})", sev, GetStatusBadge(sev), summary, fix);
    }

    public static DoctorCheckItem EvaluateWfpLoopbackHealth(bool loopbackReachable, int wfpDropCount = 0)
    {
        DoctorCheckSeverity sev = !loopbackReachable ? DoctorCheckSeverity.Fail : (wfpDropCount > 0 ? DoctorCheckSeverity.Warning : DoctorCheckSeverity.Pass);
        string summary = !loopbackReachable ? "Windows Filtering Platform (WFP) or WinNAT blocked 127.0.0.1 loopback traffic."
            : (wfpDropCount > 0 ? $"Loopback reachable with {wfpDropCount} transient WFP packet drops." : "WFP loopback path healthy (0 drops).");
        string fix = sev == DoctorCheckSeverity.Pass ? "No action required." : "Inspect WFP filters via 'netsh wfp show state' and exempt 127.0.0.1.";
        return new DoctorCheckItem("network.wfp.loopback", "WFP & Loopback Health", sev, GetStatusBadge(sev), summary, fix);
    }

    public static DoctorCheckItem EvaluateBinarySha256(bool binaryPresent, bool sha256Matched, string? actualHex = null)
    {
        DoctorCheckSeverity sev = !binaryPresent ? DoctorCheckSeverity.Fail : (!sha256Matched ? DoctorCheckSeverity.Warning : DoctorCheckSeverity.Pass);
        string prefix = !string.IsNullOrWhiteSpace(actualHex) && actualHex.Trim().Length >= 12 ? actualHex.Trim()[..12] : "unknown";
        string summary = !binaryPresent ? "AOCClient-Win64-Shipping.exe was not found in the configured ClientPakDir."
            : (sha256Matched ? $"AOCClient-Win64-Shipping.exe SHA-256 verified ({prefix}…)." : $"AOCClient-Win64-Shipping.exe SHA-256 mismatch ({prefix}…).");
        string fix = sev == DoctorCheckSeverity.Pass ? "No action required." : "Verify ClientPakDir points to the pinned Ashes of Creation build.";
        return new DoctorCheckItem("client.binary.sha256", "AOCClient-Win64-Shipping.exe SHA-256", sev, GetStatusBadge(sev), summary, fix);
    }

    public static DoctorCheckItem EvaluateNamedPipeDacl(bool pipeActive, bool currentUserOnlyDacl)
    {
        DoctorCheckSeverity sev = !currentUserOnlyDacl ? DoctorCheckSeverity.Fail : (!pipeActive ? DoctorCheckSeverity.Warning : DoctorCheckSeverity.Pass);
        string summary = !currentUserOnlyDacl ? "Named pipe DACL permits non-owner access; token handoff blocked."
            : (pipeActive ? "Ephemeral SandalAuth_ named pipe enforces CurrentUser-only DACL." : "Named pipe idle; CurrentUser-only DACL policy armed.");
        string fix = sev == DoctorCheckSeverity.Pass ? "No action required." : "Ensure TokenPipeServer restricts pipes to the current Windows user SID.";
        return new DoctorCheckItem("security.pipe.dacl", "Named-Pipe CurrentUser DACL", sev, GetStatusBadge(sev), summary, fix);
    }

    public static DoctorReport RunDiagnostics(DoctorProbeInput? input = null)
    {
        DoctorProbeInput p = input ?? new DoctorProbeInput();
        DoctorCheckItem[] checks =
        [
            EvaluatePortReadiness(7777, "UDP", "world", p.WorldUdp7777Ready),
            EvaluatePortReadiness(8080, "HTTP", "gateway", p.GatewayHttp8080Ready),
            EvaluatePortReadiness(5432, "TCP", "PostgreSQL", p.PostgresTcp5432Ready),
            EvaluatePortReadiness(4222, "TCP", "NATS", p.NatsTcp4222Ready),
            EvaluatePortReadiness(29000, "TCP", "Local Tether", p.TetherTcp29000Ready, required: false),
            EvaluateWfpLoopbackHealth(p.WfpLoopbackHealthy, p.WfpLoopbackDropCount),
            EvaluateBinarySha256(p.ClientBinaryPresent, p.ClientBinarySha256Matched, p.ClientBinaryActualHex),
            EvaluateNamedPipeDacl(p.NamedPipeActive, p.NamedPipeCurrentUserOnlyDacl)
        ];
        var overall = DoctorCheckSeverity.Pass;
        foreach (DoctorCheckItem item in checks) if (item.Severity > overall) overall = item.Severity;
        return new DoctorReport(overall, GetStatusBadge(overall), checks, p.TimestampUtc ?? DateTimeOffset.UtcNow);
    }
}
