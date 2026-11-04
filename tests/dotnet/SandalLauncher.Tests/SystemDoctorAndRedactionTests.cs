// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.IO.Compression;
using System.IO.Pipes;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using SandalLauncher.Security;
using SandalLauncher.Services;
using Xunit;

namespace SandalLauncher.Tests;

public sealed class SystemDoctorAndRedactionTests
{
    [Fact]
    public void SystemDoctorService_EvaluatesPortsWfpBinaryAndPipeDaclDeterministically()
    {
        DateTimeOffset fixedTime = new(2026, 11, 3, 12, 0, 0, TimeSpan.Zero);
        DoctorReport passReport = SystemDoctorService.RunDiagnostics(new DoctorProbeInput { TimestampUtc = fixedTime });
        Assert.Equal(DoctorCheckSeverity.Pass, passReport.OverallSeverity);
        Assert.Equal("● PASS", passReport.OverallBadge);
        Assert.Equal(8, passReport.Checks.Count);
        Assert.All(passReport.Checks, c => Assert.Equal("● PASS", c.StatusBadge));
        Assert.Contains(passReport.Checks, c => c.CheckId == "port.udp.7777" && c.Title.Contains("worldd"));
        Assert.Contains(passReport.Checks, c => c.CheckId == "port.http.8080" && c.Title.Contains("gateway"));
        Assert.Contains(passReport.Checks, c => c.CheckId == "port.tcp.5432" && c.Title.Contains("PostgreSQL"));
        Assert.Contains(passReport.Checks, c => c.CheckId == "port.tcp.4222" && c.Title.Contains("NATS"));
        Assert.Contains(passReport.Checks, c => c.CheckId == "port.tcp.29000" && c.Title.Contains("Local Tether"));

        DoctorReport warnReport = SystemDoctorService.RunDiagnostics(new DoctorProbeInput
        {
            TetherTcp29000Ready = false,
            WfpLoopbackDropCount = 3,
            ClientBinarySha256Matched = false,
            NamedPipeActive = false
        });
        Assert.Equal(DoctorCheckSeverity.Warning, warnReport.OverallSeverity);
        Assert.Equal("▲ WARN", warnReport.OverallBadge);

        DoctorReport failReport = SystemDoctorService.RunDiagnostics(new DoctorProbeInput
        {
            WorlddUdp7777Ready = false,
            WfpLoopbackHealthy = false,
            ClientBinaryPresent = false,
            NamedPipeCurrentUserOnlyDacl = false
        });
        Assert.Equal(DoctorCheckSeverity.Fail, failReport.OverallSeverity);
        Assert.Equal("■ FAIL", failReport.OverallBadge);
    }

