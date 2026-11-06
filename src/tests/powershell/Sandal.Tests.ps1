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
    function Describe([string]$Name, [scriptblock]$Fixture) { Write-Host "[Describe] $Name"; & $Fixture }
    function Context([string]$Name, [scriptblock]$Fixture) { Write-Host "  [Context] $Name"; & $Fixture }
    function BeforeAll([scriptblock]$Action) { & $Action }
    function It([string]$Name, [scriptblock]$Test) { & $Test; Write-Host "    [PASS] $Name" }
    function Should {
        param([switch]$BeTrue, [switch]$BeFalse, [object]$Be)
        $actual = @($input)[0]
        if ($BeTrue -and -not [bool]$actual) { throw "Expected True, got '$actual'" }
        if ($BeFalse -and [bool]$actual) { throw "Expected False, got '$actual'" }
        if ($PSBoundParameters.ContainsKey('Be') -and $actual -ne $Be) { throw "Expected '$Be', got '$actual'" }
    }
} elseif ((Get-Module Pester).Version.Major -lt 5) {
    function Should {
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

function Invoke-SandalLogRedaction {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $out = [regex]::Replace($Text, '-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----', '[REDACTED]')
    $out = [regex]::Replace($out, '(?i)\bBearer\s+[A-Za-z0-9\-._~+/]+=*', 'Bearer [REDACTED]')
    $out = [regex]::Replace($out, '(?i)\b(ics_token|game_connection_token|AOC_MASTER_KEY|POSTGRES_PASSWORD)\s*[:=]\s*["'']?[^\r\n"''\s,;]+["'']?', '$1=[REDACTED]')
    $out = [regex]::Replace($out, '(?i)((?:postgres(?:ql)?|redis)://[^:\s/@]+:)([^@\s]+)(@)', '$1[REDACTED]$3')
    return $out
}

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
$script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
$script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
Import-Module $script:ManifestPath -Force -DisableNameChecking
. $script:PublicCheckScript

Describe 'Sandal Repository Config Manifests, Public Hygiene Gate & Secret Redaction' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
        $script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
        $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
        Import-Module $script:ManifestPath -Force -DisableNameChecking
        . $script:PublicCheckScript
    }

    Context 'public_check.ps1 Hygiene Gate & Negative-Space Violation Detection' {
        It 'passes on the repository root and catches .inc, oversized test files, leaks, and bad env templates' {
            $repoCode = Invoke-SandalPublicCheck -Root $script:RepoRoot
            if ($repoCode -ne 0) {
                throw "Expected Invoke-SandalPublicCheck to return 0 on repo root, got $repoCode"
            }

            $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-check-' + [guid]::NewGuid().ToString('N'))
            try {
                $testsRel = 'src\tests'
                $cfgRel = 'src\config'
                $testsDir = Join-Path $tmpRoot $testsRel
                $cfgBase = Join-Path $tmpRoot $cfgRel
                $cfgDir = Join-Path $cfgBase 'profiles'
                $null = New-Item -ItemType Directory -Path $testsDir -Force
                $null = New-Item -ItemType Directory -Path $cfgDir -Force

                [System.IO.File]::WriteAllText((Join-Path $testsDir 'bad.inc'), '// inc')
                $oversized = (1..905 | ForEach-Object { "line $_" }) -join "`n"
                [System.IO.File]::WriteAllText((Join-Path $testsDir 'huge.txt'), $oversized)

                $boundsViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalTestsDirectoryBounds -RootPath $tmpRoot -Violations $boundsViolations
                if ($boundsViolations.Count -ne 2) {
                    throw "Expected 2 bounds violations (.inc and >900 lines), got $($boundsViolations.Count)"
                }

                $pemHdr = 'BEGIN ' + 'RSA PRIVATE KEY'
                $peHex = '0x140' + 'abcdef'
                [System.IO.File]::WriteAllText((Join-Path $cfgDir 'leak.txt'), "$pemHdr`n$peHex")
                $leakViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalContentLeaks -RootPath $tmpRoot -Violations $leakViolations
                if ($leakViolations.Count -ne 2) {
                    throw "Expected 2 content leak violations, got $($leakViolations.Count)"
                }

                [System.IO.File]::WriteAllText((Join-Path $cfgBase 'sandal.env.example'), 'AOC_ONLY_ONE=1')
                [System.IO.File]::WriteAllText((Join-Path $cfgDir 'local-dev.env'), 'AOC_ENV=dev')
                [System.IO.File]::WriteAllText((Join-Path $cfgDir 'docker-single.env'), 'AOC_ENV=docker')
                [System.IO.File]::WriteAllText((Join-Path $cfgDir 'prod-mesh.env'), 'DSN=postgres://aoc:aoc@db/aoc?sslmode=disable')
                $envViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalEnvSchemaTemplates -RootPath $tmpRoot -Violations $envViolations
                if ($envViolations.Count -ne 2) {
                    throw "Expected 2 env template violations (key count + prod-mesh dev creds), got $($envViolations.Count)"
                }

                foreach ($canonicalBat in @('start.bat', 'stop.bat', 'build.bat', 'scripts\ensure_pwsh.bat', 'dist\launch_launcher.bat')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $script:RepoRoot $canonicalBat) -PathType Leaf)) {
                        throw "Expected canonical batch script '$canonicalBat' to exist."
                    }
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRoot) {
                    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
            if (Test-Path -LiteralPath $tmpRoot) {
                throw "Expected hermetic temp directory $tmpRoot to be cleaned up."
            }
        }
    }

    Context 'Repository Manifest & Config Integrity (CMakePresets.json, vcpkg.json, server.json, realm.json)' {
        It 'validates CMakePresets v6 C++23 presets, vcpkg baseline SHA, and server/realm JSON port parity' {
            $presetsPath = Join-Path $script:RepoRoot 'src\CMakePresets.json'
            $presets = Get-Content -LiteralPath $presetsPath -Raw | ConvertFrom-Json
            if ($presets.version -ne 6 -or $presets.cmakeMinimumRequired.major -ne 3 -or $presets.cmakeMinimumRequired.minor -ne 28) {
                throw 'CMakePresets.json must declare version 6 and cmakeMinimumRequired >= 3.28.0'
            }
            $cfgNames = @($presets.configurePresets | ForEach-Object { $_.name })
            foreach ($req in @('windows-base', 'windows-rel', 'linux-base', 'linux-release', 'linux-asan-ubsan', 'linux-tsan')) {
                if ($cfgNames -notcontains $req) {
                    throw "Missing required configurePreset '$req' in CMakePresets.json"
                }
            }

            $vcpkgPath = Join-Path $script:RepoRoot 'src\vcpkg.json'
            $vcpkg = Get-Content -LiteralPath $vcpkgPath -Raw | ConvertFrom-Json
            if ($vcpkg.'builtin-baseline' -notmatch '^[0-9a-f]{40}$') {
                throw "Expected 40-char git commit SHA in vcpkg.json builtin-baseline, got '$($vcpkg.'builtin-baseline')'"
            }
            foreach ($dep in @('grpc', 'protobuf', 'openssl', 'spdlog', 'nlohmann-json', 'cli11', 'sqlite3')) {
                if (@($vcpkg.dependencies) -notcontains $dep) {
                    throw "Missing dependency '$dep' in vcpkg.json"
                }
            }

            $serverCfg = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'src\config\server.json') -Raw | ConvertFrom-Json
            $realmCfg = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'src\config\realm.json') -Raw | ConvertFrom-Json
            if ($serverCfg.game_port -ne 7777 -or $realmCfg.game_connection_string -notmatch ":$([int]$serverCfg.game_port)$") {
                throw 'Mismatch between server.json game_port and realm.json game_connection_string'
            }
        }
    }

    Context 'Secret & Token Redaction Patterns Across Multi-Line Telemetry' {
        It 'redacts Bearer JWTs, ics_token, env keys, PEM headers, and DSN passwords while preserving telemetry' {
            $pem = '-----BEGIN ' + 'EC PRIVATE KEY-----'
            $sample = @(
                'level=info subsystem=gateway rtt_ms=9',
                'Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxIn0.sig',
                'ics_token=mock-ics-secret-token-999',
                'AOC_MASTER_KEY=master_key_value_12345',
                'redis://default:redis_secret_pw@127.0.0.1:6379/0',
                $pem
            ) -join "`n"

            $scrubbed = Invoke-SandalLogRedaction -Text $sample
            if ($scrubbed -notmatch 'level=info subsystem=gateway rtt_ms=9') {
                throw 'Expected non-sensitive telemetry line to remain intact.'
            }
            foreach ($secret in @('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9', 'mock-ics-secret-token-999', 'master_key_value_12345', 'redis_secret_pw', $pem)) {
                if ($scrubbed.Contains($secret)) {
                    throw "Secret '$secret' was not redacted from output: $scrubbed"
                }
            }
        }
    }

    Context '4-Tier Dist/Deploy Stage Materialization & Turnkey CLI DryRun Chain' {
        It 'stages dist/ and deploy/ hermetically via Sync-SandalDistStage and Sync-SandalDeployStage without creating a root .env' {
            $tmpRepo = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-stage-' + [guid]::NewGuid().ToString('N'))
            try {
                foreach ($dir in @('src\config\hotfixes', 'src\server\services\sql\migrations', 'src\deploy\postgres\init')) {
                    $null = New-Item -ItemType Directory -Path (Join-Path $tmpRepo $dir) -Force
                }
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\config\realm.json'), '{"realm_name":"Verra","game_connection_token":"default-token"}')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\config\realm.local.json'), '{"game_connection_token":"custom-local-token"}')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\config\override.local.json'), '{"debug":true}')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\server\services\sql\migrations\001_schema.sql'), 'SELECT 1;')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\config\hotfixes\002_seed.sql'), 'SELECT 2;')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\deploy\compose.yml'), 'name: sandalcore')
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'src\config\sandal.env.example'), "AOC_ENV=dev`nPOSTGRES_PASSWORD=CHANGE_ME")

                $null = Sync-SandalDistStage -RepoRoot $tmpRepo -Config 'Release'
                $deployStage = Sync-SandalDeployStage -RepoRoot $tmpRepo

                if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\config\realm.json') -PathType Leaf)) {
                    throw 'Expected src/config/realm.json to be staged to dist/config/realm.json'
                }
                $stagedRealm = Get-Content -LiteralPath (Join-Path $tmpRepo 'dist\config\realm.json') -Raw | ConvertFrom-Json
                if ($stagedRealm.game_connection_token -ne 'custom-local-token') {
                    throw "Expected realm.local.json overlay to set game_connection_token='custom-local-token', got '$($stagedRealm.game_connection_token)'"
                }
                [System.IO.File]::WriteAllText((Join-Path $tmpRepo 'dist\config\realm.json'), '{"realm_name":"CustomRuntimeRealm","game_connection_token":"custom-local-token"}')
                $null = Sync-SandalDistStage -RepoRoot $tmpRepo -Config 'Release'
                $reStagedRealm = Get-Content -LiteralPath (Join-Path $tmpRepo 'dist\config\realm.json') -Raw | ConvertFrom-Json
                $srcTemplateRealm = Get-Content -LiteralPath (Join-Path $tmpRepo 'src\config\realm.json') -Raw | ConvertFrom-Json
                if ($reStagedRealm.realm_name -ne 'CustomRuntimeRealm' -or $srcTemplateRealm.realm_name -ne 'Verra') {
                    throw "Expected runtime edit in dist/config/realm.json ('CustomRuntimeRealm') to be preserved while src/config/realm.json remains 'Verra'"
                }
                if (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\config\override.local.json')) {
                    throw 'Expected *.local.json to be excluded from dist/config/'
                }
                if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\sql\migrations\001_schema.sql') -PathType Leaf)) {
                    throw 'Expected migrations/*.sql to be staged to dist/sql/migrations/'
                }
                if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\sql\seed\002_seed.sql') -PathType Leaf)) {
                    throw 'Expected hotfixes/*.sql to be staged to dist/sql/seed/'
                }
                if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\launcher') -PathType Container)) {
                    throw 'Expected dist/launcher directory to be created by Sync-SandalDistStage'
                }
                foreach ($launcherScript in @('dist\launch_launcher.bat', 'dist\launch_launcher.ps1', 'dist\start_server.ps1', 'dist\stop_server.ps1')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo $launcherScript) -PathType Leaf)) {
                        throw "Expected $launcherScript to be materialized by Sync-SandalDistStage"
                    }
                }
                $mockLauncherOut = Join-Path $tmpRepo 'build\dotnet\bin\SandalLauncher\Release\net10.0'
                $null = New-Item -ItemType Directory -Path $mockLauncherOut -Force
                [System.IO.File]::WriteAllText((Join-Path $mockLauncherOut 'SandalLauncher.exe'), 'MZ-mock')
                [System.IO.File]::WriteAllText((Join-Path $mockLauncherOut 'SandalLauncher.pdb'), 'PDB-mock')
                $null = Sync-SandalDistStage -RepoRoot $tmpRepo -Config 'Release'
                foreach ($stagedLauncher in @('dist\launcher\SandalLauncher.exe', 'dist\Release\SandalLauncher.exe')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo $stagedLauncher) -PathType Leaf)) {
                        throw "Expected $stagedLauncher to be synced from build/dotnet/bin by Sync-SandalDistStage"
                    }
                }
                if (Test-Path -LiteralPath (Join-Path $tmpRepo 'dist\launcher\SandalLauncher.pdb')) {
                    throw 'Expected .pdb files to be excluded from dist/launcher'
                }
                foreach ($deployItem in @('deploy\compose.yml', 'deploy\up.ps1', 'deploy\down.ps1', 'deploy\content', 'deploy\certs', 'deploy\nav', 'deploy\config')) {
                    if (-not (Test-Path -LiteralPath (Join-Path $tmpRepo $deployItem))) {
                        throw "Expected $deployItem to be materialized by Sync-SandalDeployStage"
                    }
                }
                if (Test-Path -LiteralPath (Join-Path $tmpRepo '.env')) {
                    throw 'Expected 0 .env file in repo root'
                }
                $envMap = Get-SandalEnvMap -EnvPath $deployStage.EnvPath
                foreach ($key in @('POSTGRES_PASSWORD', 'AOC_SERVICE_TOKEN', 'SANDAL_SECRET_KEY')) {
                    if ($envMap[$key] -notmatch '^[0-9a-f]{64}$') {
                        throw "Expected 64-hex CSPRNG secret for $key in deploy/.env, got '$($envMap[$key])'"
                    }
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }

        It 'verifies sandal.ps1 build, deploy, test, run, up, and down -DryRun -Json exit 0 and emit valid JSON payloads' {
            foreach ($verb in @('build', 'deploy', 'test', 'run', 'up', 'down')) {
                $out = (@(& pwsh -NoProfile -File $script:SandalCli $verb -DryRun -Json) -join "`n")
                if ($LASTEXITCODE -ne 0) {
                    throw "Expected sandal.ps1 $verb -DryRun -Json to exit 0, got $LASTEXITCODE"
                }
                $payload = $out | ConvertFrom-Json
                if (-not $payload.ok -or $payload.command -ne $verb -or -not $payload.dry_run) {
                    throw "Invalid JSON payload for sandal.ps1 $verb -DryRun -Json: $out"
                }
            }
        }

        It 'emits [Step 1/6] through [Step 6/6] in strict ascending order (1..6) for run.ps1 -DryRun and never emits [Step 2/6] after [Step 3/6]' {
            $runScript = Join-Path $script:RepoRoot 'scripts\sandal\run.ps1'
            $dryOut = (@(& pwsh -NoProfile -File $runScript -DryRun) -join "`n")
            $LASTEXITCODE | Should -Be 0
            $stepMatches = [regex]::Matches($dryOut, '\[Step\s+(\d+)/6\]')
            $stepNums = @($stepMatches | ForEach-Object { [int]$_.Groups[1].Value })
            ($stepNums -join ',') | Should -Be '1,2,3,4,5,6'
            $idxStep2 = $dryOut.IndexOf('[Step 2/6]', [System.StringComparison]::Ordinal)
            $idxStep3 = $dryOut.IndexOf('[Step 3/6]', [System.StringComparison]::Ordinal)
            $lastIdxStep2 = $dryOut.LastIndexOf('[Step 2/6]', [System.StringComparison]::Ordinal)
            ($idxStep2 -ge 0 -and $idxStep3 -gt $idxStep2 -and $lastIdxStep2 -lt $idxStep3) | Should -BeTrue
        }

        It 'runs sandal.ps1 test -Filter go -Json without -DryRun, exits 0, and emits valid JSON' {
            $raw = (@(& pwsh -NoProfile -File $script:SandalCli test -Filter go -Json) -join "`n")
            $LASTEXITCODE | Should -Be 0
            $out = $raw | ConvertFrom-Json
            $out.ok | Should -BeTrue
            $out.command | Should -Be 'test'
        }

        It 'exports Ensure-SandalDockerDaemon from scripts/lib/Env.psm1 and defines Resolve-WslDockerDistro in scripts/lib/wsl-docker.ps1' {
            $envPsm1Path = Join-Path $script:RepoRoot 'scripts\lib\Env.psm1'
            $wslDockerPath = Join-Path $script:RepoRoot 'scripts\lib\wsl-docker.ps1'
            $envModule = Import-Module $envPsm1Path -Force -DisableNameChecking -PassThru
            if ($envModule.ExportedFunctions.Keys -notcontains 'Ensure-SandalDockerDaemon') {
                throw 'Expected Ensure-SandalDockerDaemon to be exported by scripts/lib/Env.psm1'
            }
            $dryDaemon = Ensure-SandalDockerDaemon -DryRun
            if (-not $dryDaemon.DryRun -or $dryDaemon.Started) {
                throw "Expected Ensure-SandalDockerDaemon -DryRun to return DryRun=true and Started=false, got: $($dryDaemon | ConvertTo-Json -Compress)"
            }

            $wslContent = Get-Content -LiteralPath $wslDockerPath -Raw
            if ($wslContent -notmatch 'function\s+Resolve-WslDockerDistro\b') {
                throw 'Expected Resolve-WslDockerDistro function definition in scripts/lib/wsl-docker.ps1'
            }
            . $wslDockerPath
            $resolvedFallback = Resolve-WslDockerDistro -Distro 'Ubuntu-24.04' -Runner { @{ ExitCode = 0; Output = @('docker-desktop', 'Ubuntu') } }
            $resolvedExact = Resolve-WslDockerDistro -Distro 'Ubuntu-24.04' -Runner { @{ ExitCode = 0; Output = @('Ubuntu-24.04', 'Ubuntu') } }
            $resolvedFallback | Should -Be 'Ubuntu'
            $resolvedExact | Should -Be 'Ubuntu-24.04'

            $prevDockerHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $fastCalls = [System.Collections.Generic.List[string]]::new()
                $fastDaemon = Ensure-SandalDockerDaemon -Distro 'Ubuntu-24.04' -CommandRunner {
                    param($FilePath, $ArgumentList)
                    $fastCalls.Add("$FilePath $($ArgumentList -join ' ')")
                    return 0
                }
                $fastDaemon.Ready | Should -BeTrue
                $fastDaemon.Started | Should -BeFalse
                $fastCalls.Count | Should -Be 1
                $fastCalls[0] | Should -Be 'docker info'
            } finally {
                if ($null -eq $prevDockerHost) {
                    Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                } else {
                    $env:DOCKER_HOST = $prevDockerHost
                }
            }
        }
    }
}
