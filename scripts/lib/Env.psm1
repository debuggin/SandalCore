#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-SandalCsprngSecret {
    [CmdletBinding()]
    param([ValidateRange(16, 128)][int]$ByteLength = 32)
    $bytes = [System.Security.Cryptography.RandomNumberGenerator]::GetBytes($ByteLength)
    return [System.Convert]::ToHexString($bytes).ToLowerInvariant()
}

function Get-SandalEnvMap {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$EnvPath)
    $map = [ordered]@{}
    if (-not (Test-Path -LiteralPath $EnvPath -PathType Leaf)) {
        return $map
    }
    foreach ($line in [System.IO.File]::ReadAllLines($EnvPath, [System.Text.Encoding]::UTF8)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -gt 0 -and -not $trimmed.StartsWith('#') -and $trimmed -match '^([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') {
            $map[$Matches[1]] = $Matches[2].Trim()
        }
    }
    return $map
}

function Set-SandalSecretAcl {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$LiteralPath)
    if ($IsWindows) {
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        $null = & icacls $LiteralPath /inheritance:r /grant:r "${identity}:(F)" 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply restrictive ACL on '$LiteralPath' (icacls exit $LASTEXITCODE)."
        }
    } else {
        $null = & chmod 0600 $LiteralPath
    }
    return [pscustomobject]@{ Path = $LiteralPath; Mode = '0600'; InheritanceDisabled = $true }
}

function Initialize-SandalEnv {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$EnvPath,
        [Alias('ExamplePath')][string]$TemplatePath = '',
        [string[]]$SecretKeys = @('POSTGRES_PASSWORD', 'AOC_SERVICE_TOKEN', 'SANDAL_SECRET_KEY'),
        [switch]$Force
    )
    $parent = Split-Path -Parent $EnvPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
    $existingMap = if (Test-Path -LiteralPath $EnvPath -PathType Leaf) { Get-SandalEnvMap -EnvPath $EnvPath } else { [ordered]@{} }
    $sourcePath = if ($TemplatePath -and (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) { $TemplatePath } elseif (Test-Path -LiteralPath $EnvPath -PathType Leaf) { $EnvPath } else { '' }
    $inLines = if ($sourcePath) { [System.IO.File]::ReadAllLines($sourcePath, [System.Text.Encoding]::UTF8) } else { @() }
    $outLines = [System.Collections.Generic.List[string]]::new()
    $seenSecrets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $seenKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $generated = [System.Collections.Generic.List[string]]::new()

    foreach ($line in $inLines) {
        if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') { $outLines.Add($line); continue }
        $key = $Matches[1]
        $val = $Matches[2].Trim()
        $null = $seenKeys.Add($key)
        if ($key -notin $SecretKeys) { $outLines.Add($line); continue }
        $null = $seenSecrets.Add($key)
        $existingVal = if ($existingMap.Contains($key)) { [string]$existingMap[$key] } else { '' }
        if (-not $Force -and $existingVal -and -not $existingVal.StartsWith('CHANGE_ME')) {
            $val = $existingVal
        } elseif ($Force -or [string]::IsNullOrWhiteSpace($val) -or $val.StartsWith('CHANGE_ME')) {
            $val = New-SandalCsprngSecret -ByteLength 32
            $generated.Add($key)
        }
        $outLines.Add("${key}=${val}")
    }
    foreach ($extraKey in $existingMap.Keys) {
        if ($seenKeys.Contains($extraKey)) { continue }
        $val = [string]$existingMap[$extraKey]
        if ($extraKey -in $SecretKeys -and ($Force -or [string]::IsNullOrWhiteSpace($val) -or $val.StartsWith('CHANGE_ME'))) {
            $val = New-SandalCsprngSecret -ByteLength 32
            $generated.Add($extraKey)
        }
        if ($extraKey -in $SecretKeys) { $null = $seenSecrets.Add($extraKey) }
        $outLines.Add("${extraKey}=${val}")
    }
    foreach ($key in $SecretKeys) {
        if (-not $seenSecrets.Contains($key)) {
            $outLines.Add("${key}=$(New-SandalCsprngSecret -ByteLength 32)")
            $generated.Add($key)
        }
    }
    $tmpPath = "${EnvPath}.tmp.${PID}"
    [System.IO.File]::WriteAllLines($tmpPath, $outLines, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tmpPath -Destination $EnvPath -Force
    $aclInfo = Set-SandalSecretAcl -LiteralPath $EnvPath
    return [pscustomobject]@{ EnvPath = $EnvPath; GeneratedKeys = $generated.ToArray(); Acl = $aclInfo }
}

function Copy-SandalConfigStage {
    param([string]$SourceDir, [string]$DestDir)
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) { return }
    $null = New-Item -ItemType Directory -Path $DestDir -Force
    foreach ($item in @(Get-ChildItem -LiteralPath $SourceDir -Recurse -File)) {
        if ($item.Name -like '*.local.json' -or $item.Name -eq 'local.json') { continue }
        $rel = [System.IO.Path]::GetRelativePath($SourceDir, $item.FullName)
        $target = Join-Path $DestDir $rel
        $parent = Split-Path -Parent $target
        if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
        Copy-Item -LiteralPath $item.FullName -Destination $target -Force
    }
}

