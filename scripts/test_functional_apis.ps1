#Requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$Json,
    [string]$RepoRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-SandalCertsAndContentReady {
    param([string]$RootDir)
    $envModule = Join-Path $RootDir 'scripts\lib\Env.psm1'
    if (-not (Get-Command Ensure-SandalContentDbCurrent -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $envModule -PathType Leaf)) {
        Import-Module $envModule -Force -DisableNameChecking
    }
    if (Get-Command Ensure-SandalContentDbCurrent -ErrorAction SilentlyContinue) {
        $contentState = Ensure-SandalContentDbCurrent -RepoRoot $RootDir
        if (($contentState.Rebuilt -or $contentState.SyncedDeploy) -and (Get-Command Restart-SandalContentDbContainers -ErrorAction SilentlyContinue)) {
            $restarted = @(Restart-SandalContentDbContainers)
            if ($restarted.Count -gt 0 -and (Test-Path -LiteralPath (Join-Path $RootDir 'deploy\compose.yml') -PathType Leaf)) {
                & docker compose -f (Join-Path $RootDir 'deploy\compose.yml') --env-file (Join-Path $RootDir 'deploy\.env') up -d --wait *> $null
            }
        }
    }
    $caPath = Join-Path $RootDir 'dist\certs\ca.crt'
    $srvPem = Join-Path $RootDir 'dist\certs\server.pem'
    $srvKey = Join-Path $RootDir 'dist\certs\server.key'
    foreach ($p in @($srvPem, $srvKey, (Join-Path $RootDir 'deploy\content\zoi_chunks'), (Join-Path $RootDir 'deploy\content\content.db'))) {
        if (-not (Test-Path -LiteralPath $p)) {
            throw "Required certificate or Riverlands content path is missing: $p"
        }
    }
    if (Get-Command Test-SandalRuntimeGameDataComplete -ErrorAction SilentlyContinue) {
        if (-not (Test-SandalRuntimeGameDataComplete -RepoRoot $RootDir)) {
            throw 'dist/content is missing one or more of the 25 required runtime game-data artifacts (Test-SandalRuntimeGameDataComplete returned false).'
        }
        if (-not (Test-SandalRuntimeGameDataComplete -RepoRoot $RootDir -ContentRelPath 'deploy\content')) {
            throw 'deploy/content is missing one or more of the 25 required runtime game-data artifacts (Test-SandalRuntimeGameDataComplete returned false).'
        }
    }
    $srvCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem((Get-Content -LiteralPath $srvPem -Raw))
    $cn = $srvCert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($cn -eq 'sandalcore-gateway') {
        throw "dist/certs/server.pem must not use the untrusted ephemeral CN=sandalcore-gateway certificate."
    }
    if ((Test-Path -LiteralPath $caPath -PathType Leaf) -and $IsWindows -and $env:CI -ne 'true') {
        $caCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem((Get-Content -LiteralPath $caPath -Raw))
        if ($srvCert.Issuer -eq $caCert.Subject) {
            $tp = $caCert.Thumbprint
            if (-not ((Test-Path -LiteralPath "Cert:\CurrentUser\Root\$tp") -or (Test-Path -LiteralPath "Cert:\LocalMachine\Root\$tp"))) {
                throw "CA certificate thumbprint '$tp' from dist/certs/ca.crt is not installed in Cert:\CurrentUser\Root or Cert:\LocalMachine\Root."
            }
        }
    }
}

function Start-SandalComposeStackIfNeeded {
    param([string]$RootDir, [string[]]$ExpectedContainers)
    $needsStart = -not (Test-Path -LiteralPath (Join-Path $RootDir 'deploy\compose.yml') -PathType Leaf)
    if (-not $needsStart) {
        foreach ($name in $ExpectedContainers) {
            $st = (& docker inspect --format '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' $name 2>$null) -join ''
            if ($LASTEXITCODE -ne 0 -or $st.Trim() -ne 'running|healthy') { $needsStart = $true; break }
        }
    }
    if (-not $needsStart) { return }
    & pwsh -NoProfile -File (Join-Path $RootDir 'sandal.ps1') deploy -Json *> $null
    & docker compose -f (Join-Path $RootDir 'deploy\compose.yml') --env-file (Join-Path $RootDir 'deploy\.env') up -d --wait --remove-orphans *> $null
}

function Assert-WorldStartupSubsystemsLoaded {
    $bootLogs = ''
    for ($attempt = 0; $attempt -lt 45; $attempt++) {
        $allLogs = (& docker logs aoc-world 2>&1) -join "`n"
        $boots = [regex]::Matches($allLogs, '(?m)^.*\[content\] /app/content/content\.db: contentc version.*$')
        $bootLogs = if ($boots.Count -gt 0) { $allLogs.Substring($boots[$boots.Count - 1].Index) } else { $allLogs }
        if ($bootLogs -match 'foliage.*on: \d+ instances|did not open|refuses every login|no such column') { break }
        Start-Sleep -Seconds 2
    }
    $forbiddenPatterns = @(
        'did not open|refuses every login|no such column',
        'no Blueprint table',
        'no npc_motion\.tsv(?!\s+0\b)',
        '\[error\]\s*\[nav\]\s*no ground',
        'no town platforms',
        'harvesting is off',
        'no mob attack animations',
        'no mob senses',
        '(?i)(sqlite|content\.db).*error'
    )
    foreach ($pat in $forbiddenPatterns) {
        if ($bootLogs -match $pat) {
            throw "aoc-world startup logs matched forbidden missing-table/error pattern '$pat'."
        }
    }
    $requiredPatterns = @(
        'Blueprint table .*: \d+ pawn Blueprints',
        'mob attack animations: \d+ Blueprints',
        'ground .*area_nav\.bin: \d+ terrain grids',
        '\d+ NPCs on the ground',
        'foliage.*on: \d+ instances'
    )
    foreach ($req in $requiredPatterns) {
        if ($bootLogs -notmatch $req) {
            throw "aoc-world startup logs missing required subsystem confirmation '$req'."
        }
    }
}

