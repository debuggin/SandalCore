#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (Test-Path (Join-Path $PSScriptRoot 'wsl-config.ps1')) {
    . (Join-Path $PSScriptRoot 'wsl-config.ps1')
}

function Invoke-WslRunner {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string[]]$WslArgs,
        [scriptblock]$Runner = $null
    )
    if ($null -ne $Runner) {
        $res = & $Runner $WslArgs
        if ($null -ne $res -and ($res -is [System.Collections.IDictionary] -and $res.Contains('ExitCode'))) {
            return [pscustomobject]@{
                ExitCode = [int]$res['ExitCode']
                Output   = [string]($res['Output'] -join "`n").Trim()
            }
        }
        if ($null -ne $res -and $null -ne $res.PSObject.Properties['ExitCode']) {
            return [pscustomobject]@{
                ExitCode = [int]$res.ExitCode
                Output   = [string]($res.Output -join "`n").Trim()
            }
        }
        return [pscustomobject]@{
            ExitCode = 0
            Output   = [string](@($res) -join "`n").Trim()
        }
    }
    $out = & wsl.exe @WslArgs 2>&1
    return [pscustomobject]@{
        ExitCode = [int]$LASTEXITCODE
        Output   = (($out | ForEach-Object { "$($_)" }) -join "`n").Trim()
    }
}

function Invoke-DockerHostProbe {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string]$Command,
        [string[]]$Arguments = @(),
        [switch]$ClearDockerHost,
        [scriptblock]$CommandRunner = $null
    )
    $prevDockerHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
    try {
        if ($ClearDockerHost) {
            Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
        }
        if ($null -ne $CommandRunner) {
            $res = & $CommandRunner $Command $Arguments
            if ($null -ne $res -and $null -ne $res.PSObject.Properties['ExitCode']) {
                return [pscustomobject]@{ ExitCode = [int]$res.ExitCode; Output = [string]($res.Output -join "`n").Trim() }
            }
            return [pscustomobject]@{ ExitCode = 0; Output = [string](@($res) -join "`n").Trim() }
        }
        $out = & $Command @Arguments 2>&1
        return [pscustomobject]@{
            ExitCode = [int]$LASTEXITCODE
            Output   = (($out | ForEach-Object { "$($_)" }) -join "`n").Trim()
        }
    } catch {
        return [pscustomobject]@{ ExitCode = 1; Output = '' }
    } finally {
        if ($ClearDockerHost) {
            if ($null -ne $prevDockerHost) { $env:DOCKER_HOST = $prevDockerHost }
            else { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }
        }
    }
}

function Get-DockerDesktopState {
    [CmdletBinding()]
    param(
        [string]$DockerDesktopExePath = "$env:ProgramFiles\Docker\Docker\Docker Desktop.exe",
        [scriptblock]$CommandRunner = $null
    )
    $desktopInstalled = (-not [string]::IsNullOrWhiteSpace($DockerDesktopExePath)) -and (Test-Path -LiteralPath $DockerDesktopExePath)
    if (-not $desktopInstalled -and $null -eq $CommandRunner) {
        $desktopInstalled = $null -ne (Get-Service -Name 'com.docker.service' -ErrorAction SilentlyContinue)
    }
    $cliInstalled = if ($null -ne $CommandRunner) {
        (Invoke-DockerHostProbe -Command 'docker' -Arguments @('--version') -CommandRunner $CommandRunner).ExitCode -eq 0
    } else {
        [bool](Get-Command docker -ErrorAction SilentlyContinue)
    }
    $composeInstalled = $false
    $buildxInstalled = $false
    $daemonReachable = $false
    $activeBackend = 'none'
    $endpoint = ''
    if ($cliInstalled) {
        $composeInstalled = (Invoke-DockerHostProbe -Command 'docker' -Arguments @('compose', 'version', '--short') -CommandRunner $CommandRunner).ExitCode -eq 0
        $buildxInstalled = (Invoke-DockerHostProbe -Command 'docker' -Arguments @('buildx', 'version') -CommandRunner $CommandRunner).ExitCode -eq 0
        $infoRes = Invoke-DockerHostProbe -Command 'docker' -Arguments @('info', '--format', '{{.Name}}|{{.OperatingSystem}}') -CommandRunner $CommandRunner
        if ($infoRes.ExitCode -eq 0) {
            $daemonReachable = $true
            $endpoint = if (-not [string]::IsNullOrWhiteSpace($env:DOCKER_HOST)) { $env:DOCKER_HOST } else { 'default' }
            $activeBackend = if ($infoRes.Output -match 'docker-desktop|Docker Desktop') { 'docker-desktop' } elseif ($endpoint -match ':2375$') { 'wsl-docker' } else { 'existing-daemon' }
        } elseif ($env:DOCKER_HOST -match ':2375') {
            $deskRes = Invoke-DockerHostProbe -Command 'docker' -Arguments @('--context', 'desktop-linux', 'info', '--format', '{{.Name}}|{{.OperatingSystem}}') -ClearDockerHost -CommandRunner $CommandRunner
            if ($deskRes.ExitCode -eq 0) {
                $daemonReachable = $true
                $activeBackend = 'docker-desktop'
                $endpoint = 'context:desktop-linux'
            }
        }
    }
    return [pscustomobject]@{
        DesktopInstalled = [bool]$desktopInstalled
        CliInstalled     = [bool]$cliInstalled
        ComposeInstalled = [bool]$composeInstalled
        BuildxInstalled  = [bool]$buildxInstalled
        DaemonReachable  = [bool]$daemonReachable
        ActiveBackend    = [string]$activeBackend
        Endpoint         = [string]$endpoint
    }
}

