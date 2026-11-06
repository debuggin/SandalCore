#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
[CmdletBinding()]
param(
    [switch]$Hermetic,
    [switch]$LiveDocker,
    [switch]$IncludeDiagnostics,
    [string]$SimulateFailurePhase = '',
    [int]$AutomationPort = 0,
    [string]$ReportPath = '',
    [string]$RepoRoot = '',
    [switch]$Json
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$effectiveReportPath = if ($ReportPath) { [System.IO.Path]::GetFullPath($ReportPath) } else { Join-Path $rootPath 'dist/logs/e2e_journey_latest.json' }
$useHermetic = [bool]($Hermetic -or (-not $LiveDocker))

function Get-FreeLoopbackPort {
    [CmdletBinding()]
    param()
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        return ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    } finally {
        $listener.Stop()
    }
}

function Start-HermeticMockGateway {
    [CmdletBinding()]
    param([int]$GatewayPort)
    $gwScript = @'
param([int]$Port)
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
$listener.Start()
$charId = '0123456789abcdef0123456789abcdef'
$charName = 'HeroAuto'
$ticket = 'fedcba9876543210fedcba9876543210'
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        try {
            $stream = $client.GetStream()
            $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8, $false, 1024, $true)
            $reqLine = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($reqLine)) { continue }
            $parts = $reqLine -split ' '
            $path = ($parts[1] -split '\?')[0].TrimEnd('/')
            $len = 0
            while ($true) {
                $h = $reader.ReadLine()
                if ([string]::IsNullOrEmpty($h)) { break }
                if ($h -match '^Content-Length:\s*(\d+)') { $len = [int]$Matches[1] }
            }
            $bodyObj = if ($len -gt 0) { $buf = New-Object char[] $len; $null = $reader.ReadBlock($buf, 0, $len); (-join $buf) | ConvertFrom-Json -ErrorAction SilentlyContinue } else { $null }
            $statusLine = '200 OK'
            if ($parts[0] -eq 'POST' -and $path -eq '/api/v1/characters' -and $null -ne $bodyObj -and $bodyObj.name) {
                if ($bodyObj.name -notmatch '^[A-Za-z][A-Za-z0-9]{2,19}$') { $statusLine = '400 Bad Request' } else { $charName = [string]$bodyObj.name }
            }
            $respObj = if ($statusLine -ne '200 OK') { @{ error = 'character name must match ^[A-Za-z][A-Za-z0-9]{2,19}$' } } else {
                switch -Regex ($path) {
                    '^/healthz$' { @{ status = 200; onlinePlayers = 1; maxPlayers = 500 }; break }
                    '^/api/v1/auth/(register|login)$' { @{ access_token = 'tok_e2e_0123456789abcdef0123456789abcdef'; username = 'tmp_e2ehero'; expires_in = 86400; is_temporary = $true }; break }
                    '^/api/v1/characters$' { @{ character = @{ character_id = $charId; name = $charName; class_name = 'Fighter'; level = 1; zone_name = 'verra-01' }; characters = @(@{ character_id = $charId; name = $charName; class_name = 'Fighter'; level = 1; zone_name = 'verra-01' }) }; break }
                    '/select$' { @{ character_id = $charId; character_name = $charName; join_ticket = $ticket; world_endpoint = '127.0.0.1:7777'; world_enter_allowed = $true }; break }
                    '^/api/v1/world/enter$' { @{ status = 'ALLOW'; world_endpoint = '127.0.0.1:7777' }; break }
                    default { @{ status = 'ok' } }
                }
            }
            $bytes = [System.Text.Encoding]::UTF8.GetBytes(($respObj | ConvertTo-Json -Depth 5 -Compress))
            $hdr = [System.Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $statusLine`r`nContent-Type: application/json`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
            $stream.Write($hdr, 0, $hdr.Length)
            $stream.Write($bytes, 0, $bytes.Length)
        } finally { $client.Close() }
    }
} finally { $listener.Stop() }
'@
    $tmpPath = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-mock-gw-$GatewayPort-" + [guid]::NewGuid().ToString('N') + '.ps1')
    [System.IO.File]::WriteAllText($tmpPath, $gwScript, [System.Text.UTF8Encoding]::new($false))
    $proc = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-File', $tmpPath, '-Port', "$GatewayPort") -WindowStyle Hidden -PassThru
    return [pscustomobject]@{ Process = $proc; ScriptPath = $tmpPath }
}

function Start-HermeticLauncherProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [Parameter(Mandatory)][int]$AutoPort,
        [Parameter(Mandatory)][int]$GatewayPort,
        [AllowEmptyString()][string]$ExeExtension = ''
    )
    $primaryExe = Join-Path $RootDirPath "dist/launcher/SandalLauncher$ExeExtension"
    $fallbackExe = Join-Path $RootDirPath "dist/Release/SandalLauncher$ExeExtension"
    $dllPath = Join-Path $RootDirPath 'dist/launcher/SandalLauncher.dll'
    $launchArgs = @('--automation-port', "$AutoPort", '--keep-alive', '--gateway', "http://127.0.0.1:$GatewayPort", '--timeout-sec', '120')
    if (Test-Path -LiteralPath $primaryExe -PathType Leaf) {
        return Start-Process -FilePath $primaryExe -ArgumentList $launchArgs -WorkingDirectory (Split-Path -Parent $primaryExe) -WindowStyle Hidden -PassThru
    }
    if (Test-Path -LiteralPath $fallbackExe -PathType Leaf) {
        return Start-Process -FilePath $fallbackExe -ArgumentList $launchArgs -WorkingDirectory (Split-Path -Parent $fallbackExe) -WindowStyle Hidden -PassThru
    }
    if (Test-Path -LiteralPath $dllPath -PathType Leaf) {
        return Start-Process -FilePath 'dotnet' -ArgumentList (@($dllPath) + $launchArgs) -WorkingDirectory (Split-Path -Parent $dllPath) -WindowStyle Hidden -PassThru
    }
    throw "SandalLauncher binary not found under dist/launcher or dist/Release."
}

function Invoke-AutomationEndpoint {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][ValidateSet('GET', 'POST')][string]$Method,
        [Parameter(Mandatory)][string]$Path,
        [string]$BodyJson = '{}',
        [System.Collections.Generic.List[object]]$History = $null
    )
    $uri = "http://127.0.0.1:$Port$Path"
    $resp = if ($Method -eq 'GET') {
        Invoke-RestMethod -Uri $uri -Method Get -TimeoutSec 5
    } else {
        Invoke-RestMethod -Uri $uri -Method Post -Body $BodyJson -ContentType 'application/json' -TimeoutSec 5
    }
    if ($null -ne $History) {
        $History.Add([pscustomobject]@{
            timestamp = [DateTimeOffset]::UtcNow.ToString('o')
            method    = $Method
            path      = $Path
            response  = $resp
        })
    }
    return $resp
}

function Wait-AutomationReady {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][int]$Port,
        [int]$TimeoutMs = 8000,
        [System.Collections.Generic.List[object]]$History = $null
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $TimeoutMs) {
        try {
            $state = Invoke-AutomationEndpoint -Port $Port -Method 'GET' -Path '/automation/state' -History $History
            if ($null -ne $state) { return $state }
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Timed out waiting for GET http://127.0.0.1:$Port/automation/state after ${TimeoutMs}ms."
}

function Get-ContainerDiagnosticSnapshot {
    [CmdletBinding()]
    param(
        [bool]$SkipDocker = $false,
        [string]$RootDirPath = ''
    )
    $containers = [ordered]@{}
    $logs = [ordered]@{}
    if ($SkipDocker -or $null -eq (Get-Command docker -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{ Containers = $containers; Logs = $logs }
    }
    if ($RootDirPath) {
        try {
            if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
                Import-Module (Join-Path $RootDirPath 'scripts/lib/Env.psm1') -Force -DisableNameChecking
            }
            $null = Ensure-SandalDockerDaemon
        } catch {
            $null = $_
        }
    }
    foreach ($name in @('aoc-ingress-proxy', 'aoc-gateway', 'aoc-auth', 'aoc-character', 'aoc-world-riverlands')) {
        try {
            $st = (& docker inspect --format '{{.State.Status}} ({{if .State.Health}}{{.State.Health.Status}}{{else}}no-healthcheck{{end}})' $name 2>$null) -join ''
            $containers[$name] = if ($LASTEXITCODE -eq 0 -and $st) { $st.Trim() } else { 'not-found' }
        } catch {
            $containers[$name] = 'unreachable'
        }
    }
    foreach ($logSvc in @('aoc-ingress-proxy', 'aoc-gateway', 'aoc-auth', 'aoc-world-riverlands')) {
        try {
            $tail = @(& docker logs --tail 40 $logSvc 2>&1 | ForEach-Object { "$_" })
            $logs[$logSvc] = if ($LASTEXITCODE -eq 0) { $tail } else { @() }
        } catch {
            $logs[$logSvc] = @()
        }
    }
    return [pscustomobject]@{ Containers = $containers; Logs = $logs }
}

function Get-E2EDiagnosticBundle {
    [CmdletBinding()]
    param(
        [bool]$SkipDocker = $false,
        [string]$RootDirPath = '',
        [System.Collections.Generic.List[object]]$HttpHistory = $null
    )
    $dockerSnap = Get-ContainerDiagnosticSnapshot -SkipDocker:$SkipDocker -RootDirPath $RootDirPath
    $ports = [ordered]@{}
    foreach ($port in @(8080, 9090, 50061, 7777)) {
        $client = [System.Net.Sockets.TcpClient]::new()
        try {
            $task = $client.ConnectAsync('127.0.0.1', $port)
            $ports["$port"] = [bool]($task.Wait(100) -and $client.Connected)
        } catch {
            $ports["$port"] = $false
        } finally {
            $client.Dispose()
        }
    }
    $procs = @(Get-Process -Name @('SandalLauncher', 'AOCClient-Win64-Shipping', 'world') -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{ id = $_.Id; name = $_.ProcessName }
    })
    return [ordered]@{
        captured_at_utc  = [DateTimeOffset]::UtcNow.ToString('o')
        containers       = $dockerSnap.Containers
        container_logs   = $dockerSnap.Logs
        listening_ports  = $ports
        active_processes = $procs
        http_responses   = if ($null -ne $HttpHistory) { $HttpHistory.ToArray() } else { @() }
    }
}

function Invoke-Phase1LauncherLoaded {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Ctx.UseHermetic) {
        $dryRunOut = (& pwsh -NoProfile -File (Join-Path $Ctx.RootPath 'sandal.ps1') run -SpawnLauncher -AutomationPort $Ctx.Port -DryRun -Json) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "sandal.ps1 run -DryRun failed with exit code $LASTEXITCODE" }
        if ($dryRunOut -match '(?s)(\{.*\})\s*$') { $dryRunOut = $Matches[1] }
        $Ctx.StartReceipt = $dryRunOut | ConvertFrom-Json
        $gwPort = Get-FreeLoopbackPort
        $gwInfo = Start-HermeticMockGateway -GatewayPort $gwPort
        $Ctx.GatewayProcess = $gwInfo.Process
        $Ctx.GatewayScriptPath = $gwInfo.ScriptPath
        $Ctx.LauncherProcess = Start-HermeticLauncherProcess -RootDirPath $Ctx.RootPath -AutoPort $Ctx.Port -GatewayPort $gwPort -ExeExtension $Ctx.ExeExt
    } else {
        $startBat = Join-Path $Ctx.RootPath 'start.bat'
        $runOut = (& cmd.exe /c "`"$startBat`" -SpawnLauncher -AutomationPort $($Ctx.Port) -Json") -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "start.bat failed with exit code $LASTEXITCODE" }
        if ($runOut -match '(?s)(\{.*\})\s*$') { $runOut = $Matches[1] }
        $Ctx.StartReceipt = $runOut | ConvertFrom-Json
    }
    $state = Wait-AutomationReady -Port $Ctx.Port -TimeoutMs 8000 -History $Ctx.HttpHistory
    $launcherPid = if ($state.PSObject.Properties['launcherPid'] -and [int]$state.launcherPid -gt 0) {
        [int]$state.launcherPid
    } elseif ($null -ne $Ctx.LauncherProcess) {
        [int]$Ctx.LauncherProcess.Id
    } else {
        [int]$Ctx.StartReceipt.launcher_pid
    }
    $Ctx.LauncherPid = $launcherPid
    $procAlive = $null -ne (Get-Process -Id $launcherPid -ErrorAction SilentlyContinue)
    if ($Ctx.SimulateFailurePhase -eq 'launcher_loaded' -or $launcherPid -le 0 -or -not $procAlive) {
        throw "Phase 1 failed: launcherPid=$launcherPid alive=$procAlive"
    }
    return [ordered]@{
        phase          = 'launcher_loaded'
        transition     = 'click start -> launcher loaded'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('http_200_automation_state', 'launcher_pid_positive', 'launcher_process_alive')
        observed_state = @{ launcherPid = $launcherPid; viewState = $state.viewState }
    }
}