function Assert-SandalDockerStackHealthy {
    param([string]$RootDir)
    $envModule = Join-Path $RootDir 'scripts\lib\Env.psm1'
    if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $envModule -PathType Leaf)) {
        Import-Module $envModule -Force -DisableNameChecking
    }
    if (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue) {
        $null = Ensure-SandalDockerDaemon
    }
    $expected = @(
        'aoc-postgres', 'aoc-auth', 'aoc-character', 'aoc-citynode', 'aoc-quest',
        'aoc-mesh', 'aoc-gateway', 'aoc-mesh-gateway', 'aoc-web-admin', 'aoc-world', 'aoc-ingress-proxy'
    )
    Start-SandalComposeStackIfNeeded -RootDir $RootDir -ExpectedContainers $expected
    Assert-SandalCertsAndContentReady -RootDir $RootDir
    $tetherRows = @(& docker ps -a --filter 'name=^/aoc-tether$' --format '{{.Names}}' 2>$null | Where-Object { $_ -and $_.Trim() -ne '' })
    if ($tetherRows.Count -gt 0) {
        throw "Deprecated aoc-tether container still exists: $($tetherRows -join ', ')"
    }
    $statuses = [ordered]@{}
    foreach ($name in $expected) {
        $st = (& docker inspect --format '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' $name 2>$null) -join ''
        if ($LASTEXITCODE -ne 0 -or $st.Trim() -ne 'running|healthy') {
            throw "Container '$name' is not running|healthy (observed: '$($st.Trim())')."
        }
        $statuses[$name] = $st.Trim()
    }
    Assert-WorldStartupSubsystemsLoaded
    return [pscustomobject]@{
        ContainerCount = $expected.Count
        TetherPresent  = $false
        Containers     = $statuses
    }
}

function Invoke-GoFunctionalApiSuite {
    param([string]$RootDir, [bool]$Quiet = $false)
    $servicesDir = Join-Path $RootDir 'src\server\services'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $servicesDir
    try {
        if ($Quiet) {
            $out = @(& go test -v -count=1 ../../tests/go/functional_api_e2e_test.go 2>&1)
            if ($LASTEXITCODE -ne 0) {
                throw "Go functional API E2E suite failed (exit $LASTEXITCODE):`n$($out -join "`n")"
            }
        } else {
            & go test -v -count=1 ../../tests/go/functional_api_e2e_test.go
            if ($LASTEXITCODE -ne 0) {
                throw "Go functional API E2E suite failed (exit $LASTEXITCODE)."
            }
        }
    } finally {
        Pop-Location
    }
    $sw.Stop()
    return [int]$sw.ElapsedMilliseconds
}

function Invoke-LauncherFunctionalApiSuite {
    param([string]$RootDir, [bool]$Quiet = $false)
    $testProj = Join-Path $RootDir 'src\tests\dotnet\SandalLauncher.Tests\SandalLauncher.Tests.csproj'
    $testDll = Join-Path $RootDir 'build\dotnet\bin\SandalLauncher.Tests\Release\net10.0\SandalLauncher.Tests.dll'
    $dotnetArgs = @('test', $testProj, '-c', 'Release')
    if (Test-Path -LiteralPath $testDll -PathType Leaf) {
        $dotnetArgs += '--no-build'
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Quiet) {
        $out = @(& dotnet @dotnetArgs 2>&1)
        if ($LASTEXITCODE -ne 0) {
            throw "SandalLauncher functional test suite failed (exit $LASTEXITCODE):`n$($out -join "`n")"
        }
    } else {
        & dotnet @dotnetArgs
        if ($LASTEXITCODE -ne 0) {
            throw "SandalLauncher functional test suite failed (exit $LASTEXITCODE)."
        }
    }
    $sw.Stop()
    return [int]$sw.ElapsedMilliseconds
}

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$totalSw = [System.Diagnostics.Stopwatch]::StartNew()

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [1/3] Verifying 11 healthy Docker containers and 0 aoc-tether containers...')
}
$dockerCheck = Assert-SandalDockerStackHealthy -RootDir $rootPath

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [2/3] Running Go backend functional API E2E suite (HTTP :8080, gRPC :9090, ICS TLS :50061, UDP :7777)...')
}
$goElapsedMs = Invoke-GoFunctionalApiSuite -RootDir $rootPath -Quiet ([bool]$Json)

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [3/3] Running SandalLauncher functional & LocalTetherListener wire-parity suite...')
}
$launcherElapsedMs = Invoke-LauncherFunctionalApiSuite -RootDir $rootPath -Quiet ([bool]$Json)
$totalSw.Stop()

$receipt = [ordered]@{
    ok                  = $true
    container_count     = $dockerCheck.ContainerCount
    aoc_tether_present  = $dockerCheck.TetherPresent
    go_api_ms           = $goElapsedMs
    launcher_api_ms     = $launcherElapsedMs
    total_ms            = [int]$totalSw.ElapsedMilliseconds
}

if ($Json) {
    Write-Output ($receipt | ConvertTo-Json -Depth 5)
} else {
    [Console]::Out.WriteLine("[SandalCore] [PASS] All 11 container services, backend APIs, and SandalLauncher APIs verified in $($receipt.total_ms)ms.")
}
