// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using SandalLauncher.Models;

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
    })
    {
        Timeout = Timeout.InfiniteTimeSpan,
        DefaultRequestVersion = HttpVersion.Version20,
        DefaultVersionPolicy = HttpVersionPolicy.RequestVersionOrLower
    };

    private readonly HttpClient _httpClient;
    private readonly ConcurrentDictionary<string, (string ETag, string Body)> _etagCache = new(StringComparer.Ordinal);

    public Uri BaseUri { get; set; }

    public IReadOnlyDictionary<string, (string ETag, string Body)> ETagCache => _etagCache;

    public MultiProtocolGatewayClient(HttpClient? httpClient = null, Uri? baseUri = null)
    {
        _httpClient = httpClient ?? SharedHttpClient;
        BaseUri = baseUri ?? _httpClient.BaseAddress ?? new Uri("http://127.0.0.1:8080");
    }

    public MultiProtocolGatewayClient(string baseUrl, HttpClient? httpClient = null)
        : this(httpClient, new Uri(LauncherCliOptions.NormalizeGatewayUrl(baseUrl)))
    {
    }

    public bool TryGetCachedETag(string path, out string etag)
    {
        if (_etagCache.TryGetValue(path, out (string ETag, string Body) entry) && !string.IsNullOrWhiteSpace(entry.ETag))
        {
            etag = entry.ETag;
            return true;
        }
        etag = string.Empty;
        return false;
    }

    public void SetCachedETag(string path, string etag, string body = "") =>
        _etagCache[path] = (etag, body);

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

    public async Task<AuthSessionResult> RegisterAsync(
        string username, string password, bool isTemporaryAccount = false, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(username)) throw new ArgumentException("Username is required.", nameof(username));
        if (string.IsNullOrEmpty(password)) throw new ArgumentException("Password is required.", nameof(password));
        string trimmedUser = username.Trim();
        string email = trimmedUser.Contains('@', StringComparison.Ordinal) ? trimmedUser : $"{trimmedUser}@sandalcore.local";
        string json = JsonSerializer.Serialize(new { username = trimmedUser, email, password, temporary = isTemporaryAccount });
        using var req = CreateRequest(HttpMethod.Post, "/api/v1/auth/register", null, null);
        string resp = await SendJsonAsync(req, "/api/v1/auth/register", json, ct).ConfigureAwait(false);
        return ParseAuthSession(resp, trimmedUser, isTemporaryAccount);
    }

    public async Task<AuthSessionResult> LoginAsync(
        string username, string password, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(username)) throw new ArgumentException("Username is required.", nameof(username));
        if (string.IsNullOrEmpty(password)) throw new ArgumentException("Password is required.", nameof(password));
        string trimmedUser = username.Trim();
        string json = JsonSerializer.Serialize(new { username = trimmedUser, password });
        using var req = CreateRequest(HttpMethod.Post, "/api/v1/auth/login", null, null);
        string resp = await SendJsonAsync(req, "/api/v1/auth/login", json, ct).ConfigureAwait(false);
        return ParseAuthSession(resp, trimmedUser, false);
    }

    public async Task<bool> DeleteAccountAsync(
        string accessToken, string? username = null, string? password = null, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(accessToken)) throw new ArgumentException("Access token is required.", nameof(accessToken));
        string? body = !string.IsNullOrWhiteSpace(username) || !string.IsNullOrEmpty(password)
            ? JsonSerializer.Serialize(new { username = username ?? string.Empty, password = password ?? string.Empty })
            : null;
        string? ifMatch = TryGetCachedETag("/api/v1/auth/account", out string etag) ? etag : null;
        using var req = CreateRequest(HttpMethod.Delete, "/api/v1/auth/account", accessToken, ifMatch);
        _ = await SendJsonAsync(req, "/api/v1/auth/account", body, ct).ConfigureAwait(false);
        return true;
    }

    public async Task<IReadOnlyList<LauncherCharacterSummary>> ListCharactersAsync(
        string accessToken, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(accessToken)) throw new ArgumentException("Access token is required.", nameof(accessToken));
        const string path = "/api/v1/characters";
        using var req = CreateRequest(HttpMethod.Get, path, accessToken, null);
        if (_etagCache.TryGetValue(path, out (string ETag, string Body) cached) && !string.IsNullOrWhiteSpace(cached.ETag))
            req.Headers.TryAddWithoutValidation("If-None-Match", cached.ETag);

        using HttpResponseMessage resp = await _httpClient.SendAsync(req, HttpCompletionOption.ResponseContentRead, ct).ConfigureAwait(false);
        if (resp.StatusCode == HttpStatusCode.NotModified && _etagCache.TryGetValue(path, out cached))
            return ParseCharacterList(cached.Body);
        EnsureSuccessOrThrow(resp, await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false), out string payload);
        if (ExtractETag(resp) is { Length: > 0 } etag)
            _etagCache[path] = (etag, payload);
        return ParseCharacterList(payload);
    }

    public async Task<LauncherCharacterSummary> CreateCharacterAsync(
        string accessToken, string name, int classId = 1, int raceId = 1, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(accessToken)) throw new ArgumentException("Access token is required.", nameof(accessToken));
        if (string.IsNullOrWhiteSpace(name)) throw new ArgumentException("Character name is required.", nameof(name));
        string trimmedName = name.Trim();
        string json = JsonSerializer.Serialize(new { name = trimmedName, class_id = classId, race_id = raceId });
        string? ifMatch = TryGetCachedETag("/api/v1/characters", out string etag) ? etag : null;
        using var req = CreateRequest(HttpMethod.Post, "/api/v1/characters", accessToken, ifMatch);
        string resp = await SendJsonAsync(req, "/api/v1/characters", json, ct).ConfigureAwait(false);
        using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(resp) ? "{}" : resp);
        JsonElement root = doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("character", out JsonElement c) ? c : doc.RootElement;
        return ParseSingleCharacter(root, trimmedName);
    }

    public Task<CharacterSelectTicket> SelectCharacterAsync(
        string accessToken, string characterIdOrName, bool verifyWorldEnter = true, CancellationToken ct = default) =>
        SelectCharacterAsync(accessToken, characterIdOrName, verifyWorldEnter, null, ct);

    public async Task<CharacterSelectTicket> SelectCharacterAsync(
        string accessToken, string characterIdOrName, bool verifyWorldEnter, string? ifMatchETag, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(accessToken)) throw new ArgumentException("Access token is required.", nameof(accessToken));
        if (string.IsNullOrWhiteSpace(characterIdOrName)) throw new ArgumentException("Character identifier is required.", nameof(characterIdOrName));
        string target = characterIdOrName.Trim();
        string selectPath = $"/api/v1/characters/{Uri.EscapeDataString(target)}/select";
        string? effectiveIfMatch = ifMatchETag ?? (TryGetCachedETag(selectPath, out string cachedETag) ? cachedETag : null);
        string selectReq = JsonSerializer.Serialize(new { character_id = target, name = target });
        using var req = CreateRequest(HttpMethod.Post, selectPath, accessToken, effectiveIfMatch);
        string selectResp = await SendJsonAsync(req, selectPath, selectReq, ct).ConfigureAwait(false);
        CharacterSelectTicket ticket = ParseSelectTicket(selectResp, target, accessToken);
        if (!verifyWorldEnter) return ticket;
        return await VerifyWorldEnterAsync(accessToken, ticket, ct).ConfigureAwait(false);
    }

    private async Task<CharacterSelectTicket> VerifyWorldEnterAsync(
        string accessToken, CharacterSelectTicket ticket, CancellationToken ct)
    {
        string enterBody = JsonSerializer.Serialize(new { ticket = ticket.JoinTicket, token = accessToken, character_id = ticket.CharacterId });
        using var req = CreateRequest(HttpMethod.Post, "/api/v1/world/enter", accessToken, null);
        req.Content = new StringContent(enterBody, Encoding.UTF8, "application/json");
        using HttpResponseMessage resp = await _httpClient.SendAsync(req, HttpCompletionOption.ResponseContentRead, ct).ConfigureAwait(false);
        if (resp.StatusCode == HttpStatusCode.NotFound) return ticket;
        string payload = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        if (resp.StatusCode == HttpStatusCode.PreconditionFailed)
            throw new HttpRequestException("RFC 9110 412 Precondition Failed on world enter.", null, HttpStatusCode.PreconditionFailed);
        if (!resp.IsSuccessStatusCode)
            return ticket with { WorldEnterAllowed = false };
        using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(payload) ? "{}" : payload);
        string status = ReadString(doc.RootElement, "status", "ALLOW");
        string worldEp = ReadString(doc.RootElement, "world_endpoint", ReadString(doc.RootElement, "worldEndpoint", ticket.WorldEndpoint));
        bool allowed = status.Equals("ALLOW", StringComparison.OrdinalIgnoreCase) || status.Equals("ok", StringComparison.OrdinalIgnoreCase);
        return ticket with { WorldEndpoint = worldEp, WorldEnterAllowed = allowed };
    }

    private async Task<string> SendJsonAsync(
        HttpRequestMessage req, string cachePath, string? jsonBody, CancellationToken ct)
    {
        if (jsonBody is not null)
            req.Content = new StringContent(jsonBody, Encoding.UTF8, "application/json");
        using HttpResponseMessage resp = await _httpClient.SendAsync(req, HttpCompletionOption.ResponseContentRead, ct).ConfigureAwait(false);
        string payload = await resp.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        EnsureSuccessOrThrow(resp, payload, out string validated);
        if (ExtractETag(resp) is { Length: > 0 } etag)
            _etagCache[cachePath] = (etag, validated);
        return validated;
    }

    private HttpRequestMessage CreateRequest(HttpMethod method, string relativePath, string? accessToken, string? ifMatchETag)
    {
        Uri baseUri = _httpClient.BaseAddress ?? BaseUri;
        var req = new HttpRequestMessage(method, new Uri(baseUri, relativePath))
        {
            Version = HttpVersion.Version20,
            VersionPolicy = HttpVersionPolicy.RequestVersionOrLower
        };
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/problem+json"));
        if (!string.IsNullOrWhiteSpace(accessToken))
            req.Headers.Authorization = new AuthenticationHeaderValue("Bearer", accessToken.Trim());
        if (!string.IsNullOrWhiteSpace(ifMatchETag))
            req.Headers.TryAddWithoutValidation("If-Match", ifMatchETag.Trim());
        return req;
    }

    private static void EnsureSuccessOrThrow(HttpResponseMessage resp, string body, out string validatedBody)
    {
        validatedBody = body;
        if (resp.StatusCode == HttpStatusCode.PreconditionFailed)
            throw new HttpRequestException("RFC 9110 412 Precondition Failed: ETag precondition failed.", null, HttpStatusCode.PreconditionFailed);
        if (resp.IsSuccessStatusCode) return;
        string detail = ExtractErrorMessage(body, (int)resp.StatusCode);
        throw new HttpRequestException(detail, null, resp.StatusCode);
    }

    private static string? ExtractETag(HttpResponseMessage resp)
    {
        if (resp.Headers.ETag is { Tag.Length: > 0 } tag) return tag.ToString();
        if (resp.Headers.TryGetValues("ETag", out IEnumerable<string>? values))
        {
            foreach (string v in values)
                if (!string.IsNullOrWhiteSpace(v)) return v.Trim();
        }
        return null;
    }

    private static AuthSessionResult ParseAuthSession(string json, string fallbackUser, bool isTempFallback)
    {
        using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(json) ? "{}" : json);
        JsonElement root = doc.RootElement;
        string token = ReadString(root, "access_token", ReadString(root, "token", ReadString(root, "accessToken", string.Empty)));
        if (string.IsNullOrWhiteSpace(token))
            throw new HttpRequestException("Gateway auth response did not include an access_token.", null, HttpStatusCode.BadGateway);
        string user = ReadString(root, "username", fallbackUser);
        int expires = ReadInt(root, "expires_in", ReadInt(root, "expiresIn", 86400));
        bool isTemp = ReadBool(root, "is_temporary", ReadBool(root, "temporary", isTempFallback));
        return new AuthSessionResult(token, user, expires, isTemp);
    }

    private static IReadOnlyList<LauncherCharacterSummary> ParseCharacterList(string json)
    {
        if (string.IsNullOrWhiteSpace(json)) return [];
        using JsonDocument doc = JsonDocument.Parse(json);
        JsonElement arr = doc.RootElement.ValueKind == JsonValueKind.Array
            ? doc.RootElement
            : (doc.RootElement.ValueKind == JsonValueKind.Object && doc.RootElement.TryGetProperty("characters", out JsonElement c) && c.ValueKind == JsonValueKind.Array ? c : default);
        if (arr.ValueKind != JsonValueKind.Array) return [];
        var list = new List<LauncherCharacterSummary>();
        foreach (JsonElement item in arr.EnumerateArray())
            list.Add(ParseSingleCharacter(item, "Hero"));
        return list;
    }

    private static LauncherCharacterSummary ParseSingleCharacter(JsonElement el, string fallbackName)
    {
        string id = ReadIdString(el, "character_id", ReadIdString(el, "characterId", ReadIdString(el, "id", fallbackName)));
        string name = ReadString(el, "name", ReadString(el, "character_name", ReadString(el, "characterName", fallbackName)));
        string cls = ReadString(el, "class_name", ReadString(el, "className", ReadString(el, "archetype", "Fighter")));
        int level = Math.Max(1, ReadInt(el, "level", 1));
        string zone = ReadString(el, "zone_name", ReadString(el, "zoneName", ReadString(el, "zone_key", "verra-01")));
        return new LauncherCharacterSummary(id, name, cls, level, zone);
    }

    private static CharacterSelectTicket ParseSelectTicket(string json, string fallbackChar, string fallbackToken)
    {
        using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(json) ? "{}" : json);
        JsonElement r = doc.RootElement;
        string id = ReadIdString(r, "character_id", ReadIdString(r, "characterId", ReadIdString(r, "id", fallbackChar)));
        string name = ReadString(r, "character_name", ReadString(r, "characterName", ReadString(r, "name", fallbackChar)));
        string ticket = ReadString(r, "join_ticket", ReadString(r, "joinTicket", ReadString(r, "ticket", fallbackToken)));
        string worldEp = ReadString(r, "world_endpoint", ReadString(r, "worldEndpoint", "127.0.0.1:7777"));
        bool allowed = ReadBool(r, "world_enter_allowed", ReadBool(r, "worldEnterAllowed", true));
        return new CharacterSelectTicket(id, name, ticket, worldEp, allowed);
    }

    private static string ExtractErrorMessage(string json, int statusCode)
    {
        if (!string.IsNullOrWhiteSpace(json))
        {
            try
            {
                using JsonDocument doc = JsonDocument.Parse(json);
                string msg = ReadString(doc.RootElement, "message", ReadString(doc.RootElement, "detail", ReadString(doc.RootElement, "error", string.Empty)));
                if (!string.IsNullOrWhiteSpace(msg)) return $"HTTP {statusCode}: {msg}";
            }
            catch (JsonException ex) { Trace.WriteLine(ex.Message); }
        }
        return $"Gateway HTTP {statusCode} request failed.";
    }

    private static string ReadIdString(JsonElement root, string name, string fallback)
    {
        if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty(name, out JsonElement p)) return fallback;
        return p.ValueKind switch
        {
            JsonValueKind.String when p.GetString() is { Length: > 0 } s => s,
            JsonValueKind.Number => p.GetRawText(),
            _ => fallback
        };
    }

    private static bool ReadBool(JsonElement root, string name, bool fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement p) && p.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? p.GetBoolean()
            : fallback;

    private static int ReadInt(JsonElement root, string name, int fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement p) && p.ValueKind == JsonValueKind.Number && p.TryGetInt32(out int v) ? v : fallback;

    private static string ReadString(JsonElement root, string name, string fallback) =>
        root.ValueKind == JsonValueKind.Object && root.TryGetProperty(name, out JsonElement p) && p.ValueKind == JsonValueKind.String && p.GetString() is { Length: > 0 } s ? s : fallback;
}
