# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

# ============================================================================
#  run_tests.ps1 - build all targets and run every dist\<Config>\test_*.exe
#
#  Usage (PowerShell):
#      .\scripts\run_tests.ps1                         # build + run
#      .\scripts\run_tests.ps1 -NoBuild                # run only
#      .\scripts\run_tests.ps1 -SaveBaseline FILE      # write summary to FILE
#      .\scripts\run_tests.ps1 -CompareTo FILE         # exit 1 if results differ
#      .\scripts\run_tests.ps1 -Heavy                  # also run the heavy tests (below)
#      .\scripts\run_tests.ps1 -Jobs 6                 # build at most 6 projects and 6 compiler processes at a time (see build.ps1)
#
#  The heavy tests (test_worldd_region_load: 1 GB of ground, about a minute, wall-clock checks) are NOT in the default gate: on a PC that is also building or
#  playing a timing check can flip, and the PC hard-resets under load.  -Heavy runs them (G1 review B I1); the plan's last task and the lead do, alone, under the
#  heavy-job lock.  src/tests/fixtures/test-baseline.txt does not list them.
#
#  A test that skipped part or all of its checks (no content.db, no ground file: it prints a "[skip]" line or ends a line with ": skipped", and exits 0) is reported in a NOTE after
#  the summary; the compared lines stay "PASS exit=0" (review B I1).
#
#  -NoBuild runs whatever test_*.exe is already present in dist\<Config>,
#  including leftovers from a test target since dropped from CMake.
# ============================================================================
#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')] [string] $Config = 'Release',
    [switch] $NoBuild,
    [string] $SaveBaseline,
    [string] $CompareTo,
    [switch] $Heavy,
    [int]    $Jobs = 0
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($SaveBaseline -and $CompareTo) { throw "use -SaveBaseline or -CompareTo, not both" }
$root     = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$dist     = Join-Path $root 'dist' $Config
$testDist = Join-Path $root 'dist' 'tests' $Config
$pathSep  = [System.IO.Path]::PathSeparator
# ~ CoA :: C Y B ~

if (-not $NoBuild) {
    $testGlob = if ($IsWindows) { 'test_*.exe' } else { 'test_*' }
    $fuzzGlob = if ($IsWindows) { 'fuzz_*.exe' } else { 'fuzz_*' }
    Remove-Item (Join-Path $testDist $testGlob), (Join-Path $testDist $fuzzGlob), (Join-Path $dist $testGlob), (Join-Path $dist $fuzzGlob) -ErrorAction SilentlyContinue
    $allTarget = if ($IsWindows) { 'ALL_BUILD' } else { 'all' }
    & (Join-Path $PSScriptRoot 'build.ps1') -Target $allTarget -Config $Config -Jobs $Jobs
}

$ErrorActionPreference = 'Continue'
Push-Location $root
try {
    $oldPath = $env:PATH
    $env:PATH = "$dist$pathSep$oldPath"
    $heavyTests = @('test_world_region_load', 'test_worldd_region_load')
    $skippers = New-Object System.Collections.Generic.List[string]
    $testBinaries = if ($IsWindows) {
        Get-ChildItem -LiteralPath $testDist -Filter 'test_*.exe' -File -ErrorAction SilentlyContinue
    } else {
        Get-ChildItem -LiteralPath $testDist -Filter 'test_*' -File -ErrorAction SilentlyContinue |
            Where-Object { [string]::IsNullOrEmpty($_.Extension) -or $_.Extension -eq '.exe' }
    }
    $lines = @(foreach ($exe in $testBinaries | Where-Object { $Heavy -or ($heavyTests -notcontains $_.BaseName) } | Sort-Object Name) {
        $realGround = ($exe.BaseName -in @('test_world_node_pop', 'test_worldd_node_pop')) -and (Test-Path -LiteralPath (Join-Path $root 'dist' 'content' 'area_nav.bin'))
        if ($realGround) { $env:AOC_REAL_GROUND = '1' }
        $text = & $exe.FullName 2>&1 | Out-String
        $code = $LASTEXITCODE
        if ($realGround) { Remove-Item Env:AOC_REAL_GROUND -ErrorAction SilentlyContinue }
        if ($code -eq 0 -and $text -match '(?m)\[skip\]|:\s+skipped\s*$') { $skippers.Add($exe.BaseName) }
        $status = if ($code -eq 0) { 'PASS' } else { 'FAIL' }
        '{0,-36} {1} exit={2}' -f $exe.BaseName, $status, $code
    })
} finally { $env:PATH = $oldPath; Pop-Location; $ErrorActionPreference = 'Stop' }

if ($lines.Count -eq 0) { throw "no test_* binaries in $testDist" }

$lines | ForEach-Object { Write-Host $_ }
if (-not $Heavy) { Write-Host ("NOTE: not run (heavy, -Heavy): " + ($heavyTests -join ', ')) }
if ($skippers.Count -gt 0) { Write-Host ("NOTE: skipped part or all of their checks (a skip line, exit 0): " + ($skippers -join ', ')) }
$failed = @($lines | Where-Object { $_ -match ' FAIL ' }).Count
Write-Host ("{0} tests, {1} failing" -f $lines.Count, $failed)

if ($SaveBaseline) { $lines | Set-Content -Encoding utf8 $SaveBaseline }
if ($CompareTo) {
    $diff = Compare-Object @(Get-Content -LiteralPath $CompareTo) $lines
    if ($diff) {
        Write-Host 'DIFFERS FROM BASELINE:'
        $diff | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
        exit 1
    }
    Write-Host 'Matches baseline.'
}
exit 0