function Copy-SandalSqlFiles {
    param([string]$SourceDir, [string]$DestDir)
    $null = New-Item -ItemType Directory -Path $DestDir -Force
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) { return }
    foreach ($sql in @(Get-ChildItem -LiteralPath $SourceDir -Filter '*.sql' -File)) {
        Copy-Item -LiteralPath $sql.FullName -Destination (Join-Path $DestDir $sql.Name) -Force
    }
}

function Write-SandalLauncherScripts {
    param([Parameter(Mandatory)][string]$DistDir)
    $batLines = @(
        '@echo off',
        'setlocal',
        'set "DIST_DIR=%~dp0"',
        'if exist "%DIST_DIR%launcher\SandalLauncher.exe" (',
        '    start "" "%DIST_DIR%launcher\SandalLauncher.exe" %*',
        '    exit /b 0',
        ')',
        'if exist "%DIST_DIR%Release\SandalLauncher.exe" (',
        '    start "" "%DIST_DIR%Release\SandalLauncher.exe" %*',
        '    exit /b 0',
        ')',
        'if exist "%DIST_DIR%Debug\SandalLauncher.exe" (',
        '    start "" "%DIST_DIR%Debug\SandalLauncher.exe" %*',
        '    exit /b 0',
        ')',
        'echo SandalLauncher.exe not found in "%DIST_DIR%launcher", "%DIST_DIR%Release", or "%DIST_DIR%Debug". Run: pwsh ./scripts/build.ps1 -Target SandalLauncher',
        'exit /b 1'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'launch_launcher.bat'), (($batLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
    $ps1Lines = @(
        '#Requires -Version 7.2',
        'Set-StrictMode -Version Latest',
        '$ErrorActionPreference = ''Stop''',
        '$exeName = if ($IsWindows) { ''SandalLauncher.exe'' } else { ''SandalLauncher'' }',
        'foreach ($sub in @(''launcher'', ''Release'', ''Debug'')) {',
        '    $candidate = Join-Path $PSScriptRoot $sub $exeName',
        '    if (Test-Path -LiteralPath $candidate -PathType Leaf) { & $candidate @args; exit $LASTEXITCODE }',
        '    $dllCandidate = Join-Path $PSScriptRoot $sub ''SandalLauncher.dll''',
        '    if (Test-Path -LiteralPath $dllCandidate -PathType Leaf) { & dotnet $dllCandidate @args; exit $LASTEXITCODE }',
        '}',
        '[Console]::Error.WriteLine("SandalLauncher not found under $PSScriptRoot/launcher, Release, or Debug. Run: pwsh ./scripts/build.ps1 -Target SandalLauncher")',
        'exit 1'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'launch_launcher.ps1'), (($ps1Lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
}

function Sync-SandalLauncherStage {
    param([string]$RepoRoot, [string]$DistDir, [string]$Config)
    $configDir = Join-Path $DistDir $Config
    if (Test-Path -LiteralPath $configDir -PathType Container) {
        foreach ($pair in @(@('world.exe', 'worldd.exe'), @('tether.exe', 'tether_server.exe'))) {
            $srcExe = Join-Path $configDir $pair[0]
            $dstExe = Join-Path $configDir $pair[1]
            if ((Test-Path -LiteralPath $srcExe -PathType Leaf) -and ((-not (Test-Path -LiteralPath $dstExe -PathType Leaf)) -or ((Get-Item -LiteralPath $srcExe).LastWriteTimeUtc -gt (Get-Item -LiteralPath $dstExe).LastWriteTimeUtc))) {
                Copy-Item -LiteralPath $srcExe -Destination $dstExe -Force
            }
        }
    }
    $buildOut = Join-Path $RepoRoot "build\dotnet\bin\SandalLauncher\$Config\net8.0"
    if (-not (Test-Path -LiteralPath $buildOut -PathType Container)) { return }
    $srcBin = Join-Path $buildOut 'SandalLauncher.exe'
    if (-not (Test-Path -LiteralPath $srcBin -PathType Leaf)) {
        $srcBin = Join-Path $buildOut 'SandalLauncher.dll'
    }
    if (-not (Test-Path -LiteralPath $srcBin -PathType Leaf)) { return }
    $launcherDir = Join-Path $DistDir 'launcher'
    $null = New-Item -ItemType Directory -Path $launcherDir, $configDir -Force
    $destBin = Join-Path $launcherDir (Split-Path $srcBin -Leaf)
    if ((-not (Test-Path -LiteralPath $destBin -PathType Leaf)) -or ((Get-Item -LiteralPath $srcBin).LastWriteTimeUtc -gt (Get-Item -LiteralPath $destBin).LastWriteTimeUtc)) {
        Copy-Item -Path (Join-Path $buildOut '*') -Destination $launcherDir -Recurse -Force
        Copy-Item -Path (Join-Path $buildOut '*') -Destination $configDir -Recurse -Force
    }
}

function Sync-SandalDistStage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$Config = 'Release'
    )
    $distDir = Join-Path $RepoRoot 'dist'
    foreach ($sub in @('config', 'sql', 'services', 'tools', 'content', 'nav', 'certs', 'logs', 'launcher', 'sql\migrations', 'sql\seed', "$Config\config", "$Config\sql\migrations")) {
        $null = New-Item -ItemType Directory -Path (Join-Path $distDir $sub) -Force
    }
    $srcConfig = Join-Path $RepoRoot 'src\config'
    Copy-SandalConfigStage -SourceDir $srcConfig -DestDir (Join-Path $distDir 'config')
    Copy-SandalConfigStage -SourceDir $srcConfig -DestDir (Join-Path $distDir "$Config\config")

    $srcMigrations = Join-Path $RepoRoot 'src\server\services\sql\migrations'
    Copy-SandalSqlFiles -SourceDir $srcMigrations -DestDir (Join-Path $distDir 'sql\migrations')
    Copy-SandalSqlFiles -SourceDir $srcMigrations -DestDir (Join-Path $distDir "$Config\sql\migrations")
    Copy-SandalSqlFiles -SourceDir (Join-Path $RepoRoot 'src\config\hotfixes') -DestDir (Join-Path $distDir 'sql\seed')
    Copy-SandalSqlFiles -SourceDir (Join-Path $RepoRoot 'src\deploy\postgres\init') -DestDir (Join-Path $distDir 'sql\seed')

    $caBundle = Join-Path $RepoRoot 'src\config\certs\amazon_ca_bundle.pem'
    if (Test-Path -LiteralPath $caBundle -PathType Leaf) {
        Copy-Item -LiteralPath $caBundle -Destination (Join-Path $distDir 'certs\amazon_ca_bundle.pem') -Force
    }
    Sync-SandalLauncherStage -RepoRoot $RepoRoot -DistDir $distDir -Config $Config
    Write-SandalLauncherScripts -DistDir $distDir
    return [pscustomobject]@{ DistDir = $distDir; Config = $Config }
}

function Sync-SandalDeployStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepoRoot)
    $srcDeploy = Join-Path $RepoRoot 'src\deploy'
    $destDeploy = Join-Path $RepoRoot 'deploy'
    $null = New-Item -ItemType Directory -Path $destDeploy -Force
    if (Test-Path -LiteralPath $srcDeploy -PathType Container) {
        foreach ($item in @(Get-ChildItem -LiteralPath $srcDeploy -Recurse -File -Force)) {
            $rel = [System.IO.Path]::GetRelativePath($srcDeploy, $item.FullName)
            $target = Join-Path $destDeploy $rel
            $parent = Split-Path -Parent $target
            if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
    $dockerIgnorePath = Join-Path $destDeploy '.dockerignore'
    $dockerDir = Join-Path $destDeploy 'docker'
    if ((Test-Path -LiteralPath $dockerIgnorePath -PathType Leaf) -and (Test-Path -LiteralPath $dockerDir -PathType Container)) {
        foreach ($df in @(Get-ChildItem -LiteralPath $dockerDir -Filter '*.Dockerfile' -File -Force)) {
            Copy-Item -LiteralPath $dockerIgnorePath -Destination "$($df.FullName).dockerignore" -Force
        }
    }
    $envPath = Join-Path $destDeploy '.env'
    $examplePath = Join-Path $RepoRoot 'src\config\sandal.env.example'
    $envInfo = Initialize-SandalEnv -EnvPath $envPath -ExamplePath $examplePath
    return [pscustomobject]@{ DeployDir = $destDeploy; EnvPath = $envPath; Env = $envInfo }
}

function Invoke-SandalDaemonCommand {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$ArgumentList = @(),
        [scriptblock]$CommandRunner = $null
    )
    if ($null -ne $CommandRunner) {
        $res = & $CommandRunner $FilePath $ArgumentList
        if ($res -is [int]) { return $res }
        if ($res -is [bool]) { return $(if ($res) { 0 } else { 1 }) }
        if ($res -is [System.Collections.IDictionary] -and $res.Contains('ExitCode')) { return [int]$res['ExitCode'] }
        if ($null -ne $res -and $null -ne $res.PSObject.Properties['ExitCode']) { return [int]$res.ExitCode }
        return [int]$LASTEXITCODE
    }
    $null = & $FilePath @ArgumentList *> $null
    return [int]$LASTEXITCODE
}

function Set-SandalUserDockerHost {
    param([scriptblock]$CommandRunner = $null)
    if (-not $CommandRunner -and [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable('DOCKER_HOST', 'User'))) {
        [Environment]::SetEnvironmentVariable('DOCKER_HOST', 'tcp://[::1]:2375', 'User')
    }
}

function Ensure-SandalDockerDaemon {
    [CmdletBinding()]
    param(
        [string]$Distro = 'Ubuntu-24.04',
        [switch]$DryRun,
        [scriptblock]$CommandRunner = $null
    )
    if ($DryRun) {
        return [pscustomobject]@{ Ready = $true; DryRun = $true; Distro = $Distro; Started = $false }
    }
    if ($null -eq $CommandRunner -and $null -eq (Get-Command docker -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{ Ready = $false; DryRun = $false; Distro = $Distro; Started = $false }
    }
    if ([string]::IsNullOrWhiteSpace($env:DOCKER_HOST)) { $env:DOCKER_HOST = 'tcp://[::1]:2375' }
    if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner) -eq 0) {
        Set-SandalUserDockerHost -CommandRunner $CommandRunner
        return [pscustomobject]@{ Ready = $true; DryRun = $false; Distro = $Distro; Started = $false }
    }
    $env:DOCKER_HOST = 'tcp://[::1]:2375'
    Set-SandalUserDockerHost -CommandRunner $CommandRunner
    if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner) -eq 0) {
        return [pscustomobject]@{ Ready = $true; DryRun = $false; Distro = $Distro; Started = $false }
    }
    $resolvedDistro = $Distro
    if ($IsWindows -and ($null -ne $CommandRunner -or $null -ne (Get-Command wsl.exe -ErrorAction SilentlyContinue))) {
        $wslHelper = Join-Path $PSScriptRoot 'wsl-docker.ps1'
        if (-not (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslHelper -PathType Leaf)) { . $wslHelper }
        if (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) {
            $wslRunner = if ($null -ne $CommandRunner) { { param([string[]]$WslArgs) [pscustomobject]@{ ExitCode = (Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList $WslArgs -CommandRunner $CommandRunner); Output = '' } } } else { $null }
            $resolvedDistro = Resolve-WslDockerDistro -Distro $Distro -Runner $wslRunner
        }
        if (-not $CommandRunner) {
            $null = Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '--', 'bash', '-c', 'pgrep -f sandal-wsl-keepalive >/dev/null 2>&1 || exec -a sandal-wsl-keepalive sleep 86400') -WindowStyle Hidden -ErrorAction SilentlyContinue
        }
        $bootScript = 'if ! command -v docker >/dev/null 2>&1; then bash scripts/wsl/provision-docker.sh; fi && ' +
            'mkdir -p /etc/systemd/system/docker.service.d && ' +
            'if ! grep -Fq "ExecStart=/usr/bin/dockerd -H fd:// -H tcp://0.0.0.0:2375" /etc/systemd/system/docker.service.d/override.conf 2>/dev/null; then ' +
            'printf "[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://0.0.0.0:2375\n" > /etc/systemd/system/docker.service.d/override.conf && ' +
            '(systemctl daemon-reload >/dev/null 2>&1 || true) && (systemctl restart docker >/dev/null 2>&1 || service docker restart >/dev/null 2>&1 || service docker start >/dev/null 2>&1); ' +
            'else service docker status >/dev/null 2>&1 || systemctl start docker >/dev/null 2>&1 || service docker start >/dev/null 2>&1; fi'
        $null = Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '-u', 'root', '--', 'sh', '-c', $bootScript) -CommandRunner $CommandRunner
        $ready = $false
        for ($i = 0; $i -lt 10; $i++) {
            if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner) -eq 0) { $ready = $true; break }
            if (-not $CommandRunner) { Start-Sleep -Milliseconds 500 }
        }
        return [pscustomobject]@{ Ready = $ready; DryRun = $false; Distro = $resolvedDistro; Started = $true }
    }
    $finalExit = Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner
    return [pscustomobject]@{ Ready = ($finalExit -eq 0); DryRun = $false; Distro = $resolvedDistro; Started = $false }
}