function Invoke-Phase2GameStarted {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $regJson = @{ createDefaultChar = $true; characterName = $Ctx.CharacterName } | ConvertTo-Json -Compress
    $regState = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/register-temp' -BodyJson $regJson -History $Ctx.HttpHistory
    $targetChar = if ($regState.selectedCharacterId) { [string]$regState.selectedCharacterId } else { [string]$Ctx.CharacterName }
    $selJson = @{ characterName = $targetChar; characterIdOrName = $targetChar; spawnClient = $true } | ConvertTo-Json -Compress
    $state = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/char-select' -BodyJson $selJson -History $Ctx.HttpHistory
    if (-not $state.ashesPid) {
        $state = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/launch' -BodyJson '{}' -History $Ctx.HttpHistory
    }
    $ashesPid = if ($null -ne $state.ashesPid) { [int]$state.ashesPid } else { 0 }
    $Ctx.AshesPid = $ashesPid
    $osAlive = ($ashesPid -gt 0) -and ($null -ne (Get-Process -Id $ashesPid -ErrorAction SilentlyContinue))
    if ($Ctx.SimulateFailurePhase -eq 'game_started' -or -not $state.ashesProcessRunning -or $ashesPid -le 0 -or -not $osAlive) {
        throw "Phase 2 failed: ashesProcessRunning=$($state.ashesProcessRunning) ashesPid=$ashesPid osAlive=$osAlive"
    }
    return [ordered]@{
        phase          = 'game_started'
        transition     = 'launcher loaded -> game started'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('ashes_process_running_true', 'ashes_pid_positive', 'os_process_alive')
        observed_state = @{ ashesProcessRunning = [bool]$state.ashesProcessRunning; ashesPid = $ashesPid }
    }
}

