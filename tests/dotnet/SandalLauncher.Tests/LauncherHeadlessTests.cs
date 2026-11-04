// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.IO.Compression;
using System.IO.Pipes;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
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

    private sealed class StubGatewayHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
            {
                Content = new StringContent("{\"online_players\":64,\"max_players\":500}", Encoding.UTF8, "application/json")
            });
    }
}
