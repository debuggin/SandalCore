#Requires -Version 7.2
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string[]]$Arguments = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-DoctorPwsh {
    param()
    $ver = $PSVersionTable.PSVersion
    $ok = ($ver -ge [version]'7.2.0')
    return [pscustomobject]@{
        tier     = 1
        name     = 'pwsh'
        category = 'toolchain'
        status   = if ($ok) { 'PASS' } else { 'FAIL' }
        version  = $ver.ToString()
        detail   = "PowerShell $($ver.ToString()) (required >= 7.2)"
    }
}

function Test-DoctorBinary {
    param([int]$Tier, [string]$BinaryName)
    $cmd = Get-Command $BinaryName -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $cmd -and $BinaryName -eq 'dotnet') {
        $userHome = if ($HOME) { $HOME } elseif ($env:USERPROFILE) { $env:USERPROFILE } else { '' }
        if ($userHome) {
            $userExe = Join-Path $userHome '.dotnet' $(if ($IsWindows) { 'dotnet.exe' } else { 'dotnet' })
            if (Test-Path -LiteralPath $userExe -PathType Leaf) {
                $cmd = Get-Item -LiteralPath $userExe
            }
        }
    }
    if ($null -eq $cmd) {
        return [pscustomobject]@{
            tier     = $Tier
            name     = $BinaryName
            category = 'toolchain'
            status   = 'WARN'
            version  = ''
            detail   = "$BinaryName not found on PATH"
        }
    }
    $srcPath = if ($cmd.PSObject.Properties['Source']) { $cmd.Source } else { $cmd.FullName }
    $verStr = if ($cmd.PSObject.Properties['Version'] -and $cmd.Version -and $cmd.Version.Major -gt 0) { $cmd.Version.ToString() } else { 'detected' }
    return [pscustomobject]@{
        tier     = $Tier
        name     = $BinaryName
        category = 'toolchain'
        status   = 'PASS'
        version  = $verStr
        detail   = "$BinaryName ($srcPath)"
    }
}

function Get-DoctorRunningContainers {
    param([bool]$DryRunMode = $false)
    $running = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    if ($DryRunMode -or -not (Get-Command docker -ErrorAction SilentlyContinue)) { return $running }
    foreach ($name in @(& docker ps --format '{{.Names}}' 2>$null)) {
        if (-not [string]::IsNullOrWhiteSpace("$name")) { $null = $running.Add(("$name").Trim()) }
    }
    return $running
}

function Get-DoctorPortChecks {
    param([bool]$DryRunMode = $false)
    $ipProps = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties()
    $activeTcp = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($ep in $ipProps.GetActiveTcpListeners()) {
        $null = $activeTcp.Add($ep.Port)
    }
    $activeUdp = [System.Collections.Generic.HashSet[int]]::new()
    foreach ($ep in $ipProps.GetActiveUdpListeners()) {
        $null = $activeUdp.Add($ep.Port)
    }
    $runningContainers = Get-DoctorRunningContainers -DryRunMode $DryRunMode
    $specs = @(
        @{ Port = 8080;  Protocol = 'TCP'; Service = 'ingress-http'; Container = 'aoc-ingress-proxy' },
        @{ Port = 9090;  Protocol = 'TCP'; Service = 'ingress-grpc'; Container = 'aoc-ingress-proxy' },
        @{ Port = 50061; Protocol = 'TCP'; Service = 'ics-tls';      Container = 'aoc-ingress-proxy' },
        @{ Port = 7777;  Protocol = 'UDP'; Service = 'world-udp';    Container = 'aoc-world-riverlands' },
        @{ Port = 50055; Protocol = 'TCP'; Service = 'world-admin';  Container = 'aoc-world-riverlands' }
    )
    $results = [System.Collections.Generic.List[object]]::new()
    foreach ($spec in $specs) {
        $inUse = if ($spec.Protocol -eq 'UDP') { $activeUdp.Contains($spec.Port) } else { $activeTcp.Contains($spec.Port) }
        $stackActive = $inUse -and $runningContainers.Contains($spec.Container)
        $results.Add([pscustomobject]@{
            tier      = 6
            name      = "$($spec.Protocol):$($spec.Port)"
            category  = 'port'
            port      = [int]$spec.Port
            protocol  = $spec.Protocol
            service   = $spec.Service
            available = (-not $inUse)
            status    = if (-not $inUse -or $stackActive) { 'PASS' } else { 'WARN' }
            detail    = if (-not $inUse) { "127.0.0.1:$($spec.Port)/$($spec.Protocol) ($($spec.Service)) free" } elseif ($stackActive) { "127.0.0.1:$($spec.Port)/$($spec.Protocol) ($($spec.Service)) listening (stack active)" } else { "127.0.0.1:$($spec.Port)/$($spec.Protocol) ($($spec.Service)) in use" }
        })
    }
    return $results.ToArray()
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$rawArgs = @($Arguments) + @($RemainingArgs)
$useJson = [bool]($Json -or $inheritedJson -or ($rawArgs -contains '-Json') -or ($rawArgs -contains '--json'))
$useDryRun = [bool]($DryRun -or $inheritedDryRun -or ($rawArgs -contains '-DryRun') -or ($rawArgs -contains '--dry-run'))
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or ($rawArgs -contains '-NoColor') -or ($rawArgs -contains '--no-color') -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\Env.psm1') -Force -DisableNameChecking
}
if (-not $useDryRun) {
    $null = Ensure-SandalDockerDaemon
}

$toolchains = @(
    (Test-DoctorPwsh),
    (Test-DoctorBinary -Tier 2 -BinaryName 'cmake'),
    (Test-DoctorBinary -Tier 3 -BinaryName 'dotnet'),
    (Test-DoctorBinary -Tier 4 -BinaryName 'go'),
    (Test-DoctorBinary -Tier 5 -BinaryName 'docker')
)
$portChecks = @(Get-DoctorPortChecks -DryRunMode $useDryRun)
$allChecks = [System.Collections.Generic.List[object]]::new()
foreach ($tc in $toolchains) { $allChecks.Add($tc) }
foreach ($pc in $portChecks) { $allChecks.Add($pc) }

$hasFail = (@($allChecks | Where-Object { $_.status -eq 'FAIL' }).Count -gt 0)
$payload = [ordered]@{
    ok         = (-not $hasFail)
    command    = 'doctor'
    dry_run    = $useDryRun
    no_color   = $useNoColor
    tier_count = 6
    toolchains = $toolchains
    ports      = $portChecks
    checks     = $allChecks.ToArray()
}

if ($useJson) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    foreach ($item in $allChecks) {
        Write-Output ("[{0}] {1,-12} {2}" -f $item.status, $item.name, $item.detail)
    }
}
if ($hasFail) {
    exit 1
}
