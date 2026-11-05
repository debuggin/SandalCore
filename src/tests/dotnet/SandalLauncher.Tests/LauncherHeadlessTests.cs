// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.IO.Compression;
using System.IO.Pipes;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using SandalCore.Config;
using SandalLauncher.Config;
using SandalLauncher.Models;
using SandalLauncher.Security;
using SandalLauncher.Services;
using SandalLauncher.ViewModels;
using SandalLauncher.Views;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]

namespace SandalLauncher.Tests;

public sealed class LauncherHeadlessTests
{
    [Fact]
    public async Task EndToEndProfileStoreAndVaultWorkflow_PersistsCatalogWithoutPlaintextSecrets()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-e2e-prof-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string catalogPath = Path.Combine(tempDir, "LauncherProfiles.json");
            var store = new ProfileStoreService(catalogPath);
            using var vault = new InMemoryCredentialVault();
            var switcherVm = new ProfileSwitcherViewModel(store, vault);
            var editorView = new ProfileEditorView { DataContext = switcherVm };

            await switcherVm.LoadProfilesAsync();
            Assert.Same(switcherVm, editorView.DataContext);
            Assert.Equal("populated", switcherVm.CurrentViewState);

            switcherVm.PrepareNewProfile();
            switcherVm.EditId = "verra-canary";
            switcherVm.EditDisplayName = "Verra Canary Realm";
            switcherVm.EditAccountUsername = "canary_tester";
            await switcherVm.SaveProfileAsync();

            switcherVm.SessionTokenInput = "session_secret_token_alpha_99";
            await switcherVm.StoreSessionTokenAsync();
            Assert.True(switcherVm.HasVaultToken);
            Assert.Equal("session_secret_token_alpha_99", await vault.RetrieveTokenAsync("verra-canary"));

            string rawCatalog = await File.ReadAllTextAsync(catalogPath);
            Assert.DoesNotContain("session_secret_token_alpha_99", rawCatalog);
            Assert.Throws<InvalidDataException>(() => ProfileStoreService.ValidateProfile(
                new LauncherProfile { Id = "bad", DisplayName = "Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9" }));
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public async Task EndToEndLaunchPreparation_UnlatchesSettingsVerifiesHashServesTokenAndBuildsStartInfo()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-e2e-launch-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string binDir = Path.Combine(tempDir, "AOC", "Binaries", "Win64");
            Directory.CreateDirectory(binDir);
            string exePath = Path.Combine(binDir, "AOCClient-Win64-Shipping.exe");
            await File.WriteAllBytesAsync(exePath, "MZ-PE32-SHIPPING-STUB"u8.ToArray());

            string savePath = Path.Combine(tempDir, "GeneralSettings.save");
            await File.WriteAllBytesAsync(savePath, []);
            Assert.Equal(GeneralSettingsUnlatchAction.DeletedZeroByte, GeneralSettingsUnlatch.EnsureUnlatched(savePath));
            await File.WriteAllTextAsync(savePath, "{\"fov\":100}", Encoding.UTF8);
            Assert.Equal(GeneralSettingsUnlatchAction.SnapshotCreated, GeneralSettingsUnlatch.EnsureUnlatched(savePath));
            await File.WriteAllBytesAsync(savePath, []);
            Assert.Equal(GeneralSettingsUnlatchAction.RestoredFromBackup, GeneralSettingsUnlatch.EnsureUnlatched(savePath));

            string expectedHash = BinaryHashVerifier.ComputeFileSha256Hex(exePath);
            Assert.True((await BinaryHashVerifier.VerifyFileHashAsync(exePath, expectedHash)).IsMatch);
            Assert.False((await BinaryHashVerifier.VerifyFileHashAsync(exePath, new string('0', 64))).IsMatch);

            await using var pipeServer = new TokenPipeServer();
            Task<bool> serveTask = pipeServer.ServeTokenOnceAsync("session_ticket_777");
            await using var pipeClient = new NamedPipeClientStream(".", pipeServer.PipeName, PipeDirection.In, PipeOptions.Asynchronous);
            await pipeClient.ConnectAsync(2000);
            byte[] pipeBuf = new byte[64];
            int readLen = await pipeClient.ReadAsync(pipeBuf);
            Assert.True(await serveTask);
            Assert.Equal("session_ticket_777", Encoding.UTF8.GetString(pipeBuf, 0, readLen));
            Assert.False(TokenPipeServer.IsValidPrintableAsciiToken("bad token\n"));

