# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$WorlddArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-WorlddExecutable {
    param([Parameter(Mandatory)][string]$RepoRoot)

    $exeName = if ($IsWindows) { 'worldd.exe' } else { 'worldd' }
    $candidates = @(
        (Join-Path $RepoRoot 'dist' 'server' 'world' $exeName),
        (Join-Path $RepoRoot 'dist' 'Release' $exeName),
        (Join-Path $RepoRoot 'build' 'x64' 'src' 'server' 'world' 'Release' $exeName),
        (Join-Path $RepoRoot 'build' 'x64' 'src' 'server' 'world' $exeName),
        (Join-Path $RepoRoot 'build' 'linux' 'src' 'server' 'world' $exeName)
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "worldd executable ('$exeName') not found under dist/server/world or build/x64/src/server/world. Run .\scripts\build.ps1 -Target world first."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$worlddExe = Resolve-WorlddExecutable -RepoRoot $repoRoot
$binDir = Split-Path -Parent $worlddExe
$distWorldDir = Join-Path $repoRoot 'dist' 'server' 'world'
$distReleaseDir = Join-Path $repoRoot 'dist' 'Release'
$pathSep = [System.IO.Path]::PathSeparator

$savedPath = $env:PATH
$savedLdLibraryPath = $env:LD_LIBRARY_PATH
Push-Location $repoRoot
try {
    $runtimeDirs = [System.Collections.Generic.List[string]]::new()
    foreach ($dir in @($binDir, $distWorldDir, $distReleaseDir)) {
        if ((Test-Path -LiteralPath $dir -PathType Container) -and -not $runtimeDirs.Contains($dir)) {
            $runtimeDirs.Add($dir)
        }
    }
    $prefixPath = $runtimeDirs -join $pathSep
    if ($prefixPath) {
        $env:PATH = if ($savedPath) { "$prefixPath$pathSep$savedPath" } else { $prefixPath }
        if (-not $IsWindows) {
            $env:LD_LIBRARY_PATH = if ($savedLdLibraryPath) { "$prefixPath$pathSep$savedLdLibraryPath" } else { $prefixPath }
        }
    }

    & $worlddExe @WorlddArgs
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "worldd exited with code $exitCode"
    }
} finally {
    Pop-Location
    $env:PATH = $savedPath
    $env:LD_LIBRARY_PATH = $savedLdLibraryPath
}