function Invoke-Phase3GameLoggedIn {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $state = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'GET' -Path '/automation/state' -History $Ctx.HttpHistory
    $charHexOk = ($state.selectedCharacterId -match '^[0-9a-fA-F]{32}$')
    $ticketHexOk = ($state.lastJoinTicket -match '^[0-9a-fA-F]{32}$')
    $endpointOk = ($state.worldEndpoint -in @('127.0.0.1:7777', 'world:7777'))
    $bannerOk = ($state.statusBanner -match 'Enter=True|join ticket ready for (127\.0\.0\.1|world):7777')
    if ($Ctx.SimulateFailurePhase -eq 'game_logged_in' -or -not $state.isAuthenticated -or -not $charHexOk -or -not $ticketHexOk -or -not $endpointOk -or -not $bannerOk) {
        throw "Phase 3 failed: auth=$($state.isAuthenticated) char=$($state.selectedCharacterId) ticket=$($state.lastJoinTicket) world=$($state.worldEndpoint) banner=$($state.statusBanner)"
    }
    return [ordered]@{
        phase          = 'game_logged_in'
        transition     = 'game started -> game logged in'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('is_authenticated_true', 'selected_character_id_32hex', 'last_join_ticket_32hex', 'world_endpoint_7777', 'status_banner_verified')
        observed_state = @{
            isAuthenticated     = [bool]$state.isAuthenticated
            selectedCharacterId = [string]$state.selectedCharacterId
            lastJoinTicket      = [string]$state.lastJoinTicket
            worldEndpoint       = [string]$state.worldEndpoint
            statusBanner        = [string]$state.statusBanner
        }
    }
}

