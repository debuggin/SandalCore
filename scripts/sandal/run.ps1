#Requires -Version 7.2
[CmdletBinding()]
param(
    [ValidateSet('default', 'dev', 'admin', 'full')]
    [string]$Profile = 'default',
    [switch]$SpawnLauncher,
    [switch]$NoLauncher,
    [int]$AutomationPort = 0,
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-RunStep {
    param([string]$Message, [bool]$Quiet = $false)
    if (-not $Quiet -and $env:SANDAL_QUIET -ne '1') {
        [Console]::Out.WriteLine("[SandalCore] $Message")
    }
}

function Resolve-RunOptions {
    param(
        [string]$BaseProfile,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [bool]$BaseNoColor,
        [string[]]$RawArgs
    )
    $prof = $BaseProfile
    $dry = $BaseDryRun
    $jsn = $BaseJson
    $nc = $BaseNoColor
    $noLaunch = [bool]$NoLauncher -or ($env:SANDAL_NO_LAUNCHER -eq '1')
    $explicitSpawn = [bool]$SpawnLauncher -or ($env:SANDAL_SPAWN_LAUNCHER -eq '1')
    $autoPort = if ($AutomationPort -gt 0) { $AutomationPort } elseif ($env:SANDAL_AUTOMATION_PORT -match '^\d+$') { [int]$env:SANDAL_AUTOMATION_PORT } else { 0 }
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = [string]$RawArgs[$i]
        switch -Regex ($arg) {
            '^(-Profile|--profile)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $prof = $RawArgs[$i] }
            }
            '^(-SpawnLauncher|--spawn-launcher)$' { $explicitSpawn = $true; $noLaunch = $false }
            '^(-NoLauncher|--no-launcher)$'       { $noLaunch = $true }
            '^(-AutomationPort|--automation-port)$' {
                if (($i + 1) -lt $RawArgs.Count -and $RawArgs[$i + 1] -match '^\d+$') {
                    $i++
                    $autoPort = [int]$RawArgs[$i]
                }
            }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
            '^(-NoColor|--no-color)$' { $nc = $true }
        }
    }
    $spawn = (-not $noLaunch) -and ($explicitSpawn -or (-not $jsn))
    return [pscustomobject]@{
        Profile        = $prof
        DryRun         = $dry
        Json           = $jsn
        NoColor        = $nc
        SpawnLauncher  = $spawn
        AutomationPort = $autoPort
    }
}

function Ensure-SandalLauncherBinary {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [AllowEmptyString()][string]$ExeExtension = '',
        [bool]$Quiet = $false
    )
    $launcherExe = Join-Path $RootDirPath "dist/launcher/SandalLauncher$ExeExtension"
    $releaseExe = Join-Path $RootDirPath "dist/Release/SandalLauncher$ExeExtension"
    $launcherDll = Join-Path $RootDirPath 'dist/launcher/SandalLauncher.dll'
    if ((Test-Path -LiteralPath $launcherExe -PathType Leaf) -or (Test-Path -LiteralPath $releaseExe -PathType Leaf) -or (Test-Path -LiteralPath $launcherDll -PathType Leaf)) {
        Write-RunStep -Message "[Step 2/6] SandalLauncher binary is ready ($launcherExe)." -Quiet $Quiet
        return
    }
    $launcherCsproj = Join-Path $RootDirPath 'src/clients/launcher/SandalLauncher.csproj'
    if (-not (Test-Path -LiteralPath $launcherCsproj -PathType Leaf)) {
        return
    }
    $dotnetCmd = Get-Command dotnet -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dotnetCmd) {
        Write-RunStep -Message '[Step 2/6] Building and publishing SandalLauncher desktop client...' -Quiet $Quiet
        $distRelDir = Join-Path $RootDirPath 'dist/Release'
        $null = & $dotnetCmd.Source publish $launcherCsproj -c Release -o (Join-Path $RootDirPath 'dist/launcher') -p:DebugType=None -p:DebugSymbols=false --nologo 2>&1
        $null = & $dotnetCmd.Source publish $launcherCsproj -c Release -o $distRelDir -p:DebugType=None -p:DebugSymbols=false --nologo 2>&1
        Write-RunStep -Message '[Step 2/6] SandalLauncher publish complete.' -Quiet $Quiet
    }
}