function Get-InstalledWslLinuxDistros {
    param([scriptblock]$Runner = $null)
    $listRes = Invoke-WslRunner -WslArgs @('-l', '-q') -Runner $Runner
    $clean = $listRes.Output -replace "`0", ''
    return @($clean -split '\r?\n' | ForEach-Object { ($_ -replace '^\*\s*', '').Trim() } | Where-Object { $_ -ne '' -and $_ -notlike 'docker-desktop*' })
}

function Resolve-WslDockerDistro {
    [CmdletBinding()]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [scriptblock]$Runner = $null
    )
    if (-not $PSBoundParameters.ContainsKey('Distro') -and -not [string]::IsNullOrWhiteSpace($env:SANDAL_WSL_DISTRO)) {
        $Distro = $env:SANDAL_WSL_DISTRO.Trim()
    }
    $distros = @(Get-InstalledWslLinuxDistros -Runner $Runner)
    if ($distros.Count -eq 0) {
        return $Distro
    }
    $resolved = $null
    if ($distros -contains $Distro) {
        $resolved = $Distro
    } elseif ($distros -contains 'Ubuntu-24.04') {
        $resolved = 'Ubuntu-24.04'
    } elseif ($distros -contains 'Ubuntu') {
        $resolved = 'Ubuntu'
    } else {
        $debMatch = @($distros | Where-Object { $_ -like 'Ubuntu*' -or $_ -like 'Debian*' })
        $resolved = if ($debMatch.Count -gt 0) { [string]$debMatch[0] } else { [string]$distros[0] }
    }
    if ($env:SANDAL_QUIET -ne '1') {
        Write-Information "INFO: Using existing WSL2 distribution '$resolved'." -InformationAction Continue
    }
    return $resolved
}

function Test-WslDockerReady {
    [CmdletBinding()]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [scriptblock]$Runner = $null
    )
    $resolvedDistro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $r1 = Invoke-WslRunner -WslArgs @('-d', $resolvedDistro, '--', 'docker', 'info', '--format', '{{.ServerVersion}}') -Runner $Runner
    $r2 = Invoke-WslRunner -WslArgs @('-d', $resolvedDistro, '--', 'docker', 'compose', 'version', '--short') -Runner $Runner
    $sw.Stop()
    $isReady = ($r1.ExitCode -eq 0) -and (-not [string]::IsNullOrWhiteSpace($r1.Output)) -and
               ($r2.ExitCode -eq 0) -and (-not [string]::IsNullOrWhiteSpace($r2.Output))
    if ($isReady) {
        return [pscustomobject]@{
            Ready          = $true
            Distro         = $resolvedDistro
            ServerVersion  = $r1.Output.Trim()
            ComposeVersion = $r2.Output.Trim()
            ElapsedMs      = [double]$sw.Elapsed.TotalMilliseconds
        }
    }
    return [pscustomobject]@{
        Ready          = $false
        Distro         = $resolvedDistro
        ServerVersion  = ''
        ComposeVersion = ''
        ElapsedMs      = [double]$sw.Elapsed.TotalMilliseconds
    }
}

function Test-WslDistroVersion1 {
    param([string]$Distro, [scriptblock]$Runner = $null)
    $vRes = Invoke-WslRunner -WslArgs @('-l', '-v') -Runner $Runner
    if ($vRes.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($vRes.Output)) {
        return $false
    }
    $clean = $vRes.Output -replace "`0", ''
    $escaped = [regex]::Escape($Distro)
    foreach ($line in ($clean -split '\r?\n')) {
        if ($line -match "^\s*\*?\s*$escaped\s+\S+\s+1\s*$") {
            return $true
        }
    }
    return $false
}