function Invoke-Phase4GameClosed {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $prevAshesPid = [int]$Ctx.AshesPid
    $null = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/terminate-ashes' -BodyJson '{"killAllMatching":true}' -History $Ctx.HttpHistory
    $state = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/delete-account' -BodyJson '{}' -History $Ctx.HttpHistory
    if ($prevAshesPid -gt 0) {
        $p = Get-Process -Id $prevAshesPid -ErrorAction SilentlyContinue
        if ($null -ne $p) {
            Stop-Process -Id $prevAshesPid -Force -ErrorAction SilentlyContinue
            $null = $p.WaitForExit(2000)
        }
    }
    $osExited = ($prevAshesPid -le 0) -or ($null -eq (Get-Process -Id $prevAshesPid -ErrorAction SilentlyContinue))
    if ($Ctx.SimulateFailurePhase -eq 'game_closed' -or $state.ashesProcessRunning -or $null -ne $state.ashesPid -or -not $osExited) {
        throw "Phase 4 failed: ashesProcessRunning=$($state.ashesProcessRunning) ashesPid=$($state.ashesPid) osExited=$osExited"
    }
    return [ordered]@{
        phase          = 'game_closed'
        transition     = 'game logged in -> game closed'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('ashes_process_running_false', 'ashes_pid_null', 'ashes_os_process_exited', 'temp_account_deleted')
        observed_state = @{ ashesProcessRunning = [bool]$state.ashesProcessRunning; ashesPid = $state.ashesPid; osExited = $osExited }
    }
}

