# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$From = $env:SANDAL_RECAST_DIR,
    [string]$DestDir = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$commit = '6dc1667f580357e8a2154c28b7867bea7e8ad3a7' # v1.6.0
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$dest = if ($DestDir) { $DestDir } else { Join-Path $root 'build\third_party\recastnavigation' }

if (Test-Path -LiteralPath (Join-Path $dest 'Recast\CMakeLists.txt')) {
    Write-Host "build\third_party\recastnavigation is there already"
    exit 0
}

$parentDir = Split-Path -Parent $dest
if ($parentDir) {
    New-Item -ItemType Directory -Force -Path $parentDir | Out-Null
}

if ($From) {
    if (-not (Test-Path -LiteralPath (Join-Path $From 'Recast\CMakeLists.txt'))) {
        throw "$From is not a recastnavigation folder (no Recast\CMakeLists.txt in it)"
    }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Copy-Item -Path (Join-Path $From '*') -Destination $dest -Recurse -Force -Exclude '.git'
    Write-Host "recastnavigation copied from $From to $dest (commit $commit)"
    exit 0
}

& git clone --quiet https://github.com/recastnavigation/recastnavigation.git $dest
if ($LASTEXITCODE -ne 0) {
    throw "git clone of recastnavigation failed (exit $LASTEXITCODE)"
}
Push-Location $dest
try {
    & git checkout --quiet $commit
    if ($LASTEXITCODE -ne 0) {
        throw "checkout of recastnavigation $commit failed (exit $LASTEXITCODE)"
    }
} finally {
    Pop-Location
}
if (Test-Path -LiteralPath (Join-Path $dest '.git')) {
    Remove-Item -Recurse -Force -LiteralPath (Join-Path $dest '.git')
}
Write-Host "recastnavigation $commit is in $dest"