            var profile = new LauncherProfile { Id = "local-dev", ClientPakDir = tempDir, UseEos = false };
            var startInfo = ClientLaunchService.BuildLaunchStartInfo(profile, 29000, pipeServer.PipeName);
            Assert.False(startInfo.UseShellExecute);
            Assert.Contains("-LauncherTetherPort=29000", startInfo.ArgumentList);
            Assert.Contains(pipeServer.CliArgument, startInfo.ArgumentList);
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public async Task EndToEndTetherAndGatewayTelemetry_HandshakesUdpAndUpdatesServerControlView()
    {
        await using var tether = new LocalTetherListener(0);
        using var udpClient = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        var serverEp = new IPEndPoint(IPAddress.Loopback, tether.Port);

        byte[] connectPkt = new byte[LocalTetherListener.HeaderSize];
        var reqHdr = new ArqHeader(LocalTetherListener.BuildMagic(2, MsgId.Connect), 77u, LocalTetherListener.DefaultWindowSize, 0, 0, 9L, 0L, 1000L);
        LocalTetherListener.WriteHeader(connectPkt, in reqHdr);
        Task<bool> serveTask = tether.ProcessNextDatagramAsync();
        await udpClient.SendAsync(connectPkt, serverEp);
        UdpReceiveResult reply = await udpClient.ReceiveAsync();
        Assert.True(await serveTask);
        Assert.True(LocalTetherListener.TryParseHeader(reply.Buffer, out ArqHeader ackHdr));
        Assert.Equal(MsgId.Connect, ackHdr.MsgId);
        Assert.Equal(9L, ackHdr.NextExpectedRecvSeq);

        using var httpClient = new HttpClient(new StubGatewayHandler());
        var gwClient = new MultiProtocolGatewayClient(httpClient);
        GatewayProbeResult probe = await gwClient.ProbeRealmAsync(new Uri("http://127.0.0.1:8080"), GatewayTransportMode.HttpJson);
        Assert.True(probe.IsReachable);
        Assert.Equal(64, probe.OnlinePlayers);

        var controlView = new ServerControlView();
        var presetsVm = Assert.IsType<LaunchPresetsViewModel>(controlView.DataContext);
        presetsVm.RealmPill.SetCheckingState();
        Assert.Equal(RealmPillState.Loading, presetsVm.RealmPill.ViewState);
        presetsVm.RealmPill.ApplyProbeResult(probe, hashMatched: true);
        Assert.Equal(RealmPillState.Populated, presetsVm.RealmPill.ViewState);
        Assert.Equal("64 / 500", presetsVm.RealmPill.PopulationLabel);
    }

    [Fact]
    public async Task EndToEndSystemDoctorAndDiagnosticBundle_EvaluatesProbesAndRedactsExportedArchive()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-e2e-doc-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            DoctorReport report = SystemDoctorService.RunDiagnostics(new DoctorProbeInput());
            Assert.Equal(DoctorCheckSeverity.Pass, report.OverallSeverity);
            Assert.Equal(8, report.Checks.Count);

            string rawLog = "Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.pay.sig\nics_token=secret_ics_99\npostgres://aoc:hunter2@127.0.0.1:5432/db";
            string zipPath = Path.Combine(tempDir, "bundle.zip");
            await DiagnosticBundleExporter.ExportBundleAsync(zipPath, report, "{ \"ics_token\": \"tok_1\" }", rawLog);

