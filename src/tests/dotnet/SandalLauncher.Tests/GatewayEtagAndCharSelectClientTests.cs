// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SandalLauncher.Models;
using SandalLauncher.Services;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class GatewayEtagAndCharSelectClientTests
{
    [Fact]
    public async Task RegisterLoginAndDeleteAccount_SendsExpectedPayloadsBearerTokenAndCachedIfMatch()
    {
        var recorded = new List<(HttpMethod Method, string Path, string? Auth, string? IfMatch, string Body)>();
        using var handler = new DelegateHttpHandler(async req =>
        {
            string body = req.Content is not null ? await req.Content.ReadAsStringAsync() : string.Empty;
            string? auth = req.Headers.Authorization?.ToString();
            string? ifMatch = req.Headers.IfMatch.Count > 0 ? req.Headers.IfMatch.ToString() : null;
            recorded.Add((req.Method, req.RequestUri!.AbsolutePath, auth, ifMatch, body));

            if (req.RequestUri.AbsolutePath == "/api/v1/auth/register")
                return JsonResponse(HttpStatusCode.OK, "{\"token\":\"tok_reg_99\",\"expires_in\":7200,\"username\":\"tmp_hero01\"}", "\"acct-v1\"");
            if (req.RequestUri.AbsolutePath == "/api/v1/auth/login")
                return JsonResponse(HttpStatusCode.OK, "{\"access_token\":\"tok_login_88\",\"expires_in\":3600,\"username\":\"perm_hero\"}", "\"acct-v2\"");
            if (req.Method == HttpMethod.Delete && req.RequestUri.AbsolutePath == "/api/v1/auth/account")
                return JsonResponse(HttpStatusCode.NoContent, string.Empty);

            return JsonResponse(HttpStatusCode.NotFound, "{}");
        });

        using var http = new HttpClient(handler);
        var client = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);

        AuthSessionResult reg = await client.RegisterAsync("tmp_hero01", "Tmp!Pass99", isTemporaryAccount: true);
        Assert.Equal("tok_reg_99", reg.AccessToken);
        Assert.Equal("tmp_hero01", reg.Username);
        Assert.Equal(7200, reg.ExpiresIn);
        Assert.True(reg.IsTemporaryAccount);

        AuthSessionResult login = await client.LoginAsync("perm_hero", "Perm!Pass88");
        Assert.Equal("tok_login_88", login.AccessToken);
        Assert.Equal("perm_hero", login.Username);
        Assert.Equal(3600, login.ExpiresIn);
        Assert.False(login.IsTemporaryAccount);

        client.SetCachedETag("/api/v1/auth/account", "\"acct-etag-del\"");
        bool deleted = await client.DeleteAccountAsync("tok_reg_99", "tmp_hero01", "Tmp!Pass99");
        Assert.True(deleted);
        Assert.Equal(3, recorded.Count);
        Assert.Equal(HttpMethod.Delete, recorded[2].Method);
        Assert.Equal("/api/v1/auth/account", recorded[2].Path);
        Assert.Equal("Bearer tok_reg_99", recorded[2].Auth);
        Assert.Equal("\"acct-etag-del\"", recorded[2].IfMatch);
    }

    [Fact]
    public async Task ListCharactersAsync_EnforcesRfc9110ETagIfNoneMatchAnd304NotModifiedCache()
    {
        int callCount = 0;
        string? secondIfNoneMatch = null;
        using var handler = new DelegateHttpHandler(req =>
        {
            callCount++;
            if (callCount == 1)
            {
                string json = "{\"characters\":[{\"character_id\":\"char_101\",\"name\":\"TestHero\",\"class_id\":2,\"race_id\":3,\"level\":15}]}";
                return Task.FromResult(JsonResponse(HttpStatusCode.OK, json, "\"chars-etag-v1\""));
            }

            secondIfNoneMatch = req.Headers.IfNoneMatch.ToString();
            var notModified = new HttpResponseMessage(HttpStatusCode.NotModified);
            notModified.Headers.ETag = new EntityTagHeaderValue("\"chars-etag-v1\"");
            return Task.FromResult(notModified);
        });

        using var http = new HttpClient(handler);
        var client = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);

        IReadOnlyList<LauncherCharacterSummary> first = await client.ListCharactersAsync("tok_abc");
        Assert.Single(first);
        Assert.Equal("char_101", first[0].CharacterId);
        Assert.Equal("TestHero", first[0].Name);
        Assert.Equal(15, first[0].Level);
        Assert.True(client.TryGetCachedETag("/api/v1/characters", out string cachedETag));
        Assert.Equal("\"chars-etag-v1\"", cachedETag);

        IReadOnlyList<LauncherCharacterSummary> second = await client.ListCharactersAsync("tok_abc");
        Assert.Equal(2, callCount);
        Assert.Equal("\"chars-etag-v1\"", secondIfNoneMatch);
        Assert.Single(second);
        Assert.Equal("char_101", second[0].CharacterId);
        Assert.Equal("TestHero", second[0].Name);
        Assert.True(client.ETagCache.ContainsKey("/api/v1/characters"));
    }

    [Fact]
    public async Task ProbeRealmAsync_EvaluatesHttpJsonGrpcWebAndHttpsRestHealthPayloads()
    {
        string? capturedAccept = null;
        using var handler = new DelegateHttpHandler(req =>
        {
            capturedAccept = req.Headers.Accept.ToString();
            if (req.RequestUri!.AbsolutePath == "/sandal.gateway.v1.RealmService/Health")
                return Task.FromResult(JsonResponse(HttpStatusCode.OK, "{\"online_players\":88,\"max_players\":500}"));
            return Task.FromResult(JsonResponse(HttpStatusCode.OK, "{\"online_players\":42,\"max_players\":500}", "\"realm-v1\""));
        });

        using var http = new HttpClient(handler);
        var client = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);
        var baseUri = new Uri("http://127.0.0.1:18080");

        GatewayProbeResult first = await client.ProbeRealmAsync(baseUri, GatewayTransportMode.HttpJson);
        Assert.True(first.IsReachable);
        Assert.Equal(200, first.StatusCode);
        Assert.Equal(42, first.OnlinePlayers);
        Assert.Equal(500, first.MaxPlayers);

        GatewayProbeResult grpc = await client.ProbeRealmAsync(baseUri, GatewayTransportMode.GrpcWeb);
        Assert.True(grpc.IsReachable);
        Assert.Equal(200, grpc.StatusCode);
        Assert.Equal(88, grpc.OnlinePlayers);
        Assert.Contains("application/grpc-web+json", capturedAccept, StringComparison.Ordinal);

        GatewayProbeResult https = await client.ProbeRealmAsync(baseUri, GatewayTransportMode.HttpsRest);
        Assert.True(https.IsReachable);
        Assert.Equal(42, https.OnlinePlayers);
    }

    [Fact]
    public async Task CreateAndSelectCharacterAsync_SendsIfMatchAcquires32HexJoinTicketAndVerifiesWorldEnter()
    {
        string? createIfMatch = null;
        string? selectIfMatch = null;
        string? worldEnterBody = null;
        const string joinTicketHex = "0123456789abcdef0123456789abcdef";

        using var handler = new DelegateHttpHandler(async req =>
        {
            string path = req.RequestUri!.AbsolutePath;
            if (req.Method == HttpMethod.Get && path == "/api/v1/characters")
                return JsonResponse(HttpStatusCode.OK, "{\"characters\":[]}", "\"chars-etag-0\"");
            if (req.Method == HttpMethod.Post && path == "/api/v1/characters")
            {
                createIfMatch = req.Headers.IfMatch.ToString();
                string json = "{\"character_id\":\"char_777\",\"name\":\"TestHero\",\"class_id\":1,\"race_id\":1,\"level\":1}";
                return JsonResponse(HttpStatusCode.Created, json, "\"chars-etag-1\"");
            }
            if (req.Method == HttpMethod.Post && path == "/api/v1/characters/char_777/select")
            {
                selectIfMatch = req.Headers.IfMatch.ToString();
                string json = $"{{\"character_id\":\"char_777\",\"character_name\":\"TestHero\",\"join_ticket\":\"{joinTicketHex}\",\"world_endpoint\":\"127.0.0.1:7777\"}}";
                return JsonResponse(HttpStatusCode.OK, json, "\"char-sel-etag-1\"");
            }
            if (req.Method == HttpMethod.Post && path == "/api/v1/world/enter")
            {
                worldEnterBody = await req.Content!.ReadAsStringAsync();
                return JsonResponse(HttpStatusCode.OK, "{\"status\":\"ALLOW\",\"world_endpoint\":\"127.0.0.1:7777\"}");
            }
            return JsonResponse(HttpStatusCode.NotFound, "{}");
        });

        using var http = new HttpClient(handler);
        var client = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);

        _ = await client.ListCharactersAsync("tok_session_1");
        LauncherCharacterSummary created = await client.CreateCharacterAsync("tok_session_1", "TestHero", 1, 1);
        Assert.Equal("char_777", created.CharacterId);
        Assert.Equal("TestHero", created.Name);
        Assert.Equal("\"chars-etag-0\"", createIfMatch);

        CharacterSelectTicket ticket = await client.SelectCharacterAsync(
            "tok_session_1",
            "char_777",
            verifyWorldEnter: true,
            ifMatchETag: "\"chars-etag-1\"");

        Assert.Equal("char_777", ticket.CharacterId);
        Assert.Equal("TestHero", ticket.CharacterName);
        Assert.Equal(joinTicketHex, ticket.JoinTicket);
        Assert.Equal(32, ticket.JoinTicket.Length);
        Assert.Equal("127.0.0.1:7777", ticket.WorldEndpoint);
        Assert.True(ticket.WorldEnterAllowed);
        Assert.Equal("\"chars-etag-1\"", selectIfMatch);
        Assert.NotNull(worldEnterBody);
        Assert.Contains(joinTicketHex, worldEnterBody, StringComparison.Ordinal);
    }

    [Fact]
    public async Task ConditionalRequests_On412PreconditionFailed_ThrowHttpRequestExceptionWith412Status()
    {
        string failStage = "create";
        using var handler = new DelegateHttpHandler(req =>
        {
            string path = req.RequestUri!.AbsolutePath;
            if (failStage == "create" && path == "/api/v1/characters")
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.PreconditionFailed));
            if (failStage == "select" && path.EndsWith("/select", StringComparison.Ordinal))
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.PreconditionFailed));
            if (failStage == "world" && path.EndsWith("/select", StringComparison.Ordinal))
            {
                return Task.FromResult(JsonResponse(
                    HttpStatusCode.OK,
                    "{\"character_id\":\"c1\",\"character_name\":\"TestHero\",\"join_ticket\":\"abcdef0123456789abcdef0123456789\",\"world_endpoint\":\"127.0.0.1:7777\"}",
                    "\"sel-etag\""));
            }
            if (failStage == "world" && path == "/api/v1/world/enter")
                return Task.FromResult(new HttpResponseMessage(HttpStatusCode.PreconditionFailed));

            return Task.FromResult(JsonResponse(HttpStatusCode.OK, "{}"));
        });

        using var http = new HttpClient(handler);
        var client = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);

        HttpRequestException exCreate = await Assert.ThrowsAsync<HttpRequestException>(() =>
            client.CreateCharacterAsync("tok", "TestHero"));
        Assert.Equal(HttpStatusCode.PreconditionFailed, exCreate.StatusCode);

        failStage = "select";
        HttpRequestException exSelect = await Assert.ThrowsAsync<HttpRequestException>(() =>
            client.SelectCharacterAsync("tok", "c1", verifyWorldEnter: true, ifMatchETag: "\"stale\""));
        Assert.Equal(HttpStatusCode.PreconditionFailed, exSelect.StatusCode);

        failStage = "world";
        HttpRequestException exWorld = await Assert.ThrowsAsync<HttpRequestException>(() =>
            client.SelectCharacterAsync("tok", "c1", verifyWorldEnter: true));
        Assert.Equal(HttpStatusCode.PreconditionFailed, exWorld.StatusCode);
    }

    private static HttpResponseMessage JsonResponse(HttpStatusCode status, string json, string? etag = null)
    {
        var resp = new HttpResponseMessage(status)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };
        if (!string.IsNullOrWhiteSpace(etag))
            resp.Headers.ETag = new EntityTagHeaderValue(etag);
        return resp;
    }

    private sealed class DelegateHttpHandler(Func<HttpRequestMessage, Task<HttpResponseMessage>> responder) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            responder(request);
    }
}
