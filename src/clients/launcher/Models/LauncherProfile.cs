// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Text.Json.Serialization;
using Avalonia.Controls;
using Avalonia.Markup.Xaml;
using SandalCore.Config;
using SandalLauncher.ViewModels;

namespace SandalLauncher.Models
{
    public sealed record LauncherProfile
    {
        [JsonPropertyName("id")]
        public string Id { get; init; } = "local-dev";

        [JsonPropertyName("displayName")]
        public string DisplayName { get; init; } = "Local Dev (Verra Sandbox)";

        [JsonPropertyName("gatewayHttpAddr")]
        public string GatewayHttpAddr { get; init; } = "127.0.0.1:8080";

        [JsonPropertyName("gatewayIcsAddr")]
        public string GatewayIcsAddr { get; init; } = "127.0.0.1:8444";

        [JsonPropertyName("gatewayGrpcAddr")]
        public string GatewayGrpcAddr { get; init; } = "127.0.0.1:50050";

        [JsonPropertyName("adminHttpAddr")]
        public string AdminHttpAddr { get; init; } = "127.0.0.1:8090";

        [JsonPropertyName("clientPakDir")]
        public string ClientPakDir { get; init; } = @"C:\Program Files\Intrepid Studios\Ashes of Creation\AOC\Content\Paks";

        [JsonPropertyName("useEos")]
        public bool UseEos { get; init; }

        [JsonPropertyName("certificatePinSha256")]
        public string CertificatePinSha256 { get; init; } = string.Empty;

        [JsonPropertyName("lastUsedUtc")]
        public DateTimeOffset LastUsedUtc { get; init; } = DateTimeOffset.UtcNow;

        [JsonPropertyName("accountUsername")]
        public string AccountUsername { get; init; } = string.Empty;

        [JsonPropertyName("isTemporaryAccount")]
        public bool IsTemporaryAccount { get; init; }

        [JsonPropertyName("createdAtUtc")]
        public DateTimeOffset? CreatedAtUtc { get; init; }

        public static LauncherProfile FromLauncherConfig(
            LauncherConfig config,
            string id = "local-dev",
            string displayName = "Local Dev (Verra Sandbox)")
        {
            ArgumentNullException.ThrowIfNull(config);
            return new LauncherProfile
            {
                Id = string.IsNullOrWhiteSpace(id) ? "local-dev" : id.Trim(),
                DisplayName = string.IsNullOrWhiteSpace(displayName) ? "Local Dev (Verra Sandbox)" : displayName.Trim(),
                GatewayHttpAddr = config.GatewayHttpAddr,
                GatewayIcsAddr = config.GatewayIcsAddr,
                GatewayGrpcAddr = config.GatewayGrpcAddr,
                AdminHttpAddr = config.AdminHttpAddr,
                ClientPakDir = config.ClientPakDir,
                UseEos = config.ClientUseEos,
                CertificatePinSha256 = string.Empty,
                LastUsedUtc = DateTimeOffset.UtcNow,
                AccountUsername = string.Empty,
                IsTemporaryAccount = false,
                CreatedAtUtc = DateTimeOffset.UtcNow
            };
        }
    }

    public sealed record LauncherCharacterSummary(
        string CharacterId,
        string Name,
        string ClassName,
        int Level,
        string ZoneName);

    public sealed record CharacterSelectTicket(
        string CharacterId,
        string CharacterName,
        string JoinTicket,
        string WorldEndpoint,
        bool WorldEnterAllowed);

    public sealed record AuthSessionResult(
        string AccessToken,
        string Username,
        int ExpiresIn,
        bool IsTemporaryAccount);

    public sealed record LauncherProfileCatalog
    {
        [JsonPropertyName("schemaVersion")]
        public int SchemaVersion { get; init; } = 1;

        [JsonPropertyName("activeProfileId")]
        public string ActiveProfileId { get; init; } = "local-dev";

        [JsonPropertyName("profiles")]
        public LauncherProfile[] Profiles { get; init; } = Array.Empty<LauncherProfile>();
    }
}

namespace SandalLauncher.Views
{
    public partial class ProfileEditorView : UserControl
    {
        public ProfileEditorView()
        {
            AvaloniaXamlLoader.Load(this);
            DataContext ??= new ProfileSwitcherViewModel();
        }
    }
}
