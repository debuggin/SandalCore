#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
    [switch]$Detach,
    [switch]$Build,
    [switch]$SingleWorker,
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
        [hashtable]$Defaults,
        [string[]]$RawArgs
    )
    $prof = [string]$Defaults.Profile
    $det = [bool]$Defaults.Detach
    $bld = [bool]$Defaults.Build
    $sng = [bool]$Defaults.SingleWorker
    $dry = [bool]$Defaults.DryRun
    $jsn = [bool]$Defaults.Json
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-Detach|--detach|-d)$'             { $det = $true }
            '^(-Build|--build)$'                  { $bld = $true }
            '^(-SingleWorker|--single-worker)$'   { $sng = $true }
            '^(-DryRun|--dry-run)$'               { $dry = $true }
            '^(-Json|--json)$'                    { $jsn = $true }
        }
    }
    if ($prof -notin @('default', 'dev', 'admin', 'full')) {
        throw [System.ArgumentException]::new("Unsupported profile '$prof'. Valid profiles: default, dev, admin, full.")
    }
    return [pscustomobject]@{ Profile = $prof; Detach = $det; Build = $bld; SingleWorker = $sng; DryRun = $dry; Json = $jsn }
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
    $goServices = @('auth', 'character', 'citynode', 'quest', 'mesh', 'gateway', 'mesh-gateway')
    if ($ForceBuild) { return @('world') + $goServices + @('web-admin') }
    $stale = [System.Collections.Generic.List[string]]::new()
    $cppNewest = Get-NewestSourceWriteUtc -RootPath $RootPath -RelPaths @('src\server\world', 'src\libs', 'src\server\proto', 'src\deploy\docker\cpp.Dockerfile', 'build\gen\world')
    $worldCreated = Get-DockerCreatedUtc -Target 'aoc-world'
    $imgCreated = Get-DockerCreatedUtc -Target 'sandal-world:local'
    if ($imgCreated -gt $worldCreated) { $worldCreated = $imgCreated }
    if ($worldCreated -eq [datetime]::MinValue -or $cppNewest -gt $worldCreated) { $stale.Add('world') }
    $goNewest = Get-NewestSourceWriteUtc -RootPath $RootPath -RelPaths @('src\server\services', 'src\server\proto', 'src\deploy\docker\go.Dockerfile', 'build\gen\go')
    foreach ($svc in $goServices) {
        $cTime = Get-DockerCreatedUtc -Target "aoc-$svc"
        $sImgTime = Get-DockerCreatedUtc -Target "sandal-${svc}:local"
        if ($sImgTime -gt $cTime) { $cTime = $sImgTime }
        if ($cTime -eq [datetime]::MinValue -or $goNewest -gt $cTime) { $stale.Add($svc) }
    }
    $webNewest = Get-NewestSourceWriteUtc -RootPath $RootPath -RelPaths @('src\clients\web-admin\src', 'src\clients\web-admin\Dockerfile')
    $webCreated = Get-DockerCreatedUtc -Target 'aoc-web-admin'
    $webImgCreated = Get-DockerCreatedUtc -Target 'sandal-web-admin:local'
    if ($webImgCreated -gt $webCreated) { $webCreated = $webImgCreated }
    if ($webCreated -eq [datetime]::MinValue -or $webNewest -gt $webCreated) { $stale.Add('web-admin') }
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
    param([string]$ComposeFile, [string]$EnvFile, [string]$ProfileName, [string]$ShardMode = 'single', [bool]$IsDetach = $false)
    $cmdArgs = [System.Collections.Generic.List[string]]::new()
    $cmdArgs.AddRange([string[]]@('compose', '-f', $ComposeFile, '--env-file', $EnvFile))
    if ($ProfileName -ne 'default') {
        $cmdArgs.AddRange([string[]]@('--profile', $ProfileName))
    }
    $effShard = $ShardMode.Trim().ToLowerInvariant()
    switch ($effShard) {
        'mixed'                 { $cmdArgs.AddRange([string[]]@('--profile', 'mixed')) }
        'regional'              { $cmdArgs.AddRange([string[]]@('--profile', 'regional')) }
        'region'                { $cmdArgs.AddRange([string[]]@('--profile', 'regional')) }
        'zoi'                   { $cmdArgs.AddRange([string[]]@('--profile', 'zoi')) }
        'dynamic_zoi'           { $cmdArgs.AddRange([string[]]@('--profile', 'zoi')) }
        'node_plug_zoi'         { $cmdArgs.AddRange([string[]]@('--profile', 'node_plug_zoi')) }
        'dynamic_node_plug_zoi' { $cmdArgs.AddRange([string[]]@('--profile', 'node_plug_zoi')) }
    }
    $cmdArgs.Add('up')
    if ($IsDetach) {
        $cmdArgs.AddRange([string[]]@('-d', '--wait'))
    }
    if ($effShard -in @('single', 'world')) {
        $cmdArgs.AddRange([string[]]@(
            'postgres', 'auth', 'character', 'citynode', 'quest',
            'mesh', 'gateway', 'mesh-gateway', 'world', 'web-admin', 'ingress-proxy'
        ))
    }
    return $cmdArgs.ToArray()
}

