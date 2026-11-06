#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
    [switch]$Detach,
    [switch]$Build,
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [string[]]$Arguments = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-UpOptions {
    param(
        [string]$BaseProfile,
        [bool]$BaseDetach,
        [bool]$BaseBuild,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $prof = $BaseProfile
    $det = $BaseDetach
    $bld = $BaseBuild
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-Detach|--detach|-d)$' { $det = $true }
            '^(-Build|--build)$'      { $bld = $true }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
        }
    }
    if ($prof -notin @('default', 'dev', 'admin', 'full')) {
        throw [System.ArgumentException]::new("Unsupported profile '$prof'. Valid profiles: default, dev, admin, full.")
    }
    return [pscustomobject]@{ Profile = $prof; Detach = $det; Build = $bld; DryRun = $dry; Json = $jsn }
}

function Get-NewestSourceWriteUtc {
    param([string]$RootPath, [string[]]$RelPaths)
    $newest = [datetime]::MinValue
    foreach ($rel in $RelPaths) {
        $full = Join-Path $RootPath $rel
        if (-not (Test-Path -LiteralPath $full)) { continue }
        $item = Get-Item -LiteralPath $full -Force
        if (-not $item.PSIsContainer) {
            if ($item.LastWriteTimeUtc -gt $newest) { $newest = $item.LastWriteTimeUtc }
            continue
        }
        foreach ($f in (Get-ChildItem -LiteralPath $full -Recurse -File -Force -ErrorAction SilentlyContinue)) {
            if ($f.LastWriteTimeUtc -gt $newest) { $newest = $f.LastWriteTimeUtc }
        }
    }
    return $newest
}

function Get-DockerCreatedUtc {
    param([string]$Target)
    $raw = (& docker inspect --format '{{.Created}}' $Target 2>$null | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($raw)) { return [datetime]::MinValue }
    try { return ([datetime]::Parse($raw.Trim(), [System.Globalization.CultureInfo]::InvariantCulture, [System.Globalization.DateTimeStyles]::RoundtripKind)).ToUniversalTime() } catch { return [datetime]::MinValue }
}

function Get-SandalStaleComposeServices {
    param([string]$RootPath, [bool]$ForceBuild = $false)
    $goServices = @('auth', 'character', 'citynode', 'quest', 'mesh', 'gateway', 'mesh-gateway', 'web-admin')
    if ($ForceBuild) { return @('world') + $goServices }
    $stale = [System.Collections.Generic.List[string]]::new()
    $cppNewest = Get-NewestSourceWriteUtc -RootPath $RootPath -RelPaths @('src\server\world', 'src\libs', 'src\server\proto', 'src\deploy\docker\cpp.Dockerfile', 'build\gen\world')
    $worldCreated = Get-DockerCreatedUtc -Target 'aoc-world'
    $imgCreated = Get-DockerCreatedUtc -Target 'sandal-cpp:local'
    if ($imgCreated -gt $worldCreated) { $worldCreated = $imgCreated }
    if ($worldCreated -eq [datetime]::MinValue -or $cppNewest -gt $worldCreated) { $stale.Add('world') }
    $goNewest = Get-NewestSourceWriteUtc -RootPath $RootPath -RelPaths @('src\server\services', 'src\server\proto', 'src\deploy\docker\go.Dockerfile', 'build\gen\go')
    foreach ($svc in $goServices) {
        $cTime = Get-DockerCreatedUtc -Target "aoc-$svc"
        if ($cTime -eq [datetime]::MinValue -or $goNewest -gt $cTime) { $stale.Add($svc) }
    }
    return $stale.ToArray()
}

function Get-UpStageReadiness {
    param([string]$RootPath)
    $items = [System.Collections.Generic.List[object]]::new()
    foreach ($stage in (Get-SandalStageOrder)) {
        $upToDate = Test-SandalStageUpToDate -Stage $stage -RepoRoot $RootPath
        $items.Add([pscustomobject]@{
            stage      = $stage
            up_to_date = [bool]$upToDate
        })
    }
    return $items.ToArray()
}

function Build-ComposeUpArguments {
    param([string]$ComposeFile, [string]$EnvFile, [string]$ProfileName, [bool]$IsDetach)
    $cmdArgs = [System.Collections.Generic.List[string]]::new()
    $cmdArgs.AddRange([string[]]@('compose', '-f', $ComposeFile, '--env-file', $EnvFile))
    if ($ProfileName -ne 'default') {
        $cmdArgs.AddRange([string[]]@('--profile', $ProfileName))
    }
    $cmdArgs.Add('up')
    if ($IsDetach) {
        $cmdArgs.AddRange([string[]]@('-d', '--wait'))
    }
    return $cmdArgs.ToArray()
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Get-SandalStageOrder -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}
if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\Env.psm1') -Force -DisableNameChecking
}

