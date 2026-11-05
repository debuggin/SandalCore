#Requires -Version 7.2
[CmdletBinding()]
param(
    [string]$Distro = 'Ubuntu-24.04',
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [scriptblock]$Runner = $null,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
. (Join-Path $repoRoot 'scripts/lib/wsl-config.ps1')
. (Join-Path $repoRoot 'scripts/lib/wsl-docker.ps1')

function Resolve-WslDockerInstallOptions {
    param(
        [string]$BaseDistro,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [bool]$BaseNoColor,
        [string[]]$RawArgs
    )
    $dist = if ([string]::IsNullOrWhiteSpace($BaseDistro)) { 'Ubuntu-24.04' } else { $BaseDistro }
    $dry = $BaseDryRun
    $jsn = $BaseJson
    $nocol = $BaseNoColor
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Distro|--distro|-d)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $dist = $RawArgs[$i] }
            }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
            '^(-NoColor|--no-color)$' { $nocol = $true }
        }
    }
    return [pscustomobject]@{
        Distro  = $dist
        DryRun  = $dry
        Json    = $jsn
        NoColor = $nocol
    }
}

function Test-HostDockerDesktopActive {
    if (-not $IsWindows -or $null -eq (Get-Command docker -ErrorAction SilentlyContinue)) {
        return $false
    }
    if ($env:DOCKER_HOST -notin @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
        $null = & docker info *> $null
        if ($LASTEXITCODE -eq 0) { return $true }
    }
    $null = & docker --host npipe:////./pipe/docker_engine info *> $null
    if ($LASTEXITCODE -eq 0) { return $true }
    $null = & docker --context desktop-linux info *> $null
    return ($LASTEXITCODE -eq 0)
}

function Install-HostDockerCliComponents {
    param([bool]$JsonMode)
    if ($IsWindows) {
        if ($null -ne (Get-Command docker -ErrorAction SilentlyContinue)) {
            if (-not $JsonMode) { Write-Host 'INFO: Docker CLI already installed; skipping Docker.DockerCLI installation.' }
        } else {
            & winget install --id Docker.DockerCLI -e --source winget --accept-package-agreements --accept-source-agreements
            if ($LASTEXITCODE -ne 0) { throw "winget install failed for Docker.DockerCLI with exit code $LASTEXITCODE." }
        }
        $null = & docker compose version *> $null
        if ($LASTEXITCODE -ne 0) {
            & winget install --id Docker.DockerCompose -e --source winget --accept-package-agreements --accept-source-agreements
            if ($LASTEXITCODE -ne 0) { throw "winget install failed for Docker.DockerCompose with exit code $LASTEXITCODE." }
        }
        $null = & docker buildx version *> $null
        if ($LASTEXITCODE -ne 0) {
            & winget install --id Docker.DockerBuildx -e --source winget --accept-package-agreements --accept-source-agreements
            if ($LASTEXITCODE -ne 0) { throw "winget install failed for Docker.DockerBuildx with exit code $LASTEXITCODE." }
        }
    }
    if (Test-HostDockerDesktopActive) {
        if ($env:DOCKER_HOST -in @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
            Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
        }
        if (-not $JsonMode) {
            Write-Host 'INFO: Active Docker Desktop daemon detected; preserving existing Docker context and skipping DOCKER_HOST=tcp://[::1]:2375 override.'
        }
        return
    }
    $env:DOCKER_HOST = 'tcp://[::1]:2375'
    if ($IsWindows) {
        [Environment]::SetEnvironmentVariable('DOCKER_HOST', 'tcp://[::1]:2375', 'User')
    }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$baseNoColor = [bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$opts = Resolve-WslDockerInstallOptions -BaseDistro $Distro -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -BaseNoColor $baseNoColor -RawArgs $Arguments

if (-not $opts.DryRun -and -not $PSBoundParameters.ContainsKey('Runner') -and $null -eq $Runner) {
    Install-HostDockerCliComponents -JsonMode $opts.Json
}

$effectiveRunner = if ($null -ne $Runner) {
    $Runner
} elseif ($opts.DryRun) {
    { param([string[]]$WslArgs) return [pscustomobject]@{ ExitCode = 1; Output = '' } }
} else {
    $null
}
$promptReader = if ($opts.DryRun -or $opts.Json -or ($null -ne $Runner)) {
    { param($PromptText, $Key, $CurrentValue) return $CurrentValue }
} else {
    $null
}

$wslConfigPath = Join-Path $(if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }) '.wslconfig'
$wslConfig = if ($opts.DryRun) {
    $state = Get-WslSectionState -ConfigPath $wslConfigPath
    [pscustomobject]@{
        Changed             = $false
        Action              = 'DryRun'
        NetworkingMode      = if ($state.Keys.ContainsKey('networkingMode')) { $state.Keys['networkingMode'] } else { 'mirrored' }
        HostAddressLoopback = if ($state.Keys.ContainsKey('hostAddressLoopback')) { $state.Keys['hostAddressLoopback'] } else { 'true' }
        Path                = [string]$wslConfigPath
        ShutdownAdvised     = $false
    }
} else {
    Confirm-WslConfig -ConfigPath $wslConfigPath -PromptReader $promptReader
}
$ubuntuState = Ensure-WslUbuntu -Distro $opts.Distro -DryRun:$opts.DryRun -Runner $effectiveRunner
$engineState = Ensure-LinuxDockerEngine -Distro $opts.Distro -DryRun:$opts.DryRun -Runner $effectiveRunner
$readyState = Test-WslDockerReady -Distro $opts.Distro -Runner $effectiveRunner
$isSuccess = [bool]($opts.DryRun -or $readyState.Ready)

$payload = [ordered]@{
    ok             = $isSuccess
    command        = 'install-wsl-docker'
    distro         = $opts.Distro
    dry_run        = [bool]$opts.DryRun
    no_color       = [bool]$opts.NoColor
    socket         = '/var/run/docker.sock'
    docker_host    = 'tcp://[::1]:2375'
    tcp_2375       = $true
    wsl_config     = $wslConfig
    ubuntu         = $ubuntuState
    engine         = $engineState
    ready          = $readyState
    WslConfig      = $wslConfig
    WslUbuntu      = $ubuntuState
    DockerEngine   = $engineState
    DockerEndpoint = 'tcp://[::1]:2375'
    ProfileWrapper = $false
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    Write-Output ("SandalCore 'install-wsl-docker' (distro={0}, action={1}, dry_run={2}, ready={3})" -f $opts.Distro, $engineState.Action, $opts.DryRun, $readyState.Ready)
}
if (-not $isSuccess) {
    exit 1
}
exit 0
