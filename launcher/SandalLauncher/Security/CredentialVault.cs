// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Security;

public interface ICredentialVault
{
    Task<bool> StoreTokenAsync(string profileId, string token, CancellationToken cancellationToken = default);
    Task<string?> RetrieveTokenAsync(string profileId, CancellationToken cancellationToken = default);
    Task<bool> DeleteTokenAsync(string profileId, CancellationToken cancellationToken = default);
}

public static class CredentialVaultFactory
{
    public static ICredentialVault CreateDefault(string? fallbackDirectory = null) =>
        OperatingSystem.IsWindows() ? new WindowsCredentialVault(fallbackDirectory) :
        OperatingSystem.IsMacOS() ? new MacKeychainCredentialVault(fallbackDirectory) :
        OperatingSystem.IsLinux() ? new LibSecretCredentialVault(fallbackDirectory) :
        new InMemoryCredentialVault();

    internal static string NormalizeProfileKey(string profileId)
    {
        if (string.IsNullOrWhiteSpace(profileId)) throw new ArgumentException("Profile identifier cannot be empty.", nameof(profileId));
        ReadOnlySpan<char> src = profileId.AsSpan().Trim();
        int len = Math.Min(src.Length, 64);
        Span<char> chars = stackalloc char[len];
        for (int i = 0; i < len; i++) chars[i] = char.IsLetterOrDigit(src[i]) || src[i] is '-' or '_' or '.' ? src[i] : '_';
        return new string(chars);
    }

    internal static async Task<(int ExitCode, string StdOut)> RunToolAsync(string exe, string[] args, byte[]? stdIn, CancellationToken ct)
    {
        try
        {
            var psi = new ProcessStartInfo(exe) { UseShellExecute = false, RedirectStandardInput = stdIn is not null, RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (string arg in args) psi.ArgumentList.Add(arg);
            using Process? proc = Process.Start(psi);
            if (proc is null) return (-1, string.Empty);
            if (stdIn is not null) { await proc.StandardInput.BaseStream.WriteAsync(stdIn, ct).ConfigureAwait(false); proc.StandardInput.Close(); }
            string output = await proc.StandardOutput.ReadToEndAsync(ct).ConfigureAwait(false);
            await proc.WaitForExitAsync(ct).ConfigureAwait(false);
            return (proc.ExitCode, output);
        }
        catch (Exception) { return (-1, string.Empty); }
    }
}

public sealed class InMemoryCredentialVault : ICredentialVault, IDisposable
{
    private readonly ConcurrentDictionary<string, byte[]> _vault = new(StringComparer.Ordinal);

    public Task<bool> StoreTokenAsync(string profileId, string token, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (string.IsNullOrEmpty(token)) throw new ArgumentException("Session token cannot be empty.", nameof(token));
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        byte[] utf8 = Encoding.UTF8.GetBytes(token), storedCopy = new byte[utf8.Length];
        try
        {
            Buffer.BlockCopy(utf8, 0, storedCopy, 0, utf8.Length);
            _vault.AddOrUpdate(key, storedCopy, (_, existing) => { CryptographicOperations.ZeroMemory(existing); return storedCopy; });
            return Task.FromResult(true);
        }
        finally { CryptographicOperations.ZeroMemory(utf8); }
    }

    public Task<string?> RetrieveTokenAsync(string profileId, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!_vault.TryGetValue(CredentialVaultFactory.NormalizeProfileKey(profileId), out byte[]? stored) || stored.Length == 0)
            return Task.FromResult<string?>(null);
        byte[] scratch = new byte[stored.Length];
        try { Buffer.BlockCopy(stored, 0, scratch, 0, stored.Length); return Task.FromResult<string?>(Encoding.UTF8.GetString(scratch)); }
        finally { CryptographicOperations.ZeroMemory(scratch); }
    }

    public Task<bool> DeleteTokenAsync(string profileId, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (_vault.TryRemove(CredentialVaultFactory.NormalizeProfileKey(profileId), out byte[]? removed)) CryptographicOperations.ZeroMemory(removed);
        return Task.FromResult(true);
    }

