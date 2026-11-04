// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Diagnostics;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Services;

public enum GatewayTransportMode { HttpsRest = 0, HttpJson = 1, GrpcWeb = 2 }

public sealed record GatewayProbeResult(
    bool IsReachable, int StatusCode, long RttMs, int OnlinePlayers, int MaxPlayers, string ProtocolUsed, string? ErrorMessage);

public sealed class MultiProtocolGatewayClient
{
    public const int MaxProbeTimeoutMs = 2500;
    private static readonly HttpClient SharedHttpClient = new(new SocketsHttpHandler
    {
        PooledConnectionLifetime = TimeSpan.FromMinutes(2),
        ConnectTimeout = TimeSpan.FromMilliseconds(MaxProbeTimeoutMs)
    }) { Timeout = Timeout.InfiniteTimeSpan };

    private readonly HttpClient _httpClient;

    public MultiProtocolGatewayClient(HttpClient? httpClient = null) => _httpClient = httpClient ?? SharedHttpClient;

    public async Task<GatewayProbeResult> ProbeRealmAsync(
        Uri baseUri, GatewayTransportMode mode = GatewayTransportMode.HttpJson, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(baseUri);
        string protocol = mode.ToString();
        string relativePath = mode == GatewayTransportMode.GrpcWeb ? "/sandal.gateway.v1.RealmService/Health" : "/healthz";
        string mediaType = mode == GatewayTransportMode.GrpcWeb ? "application/grpc-web+json" : "application/json";
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeoutCts.CancelAfter(TimeSpan.FromMilliseconds(MaxProbeTimeoutMs));
        long startStamp = Stopwatch.GetTimestamp();
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(baseUri, relativePath));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue(mediaType));
            request.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/problem+json"));
            using HttpResponseMessage response = await _httpClient.SendAsync(request, HttpCompletionOption.ResponseContentRead, timeoutCts.Token).ConfigureAwait(false);
            long rttMs = (long)Stopwatch.GetElapsedTime(startStamp).TotalMilliseconds;
            string payload = await response.Content.ReadAsStringAsync(timeoutCts.Token).ConfigureAwait(false);
            return ParseHealthPayload(payload, rttMs, protocol, (int)response.StatusCode);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            long rttMs = (long)Stopwatch.GetElapsedTime(startStamp).TotalMilliseconds;
            return new GatewayProbeResult(false, 504, rttMs, 0, 0, protocol, "RFC 9457: Gateway probe timed out (<= 2500 ms Doherty ceiling).");
        }
        catch (HttpRequestException ex)
        {
            long rttMs = (long)Stopwatch.GetElapsedTime(startStamp).TotalMilliseconds;
            return new GatewayProbeResult(false, (int?)ex.StatusCode ?? 503, rttMs, 0, 0, protocol, $"RFC 9457: Gateway transport unreachable ({ex.Message}).");
        }
    }

    public static GatewayProbeResult ParseHealthPayload(string json, long rttMs, string protocol, int statusCode = 200)
    {
        string safeProto = string.IsNullOrWhiteSpace(protocol) ? nameof(GatewayTransportMode.HttpJson) : protocol.Trim();
        long clampedRtt = Math.Max(0L, rttMs);
        if (string.IsNullOrWhiteSpace(json))
            return new GatewayProbeResult(false, statusCode == 200 ? 502 : statusCode, clampedRtt, 0, 0, safeProto, "RFC 9457: Empty gateway health payload.");
        try
        {
            using JsonDocument doc = JsonDocument.Parse(json);
            JsonElement root = doc.RootElement;
            int resolvedStatus = ReadInt(root, "status", statusCode);
            if (resolvedStatus is < 200 or >= 300)
            {
                string title = ReadString(root, "title", "Gateway Problem Details");
                string detail = ReadString(root, "detail", $"HTTP {resolvedStatus} failure");
                return new GatewayProbeResult(false, resolvedStatus, clampedRtt, 0, 0, safeProto, $"RFC 9457 {resolvedStatus} {title}: {detail}");
            }
            int online = Math.Max(0, ReadInt(root, "onlinePlayers", ReadInt(root, "online_players", 0)));
            int max = Math.Max(online, ReadInt(root, "maxPlayers", ReadInt(root, "max_players", 500)));
            return new GatewayProbeResult(true, resolvedStatus, clampedRtt, online, max, safeProto, null);
        }
        catch (JsonException)
        {
            return new GatewayProbeResult(false, statusCode == 200 ? 502 : statusCode, clampedRtt, 0, 0, safeProto, "RFC 9457: Malformed JSON health payload.");
        }
    }

    private static int ReadInt(JsonElement root, string name, int fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement p) && p.ValueKind == JsonValueKind.Number && p.TryGetInt32(out int v) ? v : fallback;

    private static string ReadString(JsonElement root, string name, string fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement p) && p.ValueKind == JsonValueKind.String && p.GetString() is { Length: > 0 } s ? s : fallback;
}
