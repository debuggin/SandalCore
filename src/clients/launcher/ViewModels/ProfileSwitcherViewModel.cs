// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using SandalLauncher.Models;
using SandalLauncher.Security;
using SandalLauncher.Services;

namespace SandalLauncher.ViewModels;

public sealed partial class ProfileSwitcherViewModel : ObservableObject
{
    private readonly ProfileStoreService _storeService;
    private readonly ICredentialVault _credentialVault;
    [ObservableProperty] private LauncherProfile? _selectedProfile;
    [ObservableProperty] private string _activeProfileId = "local-dev";
    [ObservableProperty] private string _editId = "local-dev";
    [ObservableProperty] private string _editDisplayName = "Local Dev (Verra Sandbox)";
    [ObservableProperty] private string _editGatewayHttpAddr = "127.0.0.1:8080";
    [ObservableProperty] private string _editGatewayIcsAddr = "127.0.0.1:8444";
    [ObservableProperty] private string _editGatewayGrpcAddr = "127.0.0.1:50050";
    [ObservableProperty] private string _editAdminHttpAddr = "127.0.0.1:8090";
    [ObservableProperty] private string _editClientPakDir = string.Empty;
    [ObservableProperty] private bool _editUseEos;
    [ObservableProperty] private string _editCertificatePinSha256 = string.Empty;
    [ObservableProperty] private string _editAccountUsername = string.Empty;
    [ObservableProperty] private string _sessionTokenInput = string.Empty;
    [ObservableProperty] private bool _hasVaultToken;
    [ObservableProperty] private string _vaultStatusText = "OS Vault: No active session token cached for profile.";
    [ObservableProperty] private bool _isDeleteConfirmationArmed;
    [ObservableProperty] private string _currentViewState = "populated";
    [ObservableProperty] private bool _isLoadingState;
    [ObservableProperty] private bool _isEmptyState;
    [ObservableProperty] private bool _isErrorState;
    [ObservableProperty] private bool _isDegradedState;
    [ObservableProperty] private bool _isPopulatedState = true;
    [ObservableProperty] private string _errorTitle = string.Empty;
    [ObservableProperty] private string _errorDetail = string.Empty;
    [ObservableProperty] private string _errorRemediation = string.Empty;
    [ObservableProperty] private string _degradedSummary = string.Empty;
    [ObservableProperty] private string _statusBannerMessage = "Profile catalog loaded from LauncherProfiles.json (0 plaintext secrets on disk).";

    public ProfileSwitcherViewModel() : this(new ProfileStoreService(), CredentialVaultFactory.CreateDefault()) { }

    public ProfileSwitcherViewModel(ProfileStoreService storeService, ICredentialVault credentialVault)
    {
        ArgumentNullException.ThrowIfNull(storeService);
        ArgumentNullException.ThrowIfNull(credentialVault);
        _storeService = storeService;
        _credentialVault = credentialVault;
        Profiles = [];
        InitializeSynchronously();
    }

    public ObservableCollection<LauncherProfile> Profiles { get; }

    partial void OnSelectedProfileChanged(LauncherProfile? value)
    {
        IsDeleteConfirmationArmed = false;
        if (value is null) return;
        ActiveProfileId = value.Id; EditId = value.Id; EditDisplayName = value.DisplayName;
        EditGatewayHttpAddr = value.GatewayHttpAddr; EditGatewayIcsAddr = value.GatewayIcsAddr;
        EditGatewayGrpcAddr = value.GatewayGrpcAddr; EditAdminHttpAddr = value.AdminHttpAddr;
        EditClientPakDir = value.ClientPakDir; EditUseEos = value.UseEos;
        EditCertificatePinSha256 = value.CertificatePinSha256; EditAccountUsername = value.AccountUsername;
        SessionTokenInput = string.Empty;
    }

    [RelayCommand]
    public async Task LoadProfilesAsync(CancellationToken cancellationToken = default)
    {
        ApplyViewState("loading");
        try
        {
            PopulateFromCatalog(await _storeService.LoadCatalogAsync(cancellationToken).ConfigureAwait(false));
            if (SelectedProfile is not null) await RefreshVaultStateAsync(SelectedProfile.Id, cancellationToken).ConfigureAwait(false);
        }
        catch (Exception ex) when (ex is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            SetValidationError("■ PROFILE STORE VALIDATION ERROR (RFC 9457)", $"What: Failed to load LauncherProfiles.json ({ex.Message}). Why: Corrupted JSON or forbidden secret key detected.", "How to fix: Click Reset Default Profiles or remove forbidden secret fields from LauncherProfiles.json.");
        }
    }

