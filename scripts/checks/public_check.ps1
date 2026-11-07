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

function Add-SandalLocalConfigLeakPatterns {
    param([string]$CfgDir, [System.Collections.Generic.List[string]]$Patterns)
    if (-not (Test-Path -LiteralPath $CfgDir -PathType Container)) { return }
    foreach ($lf in @(Get-ChildItem -LiteralPath $CfgDir -Filter '*.local.json' -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        $tplPath = $lf.FullName -replace '\.local\.json$', '.json'
        $tplObj = if (Test-Path -LiteralPath $tplPath -PathType Leaf) { try { Get-Content -LiteralPath $tplPath -Raw | ConvertFrom-Json } catch { $null } } else { $null }
        $locObj = try { Get-Content -LiteralPath $lf.FullName -Raw | ConvertFrom-Json } catch { $null }
        if ($null -eq $locObj) { continue }
        foreach ($prop in $locObj.PSObject.Properties) {
            $val = [string]$prop.Value
            $tplVal = if ($null -ne $tplObj -and $null -ne $tplObj.PSObject.Properties[$prop.Name]) { [string]$tplObj.PSObject.Properties[$prop.Name].Value } else { '' }
            if ($val -and $val -ne $tplVal -and $val.Length -ge 8 -and $val -notmatch '^(?i)(?:localhost|127\.0\.0\.1|0\.0\.0\.0|CHANGE_ME)') { $Patterns.Add([regex]::Escape($val)) }
        }
    }
    foreach ($ef in @(Get-ChildItem -LiteralPath $CfgDir -Filter '*.local.env' -Recurse -File -Force -ErrorAction SilentlyContinue)) {
        foreach ($line in [System.IO.File]::ReadAllLines($ef.FullName)) {
            if ($line -notmatch '^\s*[A-Za-z_][A-Za-z0-9_]*\s*=\s*(.+)$') { continue }
            $val = $Matches[1].Trim().Trim('"', "'")
            if ($val.Length -ge 8 -and $val -notmatch '^(?i)(?:localhost|127\.0\.0\.1|0\.0\.0\.0|CHANGE_ME)') { $Patterns.Add([regex]::Escape($val)) }
        }
    }
}

function Get-SandalDynamicLeakRegex {
    param([Parameter(Mandatory)][string]$RootPath)
    $patterns = [System.Collections.Generic.List[string]]::new()
    $patterns.Add('(?i)[C-Z]:[/\\]+Users[/\\]+(?!(?:Public|Default|All Users|AliceVerra)[/\\])[A-Za-z0-9_.-]+[/\\]')
    foreach ($idVal in @($env:USERNAME, $env:COMPUTERNAME)) {
        if (-not [string]::IsNullOrWhiteSpace($idVal) -and $idVal.Length -ge 4 -and $idVal -notin @('root', 'user', 'admin', 'ubuntu', 'runner', 'docker', 'system')) {
            $patterns.Add("(?i)\b$([regex]::Escape($idVal))\b")
        }
    }
    Add-SandalLocalConfigLeakPatterns -CfgDir (Join-Path $RootPath 'src\config') -Patterns $patterns
    return ($patterns -join '|')
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
    $localLeakRegex = Get-SandalDynamicLeakRegex -RootPath $RootPath
    foreach ($entry in $tracked) {
        if ([string]::IsNullOrWhiteSpace($entry)) {
            continue
        }
        $norm = ([string]$entry).Replace('\', '/')
        if ($norm -match '^(?:dist|deploy|\.aoc)/') {
            $Violations.Add("Forbidden runtime or extracted path tracked in git: $norm")
        }
        if ($norm -match '(?:^|/)(?:content\.db|area_geom\.bin|area_nav\.bin|town_platforms\.bin|town_layouts\.bin|region_foliage\.tsv|pcap_resource_spawners\.tsv|npc_motion\.tsv|npc_abilities\.tsv|npc_montages\.tsv|npc_targeting\.tsv|npc_senses\.tsv|npc_aggro_curves\.tsv|npc_looks\.tsv|npc_appearances\.tsv|npc_names\.tsv|npc_trees\.tsv|mount_speeds\.tsv|mount_riders\.tsv|artisan_curves\.tsv|item_names\.tsv|roads\.tsv|roads_raw\.tsv|roads_levels\.tsv|town_layout_items\.tsv|DefaultCombat\.ini|DefaultAoCRecordConstants\.ini)$' -and $norm -notmatch '^src/server/services/cmd/contentc/') {
            $Violations.Add("Forbidden extracted game data file tracked in git: $norm")
        }
        if ($norm -match '\.(pak|utoc|ucas|pdb|dmp|pcapng)$') {
            $Violations.Add("Forbidden binary asset tracked in git: $norm")
        }
        if ($norm -like '*.tsv' -and $norm -notmatch '^src/server/services/cmd/contentc/(ability_names|montage_assets|montage_ids)\.tsv$') {
            $Violations.Add("Forbidden generated TSV file tracked in git: $norm")
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
    $repoUsmap = Join-Path $RootPath 'src\config\layouts\aoc.usmap'
    if (Test-Path -LiteralPath $repoUsmap -PathType Leaf) {
        $usmapLen = (Get-Item -LiteralPath $repoUsmap).Length
        if ($usmapLen -ne 2581027L) {
            $Violations.Add("Forbidden overridden aoc.usmap in src/config/layouts/aoc.usmap (expected 2581027 bytes, got $usmapLen)")
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
    $exampleText = [System.IO.File]::ReadAllText($examplePath)
    $expectedCount = 99
    $schemaKeys = @([regex]::Matches($exampleText, '(?m)^(?:AOC|SANDAL)_[A-Z0-9_]+(?==)') | ForEach-Object { $_.Value })
    if ($schemaKeys.Count -ne $expectedCount) {
        $Violations.Add("sandal.env.example expected $expectedCount keys, found $($schemaKeys.Count)")
    }
    $rootExamplePath = Join-Path $RootPath '.env.example'
    if (Test-Path -LiteralPath $rootExamplePath -PathType Leaf) {
        if ([System.IO.File]::ReadAllText($rootExamplePath) -cne $exampleText) {
            $Violations.Add("Root .env.example does not match src/config/sandal.env.example")
        }
    } elseif ($schemaKeys.Count -eq $expectedCount) {
        $Violations.Add("Missing root env template: $rootExamplePath")
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
            if ($rel -notin @('scripts/ensure_pwsh.bat', 'scripts/test.bat')) {
                $findings.Add("NoBatInScripts: forbidden .bat script in scripts/: $rel")
            }
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
    [CmdletBinding()]
    [Alias('Test-SandalMinimalBatSurface')]
    param(
        [Parameter(Mandatory)][Alias('RootPath', 'Root')][string]$RepoRoot,
        [AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations = $null
    )
    $findings = [System.Collections.Generic.List[string]]::new()
    $null = Test-SandalNoPythonInScripts -RootPath $RepoRoot -Violations $findings
    $allowedRootDirs = @('src', 'build', 'dist', 'deploy', 'docs', 'scripts', '.github')
    $allowedRootFiles = @('README.md', 'LICENSE', 'NOTICE', 'SECURITY.md', 'CONTRIBUTING.md', 'AGENTS.md', 'GEMINI.md', '.gitignore', '.gitattributes', '.graphifyignore', '.env', '.env.example', 'SandalLauncher.lnk', 'sandal.ps1', 'start.bat', 'stop.bat', 'build.bat')
    $stealthDirs = @('.git', '.agents', '.claude', '_workspace', '.beads', 'graphify-out')
    foreach ($bat in @(Get-ChildItem -LiteralPath $RepoRoot -Filter '*.bat' -File -Force -ErrorAction SilentlyContinue)) {
        if ($bat.Name -notin @('start.bat', 'stop.bat', 'build.bat')) {
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

function Test-SandalGitIdentityHygiene {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    if (-not (Test-Path -LiteralPath (Join-Path $RootPath '.git'))) {
        return
    }
    $email = [string](& git -C $RootPath config user.email 2>$null)
    $email = $email.Trim()
    if ($email -match '(?i)(?:@localhost$|^debug@)') {
        $Violations.Add("GitIdentityHygiene: forbidden placeholder git user.email configured: $email")
    }
}

function Test-SandalDocumentationRealityHygiene {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    $docFiles = [System.Collections.Generic.List[string]]::new()
    $rootReadme = Join-Path $RootPath 'README.md'
    if (Test-Path -LiteralPath $rootReadme -PathType Leaf) { $docFiles.Add($rootReadme) }
    $docsDir = Join-Path $RootPath 'docs'
    if (Test-Path -LiteralPath $docsDir -PathType Container) {
        foreach ($f in @(Get-ChildItem -LiteralPath $docsDir -Filter '*.md' -Recurse -File -ErrorAction SilentlyContinue)) { $docFiles.Add($f.FullName) }
    }
    $srcDir = Join-Path $RootPath 'src'
    if (Test-Path -LiteralPath $srcDir -PathType Container) {
        foreach ($f in @(Get-ChildItem -LiteralPath $srcDir -Filter 'README.md' -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch '[\\/]control-app[\\/]' })) { $docFiles.Add($f.FullName) }
    }
    $staleRegex = '(?:\bworldd\b|\bWORLDD\b|\bmeshgw\b|\bMESHGW\b|\bvcpkg\b|Visual Studio|Build Tools|\.NET 8\b|\bnet8\.0\b|an uncommitted draft|12-container|rpc_registered\.inc|session_sources\.cmake|hotspot_limits\.txt|check_hotspots\.ps1|looks\.py)'
    foreach ($docPath in $docFiles) {
        $content = [System.IO.File]::ReadAllText($docPath)
        $m = [regex]::Match($content, $staleRegex)
        if ($m.Success) {
            $rel = [System.IO.Path]::GetRelativePath($RootPath, $docPath).Replace('\', '/')
            $Violations.Add("DocumentationRealityHygiene: forbidden stale token '$($m.Value)' in $rel")
        }
    }
}

function Test-SandalNoVcpkgRemnants {
    param(
        [Parameter(Mandatory)][string]$RootPath,
        [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Violations
    )
    if (Test-Path -LiteralPath (Join-Path $RootPath 'src\vcpkg.json')) {
        $Violations.Add('NoVcpkgRemnants: forbidden src/vcpkg.json file must not exist')
    }
    foreach ($rel in @('src\CMakeLists.txt', 'src\CMakePresets.json', 'src\deploy\docker\cpp.Dockerfile')) {
        $p = Join-Path $RootPath $rel
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { continue }
        $text = [System.IO.File]::ReadAllText($p)
        if ($text -match '(?i)\bvcpkg\b') {
            $norm = $rel.Replace('\', '/')
            $Violations.Add("NoVcpkgRemnants: forbidden vcpkg reference in $norm")
        }
    }
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
    Test-SandalGitIdentityHygiene -RootPath $Root -Violations $violations
    Test-SandalDocumentationRealityHygiene -RootPath $Root -Violations $violations
    Test-SandalNoVcpkgRemnants -RootPath $Root -Violations $violations

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