function Set-SandalTopologyWorkerMode {
    param([string]$ShardMode = 'single', [string]$EnvFile = '', [bool]$ForceSingle = $false, [bool]$RemoveExtraContainers = $false)
    $mode = if ($ForceSingle -or [string]::IsNullOrWhiteSpace($ShardMode)) { 'single' } else { $ShardMode.Trim().ToLowerInvariant() }
    $extraToRemove = @('aoc-world-riverlands', 'aoc-world-sandsquall', 'aoc-world-jundark', 'aoc-world-anvils', 'aoc-world-turquoise-sea', 'aoc-world-sandsquall-sunhaven', 'aoc-world-sandsquall-azmaran')
    switch ($mode) {
        'single' {
            $env:AOC_WORLD_TOPOLOGY_MODE = 'single'; $env:AOC_WORLD_SHARD_MODE = 'single'
            $env:AOC_WORLD_REGION = 'riverlands'; $env:AOC_WORLD_WORKER_ID = 'world-riverlands'; $env:AOC_WORLD_WORKER_INDEX = '0'
            $env:AOC_WORLD_WORKERS = 'world-riverlands|riverlands|region|world:50061|world:50062|aoc-world|0'
            if ($ForceSingle -and $EnvFile -and (Test-Path -LiteralPath $EnvFile -PathType Leaf)) {
                $raw = [System.IO.File]::ReadAllText($EnvFile, [System.Text.Encoding]::UTF8)
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_TOPOLOGY_MODE=.*$', 'AOC_WORLD_TOPOLOGY_MODE=single')
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_SHARD_MODE=.*$', 'AOC_WORLD_SHARD_MODE=single')
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_REGION=.*$', 'AOC_WORLD_REGION=riverlands')
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_WORKER_ID=.*$', 'AOC_WORLD_WORKER_ID=world-riverlands')
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_WORKER_INDEX=.*$', 'AOC_WORLD_WORKER_INDEX=0')
                $raw = [regex]::Replace($raw, '(?m)^AOC_WORLD_WORKERS=.*$', 'AOC_WORLD_WORKERS=world-riverlands|riverlands|region|world:50061|world:50062|aoc-world|0')
                [System.IO.File]::WriteAllText($EnvFile, $raw, [System.Text.UTF8Encoding]::new($false))
            }
            $extraToRemove += @('aoc-sandsquall', 'aoc-jundark', 'aoc-anvils', 'aoc-turquoise-sea', 'aoc-verra-san-node-sunhaven', 'aoc-verra-san-node-azmaran', 'aoc-sunhaven', 'aoc-azmaran')
        }
        'world' {
            $env:AOC_WORLD_TOPOLOGY_MODE = 'world'; $env:AOC_WORLD_SHARD_MODE = 'world'
            $env:AOC_WORLD_REGION = 'all'; $env:AOC_WORLD_WORKER_ID = 'world-verra'; $env:AOC_WORLD_WORKER_INDEX = '0'
            $env:AOC_WORLD_WORKERS = 'world-verra|all|world|world:50061|world:50062|aoc-world|0'
            $extraToRemove += @('aoc-sandsquall', 'aoc-jundark', 'aoc-anvils', 'aoc-turquoise-sea', 'aoc-verra-san-node-sunhaven', 'aoc-verra-san-node-azmaran', 'aoc-sunhaven', 'aoc-azmaran')
        }
        'mixed' {
            $extraToRemove += @('aoc-sandsquall', 'aoc-sunhaven', 'aoc-azmaran')
        }
        { $_ -in @('regional', 'region') } {
            $extraToRemove += @('aoc-verra-san-node-sunhaven', 'aoc-verra-san-node-azmaran', 'aoc-sunhaven', 'aoc-azmaran')
        }
        { $_ -in @('zoi', 'dynamic_zoi') } {
            $extraToRemove += @('aoc-sandsquall', 'aoc-jundark', 'aoc-anvils', 'aoc-turquoise-sea', 'aoc-sunhaven', 'aoc-azmaran')
        }
        { $_ -in @('node_plug_zoi', 'dynamic_node_plug_zoi') } {
            $extraToRemove += @('aoc-sandsquall', 'aoc-jundark', 'aoc-anvils', 'aoc-turquoise-sea')
        }
    }
    if ($RemoveExtraContainers -and (Get-Command docker -ErrorAction SilentlyContinue)) {
        try { & docker rm -f @extraToRemove *> $null } catch { }
        $global:LASTEXITCODE = 0
    }
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

function Sync-SandalPostgresPassword {
    param([string]$ComposeFile, [string]$EnvFile)
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return }
    & docker compose -f $ComposeFile --env-file $EnvFile up -d --wait postgres *> $null
    if ($LASTEXITCODE -ne 0) { return }
    $cmd = 'psql -U "${POSTGRES_USER:-aoc}" -d "${POSTGRES_DB:-aoc}" -tAc "ALTER ROLE \"${POSTGRES_USER:-aoc}\" WITH PASSWORD ''$POSTGRES_PASSWORD'';"'
    try { $null | & docker exec -i aoc-postgres sh -c $cmd *> $null } catch { }
    $global:LASTEXITCODE = 0
}

