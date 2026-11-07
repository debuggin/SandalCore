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

function Test-SandalSingleWorkerTopology {
    if ($env:AOC_WORLD_SHARD_MODE -eq 'single') { return $true }
    $worldSt = (& docker inspect --format '{{.State.Status}}' aoc-world 2>$null) -join ''
    $global:LASTEXITCODE = 0
    if ($worldSt.Trim() -ne 'running') { return $false }
    $junSt = (& docker inspect --format '{{.State.Status}}' aoc-jundark 2>$null) -join ''
    $global:LASTEXITCODE = 0
    return ($junSt.Trim() -ne 'running')
}

function Start-SandalComposeStackIfNeeded {
    param([string]$RootDir, [string[]]$ExpectedContainers)
    $needsStart = $false
    foreach ($name in $ExpectedContainers) {
        $st = (& docker inspect --format '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' $name 2>$null) -join ''
        if ($LASTEXITCODE -ne 0 -or $st.Trim() -ne 'running|healthy') { $needsStart = $true; break }
    }
    if (-not $needsStart) { return }
    if ($env:AOC_WORLD_SHARD_MODE -eq 'single') {
        & pwsh -NoProfile -File (Join-Path $RootDir 'scripts\sandal\up.ps1') -Detach -SingleWorker -Json *> $null
    } else {
        & pwsh -NoProfile -File (Join-Path $RootDir 'scripts\sandal\up.ps1') -Detach -Json *> $null
    }
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
        'contentc version 41',
        'Blueprint table .*: [1-9]\d* pawn Blueprints',
        'mob attack animations: [1-9]\d* Blueprints, [1-9]\d* montages',
        '\[looks\] [1-9]\d* Blueprints with a look.* [1-9]\d* recorded customizations in [1-9]\d* kinds, [1-9]\d* customization presets',
        'ground .*area_nav\.bin: [1-9]\d* terrain grids',
        '[1-9]\d* NPCs on the ground',
        'foliage.*on: [1-9]\d* instances'
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
    $isSingle = Test-SandalSingleWorkerTopology
    $expected = @(
        'aoc-postgres', 'aoc-auth', 'aoc-character', 'aoc-citynode', 'aoc-quest',
        'aoc-mesh', 'aoc-gateway', 'aoc-mesh-gateway', 'aoc-web-admin', 'aoc-world'
    )
    if (-not $isSingle) {
        $expected += @('aoc-jundark', 'aoc-anvils', 'aoc-turquoise-sea', 'aoc-verra-san-node-sunhaven', 'aoc-verra-san-node-azmaran')
    }
    $expected += 'aoc-ingress-proxy'
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
        $sql = "DELETE FROM accounts WHERE lower(username) LIKE 'tmp\_%' ESCAPE '\' OR lower(username) LIKE 'temp\_%' ESCAPE '\' OR lower(username) LIKE 'e2e\_%' ESCAPE '\' OR lower(username) = 'ephem_hero';"
        try { $null | & docker exec -i aoc-postgres psql -U aoc -d aoc -tAc $sql *> $null } catch { }
        $global:LASTEXITCODE = 0
    }
    $sw.Stop()
    return [int]$sw.ElapsedMilliseconds
}

function Assert-WebAdminTilesAndMetadata {
    param([string]$BaseUrl, [hashtable]$Headers)
    foreach ($tile in @('/api/v1/world/tiles/0/0/0.png', '/api/v1/world/tiles/6/34/24.png', '/api/v1/world/tiles/7/68/49.png', '/api/v1/world/tiles/6/0/0.png', '/api/v1/world/tiles/7/0/0.png', '/api/v1/world/tiles/8/0/0.png', '/api/v1/world/tiles/8/25/4.png')) {
        $resp = Invoke-WebRequest -Uri ($BaseUrl + $tile) -Headers $Headers -Method Get -TimeoutSec 10
        if ($resp.StatusCode -ne 200 -or ($resp.Headers['Content-Type'] -join '') -notmatch 'image/png') {
            throw "GET $tile returned $($resp.StatusCode) ($($resp.Headers['Content-Type'] -join '')), expected 200 image/png."
        }
    }
    $meta = Invoke-RestMethod -Uri "$BaseUrl/api/v1/world/tiles/metadata.json" -Headers $Headers -Method Get -TimeoutSec 10
    if (@($meta.nodes).Count -ne 26 -or @($meta.icons).Count -lt 30) {
        throw "GET /api/v1/world/tiles/metadata.json invalid: nodes=$(@($meta.nodes).Count) icons=$(@($meta.icons).Count)."
    }
    $placesEnv = Invoke-RestMethod -Uri "$BaseUrl/api/v1/catalog/places?limit=300" -Headers $Headers -Method Get -TimeoutSec 10
    $places = if ($placesEnv.PSObject.Properties['places']) { @($placesEnv.places) } else { @($placesEnv.items) }
    if ($places.Count -lt 150) {
        throw "GET /api/v1/catalog/places?limit=300 returned $($places.Count) places (expected >= 150)."
    }
    $iconUrls = @($places | ForEach-Object { [string]$_.icon_url } | Where-Object { $_ } | Select-Object -Unique | Select-Object -First 3)
    foreach ($iconPath in (@('/api/v1/world/icons/TUI_Icon_Map_MinorPOI.png') + $iconUrls)) {
        $ir = Invoke-WebRequest -Uri ($BaseUrl + $iconPath) -Headers $Headers -Method Get -TimeoutSec 10
        if ($ir.StatusCode -ne 200 -or ($ir.Headers['Content-Type'] -join '') -notmatch 'image/png' -or $ir.RawContentLength -le 0) {
            throw "GET $iconPath failed: status=$($ir.StatusCode) ct=$($ir.Headers['Content-Type'] -join '')."
        }
    }
}