    [Fact]
    public async Task DiagnosticBundleExporter_ScrubsSecretsGuardsZipSlipAndBoundsEntries()
    {
        string rawLog = string.Join('\n',
            "[INFO] gateway ready on 127.0.0.1:8080",
            "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxIn0.sig_val",
            "{\"ics_token\":\"ics_live_998877\",\"session_ticket\":\"ticket_xyz\",\"password\":\"hunter2\",\"secret\":\"s3cr3t\",\"api_key\":\"k_live_1\"}",
            "hex_key=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            "b64=dGhpcyBpcyBhIHNlY3JldCBiYXNlNjQgdG9rZW4h==",
            @"client_path=C:\Users\AliceVerra\AppData\Local\AOC\Saved\Logs\AOC.log");

        string scrubbed = DiagnosticBundleExporter.RedactSensitiveText(rawLog);
        Assert.Contains("[INFO] gateway ready on 127.0.0.1:8080", scrubbed);
        Assert.Contains("Bearer [REDACTED]", scrubbed);
        Assert.Contains(@"C:\Users\[REDACTED_USER]\AppData\Local\AOC", scrubbed);
        foreach (string leaked in new[] { "ics_live_998877", "ticket_xyz", "hunter2", "s3cr3t", "k_live_1", "AliceVerra", "0123456789abcdef0123456789abcdef", "dGhpcyBpcyBhIHNlY3JldCBiYXNlNjQgdG9rZW4h==" })
            Assert.DoesNotContain(leaked, scrubbed);

        foreach (string unsafeEntry in new[] { "../evil.json", @"..\..\Windows\win.ini", "/etc/passwd", @"C:\rooted.log", @"\rooted.txt", " " })
            Assert.Throws<InvalidDataException>(() => DiagnosticBundleExporter.SanitizeZipEntryName(unsafeEntry));

        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-bundle-{Guid.NewGuid():N}");
        try
        {
            string zipPath = Path.Combine(tempDir, "diag.zip");
            DoctorReport report = SystemDoctorService.RunDiagnostics();
            await DiagnosticBundleExporter.ExportBundleAsync(zipPath, report, "{\"password\":\"topsecret\"}", rawLog);
            using ZipArchive zip = ZipFile.OpenRead(zipPath);
            Assert.Equal(3, zip.Entries.Count);
            foreach (string name in new[] { "doctor_report.json", "redacted_config.json", "redacted_tail.log" })
            {
                ZipArchiveEntry? entry = zip.GetEntry(name);
                Assert.NotNull(entry);
                Assert.True(entry!.Length <= DiagnosticBundleExporter.MaxEntryBytes);
                using var reader = new StreamReader(entry.Open(), Encoding.UTF8);
                string text = await reader.ReadToEndAsync();
                Assert.DoesNotContain("topsecret", text);
                Assert.DoesNotContain("AliceVerra", text);
            }
            using var ms = new MemoryStream();
            using var writeZip = new ZipArchive(ms, ZipArchiveMode.Create, leaveOpen: true);
            string oversize = new('A', DiagnosticBundleExporter.MaxEntryBytes + 64);
            Assert.Throws<InvalidDataException>(() => DiagnosticBundleExporter.WriteRedactedEntry(writeZip, "huge.log", oversize));
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public async Task CredentialVaultAndTokenPipeServer_SealTokensAndServeOverNamedPipe()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-vault-{Guid.NewGuid():N}");
        try
        {
            using var memVault = new InMemoryCredentialVault();
            Assert.True(await memVault.StoreTokenAsync("local-dev", "tok_alpha_123"));
            Assert.Equal("tok_alpha_123", await memVault.RetrieveTokenAsync("local-dev"));
            Assert.True(await memVault.DeleteTokenAsync("local-dev"));
            Assert.Null(await memVault.RetrieveTokenAsync("local-dev"));

            var blobVault = new LibSecretCredentialVault(tempDir);
            Assert.True(await blobVault.StoreTokenAsync("docker-single", "sealed_aes_gcm_token"));
            Assert.Equal("sealed_aes_gcm_token", await blobVault.RetrieveTokenAsync("docker-single"));
            Assert.True(await blobVault.DeleteTokenAsync("docker-single"));

            string tokenFile = Path.Combine(tempDir, "ics_token.txt");
            Assert.True(TokenPipeServer.WriteOwnerOnlyTokenFile(tokenFile, "ascii_pipe_token_77"));
            Assert.Equal("ascii_pipe_token_77", TokenPipeServer.ReadOwnerOnlyTokenFile(tokenFile));
            Assert.False(TokenPipeServer.IsValidPrintableAsciiToken("bad token with spaces"));

            string received;
            await using (var pipeServer = new TokenPipeServer())
            {
                Assert.StartsWith("SandalAuth_", pipeServer.PipeName);
                Task<bool> serveTask = pipeServer.ServeTokenOnceAsync("ephemeral_pipe_secret");
                await using var pipeClient = new NamedPipeClientStream(".", pipeServer.PipeName, PipeDirection.In, PipeOptions.Asynchronous);
                await pipeClient.ConnectAsync(2000);
                byte[] buf = new byte[64];
                int read = await pipeClient.ReadAsync(buf);
                Assert.True(await serveTask);
                received = Encoding.UTF8.GetString(buf, 0, read);
            }
            Assert.Equal("ephemeral_pipe_secret", received);
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }

    [Fact]
    public async Task UnlatchBinaryHashAndGatewayClient_EnforceSafetyInvariants()
    {
        string tempDir = Path.Combine(Path.GetTempPath(), $"sandal-svc-{Guid.NewGuid():N}");
        Directory.CreateDirectory(tempDir);
        try
        {
            string savPath = Path.Combine(tempDir, "GeneralSettings.sav");
            Assert.Equal(GeneralSettingsUnlatchAction.UntouchedMissing, GeneralSettingsUnlatch.EnsureUnlatched(savPath));
            await File.WriteAllBytesAsync(savPath, [1, 2, 3, 4]);
            Assert.Equal(GeneralSettingsUnlatchAction.SnapshotCreated, GeneralSettingsUnlatch.EnsureUnlatched(savPath));
            await File.WriteAllBytesAsync(savPath, []);
            Assert.Equal(GeneralSettingsUnlatchAction.RestoredFromBackup, GeneralSettingsUnlatch.EnsureUnlatched(savPath));
            File.Delete($"{savPath}.bak");
            await File.WriteAllBytesAsync(savPath, []);
            Assert.Equal(GeneralSettingsUnlatchAction.DeletedZeroByte, GeneralSettingsUnlatch.EnsureUnlatched(savPath));

            string binPath = Path.Combine(tempDir, "AOCClient-Win64-Shipping.exe");
            byte[] binBytes = Encoding.ASCII.GetBytes("UE5-Shipping-Stub");
            await File.WriteAllBytesAsync(binPath, binBytes);
            string expectedHex = Convert.ToHexString(SHA256.HashData(binBytes)).ToLowerInvariant();
            Assert.Equal(expectedHex, BinaryHashVerifier.ComputeFileSha256Hex(binPath));
            Assert.True((await BinaryHashVerifier.VerifyFileHashAsync(binPath, expectedHex)).IsMatch);
            Assert.False((await BinaryHashVerifier.VerifyFileHashAsync(binPath, new string('0', 64))).IsMatch);

            GatewayProbeResult okProbe = MultiProtocolGatewayClient.ParseHealthPayload("{\"status\":200,\"onlinePlayers\":42,\"maxPlayers\":500}", 14, "HttpJson");
            Assert.True(okProbe.IsReachable);
            Assert.Equal(42, okProbe.OnlinePlayers);
            GatewayProbeResult errProbe = MultiProtocolGatewayClient.ParseHealthPayload("{\"status\":503,\"title\":\"Offline\",\"detail\":\"Draining\"}", 9, "GrpcWeb", 503);
            Assert.False(errProbe.IsReachable);
            Assert.Contains("RFC 9457 503", errProbe.ErrorMessage);
        }
        finally { if (Directory.Exists(tempDir)) Directory.Delete(tempDir, recursive: true); }
    }
}
