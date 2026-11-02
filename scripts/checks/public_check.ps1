[CmdletBinding()]
param(
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-SandalTestsDirectoryBounds {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $testsDir = Join-Path $RootPath 'tests'
    if (-not (Test-Path -LiteralPath $testsDir)) {
        return
    }
    $files = @(Get-ChildItem -LiteralPath $testsDir -Recurse -File)
    foreach ($file in $files) {
        if ($file.Extension -ieq '.inc') {
            $Violations.Add("Forbidden .inc file in tests/: $($file.FullName)")
        }
        $lineCount = @(Get-Content -LiteralPath $file.FullName).Count
        if ($lineCount -gt 800) {
            $Violations.Add("File in tests/ exceeds 800 lines ($lineCount): $($file.FullName)")
        }
    }
}

function Test-SandalGitTrackedAssets {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $gitArgs = @('-C', $RootPath, 'ls-files')
    $tracked = @(& git @gitArgs)
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed with exit code $LASTEXITCODE"
    }
    foreach ($entry in $tracked) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            continue
        }
        $norm = ([string]$entry).Replace('\', '/')
        if ($norm -match '\.(pak|utoc|ucas|pdb|dmp|pcapng)$') {
            $Violations.Add("Forbidden binary asset tracked in git: $norm")
        }
        if ($norm -match '^certs/.*\.key$' -or $norm -ieq 'config/sandal.env') {
            $Violations.Add("Forbidden secret or environment file tracked in git: $norm")
        }
    }
}

function Test-SandalContentLeaks {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $pemRegex = 'BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY'
    $peBaseRegex = '0x140[0-9A-Fa-f]{6}'
    $scanDirs = @(
        (Join-Path $RootPath 'tests'),
        (Join-Path $RootPath 'scripts\checks')
    )
    foreach ($dir in $scanDirs) {
        if (-not (Test-Path -LiteralPath $dir)) {
            continue
        }
        $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File)
        foreach ($file in $files) {
            $text = [System.IO.File]::ReadAllText($file.FullName)
            if ($text -cmatch $pemRegex) {
                $Violations.Add("PEM private key header found in $($file.FullName)")
            }
            if ($text -match $peBaseRegex) {
                $Violations.Add("Raw PE32+ image-base hex literal found in $($file.FullName)")
            }
        }
    }
}

$violations = [System.Collections.Generic.List[string]]::new()
Test-SandalTestsDirectoryBounds -RootPath $RepoRoot -Violations $violations
Test-SandalGitTrackedAssets -RootPath $RepoRoot -Violations $violations
Test-SandalContentLeaks -RootPath $RepoRoot -Violations $violations

if ($violations.Count -gt 0) {
    foreach ($violation in $violations) {
        Write-Host "[FAIL] $violation"
    }
    exit 1
}

Write-Output '[PASS] public_check.ps1 verified'
exit 0
