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
$script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
$script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
$script:ExtractScript = Join-Path $script:RepoRoot 'scripts\sandal\extract.ps1'
$script:UpScript = Join-Path $script:RepoRoot 'scripts\sandal\up.ps1'
Import-Module $script:ManifestPath -Force -DisableNameChecking
. $script:PublicCheckScript

function New-StampTestFixture {
    $root = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-stamp-red-' + [guid]::NewGuid().ToString('N'))
    $files = @{
        'src/config/layouts/aoc.usmap'                  = 'usmap-v1'
        'src/tools/mapc/Program.cs'                     = 'class Mapc {}'
        'src/server/services/cmd/contentc/main.go'      = 'package main'
        'src/server/services/go.mod'                    = 'module sandal/services'
        'src/tools/schemagen/gen_zoi_surface.py'        = 'print("zoi")'
        'src/tools/schemagen/gen_region_rects.py'       = 'print("rects")'
        'src/tools/schemagen/gen_region.py'             = 'print("region")'
        'src/tools/schemagen/gen_node_constants.py'     = 'print("nodes")'
        'src/tools/schemagen/gen_gm_garden.py'          = 'print("garden")'
        'src/tools/schemagen/gen_tooltips.py'           = 'print("tooltips")'
        'src/tools/schemagen/gen_gear_fixture.py'       = 'print("gear")'
        'src/tools/schemagen/gen_talent_fixture.py'     = 'print("talent")'
        'src/tools/schemagen/gen_town_layouts.py'       = 'print("layouts")'
        'src/config/seeds/citynode_seed.json'           = '{"nodes":{}}'
        'src/libs/aocnav/nav.cpp'                       = 'int nav = 1;'
        'src/tools/navbuild/main.cpp'                   = 'int main() { return 0; }'
        'build/third_party/recastnavigation/recast.cpp' = 'int rc = 1;'
        'src/vcpkg.json'                                = '{"name":"sandal"}'
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

function Protect-SandalSensitiveText {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$InputText)
    $scrubbed = $InputText
    $pemPattern = '-----BEGIN ' + '(?:RSA |EC |OPENSSH )?PRIVATE KEY-----'
    $scrubbed = [regex]::Replace($scrubbed, $pemPattern, '[REDACTED]')
    $scrubbed = [regex]::Replace($scrubbed, '(?i)\bBearer\s+[A-Za-z0-9\-._~+/]+=*', 'Bearer [REDACTED]')
    $scrubbed = [regex]::Replace($scrubbed, '(?m)\b(AOC_MASTER_KEY|POSTGRES_PASSWORD|AOC_SERVICE_TOKEN|SANDAL_SECRET_KEY)\s*=\s*[^\r\n\s]+', '$1=[REDACTED]')
    $scrubbed = [regex]::Replace($scrubbed, '(?i)(postgres(?:ql)?://[^:\s/@]+:)([^@\s]+)(@)', '$1[REDACTED]$3')
    return $scrubbed
}

Describe 'SandalCore SHA-256 Stamp DAG, Secret Redaction & Polyglot Stage Gate Verification' {
    BeforeAll {
        Import-Module $script:ManifestPath -Force -DisableNameChecking
        . $script:PublicCheckScript
    }

    Context 'Case 1: Deterministic SHA-256 Stamp Computing, Writing & Cache-Hit Detection' {
        It 'computes deterministic 64-hex SHA-256 stamps, detects cache hits, and invalidates on input or salt mutation' {
            $tmpRepo = New-StampTestFixture
            try {
                if (Test-SandalStageUpToDate -Stage 'database' -RepoRoot $tmpRepo) {
                    throw 'Expected unstamped database stage to report cache miss ($false).'
                }
                $hash1 = Compute-SandalStageHash -Stage 'database' -RepoRoot $tmpRepo
                $hash2 = Compute-SandalStageHash -Stage 'database' -RepoRoot $tmpRepo
                if ($hash1 -ne $hash2 -or $hash1 -notmatch '^[0-9a-f]{64}$') {
                    throw "Expected deterministic 64-hex SHA-256 stamp, got '$hash1' vs '$hash2'"
                }

                $stampObj = Write-SandalStageStamp -Stage 'database' -RepoRoot $tmpRepo -InputHash $hash1
                if ($stampObj.input_hash -ne $hash1 -or -not (Test-SandalStageUpToDate -Stage 'database' -RepoRoot $tmpRepo)) {
                    throw 'Expected Test-SandalStageUpToDate to return $true after writing matching stamp.'
                }

                $saltedHash = Compute-SandalStageHash -Stage 'database' -RepoRoot $tmpRepo -Salt @{ cmd = 'contentc --strict' }
                if ($saltedHash -eq $hash1) {
                    throw 'Expected command salt mutation to produce a distinct SHA-256 stamp.'
                }

                $goFile = Join-Path $tmpRepo 'src\server\services\cmd\contentc\main.go'
                [System.IO.File]::WriteAllText($goFile, 'package main // mutated', [System.Text.UTF8Encoding]::new($false))
                if (Test-SandalStageUpToDate -Stage 'database' -RepoRoot $tmpRepo) {
                    throw 'Expected tracked file mutation to flip Test-SandalStageUpToDate to $false (cache miss).'
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRepo) {
                    Remove-Item -LiteralPath $tmpRepo -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Case 2: Secret & Credential Redaction, CSPRNG Env & Content Leak Guards' {
        It 'scrubs bearer tokens, env secrets, PEM headers, and DB URLs to [REDACTED] while preserving diagnostics' {
            $pemHeader = '-----BEGIN ' + 'PRIVATE KEY-----'
            $peLiteral = '0x140' + '001234'
            $rawLog = @(
                '[INFO] node=verra-01 status=healthy',
                'Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.payload.sig',
                'AOC_MASTER_KEY=super_secret_master_key_99',
                'POSTGRES_PASSWORD=my_db_secret_pass',
                $pemHeader,
                'dsn=postgres://aoc_user:hunter2_secret@127.0.0.1:5432/sandal'
            ) -join "`n"

            $redacted = Protect-SandalSensitiveText -InputText $rawLog
            if ($redacted -notmatch '\[INFO\] node=verra-01 status=healthy') {
                throw 'Expected non-sensitive diagnostic text to be preserved.'
            }
            foreach ($leakedToken in @('eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9', 'super_secret_master_key_99', 'my_db_secret_pass', $pemHeader, 'hunter2_secret')) {
                if ($redacted.Contains($leakedToken)) {
                    throw "Expected token '$leakedToken' to be scrubbed from redacted output: $redacted"
                }
            }
            if ($redacted -notmatch 'Bearer \[REDACTED\]' -or $redacted -notmatch 'AOC_MASTER_KEY=\[REDACTED\]' -or $redacted -notmatch 'POSTGRES_PASSWORD=\[REDACTED\]' -or $redacted -notmatch 'postgres://aoc_user:\[REDACTED\]@') {
                throw "Expected [REDACTED] markers in scrubbed output: $redacted"
            }

            $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-leak-' + [guid]::NewGuid().ToString('N'))
            try {
                $cfgRel = 'src\config'
                $cfgDir = Join-Path $tmpDir $cfgRel
                $null = New-Item -ItemType Directory -Path $cfgDir -Force
                $sampleFile = Join-Path $cfgDir 'sample.txt'
                [System.IO.File]::WriteAllText($sampleFile, "$pemHeader`n$peLiteral", [System.Text.UTF8Encoding]::new($false))
                $leaks = [System.Collections.Generic.List[string]]::new()
                Test-SandalContentLeaks -RootPath $tmpDir -Violations $leaks
                if ($leaks.Count -ne 2) {
                    throw "Expected 2 content leak violations (PEM + PE32+ literal), got $($leaks.Count)"
                }

                $null = New-Item -ItemType Directory -Path (Join-Path $tmpDir 'deploy') -Force
                $envPath = Join-Path $tmpDir 'deploy\.env'
                [System.IO.File]::WriteAllLines($envPath, @('AOC_ENV=dev', 'POSTGRES_PASSWORD=CHANGE_ME_NOW'), [System.Text.UTF8Encoding]::new($false))
                $null = Initialize-SandalEnv -EnvPath $envPath -SecretKeys @('POSTGRES_PASSWORD', 'AOC_SERVICE_TOKEN')
                $envMap = Get-SandalEnvMap -EnvPath $envPath
                if ($envMap['POSTGRES_PASSWORD'] -notmatch '^[0-9a-f]{64}$' -or $envMap['AOC_SERVICE_TOKEN'] -notmatch '^[0-9a-f]{64}$') {
                    throw 'Expected Initialize-SandalEnv to replace CHANGE_ME and missing keys with 64-hex CSPRNG secrets.'
                }
                if (Test-Path -LiteralPath (Join-Path $tmpDir '.env')) {
                    throw 'Expected Initialize-SandalEnv on deploy/.env to leave 0 .env file in repo root.'
                }
            } finally {
                if (Test-Path -LiteralPath $tmpDir) {
                    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }

    Context 'Case 3: Polyglot Stage Gate Plan, Extract/Up DryRun & Fast Hygiene Gate' {
        It 'plans extract and compose stages deterministically in DryRun mode and passes Invoke-SandalPublicCheck' {
            $extJson = (@(& pwsh -NoProfile -File $script:ExtractScript -DryRun -DataFree -Force -Json) -join "`n") | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $extJson.ok -or $extJson.mode -ne 'verify-standin' -or -not $extJson.dry_run) {
                throw "Unexpected extract.ps1 DryRun output: $($extJson | ConvertTo-Json -Compress)"
            }
            if (@($extJson.stages).Count -ne 4) {
                throw "Expected 4 extraction stages in plan, got $(@($extJson.stages).Count)"
            }

            $upJson = (@(& pwsh -NoProfile -File $script:UpScript -Profile dev -Detach -DryRun -Json) -join "`n") | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $upJson.ok -or $upJson.profile -ne 'dev' -or -not $upJson.dry_run -or @($upJson.stages).Count -ne 11) {
                throw "Unexpected up.ps1 DryRun output: $($upJson | ConvertTo-Json -Compress)"
            }

            $checkCode = Invoke-SandalPublicCheck -Root $script:RepoRoot
            if ($checkCode -ne 0) {
                throw "Expected Invoke-SandalPublicCheck on repository root to return 0, got $checkCode"
            }
        }
    }
}
