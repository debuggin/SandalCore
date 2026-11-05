#Requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$Json,
    [string]$RepoRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-SandalDockerStackHealthy {
    param([string]$RootDir)
    $envModule = Join-Path $RootDir 'scripts\lib\Env.psm1'
    if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $envModule -PathType Leaf)) {
        Import-Module $envModule -Force -DisableNameChecking
    }
    if (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue) {
        $null = Ensure-SandalDockerDaemon
    }
    $tetherRows = @(& docker ps -a --filter 'name=^/aoc-tether$' --format '{{.Names}}' 2>$null | Where-Object { $_ -and $_.Trim() -ne '' })
    if ($tetherRows.Count -gt 0) {
        throw "Deprecated aoc-tether container still exists: $($tetherRows -join ', ')"
    }
    $expected = @(
        'aoc-postgres', 'aoc-auth', 'aoc-character', 'aoc-citynode', 'aoc-quest',
        'aoc-mesh', 'aoc-gateway', 'aoc-meshgw', 'aoc-web-admin', 'aoc-world', 'aoc-ingress-proxy'
    )
    $statuses = [ordered]@{}
    foreach ($name in $expected) {
        $st = (& docker inspect --format '{{.State.Status}}|{{if .State.Health}}{{.State.Health.Status}}{{else}}none{{end}}' $name 2>$null) -join ''
        if ($LASTEXITCODE -ne 0 -or $st.Trim() -ne 'running|healthy') {
            throw "Container '$name' is not running|healthy (observed: '$($st.Trim())')."
        }
        $statuses[$name] = $st.Trim()
    }
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