function Assert-WebAdminWorldStateAndContainers {
    param([string]$BaseUrl, [hashtable]$Headers)
    foreach ($sp in @('/api/v1/world/state', '/api/v1/gm/world/state')) {
        $st = Invoke-RestMethod -Uri ($BaseUrl + $sp) -Headers $Headers -Method Get -TimeoutSec 10
        $ents = @($st.entities)
        $resCnt = if ($st.layer_counts.PSObject.Properties['resources']) { [int]$st.layer_counts.resources } else { [int]$st.layer_counts.resource_nodes }
        if ($ents.Count -lt 1000 -or [int]$st.layer_counts.monsters -le 0 -or [int]$st.layer_counts.npcs -le 0 -or $resCnt -le 0 -or [int]$st.layer_counts.pois -lt 150) {
            throw "GET $sp invalid counts: entities=$($ents.Count) monsters=$($st.layer_counts.monsters) npcs=$($st.layer_counts.npcs) resources=$resCnt pois=$($st.layer_counts.pois)."
        }
    }
    foreach ($pp in @('/api/v1/world/players', '/api/v1/gm/world/players')) {
        $rawPl = (Invoke-WebRequest -Uri ($BaseUrl + $pp) -Headers $Headers -Method Get -TimeoutSec 10).Content
        if ($rawPl -match '(?i)"(Kaelen|Lyra)"') {
            throw "GET $pp contains synthetic fake player Kaelen or Lyra."
        }
    }
    $cSw = [System.Diagnostics.Stopwatch]::StartNew()
    $cDoc = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/containers" -Headers $Headers -Method Get -TimeoutSec 10
    $cSw.Stop()
    $cList = if ($cDoc.PSObject.Properties['containers']) { @($cDoc.containers) } else { @($cDoc.items) }
    $minContainers = if (Test-SandalSingleWorkerTopology) { 11 } else { 15 }
    if ($cSw.Elapsed.TotalSeconds -ge 3.0 -or $cList.Count -lt $minContainers -or -not $cDoc.world_status.connected -or [int]$cDoc.world_status.npcs_total -le 0) {
        throw "GET /api/v1/gm/containers failed: elapsed=$($cSw.ElapsedMilliseconds)ms count=$($cList.Count) expected_min=$minContainers connected=$($cDoc.world_status.connected)."
    }
    $logsDoc = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/logs?container=aoc-world&tail=30" -Headers $Headers -Method Get -TimeoutSec 10
    if (@($logsDoc.items).Count -le 0) {
        throw 'GET /api/v1/gm/logs?container=aoc-world&tail=30 returned 0 log items.'
    }
}

function Assert-WebAdminGovernanceAndWorkItems {
    param([string]$BaseUrl, [hashtable]$Headers)
    try {
        $null = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/node/mayor" -Headers $Headers -Method Post -ContentType 'application/json' -Body '{"node_name":"NewAela","mayor_guid":"char-9001","mayor_name":"ArchonVael"}' -TimeoutSec 10
        $st = Invoke-RestMethod -Uri "$BaseUrl/api/v1/world/state" -Headers $Headers -Method Get -TimeoutSec 10
        $na = @($st.nodes | Where-Object { $_.name -match 'NewAela' -or $_.display_name -eq 'New Aela' })[0]
        if (-not $na -or $na.mayor_name -ne 'ArchonVael' -or $na.election_status -ne 'active_term') {
            throw "NewAela mayor round-trip failed: mayor='$($na.mayor_name)' election='$($na.election_status)'."
        }
        foreach ($pair in @(@('start', 'registration'), @('open_voting', 'voting'), @('conclude', 'active_term'), @('cancel', 'none'))) {
            $body = @{ node_name = 'NewAela'; action = $pair[0] } | ConvertTo-Json -Compress
            $elResp = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/node/election" -Headers $Headers -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 10
            if ($elResp.election_status -and $elResp.election_status -ne $pair[1]) {
                throw "NewAela election action '$($pair[0])' returned '$($elResp.election_status)', expected '$($pair[1])'."
            }
        }
        $aud = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/audit" -Headers $Headers -Method Get -TimeoutSec 10
        $audList = if ($aud.PSObject.Properties['entries']) { @($aud.entries) } else { @($aud.items) }
        if ($audList.Count -lt 4) {
            throw "GET /api/v1/gm/audit returned $($audList.Count) entries after GM work-item execution (expected >= 4)."
        }
    } finally {
        try {
            $null = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/node/mayor" -Headers $Headers -Method Post -ContentType 'application/json' -Body '{"node_name":"NewAela","mayor_guid":"","mayor_name":""}' -TimeoutSec 5
            $null = Invoke-RestMethod -Uri "$BaseUrl/api/v1/gm/node/election" -Headers $Headers -Method Post -ContentType 'application/json' -Body '{"node_name":"NewAela","action":"cancel"}' -TimeoutSec 5
            $null = Invoke-RestMethod -Uri "$BaseUrl/api/v1/world/nodes/NewAela" -Headers $Headers -Method Put -ContentType 'application/json' -Body '{"level":3}' -TimeoutSec 5
        } catch { }
    }
}