    [RelayCommand]
    public void PrepareNewProfile()
    {
        int n = Profiles.Count + 1;
        EditId = $"realm-{n}"; EditDisplayName = $"Custom Verra Realm #{n}";
        EditGatewayHttpAddr = "127.0.0.1:8080"; EditGatewayIcsAddr = "127.0.0.1:8444";
        EditGatewayGrpcAddr = "127.0.0.1:50050"; EditAdminHttpAddr = "127.0.0.1:8090";
        EditUseEos = false; EditCertificatePinSha256 = string.Empty; EditAccountUsername = string.Empty;
        SessionTokenInput = string.Empty; IsDeleteConfirmationArmed = false;
        ApplyViewState("populated");
        StatusBannerMessage = $"Drafting new profile '{EditId}'. Click Save Profile to persist.";
    }

    [RelayCommand]
    public async Task SaveProfileAsync(CancellationToken cancellationToken = default)
    {
        try
        {
            var candidate = new LauncherProfile
            {
                Id = EditId.Trim(), DisplayName = EditDisplayName.Trim(),
                GatewayHttpAddr = EditGatewayHttpAddr.Trim(), GatewayIcsAddr = EditGatewayIcsAddr.Trim(),
                GatewayGrpcAddr = EditGatewayGrpcAddr.Trim(), AdminHttpAddr = EditAdminHttpAddr.Trim(),
                ClientPakDir = EditClientPakDir.Trim(), UseEos = EditUseEos,
                CertificatePinSha256 = EditCertificatePinSha256.Trim(), LastUsedUtc = DateTimeOffset.UtcNow,
                AccountUsername = EditAccountUsername.Trim()
            };
            ProfileStoreService.ValidateProfile(candidate);
            UpsertProfileInMemory(candidate);
            await _storeService.SaveProfilesAsync(Profiles.ToArray(), candidate.Id, cancellationToken).ConfigureAwait(false);
            if (!string.IsNullOrWhiteSpace(SessionTokenInput))
            {
                string tokenToStore = SessionTokenInput; SessionTokenInput = string.Empty;
                await _credentialVault.StoreTokenAsync(candidate.Id, tokenToStore, cancellationToken).ConfigureAwait(false);
            }
            await RefreshVaultStateAsync(candidate.Id, cancellationToken).ConfigureAwait(false);
            ApplyViewState("populated");
            StatusBannerMessage = $"Saved profile '{candidate.DisplayName}' ({candidate.Id}) atomically.";
        }
        catch (InvalidDataException ex)
        {
            SetValidationError("■ PROFILE VALIDATION REJECTED (ISO 9241-143 / L2)", $"What: {ex.Message} Why: Profile fields must be non-empty and free of embedded tokens or passwords.", "How to fix: Correct the highlighted profile fields and store tokens exclusively in the OS Vault field.");
        }
    }

    [RelayCommand]
    public void ArmDeleteProfile()
    {
        if (SelectedProfile is null) return;
        IsDeleteConfirmationArmed = true;
        StatusBannerMessage = $"Confirm deletion of profile '{SelectedProfile.DisplayName}' ({SelectedProfile.Id}).";
    }

    [RelayCommand] public void CancelDeleteProfile() { IsDeleteConfirmationArmed = false; StatusBannerMessage = "Profile deletion cancelled."; }

    [RelayCommand]
    public async Task DeleteSelectedProfileAsync(CancellationToken cancellationToken = default)
    {
        if (SelectedProfile is null) return;
        string deletedId = SelectedProfile.Id;
        Profiles.Remove(SelectedProfile);
        IsDeleteConfirmationArmed = false;
        await _credentialVault.DeleteTokenAsync(deletedId, cancellationToken).ConfigureAwait(false);
        if (Profiles.Count == 0)
        {
            SelectedProfile = null;
            await _storeService.SaveProfilesAsync([], "local-dev", cancellationToken).ConfigureAwait(false);
            ApplyViewState("empty");
            StatusBannerMessage = $"Deleted profile '{deletedId}'. No profiles remain.";
            return;
        }
        SelectedProfile = Profiles[0];
        await _storeService.SaveProfilesAsync(Profiles.ToArray(), SelectedProfile.Id, cancellationToken).ConfigureAwait(false);
        await RefreshVaultStateAsync(SelectedProfile.Id, cancellationToken).ConfigureAwait(false);
        ApplyViewState("populated");
        StatusBannerMessage = $"Deleted profile '{deletedId}' and purged its OS vault token.";
    }