function Ensure-SandalGameData {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$GameRoot = '',
        [switch]$CheckOnly
    )
    $contentDb = Join-Path $RepoRoot 'dist\content\content.db'
    if ((Test-Path -LiteralPath $contentDb -PathType Leaf) -and ((Get-Item -LiteralPath $contentDb).Length -gt 0)) {
        return [pscustomobject]@{ Ready = $true; Extracted = $false; ContentDb = $contentDb }
    }
    $extractScript = Join-Path $RepoRoot 'scripts\extract_game_data.ps1'
    if (-not (Test-Path -LiteralPath $extractScript -PathType Leaf)) {
        return [pscustomobject]@{ Ready = $false; Extracted = $false; ContentDb = $contentDb }
    }
    $extractArgs = @()
    if (-not [string]::IsNullOrWhiteSpace($GameRoot)) { $extractArgs += @('-GameRoot', $GameRoot) }
    $clientAvailable = $false
    if ($CheckOnly) {
        try {
            $null = & $extractScript -CheckOnly @extractArgs *>$null
            $clientAvailable = ($LASTEXITCODE -eq 0)
        } catch {
            $clientAvailable = $false
        }
    }
    if ($clientAvailable -or (-not $CheckOnly)) {
        & $extractScript @extractArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Game data extraction failed (exit code $LASTEXITCODE)."
        }
    }
    $ready = (Test-Path -LiteralPath $contentDb -PathType Leaf) -and ((Get-Item -LiteralPath $contentDb).Length -gt 0)
    return [pscustomobject]@{ Ready = $ready; Extracted = $ready; ContentDb = $contentDb }
}

Export-ModuleMember -Function @(
    'New-SandalCsprngSecret',
    'Get-SandalEnvMap',
    'Set-SandalSecretAcl',
    'Initialize-SandalEnv',
    'Sync-SandalDistStage',
    'Sync-SandalDeployStage',
    'Ensure-SandalDockerDaemon',
    'Ensure-SandalGameData'
)
