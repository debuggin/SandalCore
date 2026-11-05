#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inPester = ($null -ne (Get-Module -Name Pester)) -and (
    @(Get-PSCallStack | Where-Object {
        ($null -ne $_.Command -and $_.Command -match 'Pester') -or
        ($null -ne $_.ScriptName -and $_.ScriptName -match 'Pester')
    }).Count -gt 0
)

if (-not $inPester) {
    function Describe([string]$Name, [scriptblock]$Fixture) {
        Write-Host "[Describe] $Name"
        & $Fixture
    }
    function Context([string]$Name, [scriptblock]$Fixture) {
        Write-Host "  [Context] $Name"
        & $Fixture
    }
    function BeforeAll([scriptblock]$Action) {
        & $Action
    }
    function It([string]$Name, [scriptblock]$Test) {
        & $Test
        Write-Host "    [PASS] $Name"
    }
}

Describe 'SandalCore Public Hygiene Check, Heavy Lock & Nav Stamp Decoupling' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
        $script:HeavyLockScript = Join-Path $script:RepoRoot 'scripts\checks\heavy_lock.ps1'
        $script:ExtractGameDataScript = Join-Path $script:RepoRoot 'scripts\extract_game_data.ps1'
    }

    Context 'public_check.ps1 Repository Verification' {
        It 'exits 0 and emits [PASS] public_check.ps1 verified on the repository' {
            $output = & pwsh -NoProfile -File $script:PublicCheckScript
            $exitCode = $LASTEXITCODE
            if ($exitCode -ne 0) {
                throw "Expected public_check.ps1 to exit 0, got $exitCode. Output: $($output -join ' ')"
            }
            $joined = ($output -join "`n")
            if ($joined -notmatch '\[PASS\] public_check\.ps1 verified') {
                throw "Expected '[PASS] public_check.ps1 verified' in output, got: $joined"
            }
        }

        It 'rejects forbidden .inc files, PEM headers, and PE32+ image-base literals in an isolated fixture' {
            $tmpRepo = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-pubcheck-" + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo 'tests') -Force
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo 'src\tests') -Force
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo 'scripts\checks') -Force
                & git -C $tmpRepo init -q
                if ($LASTEXITCODE -ne 0) {
                    throw "git init failed with exit code $LASTEXITCODE"
                }
                $pemHeader = 'BEGIN ' + 'PRIVATE KEY'
                $peLiteral = '0x140' + '001234'
                Set-Content -LiteralPath (Join-Path $tmpRepo 'tests\sample.inc') -Value "$pemHeader`n$peLiteral" -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\tests\sample.inc') -Value "$pemHeader`n$peLiteral" -Encoding utf8
                $out = & pwsh -NoProfile -File $script:PublicCheckScript -RepoRoot $tmpRepo
                $code = $LASTEXITCODE
                if ($code -ne 1) {
                    throw "Expected public_check.ps1 to exit 1 on contaminated fixture, got $code. Output: $($out -join ' ')"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'heavy_lock.ps1 Named Mutex & Lockfile Lifecycle' {
        It 'executes a scriptblock under Global\SandalCore_HeavyOperation_Lock and writes/cleans dist/.stamps/heavy_lock.json' {
            $lockFile = Join-Path $script:RepoRoot 'dist\.stamps\heavy_lock.json'
            $stampDir = Split-Path -Parent $lockFile
            $stampDirExisted = Test-Path -LiteralPath $stampDir
            try {
                if (-not (Test-Path -LiteralPath $stampDir)) {
                    $null = New-Item -ItemType Directory -Path $stampDir -Force
                }
                $stalePayload = [ordered]@{
                    pid            = 999999
                    operation      = 'stale-op'
                    started_at_utc = [datetime]::UtcNow.AddMinutes(-10).ToString('o')
                    command_line   = 'stale'
                } | ConvertTo-Json
                Set-Content -LiteralPath $lockFile -Value $stalePayload -Encoding utf8

                $state = @{ ObservedLock = $null }
                & $script:HeavyLockScript -Operation 'unit-test-op' -Action {
                    if (-not (Test-Path -LiteralPath $lockFile)) {
                        throw 'Expected dist/.stamps/heavy_lock.json to exist while Action is running.'
                    }
                    $state.ObservedLock = Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json
                }

                if ($null -eq $state.ObservedLock) {
                    throw 'Expected Action scriptblock to capture heavy_lock.json content.'
                }
                if ([int]$state.ObservedLock.pid -ne $PID) {
                    throw "Expected lockfile pid $PID, got $($state.ObservedLock.pid)"
                }
                if ($state.ObservedLock.operation -ne 'unit-test-op') {
                    throw "Expected lockfile operation 'unit-test-op', got '$($state.ObservedLock.operation)'"
                }
                if (Test-Path -LiteralPath $lockFile) {
                    throw 'Expected dist/.stamps/heavy_lock.json to be cleaned up after heavy_lock.ps1 completes.'
                }
            } finally {
                if ((-not $stampDirExisted) -and (Test-Path -LiteralPath $stampDir) -and @(Get-ChildItem -LiteralPath $stampDir -Force).Count -eq 0) {
                    Remove-Item -LiteralPath $stampDir -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'extract_game_data.ps1 Get-CodeNav CMakeLists Decoupling (SC-02)' {
        It 'ensures Get-CodeNav does not include CMakeLists.txt or src\server\world\CMakeLists.txt' {
            $content = Get-Content -LiteralPath $script:ExtractGameDataScript -Raw
            $match = [regex]::Match($content, '(?m)^\s*function\s+Get-CodeNav\s*\{[^\r\n]+\}')
            if (-not $match.Success) {
                throw 'Could not locate function Get-CodeNav in scripts/extract_game_data.ps1'
            }
            $line = $match.Value
            if ($line -match 'CMakeLists\.txt') {
                throw "Expected Get-CodeNav not to reference CMakeLists.txt, but found: $line"
            }
            if ($line -notmatch '(?:server\\world\\nav|libs[\\/]aocnav)' -or $line -notmatch 'third_party[\\/]recastnavigation' -or $line -notmatch 'vcpkg\.json') {
                throw "Expected Get-CodeNav to retain nav, recastnavigation, and vcpkg.json inputs, got: $line"
            }
        }
    }
}

Describe 'Test-SandalRootDirectoryHierarchy' {
    $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
    $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
    . $script:PublicCheckScript

    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
        . $script:PublicCheckScript
    }

    It 'returns 0 findings on the live repository' {
        $findings = Test-SandalRootDirectoryHierarchy -RepoRoot $script:RepoRoot
        if (@($findings).Count -ne 0) {
            throw "Expected 0 root hierarchy findings on live repository, got $(@($findings).Count): $($findings -join '; ')"
        }
    }

    It 'catches stray root .bat files, scripts/*.bat files, and orphaned root cache/lock artifacts while allowing root start.bat and stop.bat' {
        foreach ($canonicalBat in @('start.bat', 'stop.bat', 'dist\launch_launcher.bat')) {
            if (-not (Test-Path -LiteralPath (Join-Path $script:RepoRoot $canonicalBat) -PathType Leaf)) {
                throw "Expected canonical .bat file '$canonicalBat' to exist."
            }
        }
        if (Test-Path -LiteralPath (Join-Path $script:RepoRoot 'scripts\launch_all.bat')) {
            throw 'Expected scripts/launch_all.bat to be removed.'
        }
        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-bat-' + [guid]::NewGuid().ToString('N'))
        try {
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'scripts') -Force
            Set-Content -LiteralPath (Join-Path $tmpRoot 'start.bat') -Value '@echo off' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'stop.bat') -Value '@echo off' -Encoding utf8
            $cleanFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            if (@($cleanFindings).Count -ne 0) {
                throw "Expected 0 findings when start.bat and stop.bat exist on temp root, got $(@($cleanFindings).Count): $($cleanFindings -join '; ')"
            }
            Set-Content -LiteralPath (Join-Path $tmpRoot 'stray_root.bat') -Value '@echo off' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'scripts\launch_all.bat') -Value '@echo off' -Encoding utf8
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot '.pytest_cache') -Force
            Set-Content -LiteralPath (Join-Path $tmpRoot '.beads.gate.lock') -Value '' -Encoding utf8
            $findings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            $joined = $findings -join "`n"
            foreach ($expectedPattern in @(
                'NoRootBatFiles:.*stray_root\.bat',
                'NoBatInScripts:.*scripts/launch_all\.bat',
                'NoRootCacheOrGateLocks:.*\.pytest_cache',
                'NoRootCacheOrGateLocks:.*\.beads\.gate\.lock'
            )) {
                if ($joined -notmatch $expectedPattern) {
                    throw "Expected findings to match '$expectedPattern', got: $joined"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    It 'catches resurrected root micro-folders (api, layouts, certs, data, fixtures, logs)' {
        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-micro-' + [guid]::NewGuid().ToString('N'))
        try {
            $null = New-Item -ItemType Directory -Path $tmpRoot -Force
            foreach ($micro in @('api', 'layouts', 'certs', 'data', 'fixtures', 'logs')) {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot $micro) -Force
            }
            $findings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            $joined = $findings -join "`n"
            foreach ($micro in @('api', 'layouts', 'certs', 'data', 'fixtures', 'logs')) {
                if ($joined -notmatch "NoRetiredRootMicroFolders:.*$micro") {
                    throw "Expected findings to match micro-folder '$micro', got: $joined"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    It 'catches in-tree tools/launcher bin/obj, test_/fuzz_ binaries, dist/tests, .pdb files in dist, and legacy deploy/docker-compose.yml' {
        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-bin-' + [guid]::NewGuid().ToString('N'))
        try {
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'tools\foo\bin') -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'launcher\bar\obj') -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'src\tools\foo\bin') -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'src\clients\bar\obj') -Force
            $distRel = Join-Path $tmpRoot 'dist\Release'
            $null = New-Item -ItemType Directory -Path $distRel -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'dist\tests') -Force
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'deploy') -Force
            Set-Content -LiteralPath (Join-Path $distRel 'test_foo.exe') -Value 'MZ' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $distRel 'fuzz_bar.exe') -Value 'MZ' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $distRel 'world.pdb') -Value 'PDB' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'deploy\docker-compose.yml') -Value 'version: 3' -Encoding utf8
            $findings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            $joined = $findings -join "`n"
            foreach ($expectedPattern in @(
                'NoInTreeDotnetBinObj:.*(?:src/)?tools/foo/bin',
                'NoInTreeDotnetBinObj:.*(?:src/clients|launcher)/bar/obj',
                'NoTestBinariesInDistRelease:.*test_foo\.exe',
                'NoTestBinariesInDistRelease:.*fuzz_bar\.exe',
                'NoTestBinariesInDist:.*dist/tests',
                'NoPdbInDist:.*dist/Release/world\.pdb',
                'NoLegacyComposeInDeploy:.*deploy/'
            )) {
                if ($joined -notmatch $expectedPattern) {
                    throw "Expected findings to match '$expectedPattern', got: $joined"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    It 'accepts deploy as a valid 4-Tier root directory and rejects root .env, CMakeLists.txt, and .beads.gate.lock (category = root-layout)' {
        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-4tier-' + [guid]::NewGuid().ToString('N'))
        try {
            foreach ($tierDir in @('src', 'build', 'dist', 'deploy', 'docs', 'scripts', '.github')) {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot $tierDir) -Force
            }
            Set-Content -LiteralPath (Join-Path $tmpRoot 'README.md') -Value '# SandalCore' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'sandal.ps1') -Value '# CLI' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'deploy\.env') -Value 'AOC_ENV=dev' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'deploy\compose.yml') -Value 'name: sandalcore' -Encoding utf8

            $validViolations = [System.Collections.Generic.List[string]]::new()
            $validFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot -Violations $validViolations
            if (@($validFindings).Count -ne 0 -or $validViolations.Count -ne 0) {
                throw "Expected 0 findings when 4-Tier root directories including deploy are present, got: $($validFindings -join '; ')"
            }

            Set-Content -LiteralPath (Join-Path $tmpRoot '.env') -Value 'POSTGRES_PASSWORD=leaked' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot 'CMakeLists.txt') -Value 'cmake_minimum_required(VERSION 3.28)' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $tmpRoot '.beads.gate.lock') -Value 'locked' -Encoding utf8
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'stray_root_folder') -Force

            $negViolations = [System.Collections.Generic.List[string]]::new()
            $negFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot -Violations $negViolations
            $joined = $negFindings -join "`n"
            if ($joined -match 'RootDirectoryAllowlist:.*deploy') {
                throw "Expected deploy to be accepted as a valid 4-Tier root directory, got: $joined"
            }
            foreach ($expectedPattern in @(
                'RootFileAllowlist: unexpected root file: \.env',
                'RootFileAllowlist: unexpected root file: CMakeLists\.txt',
                'NoRootCacheOrGateLocks: forbidden root cache or lock artifact: \.beads\.gate\.lock',
                'RootDirectoryAllowlist: unexpected root directory: stray_root_folder'
            )) {
                if ($joined -notmatch $expectedPattern) {
                    throw "Expected root-layout finding matching '$expectedPattern', got: $joined"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    It 'verifies .editorconfig resides strictly at Tier 1 src/.editorconfig and rejects a stray root .editorconfig' {
        $rootEditorConfig = Join-Path $script:RepoRoot '.editorconfig'
        $srcEditorConfig = Join-Path $script:RepoRoot 'src\.editorconfig'
        if (Test-Path -LiteralPath $rootEditorConfig) {
            throw "Expected .editorconfig to NOT exist at repository root: $rootEditorConfig"
        }
        if (-not (Test-Path -LiteralPath $srcEditorConfig -PathType Leaf)) {
            throw "Expected Tier 1 .editorconfig to exist at: $srcEditorConfig"
        }

        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-editorcfg-' + [guid]::NewGuid().ToString('N'))
        try {
            $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'src') -Force
            Set-Content -LiteralPath (Join-Path $tmpRoot 'src\.editorconfig') -Value 'root = true' -Encoding utf8
            $cleanFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            if (@($cleanFindings).Count -ne 0) {
                throw "Expected 0 findings when .editorconfig resides in src/.editorconfig, got: $($cleanFindings -join '; ')"
            }

            Set-Content -LiteralPath (Join-Path $tmpRoot '.editorconfig') -Value 'root = true' -Encoding utf8
            $strayFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            $joined = $strayFindings -join "`n"
            if ($joined -notmatch 'RootFileAllowlist: unexpected root file: \.editorconfig') {
                throw "Expected Test-SandalRootDirectoryHierarchy to reject stray root .editorconfig, got: $joined"
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }

    It 'enforces NoPythonInScripts by rejecting any *.py file inside scripts/' {
        $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-hierarchy-nopy-' + [guid]::NewGuid().ToString('N'))
        try {
            $scriptsDir = Join-Path $tmpRoot 'scripts\sub'
            $null = New-Item -ItemType Directory -Path $scriptsDir -Force
            Set-Content -LiteralPath (Join-Path $tmpRoot 'scripts\nav_inputs.ps1') -Value '# ok' -Encoding utf8
            $cleanFindings = Test-SandalNoPythonInScripts -RootPath $tmpRoot
            if (@($cleanFindings).Count -ne 0) {
                throw "Expected 0 NoPythonInScripts findings when scripts/ contains only .ps1, got: $($cleanFindings -join '; ')"
            }

            Set-Content -LiteralPath (Join-Path $tmpRoot 'scripts\nav_inputs.py') -Value 'print(1)' -Encoding utf8
            Set-Content -LiteralPath (Join-Path $scriptsDir 'helper.py') -Value 'print(2)' -Encoding utf8
            $pyFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRoot
            $joined = $pyFindings -join "`n"
            foreach ($expectedPattern in @(
                'NoPythonInScripts:.*scripts/nav_inputs\.py',
                'NoPythonInScripts:.*scripts/sub/helper\.py'
            )) {
                if ($joined -notmatch $expectedPattern) {
                    throw "Expected findings to match '$expectedPattern', got: $joined"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $tmpRoot) { Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue }
        }
    }
}
