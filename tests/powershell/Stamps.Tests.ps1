#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($null -ne (Get-Module Pester) -and (Get-Module Pester).Version.Major -lt 5) {
    function Should {
        if ($args.Count -ge 1 -and "$($args[0])" -match '^-?Throw$') {
            $threw = $false
            foreach ($sbItem in $input) {
                try { & $sbItem *>$null } catch { $threw = $true }
            }
            if (-not $threw) { throw 'Expected expression to throw an exception.' }
            return
        }
        $mapped = [System.Collections.Generic.List[object]]::new()
        foreach ($a in $args) {
            if ($a -is [string] -and $a -eq '-BeTrue') {
                $mapped.AddRange([object[]]@('Be', $true))
            } elseif ($a -is [string] -and $a -eq '-BeFalse') {
                $mapped.AddRange([object[]]@('Be', $false))
            } elseif ($a -is [string] -and $a.StartsWith('-')) {
                $mapped.Add($a.Substring(1))
            } else {
                $mapped.Add($a)
            }
        }
        $input | Pester\Should @mapped
    }
}

Describe 'SandalCore Wave 03.1: sandal.ps1, SandalCore.psd1, Env.psm1 & 11-Stage Stamps.psm1 DAG' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
        $script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
        $script:SandalCliPath = Join-Path $script:RepoRoot 'sandal.ps1'
        Import-Module $script:ManifestPath -Force -DisableNameChecking

        function New-TestRepoFixture {
            $root = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-stamps-" + [guid]::NewGuid().ToString('N'))
            $files = @{
                'config/layouts/aoc.usmap'                 = 'usmap-v1'
                'tools/mapc/Program.cs'                    = 'class Mapc {}'
                'services/cmd/contentc/main.go'            = 'package main'
                'services/go.mod'                          = 'module sandal/services'
                'tools/schemagen/gen_zoi_surface.py'       = 'print("zoi")'
                'tools/schemagen/gen_region_rects.py'      = 'print("rects")'
                'tools/schemagen/gen_region.py'            = 'print("region")'
                'tools/schemagen/gen_node_constants.py'    = 'print("nodes")'
                'tools/schemagen/gen_gm_garden.py'         = 'print("garden")'
                'tools/schemagen/gen_tooltips.py'          = 'print("tooltips")'
                'tools/schemagen/gen_gear_fixture.py'      = 'print("gear")'
                'tools/schemagen/gen_talent_fixture.py'    = 'print("talent")'
                'tools/schemagen/gen_town_layouts.py'      = 'print("layouts")'
                'deploy/citynode_seed.json'                = '{"nodes":{"lionhold":{"level":3}}}'
                'libs/aocnav/nav.cpp'                      = 'int nav = 1;'
                'tools/navbuild/main.cpp'                  = 'int main() { return 0; }'
                'third_party/recastnavigation/recast.cpp'  = 'int rc = 1;'
                'vcpkg.json'                               = '{"name":"sandal"}'
            }
            foreach ($entry in $files.GetEnumerator()) {
                $full = Join-Path $root ($entry.Key -replace '/', [System.IO.Path]::DirectorySeparatorChar)
                $parent = Split-Path -Parent $full
                if (-not (Test-Path -LiteralPath $parent)) {
                    $null = New-Item -ItemType Directory -Path $parent -Force
                }
                [System.IO.File]::WriteAllText($full, $entry.Value, [System.Text.UTF8Encoding]::new($false))
            }
            return $root
        }
    }

    Context 'SandalCore.psd1 Module Manifest Contract' {
        It 'Validates module manifest metadata, PowerShellVersion 7.2, RootModule Stamps.psm1, and explicit exports' {
            $manifest = Import-PowerShellDataFile -LiteralPath $script:ManifestPath
            $manifest.RootModule | Should -Be 'Stamps.psm1'
            ($manifest.NestedModules -contains 'Env.psm1') | Should -BeTrue
            $manifest.PowerShellVersion | Should -Be '7.2'
            ($manifest.FunctionsToExport -contains '*') | Should -BeFalse
            ($manifest.FunctionsToExport -contains 'Get-SandalStageOrder') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Compute-SandalStageHash') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Test-SandalStageUpToDate') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Write-SandalStageStamp') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Invoke-WithHeavyOperationLock') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Initialize-SandalEnv') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Get-SandalEnvMap') | Should -BeTrue
            ($manifest.FunctionsToExport -contains 'Set-SandalSecretAcl') | Should -BeTrue
        }
    }

    Context 'Stamps.psm1 11-Stage Topological Order & Upstream Invalidation Propagation' {
        It 'Returns all 11 stages in valid topological order where every dependency precedes its dependent' {
            $order = @(Get-SandalStageOrder)
            $order.Count | Should -Be 11
            $graph = Get-SandalStageGraph
            $indexMap = @{}
            for ($i = 0; $i -lt $order.Count; $i++) {
                $indexMap[$order[$i]] = $i
            }
            foreach ($stage in $order) {
                $stageIdx = $indexMap[$stage]
                foreach ($dep in $graph[$stage].Upstream) {
                    $depIdx = $indexMap[$dep]
                    ($depIdx -lt $stageIdx) | Should -BeTrue
                }
            }
        }

        It 'Propagates upstream hash invalidation to all transitive downstream stages while preserving independent branches' {
            $tmpRepo = New-TestRepoFixture
            try {
                $order = @(Get-SandalStageOrder)
                $before = @{}
                foreach ($s in $order) {
                    $before[$s] = Compute-SandalStageHash -Stage $s -RepoRoot $tmpRepo
                    $before[$s] | Should -Match '^[0-9a-f]{64}$'
                    Write-SandalStageStamp -Stage $s -RepoRoot $tmpRepo -InputHash $before[$s] | Out-Null
                    (Test-SandalStageUpToDate -Stage $s -RepoRoot $tmpRepo) | Should -BeTrue
                }

                # Modify services/cmd/contentc/main.go (direct input to 'database' stage)
                $contentcFile = Join-Path $tmpRepo 'services\cmd\contentc\main.go'
                [System.IO.File]::WriteAllText($contentcFile, 'package main // v2', [System.Text.UTF8Encoding]::new($false))

                # Root stages 'config', 'curves', 'townfloors' do NOT depend on 'database'
                foreach ($unchanged in @('config', 'curves', 'townfloors')) {
                    $afterHash = Compute-SandalStageHash -Stage $unchanged -RepoRoot $tmpRepo
                    $afterHash | Should -Be $before[$unchanged]
                    (Test-SandalStageUpToDate -Stage $unchanged -RepoRoot $tmpRepo) | Should -BeTrue
                }

                # 'database' and all its transitive dependents ('region', 'headers', 'fixtures', 'navbuild', 'roads', 'townlayouts', 'navmesh') must invalidate
                foreach ($invalidated in @('database', 'region', 'headers', 'fixtures', 'navbuild', 'roads', 'townlayouts', 'navmesh')) {
                    $afterHash = Compute-SandalStageHash -Stage $invalidated -RepoRoot $tmpRepo
                    ($afterHash -ne $before[$invalidated]) | Should -BeTrue
                    (Test-SandalStageUpToDate -Stage $invalidated -RepoRoot $tmpRepo) | Should -BeFalse
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }

        It 'Computes deterministic SHA-256 stamps, enforces Test-SandalStageUpToDate idempotency, and ignores CMakeLists.txt in navbuild' {
            $tmpRepo = New-TestRepoFixture
            try {
                (Test-SandalStageUpToDate -Stage 'navbuild' -RepoRoot $tmpRepo) | Should -BeFalse
                $h1 = Compute-SandalStageHash -Stage 'navbuild' -RepoRoot $tmpRepo
                $h2 = Compute-SandalStageHash -Stage 'navbuild' -RepoRoot $tmpRepo
                $h1 | Should -Be $h2

                Write-SandalStageStamp -Stage 'navbuild' -RepoRoot $tmpRepo -InputHash $h1 | Out-Null
                (Test-SandalStageUpToDate -Stage 'navbuild' -RepoRoot $tmpRepo) | Should -BeTrue

                # Adding/editing CMakeLists.txt inside libs/aocnav must NOT invalidate navbuild (SC-02)
                $cmakePath = Join-Path $tmpRepo 'libs\aocnav\CMakeLists.txt'
                [System.IO.File]::WriteAllText($cmakePath, 'add_library(aocnav STATIC nav.cpp)', [System.Text.UTF8Encoding]::new($false))
                (Test-SandalStageUpToDate -Stage 'navbuild' -RepoRoot $tmpRepo) | Should -BeTrue

                # Editing libs/aocnav/nav.cpp MUST invalidate navbuild and downstream navmesh
                $navCpp = Join-Path $tmpRepo 'libs\aocnav\nav.cpp'
                [System.IO.File]::WriteAllText($navCpp, 'int nav = 2;', [System.Text.UTF8Encoding]::new($false))
                (Test-SandalStageUpToDate -Stage 'navbuild' -RepoRoot $tmpRepo) | Should -BeFalse
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Invoke-WithHeavyOperationLock Mutual Exclusion, Timeout & Abandoned Mutex Recovery' {
        It 'Enforces mutual exclusion, throws TimeoutException on contention timeout, and recovers abandoned mutexes' {
            $testMutexName = 'Global\SandalCore_HeavyOperation_Lock_Test_' + [guid]::NewGuid().ToString('N')
            $tmpLockFile = Join-Path ([System.IO.Path]::GetTempPath()) ("heavy-lock-" + [guid]::NewGuid().ToString('N') + ".json")
            try {
                $observed = Invoke-WithHeavyOperationLock -Operation 'pester-op' -MutexName $testMutexName -LockFilePath $tmpLockFile -TimeoutMs 2000 -ScriptBlock {
                    Get-Content -LiteralPath $tmpLockFile -Raw | ConvertFrom-Json
                }
                [int]$observed.pid | Should -Be $PID
                $observed.operation | Should -Be 'pester-op'
                (Test-Path -LiteralPath $tmpLockFile) | Should -BeFalse

                # Hold mutex on a dedicated background OS thread to test contention timeout
                if (-not ('SandalMutexTestHelper' -as [type])) {
                    Add-Type -TypeDefinition @'
using System;
using System.Threading;
public static class SandalMutexTestHelper {
    public static Thread StartHolder(string name, ManualResetEventSlim ready, ManualResetEventSlim release) {
        var t = new Thread(() => {
            using (var m = new Mutex(false, name)) {
                m.WaitOne(5000);
                ready.Set();
                release.Wait(5000);
                m.ReleaseMutex();
            }
        }) { IsBackground = true };
        t.Start();
        return t;
    }
    public static void AbandonMutex(string name, out Mutex leakedHandle) {
        Mutex captured = null;
        var t = new Thread(() => {
            captured = new Mutex(false, name);
            captured.WaitOne(5000);
        }) { IsBackground = true };
        t.Start();
        t.Join(5000);
        leakedHandle = captured;
    }
}
'@
                }
                $readyEvent = [System.Threading.ManualResetEventSlim]::new($false)
                $releaseEvent = [System.Threading.ManualResetEventSlim]::new($false)
                $holderThread = [SandalMutexTestHelper]::StartHolder($testMutexName, $readyEvent, $releaseEvent)
                $null = $readyEvent.Wait(5000)

                $timedOut = $false
                try {
                    Invoke-WithHeavyOperationLock -Operation 'contended-op' -MutexName $testMutexName -LockFilePath $tmpLockFile -TimeoutMs 60 -ScriptBlock { 'unreachable' }
                } catch [System.TimeoutException] {
                    $timedOut = $true
                } finally {
                    $releaseEvent.Set()
                    $holderThread.Join(5000) | Out-Null
                    $readyEvent.Dispose()
                    $releaseEvent.Dispose()
                }
                $timedOut | Should -BeTrue

                # Test deterministic abandoned-mutex recovery when an owning OS thread exits without ReleaseMutex()
                $abandonedMutexName = 'Global\SandalCore_Abandoned_Test_' + [guid]::NewGuid().ToString('N')
                $leaked = $null
                [SandalMutexTestHelper]::AbandonMutex($abandonedMutexName, [ref]$leaked)
                try {
                    $recoveredResult = Invoke-WithHeavyOperationLock -Operation 'recover-op' -MutexName $abandonedMutexName -LockFilePath $tmpLockFile -TimeoutMs 1000 -ScriptBlock {
                        'recovered-ok'
                    }
                    $recoveredResult | Should -Be 'recovered-ok'
                } finally {
                    if ($null -ne $leaked) { $leaked.Dispose() }
                }
            } finally {
                if (Test-Path -LiteralPath $tmpLockFile) {
                    Remove-Item -LiteralPath $tmpLockFile -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Env.psm1 CSPRNG Entropy, .env Idempotency & ACL Hardening (SEC-01)' {
        It 'Generates 32-byte (64-hex-char) CSPRNG secrets, preserves existing non-empty secrets idempotently, and hardens file ACLs' {
            $s1 = New-SandalCsprngSecret -ByteLength 32
            $s2 = New-SandalCsprngSecret -ByteLength 32
            $s1 | Should -Match '^[0-9a-f]{64}$'
            $s2 | Should -Match '^[0-9a-f]{64}$'
            ($s1 -ne $s2) | Should -BeTrue

            $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-env-" + [guid]::NewGuid().ToString('N'))
            $envFile = Join-Path $tmpDir 'sandal.env'
            try {
                $null = New-Item -ItemType Directory -Path $tmpDir -Force
                $initialContent = @(
                    '# Header comment preserved',
                    'AOC_ENV=development',
                    'AOC_SERVICE_TOKEN=',
                    'POSTGRES_PASSWORD=existing_non_empty_secret_value'
                )
                [System.IO.File]::WriteAllLines($envFile, $initialContent, [System.Text.UTF8Encoding]::new($false))

                $init1 = Initialize-SandalEnv -EnvPath $envFile -SecretKeys @('AOC_SERVICE_TOKEN', 'POSTGRES_PASSWORD', 'SANDAL_SECRET_KEY')
                $map1 = Get-SandalEnvMap -EnvPath $envFile
                $map1['AOC_ENV'] | Should -Be 'development'
                $map1['AOC_SERVICE_TOKEN'] | Should -Match '^[0-9a-f]{64}$'
                $map1['SANDAL_SECRET_KEY'] | Should -Match '^[0-9a-f]{64}$'
                $map1['POSTGRES_PASSWORD'] | Should -Be 'existing_non_empty_secret_value'
                $init1.Acl.InheritanceDisabled | Should -BeTrue

                # Second invocation without -Force must preserve generated secrets idempotently
                $tokenBefore = $map1['AOC_SERVICE_TOKEN']
                $keyBefore = $map1['SANDAL_SECRET_KEY']
                $null = Initialize-SandalEnv -EnvPath $envFile -SecretKeys @('AOC_SERVICE_TOKEN', 'POSTGRES_PASSWORD', 'SANDAL_SECRET_KEY')
                $map2 = Get-SandalEnvMap -EnvPath $envFile
                $map2['AOC_SERVICE_TOKEN'] | Should -Be $tokenBefore
                $map2['SANDAL_SECRET_KEY'] | Should -Be $keyBefore
                $map2['POSTGRES_PASSWORD'] | Should -Be 'existing_non_empty_secret_value'

                $rawEnv = [System.IO.File]::ReadAllText($envFile, [System.Text.Encoding]::UTF8)
                $rawEnv | Should -Match '# Header comment preserved'
                if ($IsWindows) {
                    $acl = Get-Acl -LiteralPath $envFile
                    $acl.AreAccessRulesProtected | Should -BeTrue
                }
            } finally {
                if (Test-Path -LiteralPath $tmpDir) {
                    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'sandal.ps1 CLI Entrypoint Help, Status & Exit-Code Contracts (UX-07, UX-08)' {
        It 'Returns exit code 0 on help, exit code 0 with valid JSON on status -Json, and exit code 2 on invalid usage' {
            $helpOut = @(& pwsh -NoProfile -File $script:SandalCliPath help 2>&1 | ForEach-Object { "$_" })
            $LASTEXITCODE | Should -Be 0
            ($helpOut -join "`n") | Should -Match 'Usage:.*sandal\.ps1'

            $statusOut = @(& pwsh -NoProfile -File $script:SandalCliPath status -Json -NoColor 2>&1 | ForEach-Object { "$_" })
            $LASTEXITCODE | Should -Be 0
            $statusObj = ($statusOut -join "`n") | ConvertFrom-Json
            $statusObj.command | Should -Be 'status'
            $statusObj.stages.Count | Should -Be 11

            $badOut = @(& pwsh -NoProfile -File $script:SandalCliPath unknown-subcommand 2>&1 | ForEach-Object { "$_" })
            $LASTEXITCODE | Should -Be 2
            ($badOut -join "`n") | Should -Match 'Exit 2'
        }
    }
}
