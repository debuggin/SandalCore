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
$script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
. $script:PublicCheckScript

Describe 'Sandal Repository Config Manifests, Public Hygiene Gate & Secret Redaction' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
        $script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
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
                $testsRel = if ((Get-Command Test-SandalTestsDirectoryBounds).Definition -match 'src[\\/]tests') { 'src\tests' } else { 'tests' }
                $cfgRel = if ((Get-Command Test-SandalEnvSchemaTemplates).Definition -match 'src[\\/]config') { 'src\config' } else { 'config' }
                $testsDir = Join-Path $tmpRoot $testsRel
                $cfgBase = Join-Path $tmpRoot $cfgRel
                $cfgDir = Join-Path $cfgBase 'profiles'
                $null = New-Item -ItemType Directory -Path $testsDir -Force
                $null = New-Item -ItemType Directory -Path $cfgDir -Force

                [System.IO.File]::WriteAllText((Join-Path $testsDir 'bad.inc'), '// inc')
                $oversized = (1..805 | ForEach-Object { "line $_" }) -join "`n"
                [System.IO.File]::WriteAllText((Join-Path $testsDir 'huge.txt'), $oversized)

                $boundsViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalTestsDirectoryBounds -RootPath $tmpRoot -Violations $boundsViolations
                if ($boundsViolations.Count -ne 2) {
                    throw "Expected 2 bounds violations (.inc and >800 lines), got $($boundsViolations.Count)"
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
                'ics_token=zenith-cliff-secret-token-999',
                'AOC_MASTER_KEY=master_key_value_12345',
                'redis://default:redis_secret_pw@127.0.0.1:6379/0',
                $pem
            ) -join "`n"

            $scrubbed = Invoke-SandalLogRedaction -Text $sample
            if ($scrubbed -notmatch 'level=info subsystem=gateway rtt_ms=9') {
                throw 'Expected non-sensitive telemetry line to remain intact.'
            }
            foreach ($secret in @('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9', 'zenith-cliff-secret-token-999', 'master_key_value_12345', 'redis_secret_pw', $pem)) {
                if ($scrubbed.Contains($secret)) {
                    throw "Secret '$secret' was not redacted from output: $scrubbed"
                }
            }
        }
    }
}