function Remove-SandalLegacyOrUntaggedContainers {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return }
    $toRemove = [System.Collections.Generic.List[string]]::new()
    $lines = @(& docker ps -a --filter 'name=^/aoc-' --format '{{.Names}}|{{.Image}}' 2>$null)
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line.Split('|', 2)
        $cName = $parts[0].Trim()
        $cImg = if ($parts.Count -gt 1) { $parts[1].Trim() } else { '' }
        if ($cName -eq 'aoc-world-riverlands' -or -not $cImg.StartsWith('sandal-', [System.StringComparison]::OrdinalIgnoreCase)) {
            $toRemove.Add($cName)
        }
    }
    if ($toRemove.Count -gt 0) {
        try { & docker rm -f @($toRemove) *> $null } catch { }
    }
    $global:LASTEXITCODE = 0
}

function Remove-SandalStaleTestAccounts {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return }
    $sql = "DELETE FROM accounts WHERE lower(username) LIKE 'tmp\_%' ESCAPE '\' OR lower(username) LIKE 'temp\_%' ESCAPE '\' OR lower(username) LIKE 'e2e\_%' ESCAPE '\' OR lower(username) = 'ephem_hero';"
    try { $null | & docker exec -i aoc-postgres psql -U aoc -d aoc -tAc $sql *> $null } catch { }
    $global:LASTEXITCODE = 0
}