    [RelayCommand]
    public async Task StoreSessionTokenAsync(CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(EditId) || string.IsNullOrWhiteSpace(SessionTokenInput))
        {
            SetValidationError("■ SESSION TOKEN MISSING", "What: Cannot store an empty session token.", "How to fix: Enter a valid session token before clicking Seal Token.");
            return;
        }
        string token = SessionTokenInput; SessionTokenInput = string.Empty;
        await _credentialVault.StoreTokenAsync(EditId.Trim(), token, cancellationToken).ConfigureAwait(false);
        await RefreshVaultStateAsync(EditId.Trim(), cancellationToken).ConfigureAwait(false);
        ApplyViewState("populated");
        StatusBannerMessage = $"Session token sealed in OS Credential Vault for profile '{EditId.Trim()}'.";
    }

    [RelayCommand]
    public async Task<string?> RetrieveSessionTokenAsync(CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(EditId)) return null;
        string? token = await _credentialVault.RetrieveTokenAsync(EditId.Trim(), cancellationToken).ConfigureAwait(false);
        HasVaultToken = !string.IsNullOrEmpty(token);
        VaultStatusText = HasVaultToken ? $"OS Vault: Active token verified for '{EditId.Trim()}' (redacted)." : $"OS Vault: No active session token cached for '{EditId.Trim()}'.";
        return token;
    }

    [RelayCommand]
    public async Task ClearSessionTokenAsync(CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(EditId)) return;
        await _credentialVault.DeleteTokenAsync(EditId.Trim(), cancellationToken).ConfigureAwait(false);
        await RefreshVaultStateAsync(EditId.Trim(), cancellationToken).ConfigureAwait(false);
        StatusBannerMessage = $"Cleared OS Credential Vault token for profile '{EditId.Trim()}'.";
    }

    [RelayCommand]
    public async Task ResetDefaultProfilesAsync(CancellationToken cancellationToken = default)
    {
        IReadOnlyList<LauncherProfile> defaults = _storeService.CreateDefaultProfiles();
        await _storeService.SaveProfilesAsync(defaults, defaults[0].Id, cancellationToken).ConfigureAwait(false);
        PopulateFromCatalog(new LauncherProfileCatalog { SchemaVersion = 1, ActiveProfileId = defaults[0].Id, Profiles = defaults.ToArray() });
        StatusBannerMessage = "Default Verra realm profiles restored.";
    }

    [RelayCommand] private void SetLoadingState() => ApplyViewState("loading");
    [RelayCommand] private void SetEmptyState() => ApplyViewState("empty");
    [RelayCommand] private void SetErrorState() => ApplyViewState("error");
    [RelayCommand] private void SetDegradedState() { DegradedSummary = "OS Credential Vault fallback active (encrypted user-scoped AES-256-GCM blob store)."; ApplyViewState("degraded"); }
    [RelayCommand] private void SetPopulatedState() => ApplyViewState("populated");

    private void InitializeSynchronously()
    {
        try { PopulateFromCatalog(_storeService.LoadCatalog()); }
        catch (Exception ex) when (ex is InvalidDataException or IOException or UnauthorizedAccessException)
        { SetValidationError("■ PROFILE STORE VALIDATION ERROR (RFC 9457)", $"What: {ex.Message}", "How to fix: Reset to default profiles."); }
    }

    private void UpsertProfileInMemory(LauncherProfile candidate)
    {
        for (int i = 0; i < Profiles.Count; i++)
            if (string.Equals(Profiles[i].Id, candidate.Id, StringComparison.OrdinalIgnoreCase))
            { Profiles[i] = candidate; SelectedProfile = candidate; ActiveProfileId = candidate.Id; return; }
        Profiles.Add(candidate); SelectedProfile = candidate; ActiveProfileId = candidate.Id;
    }

    private void PopulateFromCatalog(LauncherProfileCatalog catalog)
    {
        Profiles.Clear();
        foreach (LauncherProfile p in catalog.Profiles) Profiles.Add(p);
        if (Profiles.Count == 0) { SelectedProfile = null; ApplyViewState("empty"); return; }
        SelectedProfile = Profiles.FirstOrDefault(p => string.Equals(p.Id, catalog.ActiveProfileId, StringComparison.OrdinalIgnoreCase)) ?? Profiles[0];
        ApplyViewState("populated");
    }

    private async Task RefreshVaultStateAsync(string profileId, CancellationToken ct)
    {
        _ = await RetrieveSessionTokenAsync(ct).ConfigureAwait(false);
        if (string.IsNullOrEmpty(VaultStatusText)) VaultStatusText = $"OS Vault checked for '{profileId}'.";
    }

    private void SetValidationError(string title, string detail, string remediation)
    { ErrorTitle = title; ErrorDetail = detail; ErrorRemediation = remediation; ApplyViewState("error"); }

    private void ApplyViewState(string state)
    {
        CurrentViewState = state; IsLoadingState = state == "loading"; IsEmptyState = state == "empty";
        IsErrorState = state == "error"; IsDegradedState = state == "degraded"; IsPopulatedState = state is "populated" or "degraded";
    }
}