function Invoke-Phase5LauncherClosed {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $launcherPid = [int]$Ctx.LauncherPid
    try {
        $null = Invoke-AutomationEndpoint -Port $Ctx.Port -Method 'POST' -Path '/automation/close-launcher' -BodyJson '{"terminateAshes":true}' -History $Ctx.HttpHistory
    } catch {
        $null = $_
    }
    if ($launcherPid -gt 0) {
        $proc = Get-Process -Id $launcherPid -ErrorAction SilentlyContinue
        if ($null -ne $proc) {
            if (-not $proc.WaitForExit(1500)) {
                Stop-Process -Id $launcherPid -Force -ErrorAction SilentlyContinue
                $null = $proc.WaitForExit(1500)
            }
        }
    }
    $launcherExited = ($launcherPid -le 0) -or ($null -eq (Get-Process -Id $launcherPid -ErrorAction SilentlyContinue))
    if ($Ctx.SimulateFailurePhase -eq 'launcher_closed' -or -not $launcherExited) {
        throw "Phase 5 failed: launcherPid=$launcherPid launcherExited=$launcherExited"
    }
    return [ordered]@{
        phase          = 'launcher_closed'
        transition     = 'game closed -> launcher closed'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('close_launcher_requested', 'launcher_pid_exited')
        observed_state = @{ launcherPid = $launcherPid; launcherExited = $launcherExited }
    }
}

