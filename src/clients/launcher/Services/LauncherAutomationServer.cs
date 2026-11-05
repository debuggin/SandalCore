// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using SandalLauncher.Models;
using SandalLauncher.ViewModels;

namespace SandalLauncher.Services;

public sealed record LauncherAutomationStateSnapshot(
    [property: JsonPropertyName("viewState")] string ViewState,
    [property: JsonPropertyName("isAuthenticated")] bool IsAuthenticated,
    [property: JsonPropertyName("username")] string Username,
    [property: JsonPropertyName("isTemporaryAccount")] bool IsTemporaryAccount,
    [property: JsonPropertyName("realmStatus")] string RealmStatus,
    [property: JsonPropertyName("characters")] IReadOnlyList<LauncherCharacterSummary> Characters,
    [property: JsonPropertyName("selectedCharacterId")] string SelectedCharacterId,
    [property: JsonPropertyName("lastJoinTicket")] string LastJoinTicket,
    [property: JsonPropertyName("worldEndpoint")] string WorldEndpoint,
    [property: JsonPropertyName("ashesProcessRunning")] bool AshesProcessRunning,
    [property: JsonPropertyName("ashesPid")] int? AshesPid,
    [property: JsonPropertyName("statusBanner")] string StatusBanner,
    [property: JsonPropertyName("launcherPid")] int LauncherPid = 0,
    [property: JsonPropertyName("lifecyclePhase")] string LifecyclePhase = "launcher_loaded");

public sealed record AutomationHttpResponse(int StatusCode, string Body, string? ETag = null);

public sealed class LauncherAutomationController
{
    private readonly MainWindowViewModel _viewModel;
    private readonly Action? _onCloseLauncher;

    public LauncherAutomationController(MainWindowViewModel viewModel, Action? onCloseLauncher = null)
    {
        ArgumentNullException.ThrowIfNull(viewModel);
        _viewModel = viewModel;
        _onCloseLauncher = onCloseLauncher;
    }

    public MainWindowViewModel ViewModel => _viewModel;
    public bool IsCloseRequested { get; private set; }

    public void NotifyCloseLauncher() => _onCloseLauncher?.Invoke();

    public LauncherAutomationStateSnapshot GetStateSnapshot()
    {
        IReadOnlyList<AshesProcessInfo> running = AshesProcessManager.FindRunningAshesProcesses();
        int? pid = _viewModel.TrackedAshesPid ?? (running.Count > 0 ? running[0].ProcessId : null);
        bool isRunning = _viewModel.IsAshesProcessRunning || running.Count > 0;
        return new LauncherAutomationStateSnapshot(
            _viewModel.CurrentViewState,
            _viewModel.IsAuthenticated,
            _viewModel.SignedInUsername,
            _viewModel.IsTemporaryAccount,
            _viewModel.RealmStatusBadgeText,
            _viewModel.Characters.ToArray(),
            _viewModel.SelectedCharacterId,
            _viewModel.LastJoinTicket,
            _viewModel.WorldEndpoint,
            isRunning,
            pid,
            _viewModel.StatusBannerMessage,
            Environment.ProcessId,
            _viewModel.LifecyclePhase);
    }

    public static string ComputeETag(string jsonPayload)
    {
        byte[] hash = SHA256.HashData(Encoding.UTF8.GetBytes(jsonPayload ?? string.Empty));
        return $"\"{Convert.ToHexString(hash)[..16].ToLowerInvariant()}\"";
    }

    public async Task<AutomationHttpResponse> HandleRequestAsync(
        string method,
        string path,
        string? body = null,
        string? ifNoneMatch = null,
        CancellationToken ct = default)
    {
        string normMethod = (method ?? "GET").Trim().ToUpperInvariant();
        string normPath = NormalizePath(path);
        try
        {
            if (normMethod == "GET" && normPath == "/automation/state")
                return HandleGetState(ifNoneMatch);
            if (normMethod == "POST")
                return await HandlePostRouteAsync(normPath, body ?? "{}", ct).ConfigureAwait(false);
            return ErrorResponse(404, $"Route '{normMethod} {normPath}' not found.");
        }
        catch (Exception ex) when (ex is ArgumentException or InvalidOperationException or System.Net.Http.HttpRequestException)
        {
            int status = ex is System.Net.Http.HttpRequestException httpEx && httpEx.StatusCode.HasValue
                ? (int)httpEx.StatusCode.Value
                : 400;
            return ErrorResponse(status, ex.Message);
        }
    }