function Start-SandalLauncherProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [AllowEmptyString()][string]$ExeExtension = '',
        [int]$Port = 0
    )
    $primary = Join-Path $RootDirPath "dist/launcher/SandalLauncher$ExeExtension"
    $fallback = Join-Path $RootDirPath "dist/Release/SandalLauncher$ExeExtension"
    $dllPath = Join-Path $RootDirPath 'dist/launcher/SandalLauncher.dll'
    $launchArgs = [System.Collections.Generic.List[string]]::new()
    if ($Port -gt 0) {
        $launchArgs.AddRange([string[]]@(
            '--automation-port', "$Port",
            '--keep-alive',
            '--timeout-sec', '120',
            '--gateway-url', 'http://127.0.0.1:8080',
            '--world-endpoint', '127.0.0.1:7777'
        ))
    }
    if (Test-Path -LiteralPath $primary -PathType Leaf) {
        $exePath = $primary
    } elseif (Test-Path -LiteralPath $fallback -PathType Leaf) {
        $exePath = $fallback
    } elseif (Test-Path -LiteralPath $dllPath -PathType Leaf) {
        $exePath = 'dotnet'
        $launchArgs.Insert(0, $dllPath)
    } else {
        throw "SandalLauncher binary not found at '$primary' or '$fallback'."
    }
    $workDir = if ($exePath -eq 'dotnet') { Split-Path -Parent $dllPath } else { Split-Path -Parent $exePath }
    $proc = if ($launchArgs.Count -gt 0) {
        Start-Process -FilePath $exePath -ArgumentList $launchArgs.ToArray() -WorkingDirectory $workDir -PassThru
    } else {
        Start-Process -FilePath $exePath -WorkingDirectory $workDir -PassThru
    }
    if ($Port -eq 0 -and $IsWindows) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 2500 -and -not $proc.HasExited) {
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { break }
            Start-Sleep -Milliseconds 100
        }
    }
    return [int]$proc.Id
}

