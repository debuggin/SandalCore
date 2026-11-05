# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

param(
    [int]$IntervalMs = 1000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "SilentlyContinue"

try {
    $myPid = $PID
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $_.ProcessId -ne $myPid -and $_.CommandLine -like "*auto_logger.ps1*"
    } | ForEach-Object {
        Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
    }
} catch {}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = Split-Path -Parent $ScriptDir
$DistDir = Join-Path $RepoRoot "dist\Release"
$DistLogsDir = Join-Path $DistDir "logs"
$PrivateLogsRoot = if ($env:AOC_MIRROR_LOGS_ROOT) { $env:AOC_MIRROR_LOGS_ROOT } else { Join-Path $RepoRoot "dist\logs\mirror" }   # a second copy of every run
$ClientLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\AOC.log"
$CefLogPath = Join-Path $env:LOCALAPPDATA "AOC\Saved\Logs\cef3.log"
$GoLogsDir = Join-Path $RepoRoot "dist\services\logs"

function Sync-FileSafe($srcPath, $dstPath) {
    if (-not (Test-Path -LiteralPath $srcPath)) { return $false }
    try {
        $in = [System.IO.File]::Open($srcPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        $out = [System.IO.File]::Open($dstPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $in.CopyTo($out)
        $out.Flush()
        $out.Close()
        $in.Close()
        return $true
    } catch {
        return $false
    }
}
# [CoA :: C Y B]

function Write-TextSafe($text, $dstPath) {
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        $out = [System.IO.File]::Open($dstPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
        $out.Write($bytes, 0, $bytes.Length)
        $out.Flush()
        $out.Close()
    } catch {}
}

function Generate-Summary($destDir, $runId) {
    $summary = [System.Collections.Generic.List[string]]::new()
    $summary.Add("================================================================")
    $summary.Add("  AoC Private Server Run Summary: $runId")
    $summary.Add("  Updated At: " + (Get-Date).ToString("yyyy-MM-dd HH:mm:ss"))
    $summary.Add("================================================================")
    $summary.Add("")

    $gcTxt = Join-Path $destDir "game_client.txt"
    if (Test-Path $gcTxt) {
        $gcContent = Get-Content -LiteralPath $gcTxt -ErrorAction SilentlyContinue
        $loadDrop = ($gcContent | Select-String "HideLoadingScreen").Count -gt 0
        $cleanExit = ($gcContent | Select-String "FPlatformMisc::RequestExit").Count -gt 0
        $gcWarns = ($gcContent | Select-String '"severity":"Warning"').Count
        $gcErrs = ($gcContent | Select-String '"severity":"Error"').Count

        $summary.Add("[Game Client]")
        $summary.Add("  World Load Screen Dropped: " + ($(if ($loadDrop) { "YES" } else { "NO" })))
        $summary.Add("  Clean Exit (Window Close): " + ($(if ($cleanExit) { "YES" } else { "NO" })))
        $summary.Add("  Client Warnings: $gcWarns, Errors: $gcErrs")
        $summary.Add("")
    }

    return ($summary -join "`r`n")
}

$currentRunId = ""
$lastSizes = @{}
$sawProcessesRunning = $false
$idleSecondsAfterStop = 0

Write-Host "[AutoLogger] Background auto-logger active. Streaming all 4 application logs live..." -ForegroundColor Cyan

while ($true) {
    $clientProc = Get-Process -Name "AOCClient-Win64-Shipping" -ErrorAction SilentlyContinue | Select-Object -First 1
    $launcherProc = Get-Process -Name "SandalLauncher" -ErrorAction SilentlyContinue | Select-Object -First 1
    # The Go services and world from this checkout: the run is named after the first of them to start.
    $goProc = Get-Process -Name world, worldd, gateway -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($RepoRoot, [System.StringComparison]::OrdinalIgnoreCase) } |
        Sort-Object StartTime | Select-Object -First 1
    $runProc = $goProc

    $anyRunning = ($clientProc -or $launcherProc -or $goProc)
    if ($anyRunning) {
        $sawProcessesRunning = $true
        $idleSecondsAfterStop = 0
    } elseif ($sawProcessesRunning) {
        $idleSecondsAfterStop += [math]::Max(1, [int]($IntervalMs / 1000))
    }

    $runId = ""
    if ($runProc) {
        $runId = "run_" + $runProc.StartTime.ToString("yyyyMMdd_HHmmss")
    } elseif ($currentRunId) {
        # The processes have stopped: the last of their logs still go to the same run.
        $runId = $currentRunId
    }

    if ($runId) {
        if ($runId -ne $currentRunId) {
            $currentRunId = $runId
            $lastSizes.Clear()
            Write-Host "[AutoLogger] Tracking run: $currentRunId" -ForegroundColor Green

            $targetDirs = @(
                (Join-Path $RepoRoot "dist\logs\$currentRunId"),
                (Join-Path $PrivateLogsRoot $currentRunId),
                (Join-Path $DistLogsDir $currentRunId)
            )
            foreach ($td in $targetDirs) {
                if (-not (Test-Path $td)) { New-Item -ItemType Directory -Path $td -Force | Out-Null }
            }
        }

        $repoDest = Join-Path $RepoRoot "dist\logs\$currentRunId"
        $privateDest = Join-Path $PrivateLogsRoot $currentRunId
        $distDest = Join-Path $DistLogsDir $currentRunId
        $allDests = @($repoDest, $privateDest, $distDest)

        $needsSummaryUpdate = $false

        foreach ($name in 'auth', 'character', 'citynode', 'quests', 'mesh', 'gateway', 'world', 'worldd') {
            $svcLog = Join-Path $GoLogsDir "$name.log"
            if (-not (Test-Path $svcLog)) { continue }
            $fi = Get-Item $svcLog
            if ($lastSizes["go-$name"] -ne $fi.Length) {
                $lastSizes["go-$name"] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $svcLog (Join-Path $d "go-$name.txt") | Out-Null
                }
                $needsSummaryUpdate = $true
            }
        }

        if (Test-Path $ClientLogPath) {
            $fi = Get-Item $ClientLogPath
            $szKey = "client"
            if ($lastSizes[$szKey] -ne $fi.Length) {
                $lastSizes[$szKey] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $ClientLogPath (Join-Path $d "game_client.txt") | Out-Null
                }
                $needsSummaryUpdate = $true
            }
        }

        if (Test-Path $CefLogPath) {
            $fi = Get-Item $CefLogPath
            if ($lastSizes["cef"] -ne $fi.Length) {
                $lastSizes["cef"] = $fi.Length
                foreach ($d in $allDests) {
                    Sync-FileSafe $CefLogPath (Join-Path $d "cef3.txt") | Out-Null
                }
            }
        }

        if ($needsSummaryUpdate) {
            $sumText = Generate-Summary $repoDest $currentRunId
            foreach ($d in $allDests) {
                Write-TextSafe $sumText (Join-Path $d "run_summary.txt")
            }
        }
    }

    if ($sawProcessesRunning -and $idleSecondsAfterStop -ge 15) {
        Write-Host "[AutoLogger] All processes stopped. Final logs saved to $currentRunId. Exiting." -ForegroundColor Cyan
        break
    }

    Start-Sleep -Milliseconds $IntervalMs
}