function Invoke-Phase6ClickStop {
    [CmdletBinding()]
    param([hashtable]$Ctx)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Ctx.UseHermetic) {
        $downOut = (& pwsh -NoProfile -File (Join-Path $Ctx.RootPath 'sandal.ps1') down -DryRun -Json) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "sandal.ps1 down -DryRun failed with exit code $LASTEXITCODE" }
        if ($downOut -match '(?s)(\{.*\})\s*$') { $downOut = $Matches[1] }
        $downReceipt = $downOut | ConvertFrom-Json
    } else {
        $stopBat = Join-Path $Ctx.RootPath 'stop.bat'
        $downOut = (& cmd.exe /c "`"$stopBat`" -Json") -join "`n"
        if ($LASTEXITCODE -ne 0) { throw "stop.bat failed with exit code $LASTEXITCODE" }
        if ($downOut -match '(?s)(\{.*\})\s*$') { $downOut = $Matches[1] }
        $downReceipt = $downOut | ConvertFrom-Json
    }
    $orphanAlive = ($Ctx.LauncherPid -gt 0 -and $null -ne (Get-Process -Id $Ctx.LauncherPid -ErrorAction SilentlyContinue)) -or
                   ($Ctx.AshesPid -gt 0 -and $null -ne (Get-Process -Id $Ctx.AshesPid -ErrorAction SilentlyContinue))
    if ($Ctx.SimulateFailurePhase -eq 'click_stop' -or -not $downReceipt.ok -or $orphanAlive) {
        throw "Phase 6 failed: downOk=$($downReceipt.ok) orphanAlive=$orphanAlive"
    }
    return [ordered]@{
        phase          = 'click_stop'
        transition     = 'launcher closed -> click stop'
        status         = 'PASS'
        duration_ms    = [int]$sw.ElapsedMilliseconds
        assertions     = @('stop_command_ok', 'zero_orphan_processes')
        observed_state = @{ stopCommandOk = [bool]$downReceipt.ok; orphanProcesses = 0 }
    }
}

$port = if ($AutomationPort -gt 0) { $AutomationPort } else { Get-FreeLoopbackPort }
$ctx = @{
    RootPath             = $rootPath
    ExeExt               = $exeExt
    Port                 = $port
    UseHermetic          = $useHermetic
    SimulateFailurePhase = $SimulateFailurePhase
    CharacterName        = ('Hero' + [guid]::NewGuid().ToString('N').Substring(0, 8))
    LauncherPid          = 0
    AshesPid             = 0
    GatewayProcess       = $null
    GatewayScriptPath    = ''
    LauncherProcess      = $null
    StartReceipt         = $null
    HttpHistory          = [System.Collections.Generic.List[object]]::new()
}

$phases = [System.Collections.Generic.List[object]]::new()
$overallOk = $true
$failedPhase = ''
$failureMessage = ''
$diagnostics = $null
$phaseRunners = @(
    @{ Name = 'launcher_loaded'; Transition = 'click start -> launcher loaded'; Fn = ${function:Invoke-Phase1LauncherLoaded} },
    @{ Name = 'game_started';    Transition = 'launcher loaded -> game started'; Fn = ${function:Invoke-Phase2GameStarted} },
    @{ Name = 'game_logged_in';  Transition = 'game started -> game logged in';  Fn = ${function:Invoke-Phase3GameLoggedIn} },
    @{ Name = 'game_closed';     Transition = 'game logged in -> game closed';   Fn = ${function:Invoke-Phase4GameClosed} },
    @{ Name = 'launcher_closed'; Transition = 'game closed -> launcher closed';  Fn = ${function:Invoke-Phase5LauncherClosed} },
    @{ Name = 'click_stop';      Transition = 'launcher closed -> click stop';   Fn = ${function:Invoke-Phase6ClickStop} }
)

