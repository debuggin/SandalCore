#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [switch]$DryRun,
    [switch]$Json,
    [string]$RepoRoot = '',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Arguments = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($Arguments.Count -gt 0) {
    throw 'launch_all.ps1 takes no positional arguments. The native and quick options belonged to the retired replay-based login server; the Go services and worldd are the only server path.'
}

$exeExt = if ($IsWindows) { '.exe' } else { '' }
$pathSep = [System.IO.Path]::PathSeparator
$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..')).Path }
$distDir = Join-Path $rootPath 'dist/Release'
$configDir = Join-Path $rootPath 'src/config'

function Resolve-GameClientPaths {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigDirPath,
        [Parameter(Mandatory)][string]$DistDirPath
    )
    $gameExe = if ($env:GAME_EXE -and (Test-Path -LiteralPath $env:GAME_EXE -PathType Leaf)) { $env:GAME_EXE } else { '' }
    foreach ($pathsJson in @((Join-Path $ConfigDirPath 'paths.local.json'), (Join-Path $ConfigDirPath 'paths.json'), (Join-Path $DistDirPath 'config/paths.json'))) {
        if (-not $gameExe -and (Test-Path -LiteralPath $pathsJson -PathType Leaf)) {
            $parsed = Get-Content -LiteralPath $pathsJson -Raw | ConvertFrom-Json
            $cand = if ($parsed.PSObject.Properties['game_executable']) { [string]$parsed.game_executable } else { '' }
            if ($cand -and (Test-Path -LiteralPath $cand -PathType Leaf)) { $gameExe = $cand }
        }
    }
    if (-not $gameExe -and $IsWindows) {
        $winDefaults = @(
            'C:/Program Files (x86)/Steam/steamapps/common/Ashes of Creation/Game/AOC/Binaries/Win64/AOCClient-Win64-Shipping.exe',
            'C:/Ashes of Creation/Game/AOC/Binaries/Win64/AOCClient-Win64-Shipping.exe'
        )
        foreach ($cand in $winDefaults) {
            if (Test-Path -LiteralPath $cand -PathType Leaf) { $gameExe = $cand; break }
        }
    }
    $gameRoot = if ($env:GAME_ROOT) {
        $env:GAME_ROOT
    } elseif ($gameExe) {
        [System.IO.Path]::GetFullPath((Join-Path $gameExe '../../../..'))
    } elseif ($IsWindows) {
        'C:/Ashes of Creation/Game'
    } else {
        Join-Path $(if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }) 'Ashes of Creation/Game'
    }
    if (-not $gameExe) {
        $gameExe = Join-Path $gameRoot 'AOC/Binaries/Win64/AOCClient-Win64-Shipping.exe'
    }
    return [pscustomobject]@{ GameRoot = $gameRoot; GameExe = $gameExe }
}

function Resolve-DistBinaryPath {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$DistDirPath,
        [Parameter(Mandatory)][string]$PrimaryName,
        [AllowEmptyString()][string]$FallbackName = '',
        [AllowEmptyString()][string]$ExeExtension = ''
    )
    $primaryPath = Join-Path $DistDirPath "$PrimaryName$ExeExtension"
    if (Test-Path -LiteralPath $primaryPath -PathType Leaf) {
        return $primaryPath
    }
    if ($FallbackName) {
        $fallbackPath = Join-Path $DistDirPath "$FallbackName$ExeExtension"
        if (Test-Path -LiteralPath $fallbackPath -PathType Leaf) {
            return $fallbackPath
        }
    }
    return $primaryPath
}

function Assert-LaunchDistBinaries {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$DistDirPath,
        [Parameter(Mandatory)][AllowEmptyString()][string]$ExeExtension
    )
    if (-not (Test-Path -LiteralPath $DistDirPath -PathType Container)) {
        throw "Build output directory not found: $DistDirPath. Build the stack first via pwsh -NoProfile -File scripts/build.ps1 -Configure."
    }
    $missing = [System.Collections.Generic.List[string]]::new()
    $specs = @(
        @{ Primary = 'SandalLauncher'; Fallback = 'launcher' },
        @{ Primary = 'world';          Fallback = 'worldd' }
    )
    foreach ($spec in $specs) {
        $binPath = Resolve-DistBinaryPath -DistDirPath $DistDirPath -PrimaryName $spec.Primary -FallbackName $spec.Fallback -ExeExtension $ExeExtension
        if (-not (Test-Path -LiteralPath $binPath -PathType Leaf)) {
            $missing.Add($binPath)
        }
    }
    if ($missing.Count -gt 0) {
        throw "Required binaries are missing from ${DistDirPath}: $($missing -join ', '). Build the stack first via pwsh -NoProfile -File scripts/build.ps1."
    }
}

