// SPDX-License-Identifier: AGPL-3.0-only
using System;
using System.IO;

namespace SandalLauncher.Services;

public enum GeneralSettingsUnlatchAction { UntouchedMissing = 0, SnapshotCreated = 1, RestoredFromBackup = 2, DeletedZeroByte = 3 }

public static class GeneralSettingsUnlatch
{
    public static string GetDefaultSaveFilePath()
    {
        string baseDir = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(baseDir)) baseDir = Path.GetTempPath();
        return Path.Combine(baseDir, "AOC", "Saved", "SaveGames", "GeneralSettings.sav");
    }

    public static GeneralSettingsUnlatchAction EnsureUnlatched(string? saveFilePath = null)
    {
        string targetPath = Path.GetFullPath(string.IsNullOrWhiteSpace(saveFilePath) ? GetDefaultSaveFilePath() : saveFilePath);
        string backupPath = $"{targetPath}.bak";
        var fileInfo = new FileInfo(targetPath);
        if (!fileInfo.Exists) return GeneralSettingsUnlatchAction.UntouchedMissing;
        if (fileInfo.Length > 0) { File.Copy(targetPath, backupPath, overwrite: true); return GeneralSettingsUnlatchAction.SnapshotCreated; }
        var backupInfo = new FileInfo(backupPath);
        if (backupInfo.Exists && backupInfo.Length > 0) { File.Copy(backupPath, targetPath, overwrite: true); return GeneralSettingsUnlatchAction.RestoredFromBackup; }
        File.Delete(targetPath);
        return GeneralSettingsUnlatchAction.DeletedZeroByte;
    }
}
