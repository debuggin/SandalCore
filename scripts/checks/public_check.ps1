#Requires -Version 7.2
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
        if ($lineCount -gt 900) {
            $Violations.Add("File in src/tests/ exceeds 900 lines ($lineCount): $($file.FullName)")
        }
    }
}

function Test-SandalGitTrackedAssets {
    [CmdletBinding()]
    [Alias('Test-SandalTrackedFiles')]
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $gitArgs = @('-C', $RootPath, 'ls-files')
    $tracked = @(& git @gitArgs)
    if ($LASTEXITCODE -ne 0) {
        throw "git ls-files failed with exit code $LASTEXITCODE"
    }
    $localLeakRegex = 'andis\.' + 'network|8a02a4030c447486' + '5cc02c73b73e6b3b4b1f83033c1149bd|zenith-cliff-' + 'marsh-spruce'
    foreach ($entry in $tracked) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            continue
        }
        $norm = ([string]$entry).Replace('\', '/')
        if ($norm -match '\.(pak|utoc|ucas|pdb|dmp|pcapng)$') {
            $Violations.Add("Forbidden binary asset tracked in git: $norm")
        }
        if ($norm -like '*.env' -or $norm -like '*.local.json' -or $norm -like '*.local.env' -or $norm -match '\.(key|crt|pfx|p12)$') {
            $Violations.Add("Forbidden secret or environment file tracked in git: $norm")
        }
        $fullPath = Join-Path $RootPath ($norm -replace '/', [System.IO.Path]::DirectorySeparatorChar)
        if ((Test-Path -LiteralPath $fullPath -PathType Leaf) -and $norm -notmatch '\.(png|jpg|ico|woff2?|ttf|usmap|bin|db|exe|dll|so|a|lib)$') {
            $content = [System.IO.File]::ReadAllText($fullPath)
            if ($content -match $localLeakRegex) {
                $Violations.Add("Forbidden local host or secret token tracked in git: $norm")
            }
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
    foreach ($stem in @('local-dev', 'docker-single', 'prod-mesh')) {
        $exCandidate = Join-Path $profilesDir "$stem.env.example"
        $envCandidate = Join-Path $profilesDir "$stem.env"
        $profilePath = if (Test-Path -LiteralPath $exCandidate -PathType Leaf) { $exCandidate } else { $envCandidate }
        if (-not (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
            $Violations.Add("Missing profile template: $exCandidate")
        } elseif ($stem -ieq 'prod-mesh' -and ([System.IO.File]::ReadAllText($profilePath) -match 'aoc:aoc@|sslmode=disable')) {
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

function Test-SandalNoPythonInScripts {
    param(
        [Parameter(Mandatory)][Alias('Root', 'RepoRoot')][string]$RootPath,
        [AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations = $null
    )
    $findings = [System.Collections.Generic.List[string]]::new()
    $scriptsDir = Join-Path $RootPath 'scripts'
    if (Test-Path -LiteralPath $scriptsDir -PathType Container) {
        foreach ($py in @(Get-ChildItem -LiteralPath $scriptsDir -Filter '*.py' -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            $rel = [System.IO.Path]::GetRelativePath($RootPath, $py.FullName).Replace('\', '/')
            $findings.Add("NoPythonInScripts: forbidden Python script in scripts/: $rel")
        }
        foreach ($bat in @(Get-ChildItem -LiteralPath $scriptsDir -Filter '*.bat' -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            $rel = [System.IO.Path]::GetRelativePath($RootPath, $bat.FullName).Replace('\', '/')
            $findings.Add("NoBatInScripts: forbidden .bat script in scripts/: $rel")
        }
    }
    if ($null -ne $Violations) { foreach ($item in $findings) { $Violations.Add($item) } }
    return , $findings.ToArray()
}

function Test-SandalInTreeBuildArtifacts {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Findings
    )
    foreach ($rootSub in @('src/tools', 'src/clients')) {
        $parentDir = Join-Path $RepoRoot ($rootSub -replace '/', [System.IO.Path]::DirectorySeparatorChar)
        if (-not (Test-Path -LiteralPath $parentDir -PathType Container)) { continue }
        foreach ($projDir in @(Get-ChildItem -LiteralPath $parentDir -Directory -Force | Where-Object { $_.Name -ine 'control-app' })) {
            if (Test-Path -LiteralPath (Join-Path $projDir.FullName 'bin') -PathType Container) { $Findings.Add("NoInTreeDotnetBinObj: forbidden in-tree bin directory: $rootSub/$($projDir.Name)/bin") }
            if (Test-Path -LiteralPath (Join-Path $projDir.FullName 'obj') -PathType Container) { $Findings.Add("NoInTreeDotnetBinObj: forbidden in-tree obj directory: $rootSub/$($projDir.Name)/obj") }
        }
    }
    $distRelease = Join-Path $RepoRoot 'dist\Release'
    if (Test-Path -LiteralPath $distRelease -PathType Container) {
        foreach ($exe in @(Get-ChildItem -LiteralPath $distRelease -File -Force | Where-Object { $_.Name -match '^(?:test_|fuzz_).*\.exe$' })) {
            $Findings.Add("NoTestBinariesInDistRelease: forbidden test/fuzz binary in dist/Release: $($exe.Name)")
        }
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot 'dist\tests') -PathType Container) {
        $Findings.Add('NoTestBinariesInDist: forbidden test directory in dist: dist/tests')
    }
    foreach ($sub in @('Release', 'Debug', 'launcher')) {
        $subDir = Join-Path $RepoRoot "dist\$sub"
        if (-not (Test-Path -LiteralPath $subDir -PathType Container)) { continue }
        foreach ($pdb in @(Get-ChildItem -LiteralPath $subDir -Filter '*.pdb' -File -Force -ErrorAction SilentlyContinue)) {
            $Findings.Add("NoPdbInDist: forbidden debug symbol file in dist: dist/$sub/$($pdb.Name)")
        }
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot 'deploy\docker-compose.yml') -PathType Leaf) {
        $Findings.Add('NoLegacyComposeInDeploy: forbidden legacy docker-compose.yml in deploy/')
    }
}

function Test-SandalRootDirectoryHierarchy {
    param(
        [Parameter(Mandatory)][Alias('RootPath', 'Root')][string]$RepoRoot,
        [AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations = $null
    )
    $findings = [System.Collections.Generic.List[string]]::new()
    $null = Test-SandalNoPythonInScripts -RootPath $RepoRoot -Violations $findings
    $allowedRootDirs = @('src', 'build', 'dist', 'deploy', 'docs', 'scripts', '.github')
    $allowedRootFiles = @('README.md', 'LICENSE', 'NOTICE', 'SECURITY.md', 'CONTRIBUTING.md', 'AGENTS.md', 'GEMINI.md', '.gitignore', '.gitattributes', '.graphifyignore', 'sandal.ps1', 'start.bat', 'stop.bat')
    $stealthDirs = @('.git', '.agents', '.claude', '_workspace', '.beads', 'graphify-out')
    foreach ($bat in @(Get-ChildItem -LiteralPath $RepoRoot -Filter '*.bat' -File -Force -ErrorAction SilentlyContinue)) {
        if ($bat.Name -notin @('start.bat', 'stop.bat')) {
            $findings.Add("NoRootBatFiles: forbidden root .bat file: $($bat.Name)")
        }
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot '.pytest_cache')) { $findings.Add('NoRootCacheOrGateLocks: forbidden root cache or lock artifact: .pytest_cache') }
    $gateLock = Join-Path $RepoRoot '.beads.gate.lock'
    if (Test-Path -LiteralPath $gateLock) {
        if (Test-Path -LiteralPath (Join-Path $RepoRoot '.beads') -PathType Container) { Remove-Item -LiteralPath $gateLock -Force -ErrorAction SilentlyContinue }
        else { $findings.Add('NoRootCacheOrGateLocks: forbidden root cache or lock artifact: .beads.gate.lock') }
    }
    Test-SandalInTreeBuildArtifacts -RepoRoot $RepoRoot -Findings $findings
    $retiredMicroFolders = @('api', 'layouts', 'certs', 'data', 'fixtures', 'logs')
    foreach ($folder in $retiredMicroFolders) {
        if (Test-Path -LiteralPath (Join-Path $RepoRoot $folder) -PathType Container) { $findings.Add("NoRetiredRootMicroFolders: forbidden retired root micro-folder: $folder") }
    }
    if (Test-Path -LiteralPath (Join-Path $RepoRoot 'src') -PathType Container) {
        foreach ($dir in @(Get-ChildItem -LiteralPath $RepoRoot -Directory -Force -ErrorAction SilentlyContinue)) {
            if ($dir.Name -notin $allowedRootDirs -and $dir.Name -notin $stealthDirs -and $dir.Name -notin $retiredMicroFolders -and $dir.Name -ne '.pytest_cache') {
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
