// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using SandalCore.Config;
using SandalLauncher.Config;
using SandalLauncher.Security;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;
using SandalLauncher.Views;
using Xunit;

namespace SandalLauncher.Tests;

internal sealed class AvaloniaFactAttribute : FactAttribute;

public sealed class LauncherAutomationAndPostLoginUiTests
{
    private static readonly string[] RequiredMainWindowControlIds =
    [
        "SignInPanel",
        "AuthUsernameInput",
        "AuthPasswordInput",
        "RegisterTempAccountToggle",
        "LoginButton",
        "RegisterTempButton",
        "PostLoginPanel",
        "SignedInUsernameText",
        "TempAccountBadge",
        "CharacterListBox",
        "CreateCharacterButton",
        "SelectCharacterButton",
        "JoinTicketBadge",
        "TerminateAshesButton",
        "DeleteAccountButton",
        "LogoutButton"
    ];

    [AvaloniaFact]
    public async Task MainWindow_SignInAndPostLoginPanels_BindControlsAndNeverStorePermanentCredentials()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-mw-ui-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            Assert.True(typeof(Window).IsAssignableFrom(typeof(MainWindow)));
            VerifyCompiledMainWindowControlsPresent();

            using var vault = new InMemoryCredentialVault();
            var store = new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json"));
            using var http = new HttpClient(new FakeGatewayHandler());
            var gw = new MultiProtocolGatewayClient("http://127.0.0.1:18080", http);
            var vm = new MainWindowViewModel(SandalConfigLoader.LoadLauncher(), gw, vault, store);

            Assert.NotNull(vm.LoginCommand);
            Assert.NotNull(vm.RegisterTempCommand);
            Assert.NotNull(vm.CreateCharacterCommand);
            Assert.NotNull(vm.SelectCharacterCommand);
            Assert.NotNull(vm.TerminateAshesCommand);
            Assert.NotNull(vm.DeleteAccountCommand);
            Assert.NotNull(vm.LogoutCommand);
            Assert.True(vm.IsSignedOut);
            Assert.False(vm.IsAuthenticated);

            vm.AuthUsername = "permanent_player";
            vm.AuthPassword = "PermanentPassword!99";
            vm.IsTemporaryAccount = false;
            await vm.LoginCommand.ExecuteAsync(null);

            Assert.True(vm.IsAuthenticated);
            Assert.False(vm.IsSignedOut);
            Assert.False(vm.IsTemporaryAccount);
            Assert.Equal("permanent_player", vm.SignedInUsername);
            Assert.Equal(string.Empty, vm.AuthPassword);
            Assert.NotEmpty(vm.Characters);
            Assert.NotNull(vm.SelectedCharacter);
            Assert.Null(await vault.RetrieveTokenAsync("temp-permanent_player"));
            Assert.Null(await vault.RetrieveTokenAsync("permanent_player"));

            await vm.SelectCharacterCommand.ExecuteAsync(null);
            Assert.NotNull(vm.LastCharacterSelectTicket);
            Assert.Equal(32, vm.LastJoinTicket.Length);
            Assert.Equal("127.0.0.1:7777", vm.WorldEndpoint);

            vm.LogoutCommand.Execute(null);
            Assert.False(vm.IsAuthenticated);
            Assert.True(vm.IsSignedOut);
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    private static void VerifyCompiledMainWindowControlsPresent()
    {
        var flags = System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public;
        Assert.NotNull(typeof(MainWindow).GetMethod("!XamlIlPopulate", flags));
        byte[] peBytes = File.ReadAllBytes(typeof(MainWindow).Assembly.Location);
        string userStrings = Encoding.Unicode.GetString(peBytes);
        foreach (string controlId in RequiredMainWindowControlIds)
        {
            Assert.Contains(controlId, userStrings, StringComparison.Ordinal);
        }
    }

    [AvaloniaFact]
    public async Task LauncherAutomationServer_EndToEndLoopbackHttpLifecycle_DrivesRegisterCharSelectTerminateAndDelete()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-auto-http-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            using var vault = new InMemoryCredentialVault();
            var store = new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json"));
            using var gwHttp = new HttpClient(new FakeGatewayHandler());
            var gw = new MultiProtocolGatewayClient("http://127.0.0.1:18080", gwHttp);
            var vm = new MainWindowViewModel(SandalConfigLoader.LoadLauncher(), gw, vault, store);
            using var server = new LauncherAutomationServer(vm, port: 0);
            using var client = new HttpClient { BaseAddress = server.BaseUri };