function Invoke-WebAdminFunctionalApiSuite {
    param([string]$RootDir, [bool]$Quiet = $false)
    $servicesDir = Join-Path $RootDir 'src\server\services'
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $prevLiveEnv = $env:SANDAL_REQUIRE_LIVE_WEB_ADMIN
    $env:SANDAL_REQUIRE_LIVE_WEB_ADMIN = '1'
    Push-Location $servicesDir
    try {
        if ($Quiet) {
            $out = @(& go test -v -count=1 ../../tests/go/web_admin_api_e2e_test.go ../../tests/go/web_admin_handover_and_auth_e2e_test.go 2>&1)
            if ($LASTEXITCODE -ne 0) {
                throw "Web Admin Go E2E suite failed (exit $LASTEXITCODE):`n$($out -join "`n")"
            }
        } else {
            & go test -v -count=1 ../../tests/go/web_admin_api_e2e_test.go ../../tests/go/web_admin_handover_and_auth_e2e_test.go
            if ($LASTEXITCODE -ne 0) {
                throw "Web Admin Go E2E suite failed (exit $LASTEXITCODE)."
            }
        }
        $hdrs = @{ Authorization = 'Bearer gm-architect-dev-token'; Accept = 'application/json' }
        Assert-WebAdminTilesAndMetadata -BaseUrl 'http://127.0.0.1:8080' -Headers $hdrs
        Assert-WebAdminWorldStateAndContainers -BaseUrl 'http://127.0.0.1:8080' -Headers $hdrs
        Assert-WebAdminGovernanceAndWorkItems -BaseUrl 'http://127.0.0.1:8080' -Headers $hdrs
    } finally {
        Pop-Location
        $env:SANDAL_REQUIRE_LIVE_WEB_ADMIN = $prevLiveEnv
        $global:LASTEXITCODE = 0
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

$expectedContainerTarget = if (Test-SandalSingleWorkerTopology) { 11 } else { 16 }
if (-not $Json) {
    [Console]::Out.WriteLine("[SandalCore] [1/4] Verifying $expectedContainerTarget healthy Docker containers and 0 aoc-tether containers...")
}
$dockerCheck = Assert-SandalDockerStackHealthy -RootDir $rootPath

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [2/4] Running Go backend functional API E2E suite (HTTP :8080, gRPC :9090, ICS TLS :50061, UDP :7777)...')
}
$goElapsedMs = Invoke-GoFunctionalApiSuite -RootDir $rootPath -Quiet ([bool]$Json)

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [3/4] Running Web Admin real-time map, WorldStream, container analytics & governance E2E suite...')
}
$webAdminElapsedMs = Invoke-WebAdminFunctionalApiSuite -RootDir $rootPath -Quiet ([bool]$Json)

if (-not $Json) {
    [Console]::Out.WriteLine('[SandalCore] [4/4] Running SandalLauncher functional & LocalTetherListener wire-parity suite...')
}
$launcherElapsedMs = Invoke-LauncherFunctionalApiSuite -RootDir $rootPath -Quiet ([bool]$Json)
$totalSw.Stop()

$receipt = [ordered]@{
    ok                  = $true
    container_count     = $dockerCheck.ContainerCount
    aoc_tether_present  = $dockerCheck.TetherPresent
    go_api_ms           = $goElapsedMs
    web_admin_api_ms    = $webAdminElapsedMs
    launcher_api_ms     = $launcherElapsedMs
    total_ms            = [int]$totalSw.ElapsedMilliseconds
}

if ($Json) {
    Write-Output ($receipt | ConvertTo-Json -Depth 5)
} else {
    [Console]::Out.WriteLine("[SandalCore] [PASS] All $($dockerCheck.ContainerCount) container services, backend APIs, Web Admin APIs, and SandalLauncher APIs verified in $($receipt.total_ms)ms.")
}