    public void Dispose()
    {
        foreach (string key in _vault.Keys) if (_vault.TryRemove(key, out byte[]? buf)) CryptographicOperations.ZeroMemory(buf);
    }
}

internal static class EncryptedUserBlobStore
{
    public static string ResolveDirectory(string? customDirectory)
    {
        if (!string.IsNullOrWhiteSpace(customDirectory)) { Directory.CreateDirectory(customDirectory); return customDirectory; }
        string root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        string dir = Path.Combine(string.IsNullOrWhiteSpace(root) ? Path.GetTempPath() : root, "SandalCore", "Vault");
        Directory.CreateDirectory(dir);
        return dir;
    }

    public static void WriteSealedBlob(string dir, string key, ReadOnlySpan<byte> plaintext)
    {
        byte[] dek = LoadOrCreateKey(dir), nonce = RandomNumberGenerator.GetBytes(12), tag = new byte[16], ciphertext = new byte[plaintext.Length];
        try
        {
            using var aes = new AesGcm(dek, 16);
            aes.Encrypt(nonce, plaintext, ciphertext, tag, Encoding.UTF8.GetBytes(key));
            byte[] envelope = new byte[28 + ciphertext.Length];
            Buffer.BlockCopy(nonce, 0, envelope, 0, 12); Buffer.BlockCopy(tag, 0, envelope, 12, 16); Buffer.BlockCopy(ciphertext, 0, envelope, 28, ciphertext.Length);
            WriteUserRestrictedFile(Path.Combine(dir, $"{key}.vlt"), envelope);
        }
        finally { CryptographicOperations.ZeroMemory(dek); CryptographicOperations.ZeroMemory(ciphertext); }
    }

    public static string? ReadSealedBlob(string dir, string key)
    {
        string blobPath = Path.Combine(dir, $"{key}.vlt");
        if (!File.Exists(blobPath) || File.ReadAllBytes(blobPath) is not { Length: > 28 } envelope) return null;
        byte[] dek = LoadOrCreateKey(dir), plaintext = new byte[envelope.Length - 28];
        try
        {
            using var aes = new AesGcm(dek, 16);
            aes.Decrypt(envelope.AsSpan(0, 12), envelope.AsSpan(28), envelope.AsSpan(12, 16), plaintext, Encoding.UTF8.GetBytes(key));
            return Encoding.UTF8.GetString(plaintext);
        }
        catch (CryptographicException) { return null; }
        finally { CryptographicOperations.ZeroMemory(dek); CryptographicOperations.ZeroMemory(plaintext); }
    }

    public static bool DeleteSealedBlob(string dir, string key)
    {
        string blobPath = Path.Combine(dir, $"{key}.vlt");
        if (File.Exists(blobPath)) File.Delete(blobPath);
        return true;
    }

    public static async Task<bool> StoreCliOrBlobAsync((bool OsMatch, string Dir, string Exe, string[] Args, bool StdinToken) cmd, string profileId, string token, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        if (string.IsNullOrEmpty(token)) throw new ArgumentException("Session token cannot be empty.", nameof(token));
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        byte[] utf8 = Encoding.UTF8.GetBytes(token);
        try
        {
            if (!cmd.OsMatch || (await CredentialVaultFactory.RunToolAsync(cmd.Exe, cmd.Args, cmd.StdinToken ? utf8 : null, ct).ConfigureAwait(false)).ExitCode != 0)
                WriteSealedBlob(cmd.Dir, key, utf8);
            return true;
        }
        finally { CryptographicOperations.ZeroMemory(utf8); }
    }

    public static async Task<string?> RetrieveCliOrBlobAsync((bool OsMatch, string Dir, string Exe, string[] Args) cmd, string profileId, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        return cmd.OsMatch && (await CredentialVaultFactory.RunToolAsync(cmd.Exe, cmd.Args, null, ct).ConfigureAwait(false)) is (0, { Length: > 0 } s)
            ? s.TrimEnd('\r', '\n') : ReadSealedBlob(cmd.Dir, key);
    }