            HttpResponseMessage regResp = await PostJsonAsync(
                client,
                "/automation/register-temp",
                "{\"username\":\"TMP HeroNoodleTruck\",\"password\":\"Tmp!AutoPass77\"}");
            Assert.Equal(HttpStatusCode.OK, regResp.StatusCode);
            Assert.True(vm.IsAuthenticated);
            Assert.True(vm.IsTemporaryAccount);
            Assert.Equal("TMP HeroNoodleTruck", vm.SignedInUsername);
            Assert.NotNull(await vault.RetrieveTokenAsync("temp-TMP HeroNoodleTruck"));
            Assert.Single(await store.GetTemporaryProfilesAsync());

            await VerifyStateETagAnd304Async(client);

            HttpResponseMessage createResp = await PostJsonAsync(
                client,
                "/automation/characters/create",
                "{\"name\":\"TST GruelingBiscuitLauncher\",\"classId\":1,\"raceId\":1}");
            Assert.Equal(HttpStatusCode.OK, createResp.StatusCode);
            Assert.Contains(vm.Characters, c => c.Name == "TST GruelingBiscuitLauncher");

            HttpResponseMessage selectResp = await PostJsonAsync(
                client,
                "/automation/char-select",
                "{\"characterIdOrName\":\"TST GruelingBiscuitLauncher\",\"spawnClient\":false}");
            Assert.Equal(HttpStatusCode.OK, selectResp.StatusCode);
            Assert.Equal(32, vm.LastJoinTicket.Length);
            Assert.Equal("127.0.0.1:7777", vm.WorldEndpoint);
            Assert.NotNull(vm.LastCharacterSelectTicket);
            Assert.True(vm.LastCharacterSelectTicket.WorldEnterAllowed);

            HttpResponseMessage termResp = await PostJsonAsync(
                client,
                "/automation/terminate-ashes",
                "{\"killAllMatching\":true}");
            Assert.Equal(HttpStatusCode.OK, termResp.StatusCode);
            Assert.False(vm.IsAshesProcessRunning);

            HttpResponseMessage delResp = await PostJsonAsync(client, "/automation/delete-account", "{}");
            Assert.Equal(HttpStatusCode.OK, delResp.StatusCode);
            Assert.False(vm.IsAuthenticated);
            Assert.True(vm.IsEmptyState);
            Assert.Null(await vault.RetrieveTokenAsync("temp-TMP HeroNoodleTruck"));
            Assert.Empty(await store.GetTemporaryProfilesAsync());
            server.Dispose();
            await server.DisposeAsync();
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    [Fact]
    public async Task RunCliAutomationAsync_ExecutesFullTempAccountCharSelectAndCleanupWorkflow()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-cli-e2e-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            using var vault = new InMemoryCredentialVault();
            var store = new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json"));
            using var gwHttp = new HttpClient(new FakeGatewayHandler(startEmptyCharacters: true));
            var gw = new MultiProtocolGatewayClient("http://127.0.0.1:18080", gwHttp);

            LauncherCliOptions opts = LauncherCliOptions.Parse(
            [
                "--register-temp",
                "--username=TMP HeroNoodleTruck",
                "--password=Tmp!CliPass99",
                "--create-character-if-missing",
                "--select-character=TST GruelingBiscuitLauncher",
                "--verify-char-select",
                "--terminate-after-char-select",
                "--delete-temp-account-on-exit"
            ], new Dictionary<string, string?>());

            int exitCode = await Program.RunCliAutomationAsync(opts, gw, vault, store);