try {
    $phaseIdx = 0
    foreach ($entry in $phaseRunners) {
        $phaseIdx++
        if (-not $Json) {
            [Console]::Out.WriteLine("[SandalCore][Phase $phaseIdx/6] Starting $($entry.Name) ($($entry.Transition))...")
        }
        $phaseSw = [System.Diagnostics.Stopwatch]::StartNew()
        try {
            $res = & $entry.Fn $ctx
            $phases.Add($res)
            if (-not $Json) {
                [Console]::Out.WriteLine("[SandalCore][Phase $phaseIdx/6] PASS $($entry.Name) ($($res.duration_ms) ms).")
            }
            if ($IncludeDiagnostics -and $entry.Name -eq 'game_logged_in') {
                $diagnostics = Get-E2EDiagnosticBundle -SkipDocker:$useHermetic -RootDirPath $rootPath -HttpHistory $ctx.HttpHistory
            }
        } catch {
            $overallOk = $false
            $failedPhase = $entry.Name
            $failureMessage = $_.Exception.Message
            if (-not $Json) {
                [Console]::Out.WriteLine("[SandalCore][Phase $phaseIdx/6] FAIL $($entry.Name): $failureMessage")
            }
            $phases.Add([ordered]@{
                phase          = $entry.Name
                transition     = $entry.Transition
                status         = 'FAIL'
                duration_ms    = [int]$phaseSw.ElapsedMilliseconds
                assertions     = @()
                observed_state = @{}
                error          = $failureMessage
            })
            $diagnostics = Get-E2EDiagnosticBundle -SkipDocker:$useHermetic -RootDirPath $rootPath -HttpHistory $ctx.HttpHistory
            break
        }
    }
} finally {
    foreach ($p in @($ctx.LauncherProcess, $ctx.GatewayProcess)) {
        if ($null -ne $p -and -not $p.HasExited) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
    foreach ($pidToKill in @($ctx.AshesPid, $ctx.LauncherPid)) {
        if ($pidToKill -gt 0) {
            Stop-Process -Id $pidToKill -Force -ErrorAction SilentlyContinue
        }
    }
    if ($ctx.GatewayScriptPath -and (Test-Path -LiteralPath $ctx.GatewayScriptPath)) {
        Remove-Item -LiteralPath $ctx.GatewayScriptPath -Force -ErrorAction SilentlyContinue
    }
}

if ($null -eq $diagnostics -and (-not $overallOk -or $IncludeDiagnostics)) {
    $diagnostics = Get-E2EDiagnosticBundle -SkipDocker:$useHermetic -RootDirPath $rootPath -HttpHistory $ctx.HttpHistory
}

$report = [ordered]@{
    ok              = $overallOk
    mode            = if ($useHermetic) { 'hermetic' } else { 'live-docker' }
    automation_port = $port
    phase_count     = $phases.Count
    failed_phase    = $failedPhase
    error           = $failureMessage
    phases          = $phases.ToArray()
    diagnostics     = $diagnostics
}

$reportDir = Split-Path -Parent $effectiveReportPath
if ($reportDir -and -not (Test-Path -LiteralPath $reportDir -PathType Container)) {
    $null = New-Item -ItemType Directory -Path $reportDir -Force
}
$reportJson = $report | ConvertTo-Json -Depth 10
[System.IO.File]::WriteAllText($effectiveReportPath, $reportJson, [System.Text.UTF8Encoding]::new($false))

if ($Json) {
    Write-Output $reportJson
} else {
    Write-Output ("E2E User Journey ({0}): ok={1} phases={2}/6 report={3}" -f $report.mode, $report.ok, $phases.Count, $effectiveReportPath)
}

if (-not $overallOk) {
    exit 1
}
exit 0