    private static byte[] LoadOrCreateKey(string dir)
    {
        string keyPath = Path.Combine(dir, "vault.kek");
        if (File.Exists(keyPath) && File.ReadAllBytes(keyPath) is { Length: 32 } existing) return existing;
        byte[] fresh = RandomNumberGenerator.GetBytes(32);
        WriteUserRestrictedFile(keyPath, fresh);
        return fresh;
    }

    private static void WriteUserRestrictedFile(string path, byte[] content)
    {
        string temp = $"{path}.{Guid.NewGuid():N}.tmp";
        File.WriteAllBytes(temp, content);
        if (!OperatingSystem.IsWindows()) { try { File.SetUnixFileMode(temp, UnixFileMode.UserRead | UnixFileMode.UserWrite); } catch (PlatformNotSupportedException) { } }
        File.Move(temp, path, overwrite: true);
    }
}

public sealed class WindowsCredentialVault(string? fallbackDirectory = null) : ICredentialVault
{
    private const uint CredTypeGeneric = 1, CredPersistLocalMachine = 2;
    private const int ErrorNotFound = 1168;
    private readonly string _fallbackDir = EncryptedUserBlobStore.ResolveDirectory(fallbackDirectory);

    public Task<bool> StoreTokenAsync(string profileId, string token, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (string.IsNullOrEmpty(token)) throw new ArgumentException("Session token cannot be empty.", nameof(token));
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        byte[] utf8 = Encoding.UTF8.GetBytes(token);
        try
        {
            if (!OperatingSystem.IsWindows() || !TryWriteWinCred($"SandalCore/Profile/{key}", key, utf8))
                EncryptedUserBlobStore.WriteSealedBlob(_fallbackDir, key, utf8);
            return Task.FromResult(true);
        }
        finally { CryptographicOperations.ZeroMemory(utf8); }
    }

    public Task<string?> RetrieveTokenAsync(string profileId, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        return Task.FromResult(OperatingSystem.IsWindows() && TryReadWinCred($"SandalCore/Profile/{key}", out string? s) ? s : EncryptedUserBlobStore.ReadSealedBlob(_fallbackDir, key));
    }

    public Task<bool> DeleteTokenAsync(string profileId, CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string key = CredentialVaultFactory.NormalizeProfileKey(profileId);
        if (OperatingSystem.IsWindows()) _ = CredDeleteW($"SandalCore/Profile/{key}", CredTypeGeneric, 0) || Marshal.GetLastWin32Error() == ErrorNotFound;
        return Task.FromResult(EncryptedUserBlobStore.DeleteSealedBlob(_fallbackDir, key));
    }

    private static bool TryWriteWinCred(string targetName, string userName, byte[] secretBytes)
    {
        IntPtr blobPtr = Marshal.AllocHGlobal(secretBytes.Length);
        try
        {
            Marshal.Copy(secretBytes, 0, blobPtr, secretBytes.Length);
            var cred = new NativeCredential { Type = CredTypeGeneric, TargetName = targetName, CredentialBlobSize = (uint)secretBytes.Length, CredentialBlob = blobPtr, Persist = CredPersistLocalMachine, UserName = userName };
            return CredWriteW(ref cred, 0);
        }
        finally { for (int i = 0; i < secretBytes.Length; i++) Marshal.WriteByte(blobPtr, i, 0); Marshal.FreeHGlobal(blobPtr); }
    }

