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
        (Join-Path $RootPath 'config'),
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

function Test-SandalEnvSchemaTemplates {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $configDir = Join-Path $RootPath 'config'
    $examplePath = Join-Path $configDir 'sandal.env.example'
    $profilesDir = Join-Path $configDir 'profiles'
    if (-not (Test-Path -LiteralPath $examplePath)) {
        $Violations.Add("Missing env schema template: $examplePath")
        return
    }
    $schemaKeys = @([regex]::Matches([System.IO.File]::ReadAllText($examplePath), '(?m)^(?:AOC|SANDAL)_[A-Z0-9_]+(?==)') | ForEach-Object { $_.Value })
    if ($schemaKeys.Count -ne 72) {
        $Violations.Add("sandal.env.example expected 72 keys, found $($schemaKeys.Count)")
    }
    foreach ($name in @('local-dev.env', 'docker-single.env', 'prod-mesh.env')) {
        $profilePath = Join-Path $profilesDir $name
        if (-not (Test-Path -LiteralPath $profilePath)) {
            $Violations.Add("Missing profile template: $profilePath")
        } elseif ($name -ieq 'prod-mesh.env' -and ([System.IO.File]::ReadAllText($profilePath) -match 'aoc:aoc@|sslmode=disable')) {
            $Violations.Add("Unredacted dev credentials or disabled TLS in $profilePath")
        }
    }
}

function Invoke-SandalPublicCheck {
    [CmdletBinding()]
    param(
        [Alias('RepoRoot', 'RootPath')]
        [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    )
    $violations = [System.Collections.Generic.List[string]]::new()
    Test-SandalTestsDirectoryBounds -RootPath $Root -Violations $violations
    Test-SandalGitTrackedAssets -RootPath $Root -Violations $violations
    Test-SandalContentLeaks -RootPath $Root -Violations $violations
    Test-SandalEnvSchemaTemplates -RootPath $Root -Violations $violations

    if ($violations.Count -gt 0) {
        foreach ($violation in $violations) {
            Write-Host "[FAIL] $violation"
        }
        return 1
    }

    Write-Host '[PASS] public_check.ps1 verified'
    return 0
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-SandalPublicCheck -Root $repoRoot)
}