    private AutomationHttpResponse HandleGetState(string? ifNoneMatch)
    {
        LauncherAutomationStateSnapshot snapshot = GetStateSnapshot();
        string json = JsonSerializer.Serialize(snapshot);
        string etag = ComputeETag(json);
        if (!string.IsNullOrWhiteSpace(ifNoneMatch) &&
            (string.Equals(ifNoneMatch.Trim(), etag, StringComparison.Ordinal) || ifNoneMatch.Trim() == "*"))
        {
            return new AutomationHttpResponse(304, string.Empty, etag);
        }
        return new AutomationHttpResponse(200, json, etag);
    }

    private async Task<AutomationHttpResponse> HandlePostRouteAsync(string path, string body, CancellationToken ct)
    {
        using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
        JsonElement root = doc.RootElement;
        return path switch
        {
            "/automation/register-temp" => await HandleRegisterTempAsync(root, ct).ConfigureAwait(false),
            "/automation/login" => await HandleLoginAsync(root, ct).ConfigureAwait(false),
            "/automation/characters/create" => await HandleCreateCharacterAsync(root, ct).ConfigureAwait(false),
            "/automation/char-select" => await HandleCharSelectAsync(root, ct).ConfigureAwait(false),
            "/automation/launch" => HandleLaunch(),
            "/automation/terminate-ashes" => await HandleTerminateAshesAsync(root, ct).ConfigureAwait(false),
            "/automation/close-launcher" => await HandleCloseLauncherAsync(root, ct).ConfigureAwait(false),
            "/automation/delete-account" => await HandleDeleteAccountAsync(ct).ConfigureAwait(false),
            _ => ErrorResponse(404, $"POST route '{path}' not found.")
        };
    }

