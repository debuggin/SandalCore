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

function Resolve-WslDockerDistro {
    [CmdletBinding()]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [scriptblock]$Runner = $null
    )
    $listRes = Invoke-WslRunner -WslArgs @('-l', '-q') -Runner $Runner
    $clean = $listRes.Output -replace "`0", ''
    $distros = @($clean -split '\r?\n' | ForEach-Object { ($_ -replace '^\*\s*', '').Trim() } | Where-Object { $_ -ne '' })
    if ($distros.Count -eq 0 -or ($distros -contains $Distro)) {
        return $Distro
    }
    if ($distros -contains 'Ubuntu') {
        return 'Ubuntu'
    }
    $ubuntuMatch = @($distros | Where-Object { $_ -like 'Ubuntu*' })
    if ($ubuntuMatch.Count -gt 0) {
        return [string]$ubuntuMatch[0]
    }
    $linuxMatch = @($distros | Where-Object { $_ -notlike 'docker-desktop*' })
    if ($linuxMatch.Count -gt 0) {
        return [string]$linuxMatch[0]
    }
    return $Distro
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

function Ensure-WslUbuntu {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    $listRes = Invoke-WslRunner -WslArgs @('-l', '-q') -Runner $Runner
    $clean = $listRes.Output -replace "`0", ''
    $distros = @($clean -split '\r?\n' | ForEach-Object { ($_ -replace '^\*\s*', '').Trim() } | Where-Object { $_ -ne '' })
    $resolvedDistro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    if ($distros -contains $resolvedDistro) {
        return [pscustomobject]@{
            Installed = $true
            Action    = 'Reused'
            Distro    = $resolvedDistro
            Message   = "INFO: WSL2 distro $resolvedDistro already installed; reusing existing environment"
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

function Start-WslKeepaliveProcess {
    param([string]$Distro = 'Ubuntu', [scriptblock]$Runner = $null)
    if ($null -ne $Runner -or -not $IsWindows -or $null -eq (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return }
    try {
        $existing = @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'wsl.exe'" -ErrorAction Stop | Where-Object { $_.CommandLine -match 'sleep infinity' })
        if ($existing.Count -gt 0) { return }
        $null = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "wsl.exe -d $Distro -- sleep infinity" } -ErrorAction Stop
    } catch {
        $null = Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', $Distro, '--', 'sleep', 'infinity') -WindowStyle Hidden -ErrorAction SilentlyContinue
    }
}

function Ensure-LinuxDockerEngine {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$Runner = $null
    )
    $Distro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    $readyCheck = Test-WslDockerReady -Distro $Distro -Runner $Runner
    if ($readyCheck.Ready) {
        Start-WslKeepaliveProcess -Distro $Distro -Runner $Runner
        return [pscustomobject]@{ Ready = $true; Action = 'Reused'; Provisioned = $false; Distro = $Distro }
    }
    if ($DryRun -or -not $PSCmdlet.ShouldProcess($Distro, 'Ensure WSL2 Docker engine with TCP 2375 override')) {
        return [pscustomobject]@{ Ready = $false; Action = 'DryRun'; Provisioned = $false; Distro = $Distro }
    }
    Start-WslKeepaliveProcess -Distro $Distro -Runner $Runner
    $null = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'grep -q "systemd=true" /etc/wsl.conf') -Runner $Runner
    $installed = Invoke-WslRunner -WslArgs @('-d', $Distro, '--', 'sh', '-c', 'command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1') -Runner $Runner
    if ($installed.ExitCode -eq 0) {
        $startCmd = "mkdir -p /etc/systemd/system/docker.service.d && printf '[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375\n' > /etc/systemd/system/docker.service.d/override.conf && systemctl daemon-reload && systemctl enable docker && systemctl restart docker"
        $start = Invoke-WslRunner -WslArgs @('-d', $Distro, '-u', 'root', '--', 'sh', '-c', $startCmd) -Runner $Runner
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
    $Distro = Resolve-WslDockerDistro -Distro $Distro -Runner $Runner
    $wslArgs = @('-d', $Distro, '--', 'docker') + $Arguments
    $res = Invoke-WslRunner -WslArgs $wslArgs -Runner $Runner
    return [pscustomobject]@{
        ExitCode = $res.ExitCode
        Output   = $res.Output
        Socket   = '/var/run/docker.sock'
    }
}