function Ensure-WslUbuntu {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    if (-not $PSBoundParameters.ContainsKey('Distro') -and -not [string]::IsNullOrWhiteSpace($env:SANDAL_WSL_DISTRO)) {
        $Distro = $env:SANDAL_WSL_DISTRO.Trim()
    }
    $distros = @(Get-InstalledWslLinuxDistros -Runner $Runner)
    $resolvedDistro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    if ($distros -contains $resolvedDistro) {
        if (-not $DryRun -and (Test-WslDistroVersion1 -Distro $resolvedDistro -Runner $Runner)) {
            if ($env:SANDAL_QUIET -ne '1') { Write-Information "INFO: Upgrading WSL distribution '$resolvedDistro' from WSL1 to WSL2..." -InformationAction Continue }
            $null = Invoke-WslRunner -WslArgs @('--set-version', $resolvedDistro, '2') -Runner $Runner
        }
        return [pscustomobject]@{
            Installed = $true
            Action    = 'Reused'
            Distro    = $resolvedDistro
            Message   = "INFO: Using existing WSL2 distribution '$resolvedDistro'."
        }
    }
    if ($DryRun -or -not $PSCmdlet.ShouldProcess($Distro, 'Install WSL2 distro')) {
        return [pscustomobject]@{
            Installed = $false
            Action    = 'DryRun'
            Distro    = $Distro
            Message   = "DRYRUN: wsl.exe --install -d $Distro"
        }
    }
    $r = Invoke-WslRunner -WslArgs @('--install', '-d', $Distro) -Runner $Runner
    return [pscustomobject]@{
        Installed = ($r.ExitCode -eq 0)
        Action    = 'Installed'
        Distro    = $Distro
        Message   = "Installed $Distro"
    }
}

function Assert-NoTcp2375 {
    param([string[]]$Arguments = @())
    $null = $Arguments
}

function Ensure-LinuxDockerEngine {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    if (-not $PSBoundParameters.ContainsKey('Distro') -and -not [string]::IsNullOrWhiteSpace($env:SANDAL_WSL_DISTRO)) {
        $Distro = $env:SANDAL_WSL_DISTRO.Trim()
    }
    $Distro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    $readyCheck = Test-WslDockerReady -Distro $Distro -Runner $Runner
    if ($readyCheck.Ready) {
        return [pscustomobject]@{ Ready = $true; Action = 'Reused'; Provisioned = $false; Distro = $Distro }
    }
    if ($DryRun -or -not $PSCmdlet.ShouldProcess($Distro, 'Ensure WSL2 Docker engine with TCP 2375 override')) {
        return [pscustomobject]@{ Ready = $false; Action = 'DryRun'; Provisioned = $false; Distro = $Distro }
    }
    $null = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'grep -q "systemd=true" /etc/wsl.conf') -Runner $Runner
    $installed = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1') -Runner $Runner
    if ($installed.ExitCode -eq 0) {
        $startCmd = "mkdir -p /etc/systemd/system/docker.service.d && rm -f /etc/systemd/system/docker.service.d/override.conf && printf '[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375\n' > /etc/systemd/system/docker.service.d/sandal-tcp.conf && systemctl daemon-reload && systemctl enable docker && systemctl restart docker"
        $start = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'sh', '-c', $startCmd) -Runner $Runner
        if ($start.ExitCode -eq 0) {
            return [pscustomobject]@{ Ready = $true; Action = 'Started'; Provisioned = $false; Distro = $Distro }
        }
    }
    $prov = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'bash', 'scripts/wsl/provision-docker.sh') -Runner $Runner
    if ($prov.ExitCode -eq 42) {
        if ($env:SANDAL_QUIET -ne '1') { Write-Information "INFO: Restarting WSL2 distro '$Distro' to boot systemd as PID 1..." -InformationAction Continue }
        $null = Invoke-WslRunner -WslArgs @('--terminate', $Distro) -Runner $Runner
        $prov = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'bash', 'scripts/wsl/provision-docker.sh') -Runner $Runner
    }
    return [pscustomobject]@{ Ready = ($prov.ExitCode -eq 0); Action = 'Provisioned'; Provisioned = $true; Distro = $Distro }
}

function Invoke-WslDocker {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [string]$Distro = 'Ubuntu-24.04',
        [scriptblock]$Runner = $null
    )
    $Distro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    $wslArgs = @('-d', $Distro, '--', 'docker') + $Arguments
    $res = Invoke-WslRunner -WslArgs $wslArgs -Runner $Runner
    return [pscustomobject]@{
        ExitCode = $res.ExitCode
        Output   = $res.Output
        Socket   = '/var/run/docker.sock'
    }
}
