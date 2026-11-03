// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;
using System.Security.Cryptography;
using System.Threading;
using System.Threading.Tasks;

namespace SandalLauncher.Services;

public sealed record BinaryHashResult(
    bool IsMatch,
    string ActualHex,
    string ExpectedHex,
    long FileSizeBytes);

public static class BinaryHashVerifier
{
    public const int Sha256HexLength = 64;

    public static bool IsValidSha256Hex(string? candidate)
    {
        if (string.IsNullOrWhiteSpace(candidate)) return false;
        ReadOnlySpan<char> span = candidate.AsSpan().Trim();
        if (span.Length != Sha256HexLength) return false;
        foreach (char c in span)
        {
            if (!Uri.IsHexDigit(c)) return false;
        }
        return true;
    }

    public static string ComputeFileSha256Hex(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath))
            throw new ArgumentException("Client binary path cannot be empty.", nameof(filePath));
        string canonicalPath = Path.GetFullPath(filePath.Trim());
        using var stream = new FileStream(canonicalPath, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, useAsync: false);
        byte[] digest = SHA256.HashData(stream);
        return Convert.ToHexString(digest).ToLowerInvariant();
    }

    public static async Task<BinaryHashResult> VerifyFileHashAsync(
        string filePath,
        string expectedSha256Hex,
        CancellationToken ct = default)
    {
        if (string.IsNullOrWhiteSpace(filePath))
            throw new ArgumentException("Client binary path cannot be empty.", nameof(filePath));
        if (!IsValidSha256Hex(expectedSha256Hex))
            throw new ArgumentException("Expected SHA-256 digest must be a 64-character hex string.", nameof(expectedSha256Hex));
        string canonicalPath = Path.GetFullPath(filePath.Trim());
        string normalizedExpected = expectedSha256Hex.Trim().ToLowerInvariant();
        await using var stream = new FileStream(canonicalPath, FileMode.Open, FileAccess.Read, FileShare.Read, 65536, useAsync: true);
        long fileSize = stream.Length;
        byte[] actualBytes = await SHA256.HashDataAsync(stream, ct).ConfigureAwait(false);
        byte[] expectedBytes = Convert.FromHexString(normalizedExpected);
        // Constant-time digest comparison prevents byte-by-byte timing side channels (SEC-05).
        bool isMatch = CryptographicOperations.FixedTimeEquals(actualBytes, expectedBytes);
        string actualHex = Convert.ToHexString(actualBytes).ToLowerInvariant();
        return new BinaryHashResult(isMatch, actualHex, normalizedExpected, fileSize);
    }
}