function Write-UpStep {
    param([string]$Message, [bool]$Quiet = $false)
    if (-not $Quiet -and $env:SANDAL_QUIET -ne '1') {
        [Console]::Out.WriteLine("[SandalCore] $Message")
    }
}

function Remove-SandalStaleTestAccounts {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return }
    $sql = "DELETE FROM accounts WHERE lower(username) LIKE 'tmp\_%' ESCAPE '\' OR lower(username) LIKE 'temp\_%' ESCAPE '\' OR lower(username) LIKE 'e2e\_%' ESCAPE '\' OR lower(username) = 'ephem_hero';"
    try { $null | & docker exec -i aoc-postgres psql -U aoc -d aoc -tAc $sql *> $null } catch { }
    $global:LASTEXITCODE = 0
}

$opts = Resolve-UpOptions -BaseProfile $Profile -BaseDetach ([bool]$Detach) -BaseBuild ([bool]$Build) -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -RawArgs (@($Arguments) + @($RemainingArgs))
$quiet = [bool]$opts.Json
$fromRun = ($env:SANDAL_FROM_RUN -eq '1')
if ($quiet) { $env:SANDAL_QUIET = '1' }
$deploySync = $null
if (-not $fromRun) {
    Write-UpStep -Message '[Step 1/3] Staging deploy/ manifests and environment variables...' -Quiet $quiet
    $deploySync = Sync-SandalDeployStage -RepoRoot $rootPath
}
$composeFile = Join-Path $rootPath 'deploy\compose.yml'
$envFile = Join-Path $rootPath 'deploy\.env'
$composeArgs = Build-ComposeUpArguments -ComposeFile $composeFile -EnvFile $envFile -ProfileName $opts.Profile -IsDetach $opts.Detach
$stageChecks = @(Get-UpStageReadiness -RootPath $rootPath)
$allUpToDate = (@($stageChecks | Where-Object { -not $_.up_to_date }).Count -eq 0)

if (-not $opts.DryRun) {
    if (-not $fromRun) {
        Write-UpStep -Message '[Step 2/3] Verifying WSL2 & Docker daemon readiness...' -Quiet $quiet
        $null = Ensure-SandalDockerDaemon
        Write-UpStep -Message "[Step 3/3] Starting Docker Compose containers (profile=$($opts.Profile), detach=$($opts.Detach))..." -Quiet $quiet
    }
    if (Get-Command Set-SandalWslComposeMountEnv -ErrorAction SilentlyContinue) {
        Set-SandalWslComposeMountEnv -DeployDir (Join-Path $rootPath 'deploy')
    }
    Push-Location $rootPath
    try {
        $staleServices = @(Get-SandalStaleComposeServices -RootPath $rootPath -ForceBuild $opts.Build)
        if ($staleServices.Count -gt 0) {
            Write-UpStep -Message "Rebuilding updated container images ($($staleServices -join ', '))..." -Quiet $quiet
            $buildArgs = @('compose', '-f', $composeFile, '--env-file', $envFile, 'build') + $staleServices
            & docker @buildArgs
            if ($LASTEXITCODE -ne 0) {
                throw "docker compose build failed with exit code $LASTEXITCODE."
            }
        }
        & docker @composeArgs
        if ($LASTEXITCODE -ne 0) {
            throw "docker compose up failed with exit code $LASTEXITCODE."
        }
        Remove-SandalStaleTestAccounts
    } finally {
        Pop-Location
    }
} else {
    Write-UpStep -Message '[Step 2/3] (dry-run) Would verify WSL2 & Docker daemon readiness.' -Quiet ($quiet -or $fromRun)
    Write-UpStep -Message "[Step 3/3] (dry-run) Would run: docker $($composeArgs -join ' ')" -Quiet ($quiet -or $fromRun)
}

$payload = [ordered]@{
    ok                = $true
    command           = 'up'
    status            = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    profile           = $opts.Profile
    detach            = [bool]$opts.Detach
    dry_run           = [bool]$opts.DryRun
    no_color          = $useNoColor
    env_path          = $envFile
    env_initialized   = ($null -ne $deploySync) -or (Test-Path -LiteralPath $envFile -PathType Leaf)
    compose_file      = $composeFile
    docker_args       = $composeArgs
    stages_up_to_date = $allUpToDate
    stages            = $stageChecks
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} elseif (-not $fromRun) {
    Write-Output ("SandalCore 'up' (profile={0}, detach={1}, dry_run={2}): docker {3}" -f $opts.Profile, $opts.Detach, $opts.DryRun, ($composeArgs -join ' '))
}