            using ZipArchive zip = ZipFile.OpenRead(zipPath);
            using var sr = new StreamReader(zip.GetEntry(DiagnosticBundleExporter.RedactedTailLogEntryName)!.Open(), Encoding.UTF8);
            string scrubbed = await sr.ReadToEndAsync();
            Assert.DoesNotContain("eyJhbGciOiJIUzI1NiJ9", scrubbed);
            Assert.DoesNotContain("secret_ics_99", scrubbed);
            Assert.DoesNotContain("hunter2", scrubbed);
            Assert.Contains("[REDACTED]", scrubbed);
        }
        finally
        {
            if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true);
        }
    }

    [Fact]
    public async Task EndToEndSixPhaseAutomationLifecycle_TransitionsPhasesSpawnsMockGameAndClosesLauncher()
    {
        LauncherCliOptions parsed = LauncherCliOptions.Parse(["--keep-alive", "--mock-game-client", "120", "--automation-port", "18089"]);
        Assert.True(parsed.KeepAlive);
        Assert.True(parsed.MockGameClient);
        Assert.Equal(120, parsed.MockGameSleepSec);

        using var gwHttp = new HttpClient(new StubGatewayHandler()) { BaseAddress = new Uri("http://127.0.0.1:8080") };
        var gwClient = new MultiProtocolGatewayClient(gwHttp, new Uri("http://127.0.0.1:8080"));
        var vm = new MainWindowViewModel(SandalConfigLoader.LoadLauncher(), gwClient, new InMemoryCredentialVault(), new ProfileStoreService());
        var closeTcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        await using var server = new LauncherAutomationServer(vm, 0, () => closeTcs.TrySetResult(true));
        using var client = new HttpClient { BaseAddress = server.BaseUri };

        LauncherAutomationStateSnapshot s1 = await GetSnapshotAsync(client);
        Assert.Equal("launcher_loaded", s1.LifecyclePhase);
        Assert.True(s1.LauncherPid > 0);
        Assert.False(s1.AshesProcessRunning);

        LauncherAutomationStateSnapshot s2 = await PostSnapshotAsync(client, "/automation/launch", "{}");
        Assert.Equal("game_started", s2.LifecyclePhase);
        Assert.True(s2.AshesProcessRunning);
        Assert.True(s2.AshesPid is > 0 && ClientLaunchService.IsProcessAlive(s2.AshesPid));
        int spawnedPid = s2.AshesPid!.Value;

        _ = await PostSnapshotAsync(client, "/automation/register-temp", "{\"createDefaultChar\":true,\"characterName\":\"Hero_Auto\"}");
        LauncherAutomationStateSnapshot s3 = await PostSnapshotAsync(client, "/automation/char-select", "{\"characterName\":\"Hero_Auto\",\"characterIdOrName\":\"Hero_Auto\",\"spawnClient\":true}");
        Assert.Equal("game_logged_in", s3.LifecyclePhase);
        Assert.Equal("jtk_99", s3.LastJoinTicket);
        Assert.Equal("127.0.0.1:7777", s3.WorldEndpoint);
        Assert.DoesNotContain("_", s3.Characters[0].Name, StringComparison.Ordinal);
        Assert.Equal(spawnedPid, s3.AshesPid);
        Assert.True(ClientLaunchService.IsProcessAlive(spawnedPid));

        LauncherAutomationStateSnapshot s4 = await PostSnapshotAsync(client, "/automation/terminate-ashes", "{\"killAllMatching\":false}");
        Assert.Equal("game_closed", s4.LifecyclePhase);
        Assert.False(s4.AshesProcessRunning);
        Assert.Null(s4.AshesPid);
        Assert.False(ClientLaunchService.IsProcessAlive(spawnedPid));

        LauncherAutomationStateSnapshot s5 = await PostSnapshotAsync(client, "/automation/close-launcher", "{}");
        Assert.Equal("launcher_closed", s5.LifecyclePhase);
        Assert.True(await closeTcs.Task.WaitAsync(TimeSpan.FromSeconds(5)));
    }

    private static async Task<LauncherAutomationStateSnapshot> GetSnapshotAsync(HttpClient client)
    {
        string json = await client.GetStringAsync("/automation/state");
        return JsonSerializer.Deserialize<LauncherAutomationStateSnapshot>(json)!;
    }

    private static async Task<LauncherAutomationStateSnapshot> PostSnapshotAsync(HttpClient client, string path, string body)
    {
        using var content = new StringContent(body, Encoding.UTF8, "application/json");
        using HttpResponseMessage resp = await client.PostAsync(path, content);
        resp.EnsureSuccessStatusCode();
        string json = await resp.Content.ReadAsStringAsync();
        return JsonSerializer.Deserialize<LauncherAutomationStateSnapshot>(json)!;
    }

    private sealed class StubGatewayHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            string path = request.RequestUri?.AbsolutePath ?? "/";
            string body = path switch
            {
                "/api/v1/auth/register" or "/api/v1/auth/login" =>
                    "{\"access_token\":\"tok_e2e_1\",\"username\":\"tmp_e2e\",\"is_temporary\":true}",
                "/api/v1/characters" when request.Method == HttpMethod.Get => "[]",
                "/api/v1/characters" =>
                    "{\"character_id\":\"char-1\",\"name\":\"HeroAuto1234\",\"class_name\":\"Fighter\",\"level\":1,\"zone_name\":\"verra-01\"}",
                _ when path.EndsWith("/select", StringComparison.OrdinalIgnoreCase) =>
                    "{\"character_id\":\"char-1\",\"character_name\":\"HeroAuto1234\",\"join_ticket\":\"jtk_99\",\"world_endpoint\":\"world:7777\",\"world_enter_allowed\":true}",
                "/api/v1/world/enter" => "{\"status\":\"ALLOW\",\"world_endpoint\":\"world:7777\"}",
                _ => "{\"online_players\":64,\"max_players\":500}"
            };
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent(body, Encoding.UTF8, "application/json")
            });
        }
    }
}