$baseDefaults = @{
    Profile      = $Profile
    Detach       = [bool]$Detach
    Build        = [bool]$Build
    SingleWorker = [bool]($SingleWorker -or ($env:AOC_WORLD_SHARD_MODE -eq 'single'))
    DryRun       = [bool]($DryRun -or $inheritedDryRun)
    Json         = [bool]($Json -or $inheritedJson)
}
$opts = Resolve-UpOptions -Defaults $baseDefaults -RawArgs (@($Arguments) + @($RemainingArgs))
$quiet = [bool]$opts.Json
$fromRun = ($env:SANDAL_FROM_RUN -eq '1')
if ($quiet) { $env:SANDAL_QUIET = '1' }
$deploySync = $null
if (-not $fromRun) {
    Write-UpStep -Message '[Step 1/3] Staging deploy/ manifests and environment variables...' -Quiet $quiet
    $deploySync = Sync-SandalDeployStage -RepoRoot $rootPath
}
$deployDir = Join-Path $rootPath 'deploy'
$composeFile = Join-Path $deployDir 'compose.yml'
$envFile = Join-Path $deployDir '.env'
$bootSecrets = $null
if (-not $opts.DryRun -or (Test-Path -LiteralPath $envFile -PathType Leaf)) {
    $bootSecrets = Update-SandalAdminBootSecrets -DeployDir $deployDir
    if (-not $opts.DryRun -and (Get-Command Ensure-SandalRootLauncherShortcut -ErrorAction SilentlyContinue)) {
        $null = Ensure-SandalRootLauncherShortcut -RepoRoot $rootPath
    }
    if (-not $quiet -and $null -ne $bootSecrets) {
        [Console]::Out.WriteLine("[Web Admin] URL: http://127.0.0.1:8080/ | Username: $($bootSecrets.Username) | Passphrase: $($bootSecrets.Passphrase)")
        [Console]::Out.WriteLine("Launcher:    SandalLauncher.lnk -> dist\launcher\SandalLauncher.exe")
    }
}
$envMap = Get-SandalEnvMap -EnvPath $envFile
$shardMode = if ($opts.SingleWorker) { 'single' } elseif ($envMap.Contains('AOC_WORLD_SHARD_MODE') -and -not [string]::IsNullOrWhiteSpace([string]$envMap['AOC_WORLD_SHARD_MODE'])) { [string]$envMap['AOC_WORLD_SHARD_MODE'] } elseif ($envMap.Contains('AOC_WORLD_TOPOLOGY_MODE') -and -not [string]::IsNullOrWhiteSpace([string]$envMap['AOC_WORLD_TOPOLOGY_MODE'])) { [string]$envMap['AOC_WORLD_TOPOLOGY_MODE'] } else { 'single' }
Set-SandalTopologyWorkerMode -ShardMode $shardMode -EnvFile $envFile -ForceSingle $opts.SingleWorker -RemoveExtraContainers $false
$composeArgs = Build-ComposeUpArguments -ComposeFile $composeFile -EnvFile $envFile -ProfileName $opts.Profile -ShardMode $shardMode -IsDetach $opts.Detach
$stageChecks = @(Get-UpStageReadiness -RootPath $rootPath)
$allUpToDate = (@($stageChecks | Where-Object { -not $_.up_to_date }).Count -eq 0)

if (-not $opts.DryRun) {
    if (-not $fromRun) {
        Write-UpStep -Message '[Step 2/3] Verifying WSL2 & Docker daemon readiness...' -Quiet $quiet
        $null = Ensure-SandalDockerDaemon
        Write-UpStep -Message "[Step 3/3] Starting Docker Compose containers (profile=$($opts.Profile), shard=$shardMode, detach=$($opts.Detach))..." -Quiet $quiet
    }
    if (Get-Command Set-SandalWslComposeMountEnv -ErrorAction SilentlyContinue) {
        Set-SandalWslComposeMountEnv -DeployDir $deployDir
    }
    Set-SandalTopologyWorkerMode -ShardMode $shardMode -EnvFile $envFile -ForceSingle $opts.SingleWorker -RemoveExtraContainers $true
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
        Sync-SandalPostgresPassword -ComposeFile $composeFile -EnvFile $envFile
        Remove-SandalLegacyOrUntaggedContainers
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