            Assert.Equal(0, exitCode);
            Assert.Null(await vault.RetrieveTokenAsync("temp-TMP HeroNoodleTruck"));
            Assert.Empty(await store.GetTemporaryProfilesAsync());
        }
        finally
        {
            if (Directory.Exists(tempDir))
                Directory.Delete(tempDir, recursive: true);
        }
    }

    private static async Task VerifyStateETagAnd304Async(HttpClient client)
    {
        using var get1 = new HttpRequestMessage(HttpMethod.Get, "/automation/state");
        using HttpResponseMessage resp1 = await client.SendAsync(get1);
        Assert.Equal(HttpStatusCode.OK, resp1.StatusCode);
        string? etag = resp1.Headers.ETag?.Tag;
        Assert.False(string.IsNullOrWhiteSpace(etag));

        using var get2 = new HttpRequestMessage(HttpMethod.Get, "/automation/state");
        get2.Headers.IfNoneMatch.Add(new EntityTagHeaderValue(etag!));
        using HttpResponseMessage resp2 = await client.SendAsync(get2);
        Assert.Equal(HttpStatusCode.NotModified, resp2.StatusCode);
    }

    private static async Task<HttpResponseMessage> PostJsonAsync(HttpClient client, string path, string json)
    {
        using var req = new HttpRequestMessage(HttpMethod.Post, path)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };
        return await client.SendAsync(req);
    }

    private sealed class FakeGatewayHandler(bool startEmptyCharacters = false) : HttpMessageHandler
    {
        private readonly List<(string Id, string Name)> _chars = startEmptyCharacters
            ? []
            : [("char_default", "ExistingHero")];

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            string path = request.RequestUri!.AbsolutePath;
            string body = request.Content is not null ? await request.Content.ReadAsStringAsync(cancellationToken) : "{}";

            if (path is "/api/v1/auth/register" or "/api/v1/auth/login")
            {
                using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
                string user = doc.RootElement.TryGetProperty("username", out JsonElement u) ? u.GetString() ?? "hero" : "hero";
                return Reply(HttpStatusCode.OK, $"{{\"token\":\"tok_{user}\",\"account_id\":\"acc_{user}\",\"username\":\"{user}\"}}");
            }
            if (request.Method == HttpMethod.Delete && path == "/api/v1/auth/account")
                return Reply(HttpStatusCode.NoContent, string.Empty);
            if (request.Method == HttpMethod.Get && path == "/api/v1/characters")
            {
                var items = new List<string>();
                foreach ((string id, string name) in _chars)
                    items.Add($"{{\"character_id\":\"{id}\",\"name\":\"{name}\",\"class_id\":1,\"race_id\":1,\"level\":1}}");
                return Reply(HttpStatusCode.OK, $"{{\"characters\":[{string.Join(",", items)}]}}", "\"chars-v1\"");
            }
            if (request.Method == HttpMethod.Post && path == "/api/v1/characters")
            {
                using JsonDocument doc = JsonDocument.Parse(string.IsNullOrWhiteSpace(body) ? "{}" : body);
                string name = doc.RootElement.TryGetProperty("name", out JsonElement n) ? n.GetString() ?? "TST GruelingBiscuitLauncher" : "TST GruelingBiscuitLauncher";
                string id = $"char_{_chars.Count + 1}";
                _chars.Add((id, name));
                return Reply(HttpStatusCode.Created, $"{{\"character_id\":\"{id}\",\"name\":\"{name}\",\"class_id\":1,\"race_id\":1,\"level\":1}}", "\"chars-v2\"");
            }
            if (request.Method == HttpMethod.Post && path.EndsWith("/select", StringComparison.Ordinal))
            {
                return Reply(
                    HttpStatusCode.OK,
                    "{\"character_id\":\"char_1\",\"character_name\":\"TST GruelingBiscuitLauncher\",\"join_ticket\":\"fedcba9876543210fedcba9876543210\",\"world_endpoint\":\"127.0.0.1:7777\"}",
                    "\"sel-v1\"");
            }
            if (request.Method == HttpMethod.Post && path == "/api/v1/world/enter")
                return Reply(HttpStatusCode.OK, "{\"decision\":\"ALLOW\",\"allowed\":true}");

            return Reply(HttpStatusCode.OK, "{\"online_players\":5,\"max_players\":500}");
        }

        private static HttpResponseMessage Reply(HttpStatusCode status, string json, string? etag = null)
        {
            var resp = new HttpResponseMessage(status)
            {
                Content = new StringContent(json, Encoding.UTF8, "application/json")
            };
            if (!string.IsNullOrWhiteSpace(etag))
                resp.Headers.ETag = new EntityTagHeaderValue(etag);
            return resp;
        }
    }
}