    private AutomationHttpResponse HandleLaunch()
    {
        _viewModel.LaunchClient();
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleCloseLauncherAsync(JsonElement root, CancellationToken ct)
    {
        if (ReadBool(root, "terminateAshes", true) && _viewModel.TrackedAshesPid is > 0)
            await _viewModel.TerminateAshesWithPolicyAsync(killAllMatching: false, ct).ConfigureAwait(false);
        _viewModel.CloseLauncher();
        IsCloseRequested = true;
        AutomationHttpResponse resp = HandleGetState(null);
        _ = Task.Run(async () =>
        {
            await Task.Delay(25, CancellationToken.None).ConfigureAwait(false);
            NotifyCloseLauncher();
        });
        return resp;
    }

    private async Task<AutomationHttpResponse> HandleRegisterTempAsync(JsonElement root, CancellationToken ct)
    {
        _viewModel.AuthUsername = ReadStr(root, "username", string.Empty);
        _viewModel.AuthPassword = ReadStr(root, "password", string.Empty);
        await _viewModel.RegisterTempAsync(ct).ConfigureAwait(false);
        if (ReadBool(root, "createDefaultChar", false) && _viewModel.Characters.Count == 0)
        {
            string rawName = ReadStr(root, "characterName", ReadStr(root, "name", string.Empty));
            _viewModel.NewCharacterName = MainWindowViewModel.SanitizeAutoCharacterName(rawName);
            await _viewModel.CreateCharacterAsync(ct).ConfigureAwait(false);
        }
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleLoginAsync(JsonElement root, CancellationToken ct)
    {
        _viewModel.AuthUsername = ReadStr(root, "username", _viewModel.AuthUsername);
        _viewModel.AuthPassword = ReadStr(root, "password", _viewModel.AuthPassword);
        _viewModel.IsTemporaryAccount = ReadBool(root, "isTemporaryAccount", false);
        await _viewModel.LoginAsync(ct).ConfigureAwait(false);
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleCreateCharacterAsync(JsonElement root, CancellationToken ct)
    {
        _viewModel.NewCharacterName = ReadStr(root, "name", LauncherCliOptions.DefaultHeroName);
        int classId = ReadInt(root, "classId", 1);
        int raceId = ReadInt(root, "raceId", 1);
        await _viewModel.CreateCharacterWithIdsAsync(_viewModel.NewCharacterName, classId, raceId, ct).ConfigureAwait(false);
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleCharSelectAsync(JsonElement root, CancellationToken ct)
    {
        string target = ReadStr(root, "characterIdOrName", ReadStr(root, "characterName", ReadStr(root, "characterId", ReadStr(root, "name", _viewModel.SelectedCharacterId))));
        bool spawn = ReadBool(root, "spawnClient", false);
        await _viewModel.SelectCharacterByNameOrIdAsync(target, spawn, ct).ConfigureAwait(false);
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleTerminateAshesAsync(JsonElement root, CancellationToken ct)
    {
        bool killAll = ReadBool(root, "killAllMatching", true);
        await _viewModel.TerminateAshesWithPolicyAsync(killAll, ct).ConfigureAwait(false);
        return HandleGetState(null);
    }

    private async Task<AutomationHttpResponse> HandleDeleteAccountAsync(CancellationToken ct)
    {
        await _viewModel.DeleteAccountAsync(ct).ConfigureAwait(false);
        return HandleGetState(null);
    }

    private static AutomationHttpResponse ErrorResponse(int status, string detail)
    {
        string json = JsonSerializer.Serialize(new { status, error = detail });
        return new AutomationHttpResponse(status, json, null);
    }

    private static string NormalizePath(string? raw)
    {
        if (string.IsNullOrWhiteSpace(raw)) return "/";
        string path = raw.Trim();
        int q = path.IndexOf('?', StringComparison.Ordinal);
        if (q >= 0) path = path[..q];
        return path.Length > 1 ? path.TrimEnd('/') : path;
    }

    private static string ReadStr(JsonElement root, string prop, string fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(prop, out JsonElement p) && p.ValueKind == JsonValueKind.String && p.GetString() is { Length: > 0 } s ? s : fallback;

    private static int ReadInt(JsonElement root, string prop, int fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(prop, out JsonElement p) && p.ValueKind == JsonValueKind.Number && p.TryGetInt32(out int v) ? v : fallback;

    private static bool ReadBool(JsonElement root, string prop, bool fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(prop, out JsonElement p) && p.ValueKind is JsonValueKind.True or JsonValueKind.False ? p.GetBoolean() : fallback;
}

public sealed class LauncherAutomationServer : IAsyncDisposable, IDisposable
{
    private readonly LauncherAutomationController _controller;
    private readonly TcpListener _listener;
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _acceptLoopTask;

    public LauncherAutomationServer(LauncherAutomationController controller, int port = 0)
    {
        ArgumentNullException.ThrowIfNull(controller);
        _controller = controller;
        _listener = new TcpListener(IPAddress.Loopback, port);
        _listener.Start();
        Port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUri = new Uri($"http://127.0.0.1:{Port}");
        _acceptLoopTask = Task.Run(() => AcceptLoopAsync(_cts.Token));
    }

    public LauncherAutomationServer(MainWindowViewModel viewModel, int port = 0, Action? onCloseLauncher = null)
        : this(new LauncherAutomationController(viewModel, onCloseLauncher), port)
    {
    }

    public int Port { get; }
    public Uri BaseUri { get; }
    public LauncherAutomationController Controller => _controller;

    private async Task AcceptLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                TcpClient client = await _listener.AcceptTcpClientAsync(ct).ConfigureAwait(false);
                _ = HandleClientAsync(client, ct);
            }
            catch (OperationCanceledException) { break; }
            catch (ObjectDisposedException) { break; }
            catch (SocketException) { if (ct.IsCancellationRequested) break; }
        }
    }

    private async Task HandleClientAsync(TcpClient client, CancellationToken ct)
    {
        using (client)
        {
            try
            {
                await using NetworkStream stream = client.GetStream();
                using var reader = new StreamReader(stream, Encoding.UTF8, false, 4096, leaveOpen: true);
                string? requestLine = await reader.ReadLineAsync(ct).ConfigureAwait(false);
                if (string.IsNullOrWhiteSpace(requestLine)) return;
                string[] parts = requestLine.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                string method = parts.Length > 0 ? parts[0] : "GET";
                string path = parts.Length > 1 ? parts[1] : "/";
                (int contentLen, string? ifNoneMatch) = await ReadHeadersAsync(reader, ct).ConfigureAwait(false);
                string body = contentLen > 0 ? await ReadBodyAsync(reader, contentLen, ct).ConfigureAwait(false) : string.Empty;
                AutomationHttpResponse resp = await _controller.HandleRequestAsync(method, path, body, ifNoneMatch, ct).ConfigureAwait(false);
                await WriteHttpResponseAsync(stream, resp, ct).ConfigureAwait(false);
                if (_controller.IsCloseRequested)
                    _controller.NotifyCloseLauncher();
            }
            catch (Exception ex) when (ex is IOException or OperationCanceledException or ObjectDisposedException)
            {
                System.Diagnostics.Trace.WriteLine(ex.Message);
            }
        }
    }

    private static async Task<(int ContentLength, string? IfNoneMatch)> ReadHeadersAsync(StreamReader reader, CancellationToken ct)
    {
        int contentLength = 0;
        string? ifNoneMatch = null;
        for (int i = 0; i < 64; i++)
        {
            string? line = await reader.ReadLineAsync(ct).ConfigureAwait(false);
            if (string.IsNullOrEmpty(line)) break;
            int colon = line.IndexOf(':', StringComparison.Ordinal);
            if (colon <= 0) continue;
            string name = line[..colon].Trim();
            string val = line[(colon + 1)..].Trim();
            if (name.Equals("Content-Length", StringComparison.OrdinalIgnoreCase) && int.TryParse(val, out int len))
                contentLength = Math.Clamp(len, 0, 65536);
            else if (name.Equals("If-None-Match", StringComparison.OrdinalIgnoreCase))
                ifNoneMatch = val;
        }
        return (contentLength, ifNoneMatch);
    }

    private static async Task<string> ReadBodyAsync(StreamReader reader, int length, CancellationToken ct)
    {
        char[] buf = new char[length];
        int total = 0;
        while (total < length)
        {
            int n = await reader.ReadAsync(buf.AsMemory(total, length - total), ct).ConfigureAwait(false);
            if (n <= 0) break;
            total += n;
        }
        return new string(buf, 0, total);
    }

    private static async Task WriteHttpResponseAsync(NetworkStream stream, AutomationHttpResponse resp, CancellationToken ct)
    {
        byte[] bodyBytes = resp.StatusCode == 304 ? [] : Encoding.UTF8.GetBytes(resp.Body ?? string.Empty);
        string reason = resp.StatusCode switch { 200 => "OK", 304 => "Not Modified", 400 => "Bad Request", 401 => "Unauthorized", 404 => "Not Found", _ => "Error" };
        var sb = new StringBuilder(256);
        sb.Append($"HTTP/1.1 {resp.StatusCode} {reason}\r\n");
        sb.Append("Content-Type: application/json; charset=utf-8\r\n");
        if (!string.IsNullOrWhiteSpace(resp.ETag))
            sb.Append($"ETag: {resp.ETag}\r\n");
        sb.Append($"Content-Length: {bodyBytes.Length}\r\n");
        sb.Append("Connection: close\r\n\r\n");
        byte[] headerBytes = Encoding.ASCII.GetBytes(sb.ToString());
        await stream.WriteAsync(headerBytes, ct).ConfigureAwait(false);
        if (bodyBytes.Length > 0)
            await stream.WriteAsync(bodyBytes, ct).ConfigureAwait(false);
        await stream.FlushAsync(ct).ConfigureAwait(false);
    }

    public void Dispose()
    {
        _cts.Cancel();
        _listener.Stop();
        _cts.Dispose();
    }

    public async ValueTask DisposeAsync()
    {
        _cts.Cancel();
        _listener.Stop();
        try { await _acceptLoopTask.ConfigureAwait(false); }
        catch (OperationCanceledException ex) { System.Diagnostics.Trace.WriteLine(ex.Message); }
        _cts.Dispose();
    }
}
