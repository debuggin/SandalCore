#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Release',
    [string]$Filter = '',
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-TestOptions {
    param(
        [string]$BaseConfig,
        [string]$BaseFilter,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $cfg = $BaseConfig
    $flt = $BaseFilter
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Config|--config|-c)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $cfg = $RawArgs[$i] }
            }
            '^(-Filter|--filter|-f)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $flt = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$' { $dry = $true }
            '^(-Json|--json)$'      { $jsn = $true }
        }
    }
    return [pscustomobject]@{ Config = $cfg; Filter = $flt; DryRun = $dry; Json = $jsn }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or ($Arguments -contains '-NoColor') -or ($Arguments -contains '--no-color') -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$pathSep = [System.IO.Path]::PathSeparator
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/SandalCore.psd1') -Force -DisableNameChecking
}

$opts = Resolve-TestOptions `
    -BaseConfig $Config `
    -BaseFilter $Filter `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -RawArgs @($Arguments)

function Out-Host {
    [CmdletBinding()]
    param([Parameter(ValueFromPipeline = $true)] $InputObject)
    begin {}
    process {
        if (-not $opts.Json) {
            Microsoft.PowerShell.Utility\Out-Host -InputObject $InputObject
        }
    }
    end {}
}

function Write-TestStep {
    param([string]$Message, [bool]$Quiet = $false)
    if (-not $Quiet -and $env:SANDAL_QUIET -ne '1') {
        [Console]::Out.WriteLine("[SandalCore] $Message")
    }
}

function Resolve-SandalTestDotnetExe {
    $cmd = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd -and (@(& $cmd.Source --list-sdks 2>$null) | Where-Object { $_ -match '^(\d+)\.' -and [int]$Matches[1] -ge 10 })) {
        return $cmd.Source
    }
    $userHome = if ($HOME) { $HOME } elseif ($env:USERPROFILE) { $env:USERPROFILE } else { '' }
    if ($userHome) {
        $userExe = Join-Path $userHome '.dotnet' $(if ($IsWindows) { 'dotnet.exe' } else { 'dotnet' })
        if (Test-Path -LiteralPath $userExe -PathType Leaf) { return $userExe }
    }
    if ($cmd) { return $cmd.Source }
    return 'dotnet'
}

function Invoke-SandalTrackedCommand {
    param([scriptblock]$Action, [bool]$AsJson = $false, [string]$ErrorMessage = 'Command failed.')
    if ($AsJson) {
        & $Action 2>&1 6>&1 | Out-Host
    } else {
        & $Action
    }
    if ($LASTEXITCODE -ne 0) {
        throw "$ErrorMessage (exit $LASTEXITCODE)."
    }
}

function Invoke-SandalGoSuites {
    param([string]$RootDir, [bool]$AsJson = $false)
    Push-Location (Join-Path $RootDir 'src/server/services')
    try {
        Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage 'go test ./... in src/server/services failed' -Action {
            & go test ./...
        }
        $goGroups = @(Get-ChildItem (Join-Path $RootDir 'src/tests/go/*_test.go') -ErrorAction SilentlyContinue) | Group-Object {
            $m = Select-String -LiteralPath $_.FullName -Pattern '^package\s+(\S+)' | Select-Object -First 1
            if ($null -ne $m) { $m.Matches[0].Groups[1].Value } else { $_.BaseName }
        }
        foreach ($grp in @($goGroups)) {
            $pkgFiles = @($grp.Group | ForEach-Object { $_.FullName })
            $pkgLabel = ($grp.Group | ForEach-Object { $_.Name }) -join ', '
            Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage "go test failed for package $($grp.Name) ($pkgLabel)" -Action {
                & go test @pkgFiles
            }
        }
    } finally {
        Pop-Location
    }
}

function Resolve-SandalDotnetTestProject {
    param([string]$RootDir, [string]$PrimaryPath, [string]$FallbackPath = '')
    $first = Join-Path $RootDir $PrimaryPath
    if (Test-Path -LiteralPath $first -PathType Leaf) { return $first }
    if ($FallbackPath) {
        $second = Join-Path $RootDir $FallbackPath
        if (Test-Path -LiteralPath $second -PathType Leaf) { return $second }
    }
    throw "Required .NET test project not found: $PrimaryPath ($FallbackPath)"
}

function Invoke-SandalDotnetSuites {
    param([string]$RootDir, [string]$BuildConfig = 'Release', [bool]$AsJson = $false)
    $dotnetExe = Resolve-SandalTestDotnetExe
    $projs = @(
        (Resolve-SandalDotnetTestProject -RootDir $RootDir -PrimaryPath 'src/clients/launcher/tests/SandalLauncher.Tests.csproj' -FallbackPath 'src/tests/dotnet/SandalLauncher.Tests/SandalLauncher.Tests.csproj'),
        (Resolve-SandalDotnetTestProject -RootDir $RootDir -PrimaryPath 'src/tools/aoc-extractor/tests/SandalExtractor.Tests.csproj' -FallbackPath 'src/tests/dotnet/SandalExtractor.Tests.csproj'),
        (Resolve-SandalDotnetTestProject -RootDir $RootDir -PrimaryPath 'src/tools/mapc/tests/Mapc.GeometryTests.csproj')
    )
    foreach ($proj in $projs) {
        Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage "dotnet test failed for $proj" -Action {
            & $dotnetExe test $proj -c $BuildConfig --nologo
        }
        if ($proj -like '*Mapc.GeometryTests.csproj') {
            Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage "dotnet run failed for $proj" -Action {
                & $dotnetExe run --project $proj -c $BuildConfig
            }
        }
    }
}

function Invoke-SandalWebAdminSuite {
    param([string]$RootDir, [bool]$AsJson = $false)
    Push-Location (Join-Path $RootDir 'src/clients/web-admin')
    try {
        Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage 'npm test (vitest run) in src/clients/web-admin failed' -Action {
            & npm test
        }
    } finally {
        Pop-Location
    }
}

function Invoke-SandalPesterSuites {
    param([string]$RootDir, [bool]$AsJson = $false)
    $sandalSuite = Join-Path $RootDir 'src/tests/powershell/Sandal.Tests.ps1'
    if (-not (Test-Path -LiteralPath $sandalSuite -PathType Leaf)) {
        throw "Required PowerShell test suite missing: $sandalSuite"
    }
    $pesterPattern = Join-Path $RootDir 'src/tests/powershell/*.Tests.ps1'
    $res = if ($AsJson) { Invoke-Pester -Path $pesterPattern -PassThru 6>$null } else { Invoke-Pester -Path $pesterPattern -PassThru }
    if ($AsJson) { $res | Out-Host }
    if ($res.FailedCount -gt 0) {
        throw "Invoke-Pester failed with $($res.FailedCount) failed test(s)."
    }
}

function Invoke-SandalContainerCppSuites {
    param([string]$RootDir, [bool]$AsJson = $false)
    $worldSt = ((& docker inspect --format '{{.State.Status}}' aoc-world 2>$null) -join '').Trim()
    $global:LASTEXITCODE = 0
    if ($worldSt -ne 'running') { return }
    $cppBins = @(
        '/app/test_world_area',
        '/app/test_world_region',
        '/app/test_world_gather',
        '/app/test_world_foliage',
        '/app/test_world_foliage_cells',
        '/app/test_world_spawn_filter',
        '/app/test_world_bootstrap',
        '/app/test_world_move_checker',
        '/app/test_world_movement',
        '/app/test_world_combat',
        '/app/test_world_ability_pipeline',
        '/app/test_world_ability_pipeline_chains',
        '/app/test_world_ability_pipeline_casts',
        '/app/test_world_loop',
        '/app/test_mesh_handover_hysteresis'
    )
    foreach ($bin in $cppBins) {
        $null | & docker exec -i aoc-world test -x $bin *> $null
        if ($LASTEXITCODE -ne 0) { throw "Required C++ test binary missing or not executable in aoc-world: $bin" }
        Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage "Container C++ domain test $bin failed in aoc-world" -Action {
            $null | & docker exec -i aoc-world $bin
        }
    }
    $funcApiScript = Join-Path $RootDir 'scripts/test_functional_apis.ps1'
    Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage 'scripts/test_functional_apis.ps1 (/app/test_world_live_e2e) failed' -Action {
        if ($AsJson) { & pwsh -NoProfile -File $funcApiScript -Json } else { & pwsh -NoProfile -File $funcApiScript }
    }
}

function Invoke-SandalHostCppSuites {
    param([string]$RootDir, [string]$BuildConfig = 'Release', [bool]$AsJson = $false, [string]$ExeSuffix = '')
    $testsDir = Join-Path (Join-Path $RootDir 'build/tests') $BuildConfig
    $hasBuiltTests = [bool](@(Get-ChildItem -Path $testsDir -Filter "test_*$ExeSuffix" -File -ErrorAction SilentlyContinue | Where-Object {
        $IsWindows -or [string]::IsNullOrEmpty($_.Extension)
    }).Count -gt 0)
    if (-not $hasBuiltTests) { return }
    $runTestsScript = Join-Path $RootDir 'scripts/run_tests.ps1'
    Invoke-SandalTrackedCommand -AsJson $AsJson -ErrorMessage 'scripts/run_tests.ps1 failed' -Action {
        & $runTestsScript -Config $BuildConfig -NoBuild
    }
}

$quiet = [bool]$opts.Json
if ($quiet) { $env:SANDAL_QUIET = '1' }
if (-not $opts.DryRun) {
    Write-TestStep -Message "[Step 1/2] Staging dist/ test assets (config=$($opts.Config))..." -Quiet $quiet
    $null = Sync-SandalDistStage -RepoRoot $rootPath -Config $opts.Config
    $goMod = Join-Path $rootPath 'src\server\services\go.mod'
    if (Test-Path -LiteralPath $goMod -PathType Leaf) {
        $goVer = if ([System.IO.File]::ReadAllText($goMod) -match '(?m)^go\s+([0-9.]+)') { $Matches[1] } else { '1.24.0' }
        $goWork = Join-Path $rootPath 'dist\go.work'
        [System.IO.File]::WriteAllText($goWork, "go $goVer`nuse ../src/server/services`n", [System.Text.UTF8Encoding]::new($false))
        $env:GOWORK = $goWork
    }
    Write-TestStep -Message "[Step 2/2] Running test suites (filter='$($opts.Filter)')..." -Quiet $quiet
    $distConfigDir = Join-Path (Join-Path $rootPath 'dist') $opts.Config
    $prevPath = $env:PATH
    $env:PATH = "$distConfigDir$pathSep$prevPath"
    try {
        $normFilter = if ([string]::IsNullOrWhiteSpace($opts.Filter)) { '' } else { $opts.Filter.Trim().ToLowerInvariant() }
        if ($normFilter -in @('', 'cpp')) {
            Invoke-SandalHostCppSuites -RootDir $rootPath -BuildConfig $opts.Config -AsJson $opts.Json -ExeSuffix $exeExt
            Invoke-SandalContainerCppSuites -RootDir $rootPath -AsJson $opts.Json
        }
        if ($normFilter -in @('', 'go')) {
            Invoke-SandalGoSuites -RootDir $rootPath -AsJson $opts.Json
        }
        if ($normFilter -in @('', 'dotnet', 'csharp', 'launcher')) {
            Invoke-SandalDotnetSuites -RootDir $rootPath -BuildConfig $opts.Config -AsJson $opts.Json
        }
        if ($normFilter -in @('', 'web', 'web-admin', 'vitest', 'npm')) {
            Invoke-SandalWebAdminSuite -RootDir $rootPath -AsJson $opts.Json
        }
        if ($normFilter -in @('', 'powershell', 'pester')) {
            Invoke-SandalPesterSuites -RootDir $rootPath -AsJson $opts.Json
        }
        if ($normFilter -notin @('', 'cpp', 'go', 'dotnet', 'csharp', 'launcher', 'web', 'web-admin', 'vitest', 'npm', 'powershell', 'pester')) {
            $pattern = if ($normFilter -in @('e2e', 'e2e_user_journey', 'e2euserjourney')) { 'E2EUserJourney' } else { $opts.Filter }
            Push-Location (Join-Path $rootPath 'src\server\services')
            try {
                Get-ChildItem (Join-Path $rootPath 'src/tests/go/*_test.go') | Where-Object { $_.Name -match $pattern } | ForEach-Object {
                    if ($opts.Json) { & go test $_.FullName 6>&1 | Out-Host } else { & go test $_.FullName }
                    if ($LASTEXITCODE -ne 0) { throw "go test failed for $($_.Name)" }
                }
            } finally {
                Pop-Location
            }
            $pesterMatches = @(Get-ChildItem (Join-Path $rootPath 'src/tests/powershell/*.Tests.ps1') | Where-Object { $_.Name -match $pattern })
            if ($pesterMatches.Count -gt 0) {
                $paths = @($pesterMatches | ForEach-Object { $_.FullName })
                $res = if ($opts.Json) { Invoke-Pester -Path $paths -PassThru 6>$null } else { Invoke-Pester -Path $paths -PassThru }
                if ($opts.Json) { $res | Out-Host }
                if ($res.FailedCount -gt 0) { throw "Invoke-Pester failed with $($res.FailedCount) failed test(s)." }
            }
        }
    } finally {
        $env:PATH = $prevPath
    }
} else {
    Write-TestStep -Message "[Step 1/2] (dry-run) Would stage dist/ and run test suites (config=$($opts.Config), filter='$($opts.Filter)')." -Quiet $quiet
}

$payload = [ordered]@{
    ok       = $true
    command  = 'test'
    status   = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    config   = $opts.Config
    filter   = $opts.Filter
    dry_run  = [bool]$opts.DryRun
    no_color = $useNoColor
    steps    = @('sync-dist', 'run-tests')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'test' (config={0}, status={1}, dry_run={2})" -f $opts.Config, $payload.status, $opts.DryRun)
}
