// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using SandalCore.Config;
using SandalLauncher.Models;
using SandalLauncher.Security;

namespace SandalLauncher.Services;

public sealed record SavedAccountRecord(
    string Username,
    string Password,
    bool IsTemporary,
    string ServerUrl)
{
    public string DisplayLabel => IsTemporary
        ? $"{Username}  [Temp · {ServerUrl}]"
        : $"{Username}  [{ServerUrl}]";
}

public sealed class ProfileStoreService
{
    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true
    };

    private static readonly HashSet<string> ForbiddenSecretPropertyNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "password", "passwd", "pwd", "token", "access_token", "accesstoken",
        "session_token", "sessiontoken", "refresh_token", "refreshtoken",
        "secret", "client_secret", "clientsecret", "api_key", "apikey",
        "private_key", "privatekey", "bearer"
    };

    private static readonly string[] ForbiddenSecretValueMarkers =
    [
        "password=", "passwd=", "token=", "access_token=",
        "session_token=", "refresh_token=", "secret=", "bearer ", "-----BEGIN "
    ];

    private readonly Func<LauncherConfig> _configProvider;

    public ProfileStoreService(string? storeFilePath = null, Func<LauncherConfig>? configProvider = null)
    {
        StoreFilePath = string.IsNullOrWhiteSpace(storeFilePath) ? GetDefaultStoreFilePath() : Path.GetFullPath(storeFilePath);
        _configProvider = configProvider ?? (() => SandalConfigLoader.LoadLauncher());
    }

    public string StoreFilePath { get; }

    public static string GetDefaultStoreFilePath()
    {
        string baseDir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(baseDir)) baseDir = AppContext.BaseDirectory;
        return Path.Combine(baseDir, "SandalCore", "LauncherProfiles.json");
    }

    public LauncherProfile CreateDefaultProfile() => LauncherProfile.FromLauncherConfig(_configProvider());

    public IReadOnlyList<LauncherProfile> CreateDefaultProfiles()
    {
        LauncherConfig cfg = _configProvider();
        return
        [
            LauncherProfile.FromLauncherConfig(cfg, "local-dev", "Local Dev (Verra Sandbox)"),
            new LauncherProfile
            {
                Id = "docker-single",
                DisplayName = "Docker Single-Node Realm",
                GatewayHttpAddr = "127.0.0.1:8080",
                GatewayIcsAddr = "127.0.0.1:8444",
                GatewayGrpcAddr = "127.0.0.1:50050",
                AdminHttpAddr = "127.0.0.1:8090",
                ClientPakDir = cfg.ClientPakDir,
                UseEos = false,
                LastUsedUtc = DateTimeOffset.UtcNow
            }
        ];
    }

    public LauncherProfileCatalog LoadCatalog()
    {
        if (!File.Exists(StoreFilePath))
        {
            IReadOnlyList<LauncherProfile> defaults = CreateDefaultProfiles();
            var seeded = new LauncherProfileCatalog { SchemaVersion = 1, ActiveProfileId = defaults[0].Id, Profiles = defaults.ToArray() };
            SaveCatalog(seeded);
            return seeded;
        }
        return ParseAndValidateCatalog(File.ReadAllText(StoreFilePath, Encoding.UTF8));
    }

    public async Task<LauncherProfileCatalog> LoadCatalogAsync(CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!File.Exists(StoreFilePath))
        {
            IReadOnlyList<LauncherProfile> defaults = CreateDefaultProfiles();
            var seeded = new LauncherProfileCatalog { SchemaVersion = 1, ActiveProfileId = defaults[0].Id, Profiles = defaults.ToArray() };
            await SaveCatalogAsync(seeded, cancellationToken).ConfigureAwait(false);
            return seeded;
        }
        string rawJson = await File.ReadAllTextAsync(StoreFilePath, Encoding.UTF8, cancellationToken).ConfigureAwait(false);
        return ParseAndValidateCatalog(rawJson);
    }

    public IReadOnlyList<LauncherProfile> LoadProfiles() => LoadCatalog().Profiles;

    public async Task<IReadOnlyList<LauncherProfile>> LoadProfilesAsync(CancellationToken cancellationToken = default) =>
        (await LoadCatalogAsync(cancellationToken).ConfigureAwait(false)).Profiles;

    public async Task<IReadOnlyList<LauncherProfile>> GetTemporaryProfilesAsync(CancellationToken ct = default)
    {
        IReadOnlyList<LauncherProfile> profiles = await LoadProfilesAsync(ct).ConfigureAwait(false);
        return profiles.Where(p => p.IsTemporaryAccount).ToArray();
    }

    public static string GetSavedAccountVaultKey(string username) =>
        $"saved-acct-{(username ?? string.Empty).Trim().ToLowerInvariant()}";

    public async Task SaveAccountCredentialsAsync(
        ICredentialVault vault,
        string username,
        string password,
        bool isTemporary,
        LauncherConfig config,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(vault);
        ArgumentNullException.ThrowIfNull(config);
        if (string.IsNullOrWhiteSpace(username)) return;
        string cleanUser = username.Trim();
        if (!string.IsNullOrEmpty(password))
            _ = await vault.StoreTokenAsync(GetSavedAccountVaultKey(cleanUser), password, ct).ConfigureAwait(false);
        string profileId = isTemporary ? $"temp-{cleanUser}" : $"acct-{cleanUser}";
        string display = isTemporary ? $"Temp ({cleanUser})" : $"Account ({cleanUser})";
        LauncherProfile entry = LauncherProfile.FromLauncherConfig(config, profileId, display) with
        {
            AccountUsername = cleanUser,
            IsTemporaryAccount = isTemporary,
            CreatedAtUtc = DateTimeOffset.UtcNow
        };
        await UpsertProfileAsync(entry, ct).ConfigureAwait(false);
    }

    public async Task<IReadOnlyList<SavedAccountRecord>> LoadSavedAccountsAsync(
        ICredentialVault vault,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(vault);
        IReadOnlyList<LauncherProfile> profiles = await LoadProfilesAsync(ct).ConfigureAwait(false);
        var list = new List<SavedAccountRecord>();
        foreach (LauncherProfile p in profiles)
        {
            if (string.IsNullOrWhiteSpace(p.AccountUsername)) continue;
            string? secret = await vault.RetrieveTokenAsync(GetSavedAccountVaultKey(p.AccountUsername), ct).ConfigureAwait(false);
            if (string.IsNullOrEmpty(secret) && p.IsTemporaryAccount)
            {
                string? rawTemp = await vault.RetrieveTokenAsync($"temp-{p.AccountUsername}", ct).ConfigureAwait(false);
                if (!string.IsNullOrEmpty(rawTemp) && rawTemp.IndexOf('\n', StringComparison.Ordinal) is int nl and >= 0)
                    secret = rawTemp[(nl + 1)..];
            }
            list.Add(new SavedAccountRecord(p.AccountUsername, secret ?? string.Empty, p.IsTemporaryAccount, p.GatewayHttpAddr));
        }
        return list;
    }

    public async Task ForgetSavedAccountAsync(
        ICredentialVault vault,
        string username,
        CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(vault);
        if (string.IsNullOrWhiteSpace(username)) return;
        string cleanUser = username.Trim();
        _ = await vault.DeleteTokenAsync(GetSavedAccountVaultKey(cleanUser), ct).ConfigureAwait(false);
        _ = await vault.DeleteTokenAsync($"temp-{cleanUser}", ct).ConfigureAwait(false);
        _ = await DeleteProfileAsync($"acct-{cleanUser}", ct).ConfigureAwait(false);
        _ = await DeleteProfileAsync($"temp-{cleanUser}", ct).ConfigureAwait(false);
    }

    public async Task UpsertProfileAsync(LauncherProfile profile, CancellationToken ct = default)
    {
        ValidateProfile(profile);
        LauncherProfileCatalog catalog = await LoadCatalogAsync(ct).ConfigureAwait(false);
        var list = new List<LauncherProfile>(catalog.Profiles.Length + 1);
        bool replaced = false;
        foreach (LauncherProfile existing in catalog.Profiles)
        {
            if (string.Equals(existing.Id, profile.Id, StringComparison.OrdinalIgnoreCase))
            {
                list.Add(profile);
                replaced = true;
            }
            else
            {
                list.Add(existing);
            }
        }
        if (!replaced) list.Add(profile);
        await SaveProfilesAsync(list, profile.Id, ct).ConfigureAwait(false);
    }

    public async Task<bool> DeleteProfileAsync(string profileId, CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(profileId))
            throw new ArgumentException("Profile identifier cannot be empty.", nameof(profileId));
        string targetId = profileId.Trim();
        LauncherProfileCatalog catalog = await LoadCatalogAsync(ct).ConfigureAwait(false);
        LauncherProfile[] remaining = catalog.Profiles
            .Where(p => !string.Equals(p.Id, targetId, StringComparison.OrdinalIgnoreCase))
            .ToArray();
        if (remaining.Length == catalog.Profiles.Length)
            return false;
        string nextActive = string.Equals(catalog.ActiveProfileId, targetId, StringComparison.OrdinalIgnoreCase)
            ? (remaining.Length > 0 ? remaining[0].Id : "local-dev")
            : catalog.ActiveProfileId;
        await SaveProfilesAsync(remaining, nextActive, ct).ConfigureAwait(false);
        return true;
    }

    public void SaveProfiles(IReadOnlyList<LauncherProfile> profiles, string? activeProfileId = null)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        string active = !string.IsNullOrWhiteSpace(activeProfileId) ? activeProfileId : (profiles.Count > 0 ? profiles[0].Id : "local-dev");
        SaveCatalog(new LauncherProfileCatalog { SchemaVersion = 1, ActiveProfileId = active, Profiles = profiles.ToArray() });
    }

    public Task SaveProfilesAsync(IReadOnlyList<LauncherProfile> profiles, string? activeProfileId = null, CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        string active = !string.IsNullOrWhiteSpace(activeProfileId) ? activeProfileId : (profiles.Count > 0 ? profiles[0].Id : "local-dev");
        return SaveCatalogAsync(new LauncherProfileCatalog { SchemaVersion = 1, ActiveProfileId = active, Profiles = profiles.ToArray() }, cancellationToken);
    }

    public void SaveCatalog(LauncherProfileCatalog catalog)
    {
        string payload = SerializeValidatedCatalog(catalog);
        if (Path.GetDirectoryName(StoreFilePath) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        string tempPath = $"{StoreFilePath}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            using (var stream = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
            using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
            {
                writer.Write(payload);
                writer.Flush();
                stream.Flush(flushToDisk: true);
            }
            File.Move(tempPath, StoreFilePath, overwrite: true);
        }
        finally
        {
            TryDeleteTempFile(tempPath);
        }
    }

    public async Task SaveCatalogAsync(LauncherProfileCatalog catalog, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string payload = SerializeValidatedCatalog(catalog);
        if (Path.GetDirectoryName(StoreFilePath) is { Length: > 0 } dir) Directory.CreateDirectory(dir);
        string tempPath = $"{StoreFilePath}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            byte[] utf8Bytes = Encoding.UTF8.GetBytes(payload);
            using (var stream = new FileStream(tempPath, FileMode.CreateNew, FileAccess.Write, FileShare.None, 4096, FileOptions.Asynchronous | FileOptions.WriteThrough))
            {
                await stream.WriteAsync(utf8Bytes, cancellationToken).ConfigureAwait(false);
                await stream.FlushAsync(cancellationToken).ConfigureAwait(false);
                stream.Flush(flushToDisk: true);
            }
            File.Move(tempPath, StoreFilePath, overwrite: true);
        }
        finally
        {
            TryDeleteTempFile(tempPath);
        }
    }

    public static LauncherProfileCatalog ParseAndValidateCatalog(string rawJson)
    {
        if (string.IsNullOrWhiteSpace(rawJson)) throw new InvalidDataException("LauncherProfiles.json payload is empty.");
        using JsonDocument doc = JsonDocument.Parse(rawJson);
        InspectElementForForbiddenSecrets(doc.RootElement);
        LauncherProfileCatalog? catalog = doc.RootElement.ValueKind == JsonValueKind.Array
            ? new LauncherProfileCatalog { SchemaVersion = 1, Profiles = JsonSerializer.Deserialize<LauncherProfile[]>(rawJson, SerializerOptions) ?? [] }
            : JsonSerializer.Deserialize<LauncherProfileCatalog>(rawJson, SerializerOptions);
        if (catalog?.Profiles is null) throw new InvalidDataException("LauncherProfiles.json could not be deserialized.");
        foreach (LauncherProfile profile in catalog.Profiles) ValidateProfile(profile);
        return catalog;
    }

    public static void ValidateProfile(LauncherProfile profile)
    {
        ArgumentNullException.ThrowIfNull(profile);
        if (string.IsNullOrWhiteSpace(profile.Id) || string.IsNullOrWhiteSpace(profile.DisplayName))
            throw new InvalidDataException("Profile Id and DisplayName are required.");
        if (string.IsNullOrWhiteSpace(profile.GatewayHttpAddr) || string.IsNullOrWhiteSpace(profile.GatewayIcsAddr) ||
            string.IsNullOrWhiteSpace(profile.GatewayGrpcAddr) || string.IsNullOrWhiteSpace(profile.AdminHttpAddr))
            throw new InvalidDataException($"Profile '{profile.Id}' is missing required gateway or admin endpoint addresses.");
        foreach ((string val, string name) in new[]
        {
            (profile.Id, nameof(profile.Id)), (profile.DisplayName, nameof(profile.DisplayName)),
            (profile.GatewayHttpAddr, nameof(profile.GatewayHttpAddr)), (profile.GatewayIcsAddr, nameof(profile.GatewayIcsAddr)),
            (profile.GatewayGrpcAddr, nameof(profile.GatewayGrpcAddr)), (profile.AdminHttpAddr, nameof(profile.AdminHttpAddr)),
            (profile.ClientPakDir, nameof(profile.ClientPakDir)), (profile.CertificatePinSha256, nameof(profile.CertificatePinSha256)),
            (profile.AccountUsername, nameof(profile.AccountUsername))
        })
        {
            ValidateFieldContainsNoSecret(val, name);
        }
    }

    private static string SerializeValidatedCatalog(LauncherProfileCatalog catalog)
    {
        ArgumentNullException.ThrowIfNull(catalog);
        ArgumentNullException.ThrowIfNull(catalog.Profiles);
        foreach (LauncherProfile profile in catalog.Profiles) ValidateProfile(profile);
        string json = JsonSerializer.Serialize(catalog, SerializerOptions);
        using JsonDocument doc = JsonDocument.Parse(json);
        InspectElementForForbiddenSecrets(doc.RootElement);
        return json;
    }

    private static void InspectElementForForbiddenSecrets(JsonElement element)
    {
        if (element.ValueKind == JsonValueKind.Object)
        {
            foreach (JsonProperty prop in element.EnumerateObject())
            {
                if (ForbiddenSecretPropertyNames.Contains(prop.Name))
                    throw new InvalidDataException($"Forbidden secret field '{prop.Name}' rejected in LauncherProfiles.json (L2 zero-plaintext policy).");
                InspectElementForForbiddenSecrets(prop.Value);
            }
        }
        else if (element.ValueKind == JsonValueKind.Array)
        {
            foreach (JsonElement item in element.EnumerateArray()) InspectElementForForbiddenSecrets(item);
        }
        else if (element.ValueKind == JsonValueKind.String)
        {
            ValidateFieldContainsNoSecret(element.GetString() ?? string.Empty, "jsonString");
        }
    }

    private static void ValidateFieldContainsNoSecret(string value, string fieldName)
    {
        if (string.IsNullOrEmpty(value)) return;
        foreach (string marker in ForbiddenSecretValueMarkers)
        {
            if (value.Contains(marker, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Profile field '{fieldName}' contains forbidden credential or token marker '{marker}'.");
        }
        if (value.StartsWith("eyJ", StringComparison.Ordinal) && value.Count(c => c == '.') >= 2)
            throw new InvalidDataException($"Profile field '{fieldName}' appears to contain a raw JWT bearer token.");
    }

    private static void TryDeleteTempFile(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch (IOException) { }
    }
}
