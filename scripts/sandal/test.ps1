#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$Filter = '',
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-TestOptions {
    param(
        [string]$BaseConfig,
        [string]$BaseFilter,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $cfg = $BaseConfig
    $flt = $BaseFilter
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Config|--config|-c)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $cfg = $RawArgs[$i] }
            }
            '^(-Filter|--filter|-f)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $flt = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$' { $dry = $true }
            '^(-Json|--json)$'      { $jsn = $true }
        }
    }
    return [pscustomobject]@{ Config = $cfg; Filter = $flt; DryRun = $dry; Json = $jsn }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or ($Arguments -contains '-NoColor') -or ($Arguments -contains '--no-color') -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$pathSep = [System.IO.Path]::PathSeparator
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/SandalCore.psd1') -Force -DisableNameChecking
}

$opts = Resolve-TestOptions `
    -BaseConfig $Config `
    -BaseFilter $Filter `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -RawArgs @($Arguments)

function Out-Host {
    [CmdletBinding()]
    param([Parameter(ValueFromPipeline = $true)] $InputObject)
    begin {}
    process {
        if (-not $opts.Json) {
            Microsoft.PowerShell.Utility\Out-Host -InputObject $InputObject
        }
    }
    end {}
}

if (-not $opts.DryRun) {
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    $distConfigDir = Join-Path (Join-Path $rootPath 'dist') $opts.Config
    $prevPath = $env:PATH
    $env:PATH = "$distConfigDir$pathSep$prevPath"
    try {
        $normFilter = if ([string]::IsNullOrWhiteSpace($opts.Filter)) { '' } else { $opts.Filter.Trim().ToLowerInvariant() }
        if ($normFilter -in @('', 'cpp')) {
            $testsDir = Join-Path (Join-Path $rootPath 'build/tests') $opts.Config
            $testFilter = "test_*$exeExt"
            $hasBuiltTests = [bool](@(Get-ChildItem -Path $testsDir -Filter $testFilter -File -ErrorAction SilentlyContinue | Where-Object {
                $IsWindows -or [string]::IsNullOrEmpty($_.Extension)
            }).Count -gt 0)
            $runTestsScript = Join-Path $rootPath 'scripts/run_tests.ps1'
            if ($opts.Json) {
                & $runTestsScript -Config $opts.Config -NoBuild:$hasBuiltTests 6>&1 | Out-Host
            } else {
                & $runTestsScript -Config $opts.Config -NoBuild:$hasBuiltTests
            }
            if ($LASTEXITCODE -ne 0) {
                throw "scripts/run_tests.ps1 failed with exit code $LASTEXITCODE."
            }
        }
        if ($normFilter -in @('', 'go')) {
            Get-ChildItem (Join-Path $rootPath 'src/tests/go/*_test.go') | ForEach-Object {
                if ($opts.Json) {
                    & go test $_.FullName 6>&1 | Out-Host
                } else {
                    & go test $_.FullName
                }
                if ($LASTEXITCODE -ne 0) {
                    throw "go test failed for $($_.Name)"
                }
            }
        }
        if ($normFilter -in @('', 'powershell', 'pester')) {
            $pesterPattern = Join-Path $rootPath 'src/tests/powershell/*.Tests.ps1'
            if ($opts.Json) {
                $res = Invoke-Pester -Path $pesterPattern -PassThru 6>$null
                $res | Out-Host
            } else {
                $res = Invoke-Pester -Path $pesterPattern -PassThru
            }
            if ($res.FailedCount -gt 0) {
                throw "Invoke-Pester failed with $($res.FailedCount) failed test(s)."
            }
        }
        if ($normFilter -notin @('', 'cpp', 'go', 'powershell', 'pester')) {
            Get-ChildItem (Join-Path $rootPath 'src/tests/go/*_test.go') | Where-Object { $_.Name -match $opts.Filter } | ForEach-Object {
                if ($opts.Json) { & go test $_.FullName 6>&1 | Out-Host } else { & go test $_.FullName }
                if ($LASTEXITCODE -ne 0) { throw "go test failed for $($_.Name)" }
            }
            $pesterMatches = @(Get-ChildItem (Join-Path $rootPath 'src/tests/powershell/*.Tests.ps1') | Where-Object { $_.Name -match $opts.Filter })
            if ($pesterMatches.Count -gt 0) {
                $paths = @($pesterMatches | ForEach-Object { $_.FullName })
                $res = if ($opts.Json) { Invoke-Pester -Path $paths -PassThru 6>$null } else { Invoke-Pester -Path $paths -PassThru }
                if ($opts.Json) { $res | Out-Host }
                if ($res.FailedCount -gt 0) { throw "Invoke-Pester failed with $($res.FailedCount) failed test(s)." }
            }
        }
    } finally {
        $env:PATH = $prevPath
    }
}

$payload = [ordered]@{
    ok       = $true
    command  = 'test'
    status   = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    config   = $opts.Config
    filter   = $opts.Filter
    dry_run  = [bool]$opts.DryRun
    no_color = $useNoColor
    steps    = @('sync-dist', 'run-tests')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'test' (config={0}, status={1}, dry_run={2})" -f $opts.Config, $payload.status, $opts.DryRun)
}
