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
    $testsDir = Join-Path $RootPath 'src\tests'
    if (-not (Test-Path -LiteralPath $testsDir)) { $testsDir = Join-Path $RootPath 'tests' }
    if (-not (Test-Path -LiteralPath $testsDir)) {
        return
    }
    $files = @(Get-ChildItem -LiteralPath $testsDir -Recurse -File)
    foreach ($file in $files) {
        if ($file.Extension -ieq '.inc') {
            $Violations.Add("Forbidden .inc file in src/tests/: $($file.FullName)")
        }
        if ($file.FullName -match '[\\/]fixtures[\\/]') {
            continue
        }
        $lineCount = @(Get-Content -LiteralPath $file.FullName).Count
        if ($lineCount -gt 800) {
            $Violations.Add("File in src/tests/ exceeds 800 lines ($lineCount): $($file.FullName)")
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
        if ($norm -match '^(?:certs|config/certs|src/config/certs|dist/certs)/.*\.key$' -or $norm -in @('config/sandal.env', 'src/config/sandal.env')) {
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
        (Join-Path $RootPath 'src\config'),
        (Join-Path $RootPath 'src\tests'),
        (Join-Path $RootPath 'config'),
        (Join-Path $RootPath 'tests'),
        (Join-Path $RootPath 'scripts\checks')
    )
    foreach ($dir in $scanDirs) {
        if (-not (Test-Path -LiteralPath $dir)) {
            continue
        }
        $files = @(Get-ChildItem -LiteralPath $dir -Recurse -File | Where-Object { $_.FullName -notmatch '[\\/](?:control-app|notes)[\\/]' })
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
    $configDir = Join-Path $RootPath 'src\config'
    if (-not (Test-Path -LiteralPath $configDir)) { $configDir = Join-Path $RootPath 'config' }
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

function Test-SandalRetiredLegacyScripts {
    param(
        [Parameter(Mandatory)][Alias('RootPath')][string]$Root,
        [AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations = $null
    )
    if ($null -eq $Violations) {
        $callerVar = Get-Variable -Name violations -Scope 1 -ErrorAction SilentlyContinue
        if ($null -ne $callerVar) {
            $Violations = $callerVar.Value
        }
    }
    foreach ($relPath in @('build_launcher.bat', 'build_launcher.ps1', 'scripts/save_run_logs.bat', 'scripts/save_run_logs.ps1')) {
        $retiredPath = Join-Path $Root $relPath
        if (Test-Path -LiteralPath $retiredPath) {
            if ($null -ne $Violations) {
                $Violations.Add("Retired legacy script must not exist: $relPath")
            }
        }
    }
}

function Test-SandalRootDirectoryHierarchy {
    param(
        [Parameter(Mandatory)][Alias('RootPath', 'Root')][string]$RepoRoot,
        [AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations = $null
    )
    $findings = [System.Collections.Generic.List[string]]::new()
    $allowedRootDirs = @('src', 'build', 'dist', 'deploy', 'docs', 'scripts', '.github', '.git', '.agents', '.claude', '_workspace', '.beads', 'graphify-out')
    $allowedRootFiles = @('README.md', 'LICENSE', 'NOTICE', 'AGENTS.md', 'GEMINI.md', 'sandal.ps1', '.gitignore', '.gitattributes', '.editorconfig', '.graphifyignore', '.env')
    foreach ($bat in @(Get-ChildItem -LiteralPath $RepoRoot -Filter '*.bat' -File -Force -ErrorAction SilentlyContinue)) {
        $findings.Add("NoRootBatFiles: forbidden root .bat file: $($bat.Name)")
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot '.pytest_cache')) { $findings.Add('NoRootCacheOrGateLocks: forbidden root cache or lock artifact: .pytest_cache') }
    $gateLock = Join-Path $RepoRoot '.beads.gate.lock'
    if (Test-Path -LiteralPath $gateLock) {
        if (Test-Path -LiteralPath (Join-Path $RepoRoot '.beads') -PathType Container) { Remove-Item -LiteralPath $gateLock -Force -ErrorAction SilentlyContinue }
        else { $findings.Add('NoRootCacheOrGateLocks: forbidden root cache or lock artifact: .beads.gate.lock') }
    }
    foreach ($rootSub in @('src/tools', 'src/clients', 'tools', 'launcher')) {
        $parentDir = Join-Path $RepoRoot ($rootSub -replace '/', [System.IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $parentDir -PathType Container)) { continue }
        foreach ($projDir in @(Get-ChildItem -LiteralPath $parentDir -Directory -Force | Where-Object { $_.Name -ine 'control-app' })) {
            if (Test-Path -LiteralPath (Join-Path $projDir.FullName 'bin') -PathType Container) { $findings.Add("NoInTreeDotnetBinObj: forbidden in-tree bin directory: $rootSub/$($projDir.Name)/bin") }
            if (Test-Path -LiteralPath (Join-Path $projDir.FullName 'obj') -PathType Container) { $findings.Add("NoInTreeDotnetBinObj: forbidden in-tree obj directory: $rootSub/$($projDir.Name)/obj") }
        }
    }
    $distRelease = Join-Path $RepoRoot 'dist\Release'
    if (Test-Path -LiteralPath $distRelease -PathType Container) {
        foreach ($exe in @(Get-ChildItem -LiteralPath $distRelease -File -Force | Where-Object { $_.Name -match '^(?:test_|fuzz_).*\.exe$' })) {
            $findings.Add("NoTestBinariesInDistRelease: forbidden test/fuzz binary in dist/Release: $($exe.Name)")
        }
    }
    $retiredMicroFolders = @('api', 'layouts', 'certs', 'data', 'fixtures', 'logs')
    foreach ($folder in $retiredMicroFolders) {
        if (Test-Path -LiteralPath (Join-Path $RepoRoot $folder) -PathType Container) { $findings.Add("NoRetiredRootMicroFolders: forbidden retired root micro-folder: $folder") }
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot 'src') -PathType Container) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $RepoRoot -Directory -Force -ErrorAction SilentlyContinue)) {
            if ($dir.Name -notin $allowedRootDirs -and $dir.Name -notin $retiredMicroFolders -and $dir.Name -ne '.pytest_cache') {
                $findings.Add("RootDirectoryAllowlist: unexpected root directory: $($dir.Name)")
            }
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $RepoRoot -File -Force -ErrorAction SilentlyContinue)) {
            if ($file.Name -notin $allowedRootFiles -and $file.Extension -ine '.bat' -and $file.Name -ne '.beads.gate.lock') {
                $findings.Add("RootFileAllowlist: unexpected root file: $($file.Name)")
            }
        }
    }
    if ($null -ne $Violations) { foreach ($item in $findings) { $Violations.Add($item) } }
    return , $findings.ToArray()
}

function Invoke-SandalPublicCheck {
    [CmdletBinding()]
    [Alias('Invoke-SandalPublicChecks')]
    param(
        [Alias('RepoRoot', 'RootPath')]
        [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    )
    $violations = [System.Collections.Generic.List[string]]::new()
    Test-SandalTestsDirectoryBounds -RootPath $Root -Violations $violations
    Test-SandalGitTrackedAssets -RootPath $Root -Violations $violations
    Test-SandalContentLeaks -RootPath $Root -Violations $violations
    Test-SandalEnvSchemaTemplates -RootPath $Root -Violations $violations
    Test-SandalRetiredLegacyScripts -Root $Root -Violations $violations
    $null = Test-SandalRootDirectoryHierarchy -RepoRoot $Root -Violations $violations

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
