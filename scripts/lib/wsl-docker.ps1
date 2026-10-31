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

function Test-WslDockerReady {
    [CmdletBinding()]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [scriptblock]$Runner = $null
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $r1 = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'docker', 'info', '--format', '{{.ServerVersion}}') -Runner $Runner
    $r2 = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'docker', 'compose', 'version', '--short') -Runner $Runner
    $sw.Stop()
    $isReady = ($r1.ExitCode -eq 0) -and (-not [string]::IsNullOrWhiteSpace($r1.Output)) -and
               ($r2.ExitCode -eq 0) -and (-not [string]::IsNullOrWhiteSpace($r2.Output))
    if ($isReady) {
        return [pscustomobject]@{
            Ready          = $true
            ServerVersion  = $r1.Output.Trim()
            ComposeVersion = $r2.Output.Trim()
            ElapsedMs      = [double]$sw.Elapsed.TotalMilliseconds
        }
    }
    return [pscustomobject]@{
        Ready          = $false
        ServerVersion  = ''
        ComposeVersion = ''
        ElapsedMs      = [double]$sw.Elapsed.TotalMilliseconds
    }
}

function Ensure-WslUbuntu {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    $listRes = Invoke-WslRunner -WslArgs @('--list', '--quiet') -Runner $Runner
    $clean = $listRes.Output -replace "`0", ''
    $distros = @($clean -split '\r?\n' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
    if ($distros -contains $Distro) {
        return [pscustomobject]@{
            Installed = $true
            Action    = 'Reused'
            Distro    = $Distro
            Message   = "INFO: WSL2 distro $Distro already installed; reusing existing environment"
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

function Ensure-LinuxDockerEngine {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    if (-not $DryRun -and $PSCmdlet.ShouldProcess($Distro, 'Purge legacy TCP 2375 and ensure Docker engine')) {
        $purgeCmd = "if [ -f /etc/systemd/system/docker.service.d/override.conf ]; then sed -i 's/-H tcp:\/\/[^ ]*//g; s/-H=tcp:\/\/[^ ]*//g' /etc/systemd/system/docker.service.d/override.conf; fi"
        $null = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'sh', '-c', $purgeCmd) -Runner $Runner
    }
    $readyCheck = Test-WslDockerReady -Distro $Distro -Runner $Runner
    if ($readyCheck.Ready) {
        return [pscustomobject]@{ Ready = $true; Action = 'Reused'; Provisioned = $false; Distro = $Distro }
    }
    if ($DryRun) {
        return [pscustomobject]@{ Ready = $false; Action = 'DryRun'; Provisioned = $false; Distro = $Distro }
    }
    $null = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'grep -q "systemd=true" /etc/wsl.conf') -Runner $Runner
    $installed = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1') -Runner $Runner
    if ($installed.ExitCode -eq 0) {
        $start = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'systemctl', 'start', 'docker') -Runner $Runner
        return [pscustomobject]@{ Ready = ($start.ExitCode -eq 0); Action = 'Started'; Provisioned = $false; Distro = $Distro }
    }
    $prov = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'bash', 'scripts/wsl/provision-docker.sh') -Runner $Runner
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
    $pattern = '(?i)(tcp://|:2375\b|127\.0\.0\.1:2375|0\.0\.0\.0:2375)'
    $dockerHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { '' }
    $joinedArgs = $Arguments -join ' '
    if ($dockerHost -match $pattern -or $joinedArgs -match $pattern) {
        throw [System.InvalidOperationException]::new('SEC-12: TCP 2375 / tcp:// Docker endpoint is prohibited; use /var/run/docker.sock')
    }
    $wslArgs = @('-d', $Distro, '--', 'docker') + $Arguments
    $res = Invoke-WslRunner -WslArgs $wslArgs -Runner $Runner
    return [pscustomobject]@{
        ExitCode = $res.ExitCode
        Output   = $res.Output
        Socket   = '/var/run/docker.sock'
    }
}
