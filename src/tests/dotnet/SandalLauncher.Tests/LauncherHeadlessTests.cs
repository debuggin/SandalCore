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
        finally { DeleteTempDirWithRetry(tempDir); }
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
        finally { DeleteTempDirWithRetry(tempDir); }
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
        finally { DeleteTempDirWithRetry(tempDir); }
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

    [Fact]
    public async Task LocalTetherListener_CppWireParity_HandlesConnectOpenSessionConnectionInfoEchoCloseAndAskWindow()
    {
        await using var tether = new LocalTetherListener(0)
        {
            ActiveAccessToken = "live_mem_jwt_token_50061",
            IcsEndpoint = "127.0.0.1:50061"
        };
        using var udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
        var ep = new IPEndPoint(IPAddress.Loopback, tether.Port);

        byte[] c1 = await SendTetherHeaderOnlyAsync(udp, ep, tether, 1, MsgId.Connect, 42u, 1L, 0L);
        Assert.Equal(LocalTetherListener.HeaderSize, c1.Length);
        byte[] c2 = await SendTetherHeaderOnlyAsync(udp, ep, tether, 2, MsgId.Connect, 42u, 5L, 0L);
        Assert.Equal(LocalTetherListener.HeaderSize + 4 + LocalTetherListener.LauncherProcessorId.Length, c2.Length);

        var openTags = await SendTetherDataAndReadReplyAsync(udp, ep, tether, 42u, 6L, "request_open_session");
        Assert.Equal("live_mem_jwt_token_50061", openTags["auth_token"]);
        Assert.Equal("127.0.0.1", openTags["ics_host"]);
        Assert.Equal("50061", openTags["ics_port"]);
        Assert.Equal("127.0.0.1:50061", openTags["ics_endpoint"]);
        Assert.Equal("https://127.0.0.1:50061", openTags["auth_endpoint"]);
        Assert.Equal("x-ics-auth", openTags["grpc_metadata_magic_key"]);
        Assert.Equal("AOC/1.0", openTags["user_agent_prefix"]);

        var infoTags = await SendTetherDataAndReadReplyAsync(udp, ep, tether, 42u, 7L, "request_game_client_connection_info");
        Assert.Equal("reply_game_client_connection_info", infoTags["message_type_name"]);
        Assert.Equal("ok", infoTags["result"]);
        Assert.Equal("127.0.0.1:50061", infoTags["ics_endpoint"]);

        var echoTags = await SendTetherDataAndReadReplyAsync(udp, ep, tether, 42u, 8L, "request_echo");
        Assert.Equal("reply_echo", echoTags["message_type_name"]);

        var closeTags = await SendTetherDataAndReadReplyAsync(udp, ep, tether, 42u, 9L, "request_close_session");
        Assert.Equal("reply_close_session", closeTags["message_type_name"]);

        byte[] retransmit = await SendTetherHeaderOnlyAsync(udp, ep, tether, 2, MsgId.AskWindow, 42u, 9L, 0L);
        Assert.True(LocalTetherListener.TryParseHeader(retransmit, out ArqHeader rHdr) && rHdr.MsgId == MsgId.Data && rHdr.SequenceNumber == 0L);
        byte[] tellWin = await SendTetherHeaderOnlyAsync(udp, ep, tether, 2, MsgId.AskWindow, 42u, 0L, 99L);
        Assert.True(LocalTetherListener.TryParseHeader(tellWin, out ArqHeader twHdr) && twHdr.MsgId == MsgId.TellWindow && twHdr.NextExpectedRecvSeq == 10L);

        _ = await SendTetherHeaderOnlyAsync(udp, ep, tether, 2, MsgId.Connect, 77u, 0L, 0L);
        _ = await SendTetherDataAndReadReplyAsync(udp, ep, tether, 77u, 1L, "request_open_session");
        byte[] keepAlive = await SendTetherHeaderOnlyAsync(udp, ep, tether, 2, MsgId.AskWindow, 77u, 0L, 1L);
        Assert.True(LocalTetherListener.TryParseHeader(keepAlive, out ArqHeader kaHdr) && kaHdr.MsgId == MsgId.TellWindow);
        Assert.NotEqual(1L, kaHdr.NextExpectedRecvSeq);
        Assert.Equal(2L, kaHdr.NextExpectedRecvSeq);
    }

    [Fact]
    public void SandalConfigAndProfileStore_NormalizesIcsPort50061SingleLocalDevServerAndTlsEnvVars()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-28c-cfg-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            Assert.Equal("127.0.0.1:50061", SandalConfigLoader.NormalizeIcsEndpoint(null, tempDir));
            Assert.Equal("127.0.0.1:50061", SandalConfigLoader.NormalizeIcsEndpoint("127.0.0.1:8444", tempDir));
            Assert.Equal("127.0.0.1:50061", SandalConfigLoader.NormalizeIcsEndpoint("gateway:50061", tempDir));
            Assert.Equal("127.0.0.1:50061", SandalConfigLoader.NormalizeIcsEndpoint("0.0.0.0:50061", tempDir));

            string expectedWorkspaceIcs = $"{SandalConfigLoader.ResolveLocalIcsHost()}:50061";
            var store = new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json"));
            var defaults = store.CreateDefaultProfiles();
            Assert.Single(defaults);
            Assert.Equal("local-dev", defaults[0].Id);
            Assert.Equal(expectedWorkspaceIcs, defaults[0].GatewayIcsAddr);

            string legacyJson = "{\"schemaVersion\":1,\"activeProfileId\":\"local-dev\",\"profiles\":[{\"id\":\"local-dev\",\"displayName\":\"Local\",\"gatewayHttpAddr\":\"127.0.0.1:8080\",\"gatewayIcsAddr\":\"127.0.0.1:8444\",\"gatewayGrpcAddr\":\"127.0.0.1:50050\",\"adminHttpAddr\":\"127.0.0.1:8090\"},{\"id\":\"docker-single\",\"displayName\":\"Docker\",\"gatewayHttpAddr\":\"127.0.0.1:8080\",\"gatewayIcsAddr\":\"gateway:50061\",\"gatewayGrpcAddr\":\"127.0.0.1:50050\",\"adminHttpAddr\":\"127.0.0.1:8090\"}]}";
            File.WriteAllText(store.StoreFilePath, legacyJson, Encoding.UTF8);
            var servers = store.LoadServerProfiles();
            Assert.Single(servers);
            Assert.Equal("local-dev", servers[0].Id);
            Assert.Equal(expectedWorkspaceIcs, servers[0].GatewayIcsAddr);

            string caFile = Path.Combine(tempDir, "ca.crt");
            File.WriteAllText(caFile, "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----");
            var psi = new System.Diagnostics.ProcessStartInfo("dotnet");
            ClientLaunchService.ConfigureTlsEnvironment(psi, caFile);
            Assert.Equal(Path.GetFullPath(caFile), psi.Environment["SSL_CERT_FILE"]);
            Assert.Equal(Path.GetFullPath(caFile), psi.Environment["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"]);
        }
        finally { DeleteTempDirWithRetry(tempDir); }
    }

    [Fact]
    public void SandalConfigLoader_WildcardCertDnsResolution_MapsDomainToLocalhostSubdomainAndIgnoresDevLocalhost()
    {
        Assert.Equal("localhost.example.test", SandalConfigLoader.NormalizeCertDnsCandidate("*.example.test"));
        Assert.Null(SandalConfigLoader.NormalizeCertDnsCandidate("*.dev.localhost"));
        Assert.Null(SandalConfigLoader.NormalizeCertDnsCandidate("*.dev.internal"));
        Assert.Null(SandalConfigLoader.NormalizeCertDnsCandidate("host.docker.internal"));
        Assert.Null(SandalConfigLoader.NormalizeCertDnsCandidate("host.containers.internal"));

        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-wildcard-cert-{Guid.NewGuid():N}");
        string certsDir = Path.Combine(tempDir, "dist", "certs");
        Directory.CreateDirectory(certsDir);
        try
        {
            string pemPath = Path.Combine(certsDir, "server.pem");
            WriteSelfSignedWildcardPem(pemPath, "*.example.test");
            Assert.Equal("localhost.example.test", SandalConfigLoader.TryExtractCertDnsHost(pemPath));
            Assert.Equal("localhost.example.test", SandalConfigLoader.ResolveLocalIcsHost(tempDir));
            Assert.Equal("localhost.example.test:50061", SandalConfigLoader.NormalizeIcsEndpoint("127.0.0.1:50061", tempDir));

            string devLocalPem = Path.Combine(certsDir, "devlocal.pem");
            WriteSelfSignedWildcardPem(devLocalPem, "*.dev.localhost");
            Assert.Null(SandalConfigLoader.TryExtractCertDnsHost(devLocalPem));
        }
        finally { DeleteTempDirWithRetry(tempDir); }
    }

    private static void WriteSelfSignedWildcardPem(string pemPath, string wildcardDns)
    {
        using var rsa = System.Security.Cryptography.RSA.Create(2048);
        var req = new System.Security.Cryptography.X509Certificates.CertificateRequest(
            $"CN={wildcardDns}",
            rsa,
            System.Security.Cryptography.HashAlgorithmName.SHA256,
            System.Security.Cryptography.RSASignaturePadding.Pkcs1);
        var san = new System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder();
        san.AddDnsName(wildcardDns);
        req.CertificateExtensions.Add(san.Build());
        using var cert = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(1));
        File.WriteAllText(pemPath, cert.ExportCertificatePem(), Encoding.ASCII);
    }

    [Fact]
    public async Task SteamDiscoveryAndPlayGate_ParsesVdfDiscoversMultiInstallsAndBlocksLaunchOnAuthFailure()
    {
        string vdf = "\"libraryfolders\"\n{\n  \"0\" { \"path\" \"C:\\\\Program Files (x86)\\\\Steam\" }\n  \"1\" { \"path\" \"D:\\\\SteamLibrary\" }\n}";
        var libs = ClientPathDetector.ParseLibraryFoldersVdf(vdf);
        Assert.Equal(2, libs.Count);
        Assert.Equal(@"C:\Program Files (x86)\Steam", libs[0]);
        Assert.Equal(@"D:\SteamLibrary", libs[1]);

        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-28c-steam-{Guid.NewGuid():N}");
        try
        {
            string retailBin = Path.Combine(tempDir, "Ashes of Creation", "Game", "AOC", "Binaries", "Win64");
            string playtestBin = Path.Combine(tempDir, "Ashes of Creation Playtest", "Game", "AOC", "Binaries", "Win64");
            Directory.CreateDirectory(retailBin);
            Directory.CreateDirectory(playtestBin);
            await File.WriteAllBytesAsync(Path.Combine(retailBin, ClientLaunchService.ShippingExeName), "MZ"u8.ToArray());
            await File.WriteAllBytesAsync(Path.Combine(playtestBin, ClientLaunchService.ShippingExeName), "MZ"u8.ToArray());

            var installs = ClientPathDetector.DiscoverSteamInstallations([tempDir]);
            Assert.Equal(2, installs.Count);
            Assert.Contains(installs, i => i.Label == "Ashes of Creation (Retail)");
            Assert.Contains(installs, i => i.Label == "Ashes of Creation Playtest");

            using var failHttp = new HttpClient(new UnauthorizedGatewayHandler()) { BaseAddress = new Uri("http://127.0.0.1:8080") };
            var vm = new MainWindowViewModel(
                SandalConfigLoader.LoadLauncher() with { ClientPakDir = string.Empty },
                new MultiProtocolGatewayClient(failHttp, new Uri("http://127.0.0.1:8080")),
                new InMemoryCredentialVault(),
                new ProfileStoreService(Path.Combine(tempDir, "LauncherProfiles.json")));

            vm.InitializeClientDiscovery([installs[0]]);
            Assert.False(vm.IsClientPathModalOpen);
            Assert.Equal(installs[0].PakDir, vm.EditClientPakDir);

            vm.InitializeClientDiscovery(installs);
            Assert.Equal(2, vm.DiscoveredClientInstalls.Count);

            vm.AuthUsername = "bad_user";
            vm.AuthPassword = "wrong_password";
            await vm.PlayGameAsync();
            Assert.False(vm.IsAuthenticated);
            Assert.False(vm.IsAshesProcessRunning);
            Assert.Equal("Login error: invalid username or password.", vm.StatusBannerMessage);
        }
        finally { DeleteTempDirWithRetry(tempDir); }
    }

    private static void DeleteTempDirWithRetry(string path)
    {
        for (int attempt = 0; attempt < 400; attempt++)
        {
            if (!Directory.Exists(path)) return;
            try
            {
                Directory.Delete(path, recursive: true);
                return;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                if (attempt == 399) return;
                // Wait briefly between retries while background profile-save tasks release their temporary file handles on Windows.
                GC.Collect();
                GC.WaitForPendingFinalizers();
                Thread.Yield();
            }
        }
    }

    private static async Task<byte[]> SendTetherHeaderOnlyAsync(
        UdpClient udp, IPEndPoint ep, LocalTetherListener tether, byte ver, MsgId msgId, uint sessionId, long seq, long nextRecv)
    {
        byte[] pkt = new byte[LocalTetherListener.HeaderSize];
        var hdr = new ArqHeader(LocalTetherListener.BuildMagic(ver, msgId), sessionId, LocalTetherListener.DefaultWindowSize, 0, 0, seq, nextRecv, 1000L);
        LocalTetherListener.WriteHeader(pkt, in hdr);
        Task<bool> serve = tether.ProcessNextDatagramAsync();
        await udp.SendAsync(pkt, ep);
        UdpReceiveResult res = await udp.ReceiveAsync();
        Assert.True(await serve);
        return res.Buffer;
    }

    private static async Task<System.Collections.Generic.Dictionary<string, string>> SendTetherDataAndReadReplyAsync(
        UdpClient udp, IPEndPoint ep, LocalTetherListener tether, uint sessionId, long seq, string messageTypeName)
    {
        byte[] payload = LocalTetherListener.EncodeWireMessage(
            new System.Collections.Generic.Dictionary<string, string> { ["message_type_name"] = messageTypeName },
            "echo-payload");
        byte[] pkt = new byte[LocalTetherListener.HeaderSize + payload.Length];
        var hdr = new ArqHeader(LocalTetherListener.BuildMagic(2, MsgId.Data), sessionId, LocalTetherListener.DefaultWindowSize, 0, (ushort)payload.Length, seq, 0L, 1000L);
        LocalTetherListener.WriteHeader(pkt, in hdr);
        payload.CopyTo(pkt.AsSpan(LocalTetherListener.HeaderSize));
        Task<bool> serve = tether.ProcessNextDatagramAsync();
        await udp.SendAsync(pkt, ep);
        UdpReceiveResult ackRes = await udp.ReceiveAsync();
        UdpReceiveResult dataRes = await udp.ReceiveAsync();
        Assert.True(await serve);
        Assert.True(LocalTetherListener.TryParseHeader(ackRes.Buffer, out ArqHeader ackHdr) && ackHdr.MsgId == MsgId.Ack);
        Assert.True(LocalTetherListener.TryParseHeader(dataRes.Buffer, out ArqHeader dataHdr) && dataHdr.MsgId == MsgId.Data);
        return LocalTetherListener.DecodeWireMessage(dataRes.Buffer.AsSpan(LocalTetherListener.HeaderSize)).Tags;
    }

    private sealed class UnauthorizedGatewayHandler : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
            Task.FromResult(new HttpResponseMessage(HttpStatusCode.Unauthorized)
            {
                Content = new StringContent("{\"error\":\"invalid credentials\"}", Encoding.UTF8, "application/json")
            });
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
                    "{\"access_token\":\"tok_e2e_1\",\"username\":\"TMP HeroNoodleTruck\",\"is_temporary\":true}",
                "/api/v1/characters" when request.Method == HttpMethod.Get => "[]",
                "/api/v1/characters" =>
                    "{\"character_id\":\"char-1\",\"name\":\"TST GruelingBiscuitLauncher\",\"class_name\":\"Fighter\",\"level\":1,\"zone_name\":\"verra-01\"}",
                _ when path.EndsWith("/select", StringComparison.OrdinalIgnoreCase) =>
                    "{\"character_id\":\"char-1\",\"character_name\":\"TST GruelingBiscuitLauncher\",\"join_ticket\":\"jtk_99\",\"world_endpoint\":\"world:7777\",\"world_enter_allowed\":true}",
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
