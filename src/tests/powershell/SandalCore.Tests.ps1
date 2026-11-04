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

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
$script:WslInstallScript = Join-Path $script:RepoRoot 'scripts\sandal\Install-WslDocker.ps1'
$script:HeavyLockScript = Join-Path $script:RepoRoot 'scripts\checks\heavy_lock.ps1'
$script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
$script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
Import-Module $script:ManifestPath -Force -DisableNameChecking
. $script:HeavyLockScript
. $script:PublicCheckScript

Describe 'SandalCore CLI Dispatcher, WSL Docker Preflight, Heavy Lock & Legacy Script Retirement' {
    BeforeAll {
        Import-Module $script:ManifestPath -Force -DisableNameChecking
        . $script:HeavyLockScript
        . $script:PublicCheckScript
    }

    Context 'Case 1: sandal.ps1 Command Dispatcher, Help, DryRun JSON Envelopes & Exit Codes' {
        It 'dispatches help, doctor, status, lifecycle DryRun JSON commands (Exit 0), and rejects unknown verbs (Exit 2)' {
            $helpOut = (@(& pwsh -NoProfile -File $script:SandalCli help) -join "`n")
            if ($LASTEXITCODE -ne 0 -or $helpOut -notmatch 'Usage: pwsh \./sandal\.ps1') {
                throw "Expected sandal.ps1 help to exit 0 with usage banner, got exit=$LASTEXITCODE out=$helpOut"
            }

            $docJson = (@(& pwsh -NoProfile -File $script:SandalCli doctor -DryRun -Json) -join "`n") | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $docJson.ok -or $docJson.command -ne 'doctor' -or -not $docJson.dry_run -or $docJson.tier_count -ne 6) {
                throw "Unexpected doctor -DryRun -Json payload: $($docJson | ConvertTo-Json -Compress)"
            }
            if (@($docJson.toolchains).Count -ne 5 -or @($docJson.ports).Count -ne 5) {
                throw "Expected 5 toolchains and 5 port checks in doctor output."
            }

            foreach ($verb in @('up', 'down', 'extract', 'certs', 'build', 'deploy', 'test', 'run')) {
                $envOut = (@(& pwsh -NoProfile -File $script:SandalCli $verb -DryRun -Json) -join "`n") | ConvertFrom-Json
                if ($LASTEXITCODE -ne 0 -or -not $envOut.ok -or $envOut.command -ne $verb -or -not $envOut.dry_run) {
                    throw "Expected sandal.ps1 $verb -DryRun -Json to exit 0 with dry_run=true, got: $($envOut | ConvertTo-Json -Compress)"
                }
            }

            $badOut = (@(& pwsh -NoProfile -File $script:SandalCli invalid-subcommand 2>&1 | ForEach-Object { "$_" }) -join "`n")
            if ($LASTEXITCODE -ne 2 -or $badOut -notmatch 'What:.*Why:.*How-to-fix:.*\(Exit 2\)') {
                throw "Expected unknown subcommand to exit 2 with What/Why/How-to-fix diagnostic, got exit=$LASTEXITCODE out=$badOut"
            }
        }

        It 'executes the turnkey CLI chain (build, deploy, test, run, up, down) in DryRun JSON mode with valid payloads and 0 root .env' {
            $chainResults = @{}
            foreach ($verb in @('build', 'deploy', 'test', 'run', 'up', 'down')) {
                $rawJson = (@(& pwsh -NoProfile -File $script:SandalCli $verb -DryRun -Json) -join "`n")
                if ($LASTEXITCODE -ne 0) {
                    throw "Expected sandal.ps1 $verb -DryRun -Json to exit 0, got $LASTEXITCODE. Output: $rawJson"
                }
                $parsed = $rawJson | ConvertFrom-Json
                if (-not $parsed.ok -or $parsed.command -ne $verb -or -not $parsed.dry_run -or $parsed.status -ne 'dry-run') {
                    throw "Unexpected DryRun JSON envelope for '$verb': $rawJson"
                }
                $chainResults[$verb] = $parsed
            }
            if (@($chainResults['build'].steps) -notcontains 'sync-dist' -or @($chainResults['build'].steps) -notcontains 'sync-deploy') {
                throw 'Expected build -DryRun -Json steps to include sync-dist and sync-deploy.'
            }
            if ($chainResults['deploy'].compose_file -notmatch 'deploy[\\/]compose\.yml$' -or $chainResults['deploy'].env_path -notmatch 'deploy[\\/]\.env$') {
                throw "Expected deploy -DryRun -Json paths to target deploy/compose.yml and deploy/.env, got compose=$($chainResults['deploy'].compose_file) env=$($chainResults['deploy'].env_path)"
            }
            if (@($chainResults['test'].steps) -notcontains 'sync-dist' -or @($chainResults['test'].steps) -notcontains 'run-tests') {
                throw 'Expected test -DryRun -Json steps to include sync-dist and run-tests.'
            }
            if (($chainResults['run'].pipeline -join ',') -ne 'extract,build,deploy,up') {
                throw "Expected run -DryRun -Json pipeline 'extract,build,deploy,up', got '$($chainResults['run'].pipeline -join ',')'"
            }
            if ($chainResults['up'].compose_file -notmatch 'deploy[\\/]compose\.yml$' -or $chainResults['down'].compose_file -notmatch 'deploy[\\/]compose\.yml$') {
                throw 'Expected up and down -DryRun -Json compose_file to target deploy/compose.yml.'
            }
            if (Test-Path -LiteralPath (Join-Path $script:RepoRoot '.env')) {
                throw 'Expected 0 .env file in repository root after turnkey CLI DryRun chain.'
            }
        }
    }

    Context 'Case 2: Install-WslDocker.ps1 DryRun Preflight & Canonical TCP 2375 Contract' {
        It 'returns a deterministic DryRun preflight plan with tcp://[::1]:2375 without mutating Process DOCKER_HOST' {
            $prevDockerHost = [Environment]::GetEnvironmentVariable('DOCKER_HOST', 'Process')
            try {
                [Environment]::SetEnvironmentVariable('DOCKER_HOST', 'sentinel-dryrun-unchanged', 'Process')
                $planRaw = (@(& $script:WslInstallScript -DryRun -Json) -join "`n")
                $afterDockerHost = [Environment]::GetEnvironmentVariable('DOCKER_HOST', 'Process')
            } finally {
                [Environment]::SetEnvironmentVariable('DOCKER_HOST', $prevDockerHost, 'Process')
            }
            $plan = $planRaw | ConvertFrom-Json
            if (-not $plan.ok -or $plan.command -ne 'install-wsl-docker' -or -not $plan.dry_run) {
                throw "Unexpected Install-WslDocker DryRun envelope: $planRaw"
            }
            if ($plan.distro -notmatch '^Ubuntu(-24\.04)?$' -or $plan.socket -ne '/var/run/docker.sock' -or $plan.tcp_2375 -ne $true -or $plan.docker_host -ne 'tcp://[::1]:2375') {
                throw "Expected Ubuntu(-24.04), socket=/var/run/docker.sock, tcp_2375=true, and docker_host=tcp://[::1]:2375, got: $planRaw"
            }
            if ($plan.wsl_config.Action -ne 'DryRun' -or $plan.engine.Action -ne 'DryRun') {
                throw "Expected wsl_config.Action and engine.Action to be DryRun, got wsl=$($plan.wsl_config.Action) engine=$($plan.engine.Action)"
            }
            if ($afterDockerHost -ne 'sentinel-dryrun-unchanged') {
                throw "Expected -DryRun not to mutate Process DOCKER_HOST, got '$afterDockerHost'"
            }
        }
    }

    Context 'Case 3: heavy_lock.ps1 Mutex Acquisition, Inner Return Value & Contention Timeout' {
        It 'acquires mutex, returns inner scriptblock result, and throws deterministically when contended' {
            $mutexName = 'Global\SandalCore_TestLock_' + [guid]::NewGuid().ToString('N')
            $lockFile = Join-Path $script:RepoRoot 'dist\.stamps\heavy_lock.json'
            $result = Invoke-WithHeavyLock -Operation 'test-heavy-lock' -MutexName $mutexName -NoWait -Action {
                if (-not (Test-Path -LiteralPath $lockFile)) {
                    throw 'Expected heavy_lock.json to exist while lock is held.'
                }
                'lock-acquired-ok'
            }
            if ($result -ne 'lock-acquired-ok') {
                throw "Expected inner scriptblock return value 'lock-acquired-ok', got '$result'"
            }

            if (-not ('SandalCoreLockContentionHelper' -as [type])) {
                Add-Type -TypeDefinition @'
using System.Threading;
public static class SandalCoreLockContentionHelper {
    public static Thread HoldMutex(string name, ManualResetEventSlim acquired, ManualResetEventSlim release) {
        var thread = new Thread(() => {
            using (var mutex = new Mutex(false, name)) {
                mutex.WaitOne(5000);
                acquired.Set();
                release.Wait(5000);
                mutex.ReleaseMutex();
            }
        }) { IsBackground = true };
        thread.Start();
        return thread;
    }
}
'@
            }

            $acquiredEvent = [System.Threading.ManualResetEventSlim]::new($false)
            $releaseEvent = [System.Threading.ManualResetEventSlim]::new($false)
            $holder = [SandalCoreLockContentionHelper]::HoldMutex($mutexName, $acquiredEvent, $releaseEvent)
            $null = $acquiredEvent.Wait(5000)
            try {
                $noWaitThrew = $false
                try {
                    Invoke-WithHeavyLock -Operation 'contended-nowait' -MutexName $mutexName -NoWait -Action { 'unreachable' }
                } catch {
                    $noWaitThrew = ($_.Exception.Message -match 'Heavy lock held|held by another process')
                }
                if (-not $noWaitThrew) {
                    throw 'Expected Invoke-WithHeavyLock -NoWait to throw when mutex is held by competing thread.'
                }

                $timeoutThrew = $false
                try {
                    Invoke-WithHeavyOperationLock -Operation 'contended-timeout' -MutexName $mutexName -TimeoutMs 50 -ScriptBlock { 'unreachable' }
                } catch [System.TimeoutException] {
                    $timeoutThrew = $true
                }
                if (-not $timeoutThrew) {
                    throw 'Expected Invoke-WithHeavyOperationLock to throw TimeoutException on contention.'
                }
            } finally {
                $releaseEvent.Set()
                $null = $holder.Join(5000)
                $acquiredEvent.Dispose()
                $releaseEvent.Dispose()
            }
        }
    }

    Context 'Case 4: Test-SandalRetiredLegacyScripts Positive & Negative-Space Fixture' {
        It 'records 0 violations on a clean root and all 4 violations when retired scripts exist' {
            $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-retired-' + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'scripts') -Force
                $cleanViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot -Violations $cleanViolations
                if ($cleanViolations.Count -ne 0) {
                    throw "Expected 0 violations on clean root, got $($cleanViolations.Count)"
                }

                $retiredRelPaths = @('build_launcher.bat', 'build_launcher.ps1', 'scripts/save_run_logs.bat', 'scripts/save_run_logs.ps1')
                foreach ($rel in $retiredRelPaths) {
                    Set-Content -LiteralPath (Join-Path $tmpRoot $rel) -Value 'rem retired' -Encoding utf8
                }

                $explicitViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot -Violations $explicitViolations
                if ($explicitViolations.Count -ne 4) {
                    throw "Expected 4 violations for retired scripts, got $($explicitViolations.Count): $($explicitViolations -join '; ')"
                }

                $violations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot
                if ($violations.Count -ne 4) {
                    throw "Expected 4 violations via caller-scope fallback, got $($violations.Count)"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRoot) {
                    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Case 5: Sync-SandalDistStage & Sync-SandalDeployStage Hermetic 4-Tier Staging' {
        It 'stages config, migrations, and hotfix seeds to dist/ excluding *.local.json and materializes deploy/ with 64-hex CSPRNG deploy/.env and 0 root .env' {
            $tmpRepo = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-stage-sync-' + [guid]::NewGuid().ToString('N'))
            try {
                foreach ($relDir in @(
                    'src\config\profiles',
                    'src\config\certs',
                    'src\config\hotfixes',
                    'src\server\services\sql\migrations',
                    'src\deploy\postgres\init',
                    'src\deploy\k8s'
                )) {
                    $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo $relDir) -Force
                }
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\server.json') -Value '{"game_port":7777}' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\profiles\local-dev.env') -Value 'AOC_ENV=dev' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\server.local.json') -Value '{"secret":"exclude-me"}' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\local.json') -Value '{"secret":"exclude-me-too"}' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\certs\amazon_ca_bundle.pem') -Value '-----BEGIN CERTIFICATE-----' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\server\services\sql\migrations\001_accounts.sql') -Value 'CREATE TABLE accounts(id INT);' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\server\services\sql\migrations\notes.txt') -Value 'ignore non-sql' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\hotfixes\010_hotfix.sql') -Value 'UPDATE nodes SET level = 3;' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\deploy\postgres\init\000_init.sql') -Value 'CREATE EXTENSION pgcrypto;' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\deploy\compose.yml') -Value 'name: sandalcore' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\deploy\k8s\worldd-statefulset.yaml') -Value 'kind: StatefulSet' -Encoding utf8
                Set-Content -LiteralPath (Join-Path $tmpRepo 'src\config\sandal.env.example') -Value "AOC_ENV=dev`nPOSTGRES_PASSWORD=CHANGE_ME_DB`nAOC_SERVICE_TOKEN=CHANGE_ME_TOKEN`nSANDAL_SECRET_KEY=" -Encoding utf8

                $distRes = Sync-SandalDistStage -RepoRoot $tmpRepo -Config 'Release'
                if ($distRes.DistDir -ne (Join-Path $tmpRepo 'dist') -or $distRes.Config -ne 'Release') {
                    throw "Unexpected Sync-SandalDistStage result: $($distRes | ConvertTo-Json -Compress)"
                }
                foreach ($expectedRel in @(
                    'dist\config\server.json',
                    'dist\config\profiles\local-dev.env',
                    'dist\Release\config\server.json',
                    'dist\sql\migrations\001_accounts.sql',
                    'dist\Release\sql\migrations\001_accounts.sql',
                    'dist\sql\seed\010_hotfix.sql',
                    'dist\sql\seed\000_init.sql',
                    'dist\certs\amazon_ca_bundle.pem'
                )) {
                    if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo $expectedRel) -PathType Leaf)) {
                        throw "Expected Sync-SandalDistStage to materialize '$expectedRel'"
                    }
                }
                foreach ($excludedRel in @(
                    'dist\config\server.local.json',
                    'dist\config\local.json',
                    'dist\Release\config\server.local.json',
                    'dist\Release\config\local.json',
                    'dist\sql\migrations\notes.txt'
                )) {
                    if (Test-Path -LiteralPath (Join-Path $tmpRepo $excludedRel)) {
                        throw "Expected Sync-SandalDistStage to exclude '$excludedRel'"
                    }
                }

                $deployRes = Sync-SandalDeployStage -RepoRoot $tmpRepo
                if ($deployRes.DeployDir -ne (Join-Path $tmpRepo 'deploy') -or $deployRes.EnvPath -ne (Join-Path $tmpRepo 'deploy\.env')) {
                    throw "Unexpected Sync-SandalDeployStage result: $($deployRes | ConvertTo-Json -Compress)"
                }
                foreach ($expectedDeployRel in @('deploy\compose.yml', 'deploy\k8s\worldd-statefulset.yaml', 'deploy\.env')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo $expectedDeployRel) -PathType Leaf)) {
                        throw "Expected Sync-SandalDeployStage to materialize '$expectedDeployRel'"
                    }
                }
                if (Test-Path -LiteralPath (Join-Path $tmpRepo '.env')) {
                    throw 'Expected Sync-SandalDeployStage to create 0 .env file in the repository root.'
                }
                $envMap = Get-SandalEnvMap -EnvPath $deployRes.EnvPath
                foreach ($secretKey in @('POSTGRES_PASSWORD', 'AOC_SERVICE_TOKEN', 'SANDAL_SECRET_KEY')) {
                    if ($envMap[$secretKey] -notmatch '^[0-9a-f]{64}$') {
                        throw "Expected '$secretKey' in deploy/.env to be a 64-hex CSPRNG secret, got '$($envMap[$secretKey])'"
                    }
                }
                $rootFindings = Test-SandalRootDirectoryHierarchy -RepoRoot $tmpRepo
                if (@($rootFindings).Count -ne 0) {
                    throw "Expected 0 root hierarchy findings after Sync-SandalDistStage and Sync-SandalDeployStage, got: $($rootFindings -join '; ')"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}