function Merge-LaunchConfigJsonFile {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][System.IO.FileInfo]$SourceFile,
        [Parameter(Mandatory)][string]$DestPath
    )
    if (-not (Test-Path -LiteralPath $DestPath -PathType Leaf)) {
        Copy-Item -LiteralPath $SourceFile.FullName -Destination $DestPath -Force
    } elseif ($SourceFile.Extension -ieq '.json') {
        $tplObj = Get-Content -LiteralPath $SourceFile.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
        $dstObj = Get-Content -LiteralPath $DestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -ne $tplObj -and $null -ne $dstObj -and $tplObj -isnot [System.Array] -and $dstObj -isnot [System.Array]) {
            foreach ($prop in $dstObj.PSObject.Properties) {
                $tplObj | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
            }
            [System.IO.File]::WriteAllText($DestPath, (($tplObj | ConvertTo-Json -Depth 10) + "`n"), [System.Text.UTF8Encoding]::new($false))
        }
    }
    if ($SourceFile.Extension -ieq '.json') {
        $localJson = Join-Path $SourceFile.DirectoryName "$($SourceFile.BaseName).local.json"
        if (Test-Path -LiteralPath $localJson -PathType Leaf) {
            $baseObj = Get-Content -LiteralPath $DestPath -Raw -Encoding UTF8 | ConvertFrom-Json
            $localObj = Get-Content -LiteralPath $localJson -Raw -Encoding UTF8 | ConvertFrom-Json
            foreach ($prop in $localObj.PSObject.Properties) {
                $baseObj | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
            }
            [System.IO.File]::WriteAllText($DestPath, (($baseObj | ConvertTo-Json -Depth 10) + "`n"), [System.Text.UTF8Encoding]::new($false))
        }
    }
}

function Sync-LaunchDistConfig {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory)][string]$SourceConfigDir,
        [Parameter(Mandatory)][string]$DistDirPath,
        [AllowEmptyString()][string]$GameExePath = ''
    )
    if (-not $PSCmdlet.ShouldProcess($DistDirPath, 'Synchronize dist/Release/config and logs')) {
        return
    }
    $null = New-Item -ItemType Directory -Path (Join-Path $DistDirPath 'logs') -Force
    $targetConfigDir = Join-Path $DistDirPath 'config'
    $null = New-Item -ItemType Directory -Path $targetConfigDir -Force
    if (Test-Path -LiteralPath $SourceConfigDir -PathType Container) {
        Get-ChildItem -LiteralPath $SourceConfigDir -Recurse -File | Where-Object { $_.Name -notlike '*.local.json' -and $_.Name -notlike '*.local.env' -and $_.Name -notlike '*.example' -and $_.Extension -ine '.md' } | ForEach-Object {
            $rel = [System.IO.Path]::GetRelativePath($SourceConfigDir, $_.FullName)
            $dest = Join-Path $targetConfigDir $rel
            $destParent = Split-Path -Path $dest -Parent
            if ($destParent -and -not (Test-Path -LiteralPath $destParent -PathType Container)) {
                $null = New-Item -ItemType Directory -Path $destParent -Force
            }
            Merge-LaunchConfigJsonFile -SourceFile $_ -DestPath $dest
        }
    }
    Get-ChildItem -LiteralPath $targetConfigDir -Filter '*.local.json' -Recurse -File -ErrorAction SilentlyContinue | Remove-Item -Force
    if ($GameExePath -and (Test-Path -LiteralPath $GameExePath -PathType Leaf)) {
        $pathsFile = Join-Path $targetConfigDir 'paths.json'
        $jsonObj = if (Test-Path -LiteralPath $pathsFile -PathType Leaf) {
            Get-Content -LiteralPath $pathsFile -Raw | ConvertFrom-Json
        } else {
            [pscustomobject]@{}
        }
        $jsonObj | Add-Member -NotePropertyName game_executable -NotePropertyValue $GameExePath -Force
        Set-Content -LiteralPath $pathsFile -Value ($jsonObj | ConvertTo-Json) -Encoding ascii
    }
}

function Wait-LaunchEndpoint {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][ValidateSet('TCP', 'UDP')][string]$Protocol,
        [Parameter(Mandatory)][int]$Port,
        [Parameter(Mandatory)][string]$ServiceName,
        [Parameter(Mandatory)][int]$TimeoutSeconds,
        [Parameter(Mandatory)][string]$RootDirPath
    )
    if ($IsWindows) {
        $checkScript = Join-Path $RootDirPath 'scripts/check_port.ps1'
        & pwsh -NoProfile -File $checkScript -Protocol $Protocol -Port $Port -ServiceName $ServiceName -TimeoutSeconds $TimeoutSeconds -RepoRoot $RootDirPath
        return ($LASTEXITCODE -eq 0)
    }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $names = switch ($ServiceName) {
        'world'  { @('world', 'worldd') }
        default  { @($ServiceName) }
    }
    while ((Get-Date) -lt $deadline) {
        if ($Protocol -eq 'UDP') {
            $proc = Get-Process -Name $names -ErrorAction SilentlyContinue
            if ($null -ne $proc) { return $true }
        } else {
            $client = [System.Net.Sockets.TcpClient]::new()
            try {
                $task = $client.ConnectAsync('127.0.0.1', $Port)
                if ($task.Wait(250) -and $client.Connected) { return $true }
            } catch {
                $null = $_
            } finally {
                $client.Dispose()
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Stop-StaleDistProcesses {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory)][string]$DistDirPath)
    if (-not $PSCmdlet.ShouldProcess($DistDirPath, 'Stop stale launcher processes')) {
        return
    }
    $resolvedDist = (Resolve-Path -LiteralPath $DistDirPath).Path
    Get-Process -Name @('SandalLauncher', 'launcher') -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and $_.Path.StartsWith($resolvedDist, [System.StringComparison]::OrdinalIgnoreCase)
    } | Stop-Process -Force
}