    private static bool TryReadWinCred(string targetName, out string? token)
    {
        token = null;
        if (!CredReadW(targetName, CredTypeGeneric, 0, out IntPtr credPtr) || credPtr == IntPtr.Zero) return false;
        try
        {
            NativeCredential cred = Marshal.PtrToStructure<NativeCredential>(credPtr);
            if (cred.CredentialBlob == IntPtr.Zero || cred.CredentialBlobSize == 0) return false;
            byte[] buffer = new byte[cred.CredentialBlobSize];
            try { Marshal.Copy(cred.CredentialBlob, buffer, 0, buffer.Length); token = Encoding.UTF8.GetString(buffer); return true; }
            finally { CryptographicOperations.ZeroMemory(buffer); }
        }
        finally { CredFree(credPtr); }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct NativeCredential
    {
        public uint Flags, Type; public string TargetName; public string? Comment;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
        public uint CredentialBlobSize; public IntPtr CredentialBlob; public uint Persist, AttributeCount;
        public IntPtr Attributes; public string? TargetAlias; public string UserName;
    }

    [DllImport("advapi32.dll", EntryPoint = "CredWriteW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool CredWriteW(ref NativeCredential userCredential, uint flags);
    [DllImport("advapi32.dll", EntryPoint = "CredReadW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool CredReadW(string target, uint type, int reservedFlag, out IntPtr credentialPtr);
    [DllImport("advapi32.dll", EntryPoint = "CredDeleteW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool CredDeleteW(string target, uint type, int flags);
    [DllImport("advapi32.dll", EntryPoint = "CredFree", SetLastError = true)] private static extern void CredFree(IntPtr cred);
}

public sealed class LibSecretCredentialVault(string? fallbackDirectory = null) : ICredentialVault
{
    private readonly string _dir = EncryptedUserBlobStore.ResolveDirectory(fallbackDirectory);
    public Task<bool> StoreTokenAsync(string profileId, string token, CancellationToken cancellationToken = default) =>
        EncryptedUserBlobStore.StoreCliOrBlobAsync((OperatingSystem.IsLinux(), _dir, "secret-tool", ["store", "--label=SandalCore", "service", "SandalCore", "profile", CredentialVaultFactory.NormalizeProfileKey(profileId)], true), profileId, token, cancellationToken);
    public Task<string?> RetrieveTokenAsync(string profileId, CancellationToken cancellationToken = default) =>
        EncryptedUserBlobStore.RetrieveCliOrBlobAsync((OperatingSystem.IsLinux(), _dir, "secret-tool", ["lookup", "service", "SandalCore", "profile", CredentialVaultFactory.NormalizeProfileKey(profileId)]), profileId, cancellationToken);
    public Task<bool> DeleteTokenAsync(string profileId, CancellationToken cancellationToken = default) =>
        Task.FromResult(EncryptedUserBlobStore.DeleteSealedBlob(_dir, CredentialVaultFactory.NormalizeProfileKey(profileId)));
}

public sealed class MacKeychainCredentialVault(string? fallbackDirectory = null) : ICredentialVault
{
    private readonly string _dir = EncryptedUserBlobStore.ResolveDirectory(fallbackDirectory);
    public Task<bool> StoreTokenAsync(string profileId, string token, CancellationToken cancellationToken = default) =>
        EncryptedUserBlobStore.StoreCliOrBlobAsync((OperatingSystem.IsMacOS(), _dir, "security", ["add-generic-password", "-U", "-s", "SandalCore", "-a", CredentialVaultFactory.NormalizeProfileKey(profileId), "-w", token], false), profileId, token, cancellationToken);
    public Task<string?> RetrieveTokenAsync(string profileId, CancellationToken cancellationToken = default) =>
        EncryptedUserBlobStore.RetrieveCliOrBlobAsync((OperatingSystem.IsMacOS(), _dir, "security", ["find-generic-password", "-s", "SandalCore", "-a", CredentialVaultFactory.NormalizeProfileKey(profileId), "-w"]), profileId, cancellationToken);
    public Task<bool> DeleteTokenAsync(string profileId, CancellationToken cancellationToken = default) =>
        Task.FromResult(EncryptedUserBlobStore.DeleteSealedBlob(_dir, CredentialVaultFactory.NormalizeProfileKey(profileId)));
}
