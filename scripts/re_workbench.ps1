#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding()]
param(
    [ValidateSet('sync', 'test', 'run')]
    [string] $Action = 'test',
    [Parameter(ValueFromRemainingArguments)]
    [string[]] $RemainingArgs = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path $PSScriptRoot -Parent
$env:PYTHONPYCACHEPREFIX = Join-Path $repoRoot 'build\pycache'
$venvDir = Join-Path $repoRoot 'build\venv\re-workbench'
$pytestCacheDir = Join-Path $repoRoot 'build\pytest_cache'
$workbenchDir = Join-Path $repoRoot 'src\tools\re-workbench'
$env:UV_PROJECT_ENVIRONMENT = $venvDir

function Ensure-ReWorkbenchVenv {
    $venvPython = Join-Path $venvDir 'Scripts\python.exe'
    if (Test-Path -LiteralPath $venvPython) {
        return $venvPython
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $venvDir -Parent) | Out-Null
    $uv = Get-Command uv -ErrorAction SilentlyContinue
    Push-Location $workbenchDir
    try {
        if ($uv) {
            & $uv.Source sync --frozen
            if ($LASTEXITCODE -ne 0) { throw "uv sync failed ($LASTEXITCODE)" }
        } else {
            & python -m venv $venvDir
            if ($LASTEXITCODE -ne 0) { throw "python -m venv failed ($LASTEXITCODE)" }
            & $venvPython -m pip install -e $workbenchDir
            if ($LASTEXITCODE -ne 0) { throw "pip install -e failed ($LASTEXITCODE)" }
        }
    } finally {
        Pop-Location
    }
    return $venvPython
}

$venvPython = Ensure-ReWorkbenchVenv
Push-Location $workbenchDir
try {
    switch ($Action) {
        'sync' {
            Write-Host "[re-workbench] Virtual environment ready at $venvDir"
        }
        'test' {
            & $venvPython -m pytest -o "cache_dir=$pytestCacheDir" @RemainingArgs
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
        'run' {
            & $venvPython @RemainingArgs
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }
    }
} finally {
    Pop-Location
}