function Invoke-RunPipeline {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [Parameter(Mandatory)][pscustomobject]$Options,
        [AllowEmptyString()][string]$ExeExtension = '',
        [Parameter(Mandatory)][string]$PathSeparator
    )
    $quiet = [bool]$Options.Json
    if ($Options.DryRun) {
        foreach ($msg in @(
            '[Step 1/6] (dry-run) Would verify extracted game data in dist/content/.',
            '[Step 2/6] (dry-run) Would verify SandalLauncher binary in dist/launcher/.',
            '[Step 3/6] (dry-run) Would stage runtime and deploy manifests (dist/ & deploy/).',
            '[Step 4/6] (dry-run) Would verify local TLS certificates in dist/certs/.',
            "[Step 5/6] (dry-run) Would start Docker Compose stack (profile=$($Options.Profile)).",
            "[Step 6/6] (dry-run) Would launch SandalLauncher window (spawn=$($Options.SpawnLauncher))."
        )) { Write-RunStep -Message $msg -Quiet $quiet }
        return $null
    }
    $distRelDir = Join-Path $RootDirPath 'dist/Release'
    $distSvcDir = Join-Path $RootDirPath 'dist/services'
    $prevPath = $env:PATH
    $env:PATH = "$distRelDir$PathSeparator$distSvcDir$PathSeparator$prevPath"
    try {
        Write-RunStep -Message '[Step 1/6] Checking extracted game data in dist/content/...' -Quiet $quiet
        $null = Ensure-SandalGameData -RepoRoot $RootDirPath
        Write-RunStep -Message '[Step 2/6] Verifying SandalLauncher desktop executable...' -Quiet $quiet
        Ensure-SandalLauncherBinary -RootDirPath $RootDirPath -ExeExtension $ExeExtension -Quiet $quiet
        Write-RunStep -Message '[Step 3/6] Staging runtime assets and Docker Compose manifests (dist/ & deploy/)...' -Quiet $quiet
        $null = Sync-SandalDistStage -RepoRoot $RootDirPath -Config 'Release'
        $null = Sync-SandalDeployStage -RepoRoot $RootDirPath
        Write-RunStep -Message '[Step 4/6] Verifying local TLS certificates in dist/certs/...' -Quiet $quiet
        $certsDir = Join-Path $RootDirPath 'dist/certs'
        $certsScript = Join-Path $RootDirPath 'scripts/sandal/certs.ps1'
        if (-not (Test-Path -LiteralPath (Join-Path $certsDir 'server.crt') -PathType Leaf) -and (Test-Path -LiteralPath $certsScript -PathType Leaf)) {
            $null = & $certsScript -OutputDir $certsDir -RepoRoot $RootDirPath -Json
        }
        Write-RunStep -Message "[Step 5/6] Waking WSL2 Docker daemon and starting SandalCore containers (profile=$($Options.Profile))..." -Quiet $quiet
        $null = Ensure-SandalDockerDaemon
        $upScript = Join-Path $RootDirPath 'scripts/sandal/up.ps1'
        if ($quiet) { $null = & $upScript -Profile $Options.Profile -Detach -RepoRoot $RootDirPath -Json } else { & $upScript -Profile $Options.Profile -Detach -RepoRoot $RootDirPath }
        if ($Options.SpawnLauncher) {
            Write-RunStep -Message '[Step 6/6] Launching SandalLauncher desktop window...' -Quiet $quiet
            $spawnedPid = Start-SandalLauncherProcess -RootDirPath $RootDirPath -ExeExtension $ExeExtension -Port $Options.AutomationPort
            Write-RunStep -Message "[Step 6/6] SandalLauncher is open and running (PID $spawnedPid)." -Quiet $quiet
            return $spawnedPid
        }
        Write-RunStep -Message '[Step 6/6] Skipping SandalLauncher launch (-NoLauncher specified).' -Quiet $quiet
        return $null
    } finally {
        $env:PATH = $prevPath
    }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$pathSep = [System.IO.Path]::PathSeparator
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
if (-not (Get-Command Sync-SandalDistStage -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/SandalCore.psd1') -Force -DisableNameChecking
}
if (-not (Get-Command Ensure-SandalDockerDaemon -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts/lib/Env.psm1') -Force -DisableNameChecking
}

$opts = Resolve-RunOptions `
    -BaseProfile $Profile `
    -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) `
    -BaseJson ([bool]($Json -or $inheritedJson)) `
    -BaseNoColor ([bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))) `
    -RawArgs @($Arguments)

$prevQuiet = $env:SANDAL_QUIET
if ($opts.Json) { $env:SANDAL_QUIET = '1' }
try {
    $launcherPid = Invoke-RunPipeline -RootDirPath $rootPath -Options $opts -ExeExtension $exeExt -PathSeparator $pathSep
} finally {
    $env:SANDAL_QUIET = $prevQuiet
}

$payload = [ordered]@{
    ok              = $true
    command         = 'run'
    status          = if ($opts.DryRun) { 'dry-run' } else { 'ready' }
    profile         = $opts.Profile
    dry_run         = [bool]$opts.DryRun
    no_color        = [bool]$opts.NoColor
    spawn_launcher  = [bool]$opts.SpawnLauncher
    launcher_pid    = $launcherPid
    automation_port = if ($opts.AutomationPort -gt 0) { [int]$opts.AutomationPort } else { $null }
    pipeline        = @('extract', 'build', 'deploy', 'up')
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("SandalCore 'run' (profile={0}, status={1}, dry_run={2})" -f $opts.Profile, $payload.status, $opts.DryRun)
}