function Start-LaunchServicesAndVerify {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([Parameter(Mandatory)][string]$RootDirPath)
    if (-not $PSCmdlet.ShouldProcess($RootDirPath, 'Extract data, ensure certs, and start Go services + world')) {
        return
    }
    & pwsh -NoProfile -File (Join-Path $RootDirPath 'scripts/extract_game_data.ps1')
    if ($LASTEXITCODE -ne 0) { throw "extract_game_data.ps1 failed with exit code $LASTEXITCODE." }
    & pwsh -NoProfile -File (Join-Path $RootDirPath 'scripts/sandal/certs.ps1')
    if ($LASTEXITCODE -ne 0) { throw "scripts/sandal/certs.ps1 failed with exit code $LASTEXITCODE." }
    $servicesScript = Join-Path $RootDirPath 'scripts/services.ps1'
    & pwsh -NoProfile -File $servicesScript -Action up
    if ($LASTEXITCODE -ne 0) { throw "scripts/services.ps1 up failed with exit code $LASTEXITCODE." }
    & pwsh -NoProfile -File $servicesScript -Action run
    if ($LASTEXITCODE -ne 0) { throw "scripts/services.ps1 run failed with exit code $LASTEXITCODE." }
    $checks = @(
        @{ Proto = 'TCP'; Port = 8081;  Name = 'auth' },
        @{ Proto = 'TCP'; Port = 443;   Name = 'gateway' },
        @{ Proto = 'TCP'; Port = 50055; Name = 'citynode' },
        @{ Proto = 'TCP'; Port = 50058; Name = 'quests' },
        @{ Proto = 'UDP'; Port = 7777;  Name = 'world' }
    )
    foreach ($chk in $checks) {
        if (-not (Wait-LaunchEndpoint -Protocol $chk.Proto -Port $chk.Port -ServiceName $chk.Name -TimeoutSeconds 15 -RootDirPath $RootDirPath)) {
            throw "Service $($chk.Name) failed to bind $($chk.Proto) port $($chk.Port)."
        }
    }
}

function Start-LaunchClientCompanions {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param(
        [Parameter(Mandatory)][string]$RootDirPath,
        [Parameter(Mandatory)][string]$DistDirPath,
        [Parameter(Mandatory)][AllowEmptyString()][string]$ExeExtension
    )
    if (-not $PSCmdlet.ShouldProcess($DistDirPath, 'Start SandalLauncher and auto_logger')) {
        return
    }
    $launcherExe = Resolve-DistBinaryPath -DistDirPath $DistDirPath -PrimaryName 'SandalLauncher' -FallbackName 'launcher' -ExeExtension $ExeExtension
    $null = Start-Process -FilePath $launcherExe -WorkingDirectory $DistDirPath
    $autoLogger = Join-Path $RootDirPath 'scripts/auto_logger.ps1'
    if (Test-Path -LiteralPath $autoLogger -PathType Leaf) {
        $null = Start-Process -FilePath 'pwsh' -ArgumentList @('-NoProfile', '-File', $autoLogger) -WorkingDirectory $RootDirPath
    }
}

$gamePaths = Resolve-GameClientPaths -ConfigDirPath $configDir -DistDirPath $distDir
$env:GAME_ROOT = $gamePaths.GameRoot
$env:GAME_EXE = $gamePaths.GameExe

if (-not $DryRun) {
    Assert-LaunchDistBinaries -DistDirPath $distDir -ExeExtension $exeExt
    $prevPath = $env:PATH
    $distSvcDir = Join-Path $rootPath 'dist/services'
    $env:PATH = "$distDir$pathSep$distSvcDir$pathSep$prevPath"
    Push-Location $distDir
    try {
        Sync-LaunchDistConfig -SourceConfigDir $configDir -DistDirPath $distDir -GameExePath $gamePaths.GameExe
        Stop-StaleDistProcesses -DistDirPath $distDir
        Start-LaunchServicesAndVerify -RootDirPath $rootPath
        Start-LaunchClientCompanions -RootDirPath $rootPath -DistDirPath $distDir -ExeExtension $exeExt
    } finally {
        Pop-Location
        $env:PATH = $prevPath
    }
}

$payload = [ordered]@{
    ok        = $true
    command   = 'launch_all'
    status    = if ($DryRun) { 'dry-run' } else { 'running' }
    dry_run   = [bool]$DryRun
    repo_root = $rootPath
    dist_dir  = $distDir
    game_root = $gamePaths.GameRoot
    game_exe  = $gamePaths.GameExe
}

if ($Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 5)
} else {
    Write-Output ("AoC Emulator Stack is Running (status={0}, dist={1})" -f $payload.status, $distDir)
}
