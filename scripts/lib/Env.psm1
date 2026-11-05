#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Write-SandalStep {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Step,
        [Parameter(Mandatory)][string]$Message,
        [switch]$Quiet
    )
    if ($Quiet -or $env:SANDAL_QUIET -eq '1') { return }
    $callerJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue) -or
                  [bool](Get-Variable -Name Json -ValueOnly -ErrorAction SilentlyContinue)
    if (-not $callerJson) {
        $callerOpts = Get-Variable -Name opts -ValueOnly -ErrorAction SilentlyContinue
        if ($null -ne $callerOpts -and $null -ne $callerOpts.PSObject.Properties['Json'] -and [bool]$callerOpts.Json) {
            $callerJson = $true
        }
    }
    if ($callerJson) { return }
    [Console]::Out.WriteLine("[SandalCore][$Step] $Message")
    [Console]::Out.Flush()
}

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

function Get-SandalLocalOverlayMap {
    param([string]$TemplatePath, [string]$EnvPath)
    $candidates = [System.Collections.Generic.List[string]]::new()
    if ($TemplatePath) {
        $tplParent = Split-Path -Parent $TemplatePath
        if ($tplParent) { $candidates.Add((Join-Path $tplParent 'sandal.local.env')) }
    }
    if ($EnvPath) {
        $envParent = Split-Path -Parent $EnvPath
        if ($envParent) { $candidates.Add((Join-Path $envParent 'sandal.local.env')) }
    }
    foreach ($cand in $candidates) {
        if (Test-Path -LiteralPath $cand -PathType Leaf) { return (Get-SandalEnvMap -EnvPath $cand) }
    }
    return [ordered]@{}
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
    $localMap = if (-not $Force) { Get-SandalLocalOverlayMap -TemplatePath $TemplatePath -EnvPath $EnvPath } else { [ordered]@{} }
    foreach ($lk in $localMap.Keys) { $existingMap[$lk] = $localMap[$lk] }
    $sourcePath = if ($TemplatePath -and (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) { $TemplatePath } elseif (Test-Path -LiteralPath $EnvPath -PathType Leaf) { $EnvPath } else { '' }
    $inLines = if ($sourcePath) { [System.IO.File]::ReadAllLines($sourcePath, [System.Text.Encoding]::UTF8) } else { @() }
    $outLines = [System.Collections.Generic.List[string]]::new()
    $seenSecrets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $seenKeys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $generated = [System.Collections.Generic.List[string]]::new()

    foreach ($line in $inLines) {
        if ($line -notmatch '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') { $outLines.Add($line); continue }
        $key = $Matches[1]
        $val = if ($localMap.Contains($key)) { [string]$localMap[$key] } else { $Matches[2].Trim() }
        $null = $seenKeys.Add($key)
        if ($key -notin $SecretKeys) { $outLines.Add("${key}=${val}"); continue }
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

function Merge-SandalLocalJsonOverlay {
    param(
        [Parameter(Mandatory)][string]$TemplateJsonPath,
        [Parameter(Mandatory)][string]$TargetJsonPath,
        [string]$LocalJsonPath = ''
    )
    if (-not (Test-Path -LiteralPath $TargetJsonPath -PathType Leaf)) {
        Copy-Item -LiteralPath $TemplateJsonPath -Destination $TargetJsonPath -Force
    } else {
        $tplObj = Get-Content -LiteralPath $TemplateJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $dstObj = Get-Content -LiteralPath $TargetJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($null -ne $tplObj -and $null -ne $dstObj -and $tplObj -isnot [System.Array] -and $dstObj -isnot [System.Array]) {
            foreach ($prop in $dstObj.PSObject.Properties) {
                $tplObj | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
            }
            [System.IO.File]::WriteAllText($TargetJsonPath, (($tplObj | ConvertTo-Json -Depth 10) + "`n"), [System.Text.UTF8Encoding]::new($false))
        }
    }
    if (-not $LocalJsonPath -or -not (Test-Path -LiteralPath $LocalJsonPath -PathType Leaf)) { return }
    $baseObj = Get-Content -LiteralPath $TargetJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $localObj = Get-Content -LiteralPath $LocalJsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $baseObj -or $null -eq $localObj -or $baseObj -is [System.Array]) { return }
    foreach ($prop in $localObj.PSObject.Properties) {
        if ($prop.Name -ieq 'secret') { continue }
        $baseObj | Add-Member -NotePropertyName $prop.Name -NotePropertyValue $prop.Value -Force
    }
    [System.IO.File]::WriteAllText($TargetJsonPath, (($baseObj | ConvertTo-Json -Depth 10) + "`n"), [System.Text.UTF8Encoding]::new($false))
}

function Copy-SandalConfigStage {
    param([string]$SourceDir, [string]$DestDir)
    if (-not (Test-Path -LiteralPath $SourceDir -PathType Container)) { return }
    $null = New-Item -ItemType Directory -Path $DestDir -Force
    foreach ($item in @(Get-ChildItem -LiteralPath $SourceDir -Recurse -File)) {
        if ($item.Name -like '*.local.json' -or $item.Name -eq 'local.json' -or $item.Name -like '*.local.env' -or $item.Name -like '*.example' -or $item.Extension -ieq '.md') { continue }
        $rel = [System.IO.Path]::GetRelativePath($SourceDir, $item.FullName)
        $target = Join-Path $DestDir $rel
        $parent = Split-Path -Parent $target
        if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
        if ($item.Extension -ieq '.json') {
            $localFile = Join-Path $item.DirectoryName "$($item.BaseName).local.json"
            Merge-SandalLocalJsonOverlay -TemplateJsonPath $item.FullName -TargetJsonPath $target -LocalJsonPath $localFile
        } elseif (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
    $srcProfiles = Join-Path $SourceDir 'profiles'
    if (Test-Path -LiteralPath $srcProfiles -PathType Container) {
        $dstProfiles = Join-Path $DestDir 'profiles'
        $null = New-Item -ItemType Directory -Path $dstProfiles -Force
        foreach ($ex in @(Get-ChildItem -LiteralPath $srcProfiles -Filter '*.env.example' -File)) {
            $envName = $ex.Name -replace '\.example$', ''
            $dstEnv = Join-Path $dstProfiles $envName
            $srcEnv = Join-Path $srcProfiles $envName
            if (-not (Test-Path -LiteralPath $srcEnv -PathType Leaf) -and -not (Test-Path -LiteralPath $dstEnv -PathType Leaf)) {
                Copy-Item -LiteralPath $ex.FullName -Destination $dstEnv -Force
            }
        }
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

function Write-SandalDistServerScripts {
    param([Parameter(Mandatory)][string]$DistDir)
    $startLines = @(
        '#Requires -Version 7.2',
        'param([switch]$DryRun)',
        'Set-StrictMode -Version Latest; $ErrorActionPreference = ''Stop''',
        '$distRoot = $PSScriptRoot; $svcDir = Join-Path $distRoot ''services''; $relDir = Join-Path $distRoot ''Release''; $logsDir = Join-Path $distRoot ''logs''',
        '$contentDb = if ($env:AOC_CONTENT_DB) { $env:AOC_CONTENT_DB } else { Join-Path $distRoot ''content\content.db'' }',
        '$tokenFile = Join-Path $svcDir ''service.token''; $exeExt = if ($IsWindows) { ''.exe'' } else { '''' }',
        'if (-not $DryRun) {',
        '    $null = New-Item -ItemType Directory -Path $svcDir, $logsDir -Force',
        '    if (-not (Test-Path -LiteralPath $tokenFile -PathType Leaf)) {',
        '        $bytes = [byte[]]::new(32); [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)',
        '        [System.IO.File]::WriteAllText($tokenFile, ([System.Convert]::ToHexString($bytes).ToLowerInvariant()))',
        '    }',
        '    $env:AOC_SERVICE_TOKEN = (Get-Content -LiteralPath $tokenFile -Raw).Trim()',
        '}',
        '$names = @(''auth'', ''character'', ''citynode'', ''quests'', ''mesh'', ''gateway'', ''meshgw'', ''admin'', ''tether_server'', ''worldd'')',
        'foreach ($name in $names) {',
        '    $exe = if ($name -in @(''tether_server'', ''worldd'')) { Join-Path $relDir "$name$exeExt" } else { Join-Path $svcDir "$name$exeExt" }',
        '    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { continue }',
        '    $argList = if ($name -in @(''worldd'', ''character'') -and (Test-Path -LiteralPath $contentDb -PathType Leaf)) { @(''--content-db'', $contentDb) } else { @() }',
        '    if ($DryRun) { Write-Output "DRYRUN start $exe $($argList -join '' '')"; continue }',
        '    $outLog = Join-Path $logsDir "$name.out.log"; $errLog = Join-Path $logsDir "$name.log"',
        '    $null = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $distRoot -RedirectStandardOutput $outLog -RedirectStandardError $errLog -WindowStyle Hidden -PassThru',
        '}'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'start_server.ps1'), (($startLines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
    $stopLines = @(
        '#Requires -Version 7.2',
        'param([switch]$DryRun)',
        'Set-StrictMode -Version Latest; $ErrorActionPreference = ''Stop''',
        '$distRoot = [System.IO.Path]::GetFullPath($PSScriptRoot)',
        '$names = @(''worldd'', ''world'', ''tether_server'', ''tether'', ''gateway'', ''meshgw'', ''admin'', ''mesh'', ''quests'', ''citynode'', ''character'', ''auth'')',
        '$procs = @(Get-Process -Name $names -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($distRoot, [System.StringComparison]::OrdinalIgnoreCase) })',
        'if ($DryRun) { Write-Output "DRYRUN stop $($procs.Count) process(es) under $distRoot"; exit 0 }',
        'foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'stop_server.ps1'), (($stopLines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
}

function Write-SandalLauncherScripts {
    param([Parameter(Mandatory)][string]$DistDir)
    $batLines = @(
        '@echo off', 'setlocal', 'set "DIST_DIR=%~dp0"',
        'if exist "%DIST_DIR%launcher\SandalLauncher.exe" ( start "" "%DIST_DIR%launcher\SandalLauncher.exe" %* & exit /b 0 )',
        'if exist "%DIST_DIR%Release\SandalLauncher.exe" ( start "" "%DIST_DIR%Release\SandalLauncher.exe" %* & exit /b 0 )',
        'if exist "%DIST_DIR%Debug\SandalLauncher.exe" ( start "" "%DIST_DIR%Debug\SandalLauncher.exe" %* & exit /b 0 )',
        'echo SandalLauncher.exe not found in "%DIST_DIR%launcher", "%DIST_DIR%Release", or "%DIST_DIR%Debug". Run: pwsh ./scripts/build.ps1 -Target SandalLauncher',
        'exit /b 1'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'launch_launcher.bat'), (($batLines -join "`r`n") + "`r`n"), [System.Text.UTF8Encoding]::new($false))
    $ps1Lines = @(
        '#Requires -Version 7.2',
        'Set-StrictMode -Version Latest; $ErrorActionPreference = ''Stop''',
        '$exeName = if ($IsWindows) { ''SandalLauncher.exe'' } else { ''SandalLauncher'' }',
        'foreach ($sub in @(''launcher'', ''Release'', ''Debug'')) {',
        '    $candidate = Join-Path $PSScriptRoot $sub $exeName',
        '    if (Test-Path -LiteralPath $candidate -PathType Leaf) { & $candidate @args; exit $LASTEXITCODE }',
        '    $dllCandidate = Join-Path $PSScriptRoot $sub ''SandalLauncher.dll''',
        '    if (Test-Path -LiteralPath $dllCandidate -PathType Leaf) { & dotnet $dllCandidate @args; exit $LASTEXITCODE }',
        '}',
        '[Console]::Error.WriteLine("SandalLauncher not found under $PSScriptRoot/launcher, Release, or Debug. Run: pwsh ./scripts/build.ps1 -Target SandalLauncher"); exit 1'
    )
    [System.IO.File]::WriteAllText((Join-Path $DistDir 'launch_launcher.ps1'), (($ps1Lines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
    Write-SandalDistServerScripts -DistDir $DistDir
}

function Sync-SandalLauncherStage {
    param([string]$RepoRoot, [string]$DistDir, [string]$Config)
    $configDir = Join-Path $DistDir $Config
    $launcherDir = Join-Path $DistDir 'launcher'
    foreach ($d in @($launcherDir, $configDir)) {
        if (Test-Path -LiteralPath $d -PathType Container) { Get-ChildItem -LiteralPath $d -Filter '*.pdb' -Recurse -File -Force -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue }
    }
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
    $srcPrimary = @('SandalLauncher.dll', 'SandalLauncher.exe', 'SandalLauncher') | ForEach-Object { Join-Path $buildOut $_ } | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $srcPrimary) { return }
    $null = New-Item -ItemType Directory -Path $launcherDir, $configDir -Force
    $destPrimary = Join-Path $launcherDir (Split-Path -Leaf $srcPrimary)
    $srcItem = Get-Item -LiteralPath $srcPrimary
    $destItem = Get-Item -LiteralPath $destPrimary -ErrorAction SilentlyContinue
    if ($null -eq $destItem -or $srcItem.LastWriteTimeUtc -gt $destItem.LastWriteTimeUtc -or $srcItem.Length -ne $destItem.Length) {
        foreach ($f in @(Get-ChildItem -LiteralPath $buildOut -Recurse -File -Force)) {
            if ($f.Extension -in @('.pdb', '.xml') -or $f.Name -like '*.deps.json') { continue }
            $rel = [System.IO.Path]::GetRelativePath($buildOut, $f.FullName)
            foreach ($destRoot in @($launcherDir, $configDir)) {
                $destFile = Join-Path $destRoot $rel
                $destParent = Split-Path -Parent $destFile
                if ($destParent -and -not (Test-Path -LiteralPath $destParent)) { $null = New-Item -ItemType Directory -Path $destParent -Force }
                Copy-Item -LiteralPath $f.FullName -Destination $destFile -Force
            }
        }
    }
}

function Sync-SandalDistStage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$Config = 'Release'
    )
    $distDir = Join-Path $RepoRoot 'dist'
    Write-SandalStep -Step 'DistStage' -Message "Staging runtime configs, SQL migrations, and launcher into $distDir..."
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

function Sync-SandalDeployRuntimeDirs {
    param([Parameter(Mandatory)][string]$RepoRoot, [Parameter(Mandatory)][string]$DeployDir)
    $distDir = Join-Path $RepoRoot 'dist'
    foreach ($sub in @('content', 'certs', 'nav', 'config')) {
        $null = New-Item -ItemType Directory -Path (Join-Path $DeployDir $sub) -Force
    }
    Copy-SandalConfigStage -SourceDir (Join-Path $RepoRoot 'src\config') -DestDir (Join-Path $DeployDir 'config')
    foreach ($sub in @('certs', 'nav')) {
        $srcSub = Join-Path $distDir $sub
        if (-not (Test-Path -LiteralPath $srcSub -PathType Container)) { continue }
        foreach ($f in @(Get-ChildItem -LiteralPath $srcSub -Recurse -File -Force)) {
            $rel = [System.IO.Path]::GetRelativePath($srcSub, $f.FullName)
            $dst = Join-Path $DeployDir $sub $rel
            $parent = Split-Path -Parent $dst
            if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
            Copy-Item -LiteralPath $f.FullName -Destination $dst -Force
        }
    }
    $distContent = Join-Path $distDir 'content'
    $deployContent = Join-Path $DeployDir 'content'
    if (-not (Test-Path -LiteralPath $distContent -PathType Container)) { return }
    foreach ($f in @(Get-ChildItem -LiteralPath $distContent -File -Force | Where-Object { $_.Extension -in @('.tsv', '.usmap', '.db') -or $_.Name -eq 'area_nav.bin' })) {
        $dst = Join-Path $deployContent $f.Name
        if ((Test-Path -LiteralPath $dst -PathType Leaf) -and ((Get-Item -LiteralPath $dst).Length -eq $f.Length)) { continue }
        if (Test-Path -LiteralPath $dst -PathType Leaf) { Remove-Item -LiteralPath $dst -Force }
        try { $null = New-Item -ItemType HardLink -Path $dst -Target $f.FullName -ErrorAction Stop }
        catch { Copy-Item -LiteralPath $f.FullName -Destination $dst -Force }
    }
}

function Write-SandalDeployScripts {
    param([Parameter(Mandatory)][string]$DeployDir)
    $wakeLines = @(
        '$null = & docker info *> $null',
        'if ($LASTEXITCODE -ne 0 -and $IsWindows -and $env:DOCKER_HOST -in @(''tcp://[::1]:2375'', ''tcp://127.0.0.1:2375'') -and ([System.IO.File]::Exists(''\\.\pipe\docker_engine'') -or ($null -ne (Get-Process -Name ''Docker Desktop'' -ErrorAction SilentlyContinue)))) {',
        '    $null = & docker --host npipe:////./pipe/docker_engine info *> $null; if ($LASTEXITCODE -eq 0) { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }',
        '}',
        'if ($LASTEXITCODE -ne 0) { $env:DOCKER_HOST = ''tcp://127.0.0.1:2375''; $null = & docker info *> $null }',
        'if ($LASTEXITCODE -ne 0) { $env:DOCKER_HOST = ''tcp://[::1]:2375''; $null = & docker info *> $null }',
        'if ($LASTEXITCODE -ne 0 -and $IsWindows -and (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {',
        '    $allDistros = @((@(& wsl.exe -l -q 2>$null) -replace "`0", '''') | ForEach-Object { ($_ -replace ''^\*\s*'', '''').Trim() } | Where-Object { $_ -and $_ -notlike ''docker-desktop*'' })',
        '    $distro = @($allDistros | Where-Object { $_ -like ''Ubuntu*'' -or $_ -like ''Debian*'' } | Select-Object -First 1)[0]',
        '    if (-not $distro) { $distro = if ($allDistros.Count -gt 0) { $allDistros[0] } else { ''Ubuntu'' } }',
        '    $null = & wsl.exe -d $distro -u root -- sh -c "systemctl start docker.socket docker.service >/dev/null 2>&1 || service docker start >/dev/null 2>&1"',
        '    for ($w = 0; $w -lt 10; $w++) {',
        '        $env:DOCKER_HOST = ''tcp://127.0.0.1:2375''; $null = & docker info *> $null; if ($LASTEXITCODE -eq 0) { break }',
        '        $env:DOCKER_HOST = ''tcp://[::1]:2375''; $null = & docker info *> $null; if ($LASTEXITCODE -eq 0) { break }',
        '        Start-Sleep -Milliseconds 500',
        '    }',
        '}',
        'if ($IsWindows -and $env:DOCKER_HOST -in @(''tcp://[::1]:2375'', ''tcp://127.0.0.1:2375'')) {',
        '    $normDeploy = ([System.IO.Path]::GetFullPath($PSScriptRoot)).Replace(''\'', ''/'').TrimEnd(''/'')',
        '    $wslDeploy = if ($normDeploy -match ''^([A-Za-z]):/(.*)$'') { "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2])" } else { $normDeploy }',
        '    $env:AOC_DIST_CONTENT_DIR = "$wslDeploy/content"; $env:AOC_DIST_CERTS_DIR = "$wslDeploy/certs"; $env:AOC_DIST_NAV_DIR = "$wslDeploy/nav"; $env:AOC_DIST_CONFIG_DIR = "$wslDeploy/config"',
        '}'
    )
    $upLines = @(
        '#Requires -Version 7.2',
        'param([string]$Profile = ''default'', [switch]$Detach, [switch]$DryRun)',
        'Set-StrictMode -Version Latest; $ErrorActionPreference = ''Stop''',
        '$composeFile = Join-Path $PSScriptRoot ''compose.yml''; $envFile = Join-Path $PSScriptRoot ''.env''',
        '$cmdArgs = [System.Collections.Generic.List[string]]::new()',
        '$cmdArgs.AddRange([string[]]@(''compose'', ''-f'', $composeFile, ''--env-file'', $envFile))',
        'if ($Profile -ne ''default'') { $cmdArgs.AddRange([string[]]@(''--profile'', $Profile)) }',
        '$cmdArgs.Add(''up''); if ($Detach) { $cmdArgs.AddRange([string[]]@(''-d'', ''--wait'')) }',
        'if ($DryRun) { Write-Output ("docker " + ($cmdArgs -join '' '')); exit 0 }'
    ) + $wakeLines + @('Push-Location $PSScriptRoot; try { & docker @cmdArgs; exit $LASTEXITCODE } finally { Pop-Location }')
    [System.IO.File]::WriteAllText((Join-Path $DeployDir 'up.ps1'), (($upLines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
    $downLines = @(
        '#Requires -Version 7.2',
        'param([switch]$Volumes, [switch]$DryRun)',
        'Set-StrictMode -Version Latest; $ErrorActionPreference = ''Stop''',
        '$composeFile = Join-Path $PSScriptRoot ''compose.yml''; $envFile = Join-Path $PSScriptRoot ''.env''',
        '$cmdArgs = [System.Collections.Generic.List[string]]::new()',
        '$cmdArgs.AddRange([string[]]@(''compose'', ''-f'', $composeFile, ''--env-file'', $envFile, ''down'', ''--remove-orphans''))',
        'if ($Volumes) { $cmdArgs.Add(''-v'') }',
        'if ($DryRun) { Write-Output ("docker " + ($cmdArgs -join '' '')); exit 0 }'
    ) + $wakeLines + @('Push-Location $PSScriptRoot; try { & docker @cmdArgs; exit $LASTEXITCODE } finally { Pop-Location }')
    [System.IO.File]::WriteAllText((Join-Path $DeployDir 'down.ps1'), (($downLines -join "`n") + "`n"), [System.Text.UTF8Encoding]::new($false))
}

function Sync-SandalDeployStage {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepoRoot)
    $srcDeploy = Join-Path $RepoRoot 'src\deploy'
    $destDeploy = Join-Path $RepoRoot 'deploy'
    Write-SandalStep -Step 'DeployStage' -Message "Synchronizing Docker Compose deployment stage into $destDeploy..."
    $null = New-Item -ItemType Directory -Path $destDeploy -Force
    $legacyCompose = Join-Path $destDeploy 'docker-compose.yml'
    if (Test-Path -LiteralPath $legacyCompose -PathType Leaf) { Remove-Item -LiteralPath $legacyCompose -Force }
    if (Test-Path -LiteralPath $srcDeploy -PathType Container) {
        foreach ($item in @(Get-ChildItem -LiteralPath $srcDeploy -Recurse -File -Force)) {
            if ($item.Name -ieq 'docker-compose.yml') { continue }
            $rel = [System.IO.Path]::GetRelativePath($srcDeploy, $item.FullName)
            $target = Join-Path $destDeploy $rel
            $parent = Split-Path -Parent $target
            if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
            Copy-Item -LiteralPath $item.FullName -Destination $target -Force
        }
    }
    $dockerDir = Join-Path $destDeploy 'docker'
    if (Test-Path -LiteralPath $dockerDir -PathType Container) {
        foreach ($dupIgnore in @(Get-ChildItem -LiteralPath $dockerDir -Filter '*.Dockerfile.dockerignore' -File -Force -ErrorAction SilentlyContinue)) {
            Remove-Item -LiteralPath $dupIgnore.FullName -Force
        }
    }
    Sync-SandalDeployRuntimeDirs -RepoRoot $RepoRoot -DeployDir $destDeploy
    Write-SandalDeployScripts -DeployDir $destDeploy
    $envPath = Join-Path $destDeploy '.env'
    $examplePath = Join-Path $RepoRoot 'src\config\sandal.env.example'
    $envInfo = Initialize-SandalEnv -EnvPath $envPath -ExamplePath $examplePath
    return [pscustomobject]@{ DeployDir = $destDeploy; EnvPath = $envPath; Env = $envInfo }
}

function Invoke-SandalDaemonCommand {
    param([Parameter(Mandatory)][string]$FilePath, [string[]]$ArgumentList = @(), [scriptblock]$CommandRunner = $null)
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
    param([Parameter(Position = 0)][string]$Endpoint = 'tcp://127.0.0.1:2375', [scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner) { return }
    $currentUserHost = [Environment]::GetEnvironmentVariable('DOCKER_HOST', 'User')
    if ([string]::IsNullOrWhiteSpace($currentUserHost) -or $currentUserHost -eq 'tcp://[::1]:2375') {
        [Environment]::SetEnvironmentVariable('DOCKER_HOST', $Endpoint, 'User')
    }
}

function Confirm-SandalWslConfig {
    param([scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner -or -not $IsWindows -or $null -eq (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return }
    $wslCfgHelper = Join-Path $PSScriptRoot 'wsl-config.ps1'
    if (-not (Get-Command Confirm-WslConfig -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslCfgHelper -PathType Leaf)) { . $wslCfgHelper }
    if (Get-Command Confirm-WslConfig -ErrorAction SilentlyContinue) { $null = Confirm-WslConfig -PromptReader { '' } }
}

function Test-SandalDockerDaemonReady {
    [CmdletBinding()]
    param([scriptblock]$CommandRunner = $null)
    if ($env:DOCKER_HOST -eq 'tcp://[::1]:2375') {
        $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
        Set-SandalUserDockerHost -Endpoint 'tcp://127.0.0.1:2375' -CommandRunner $CommandRunner
    }
    if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner) -eq 0) {
        return @{ Ready = $true; Mode = 'current'; Endpoint = [string]$env:DOCKER_HOST }
    }
    $hasDesktopPipe = $IsWindows -and (($null -ne (Get-Process -Name 'Docker Desktop' -ErrorAction SilentlyContinue)) -or [System.IO.File]::Exists('\\.\pipe\docker_engine') -or [System.IO.File]::Exists('\\.\pipe\dockerDesktopLinuxEngine'))
    if ($null -eq $CommandRunner -and $hasDesktopPipe -and $env:DOCKER_HOST -in @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
        $pipeOk = ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('--host', 'npipe:////./pipe/docker_engine', 'info')) -eq 0) -or
                  ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('--context', 'desktop-linux', 'info')) -eq 0)
        if ($pipeOk) {
            Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
            if ([Environment]::GetEnvironmentVariable('DOCKER_HOST', 'User') -in @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
                [Environment]::SetEnvironmentVariable('DOCKER_HOST', $null, 'User')
            }
            Write-Host 'INFO: Detected active Docker Desktop daemon; cleared stale TCP DOCKER_HOST and using Docker Desktop.'
            return @{ Ready = $true; Mode = 'docker-desktop'; Endpoint = 'npipe:////./pipe/docker_engine' }
        }
    }
    return @{ Ready = $false; Mode = 'none'; Endpoint = [string]$env:DOCKER_HOST }
}

function Start-SandalDockerDesktopIfInstalled {
    [CmdletBinding()]
    param([scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner -or -not $IsWindows -or $env:SANDAL_DOCKER_BACKEND -eq 'wsl') { return $false }
    $desktopExe = Join-Path $env:ProgramFiles 'Docker\Docker\Docker Desktop.exe'
    if (-not (Test-Path -LiteralPath $desktopExe -PathType Leaf)) { return $false }
    $isRunning = $null -ne (Get-Process -Name 'Docker Desktop' -ErrorAction SilentlyContinue)
    if (-not $isRunning -and $env:SANDAL_PREFER_DOCKER_DESKTOP -ne '1') {
        Write-Host 'INFO: Docker Desktop is installed (not running); bootstrapping WSL2 Docker Engine...'
        return $false
    }
    if (-not $isRunning) { Start-Process -FilePath $desktopExe | Out-Null }
    for ($i = 0; $i -lt 15; $i++) {
        if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('--host', 'npipe:////./pipe/docker_engine', 'info')) -eq 0) {
            Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
            return $true
        }
        Start-Sleep -Seconds 1
    }
    return $false
}

function Wait-SandalWslTcpDockerReady {
    param([int]$MaxAttempts = 8, [int]$DelayMs = 500, [scriptblock]$CommandRunner = $null)
    if ($MaxAttempts -gt 1) {
        Write-SandalStep -Step 'Docker' -Message "Waiting for WSL2 Docker TCP endpoint (:2375) (up to $MaxAttempts attempts)..." -Quiet:($null -ne $CommandRunner)
    }
    for ($i = 0; $i -lt $MaxAttempts; $i++) {
        $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
        if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner) -eq 0) { return $true }
        if ($null -eq $CommandRunner) {
            $env:DOCKER_HOST = 'tcp://[::1]:2375'
            if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info')) -eq 0) { return $true }
            $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
            Start-Sleep -Milliseconds $DelayMs
        }
    }
    return $false
}

function ConvertTo-SandalWslPath {
    param([string]$WinPath, [string]$Distro = 'Ubuntu-24.04', [scriptblock]$CommandRunner = $null)
    $norm = $WinPath.Replace('\', '/')
    $fallback = if ($norm -match '^([A-Za-z]):/(.*)$') { "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2])" } else { $norm }
    if ($null -ne $CommandRunner) { return $fallback }
    $out = @(& wsl.exe -d $Distro -- wslpath -u $WinPath 2>$null)
    if ($LASTEXITCODE -eq 0 -and $out.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace("$($out[0])")) { return ("$($out[0])").Trim() }
    return $fallback
}

function Sync-SandalWorldImageAlias {
    param([scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner) { return }
    if ((Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('image', 'inspect', 'sandal-world:local')) -ne 0 -and
        (Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('image', 'inspect', 'sandal-worldd:local')) -eq 0) {
        $null = Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('tag', 'sandal-worldd:local', 'sandal-world:local')
    }
}

function Set-SandalWslComposeMountEnv {
    param([string]$DeployDir = (Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'deploy'))
    $vars = @{ AOC_DIST_CONTENT_DIR = 'content'; AOC_DIST_CERTS_DIR = 'certs'; AOC_DIST_NAV_DIR = 'nav'; AOC_DIST_CONFIG_DIR = 'config' }
    if ($IsWindows -and $env:DOCKER_HOST -in @('tcp://[::1]:2375', 'tcp://127.0.0.1:2375')) {
        $baseNorm = ([System.IO.Path]::GetFullPath($DeployDir)).Replace('\', '/').TrimEnd('/')
        $wslBase = if ($baseNorm -match '^([A-Za-z]):/(.*)$') { "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2])" } else { $baseNorm }
        foreach ($kv in $vars.GetEnumerator()) { [System.Environment]::SetEnvironmentVariable($kv.Key, "$wslBase/$($kv.Value)", 'Process') }
        return
    }
    foreach ($key in $vars.Keys) { Remove-Item "Env:\$key" -ErrorAction SilentlyContinue }
}

function Start-SandalWslKeepAlive {
    param([string]$Distro = 'Ubuntu-24.04', [scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner -or -not $IsWindows -or $null -eq (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return }
    $wslHelper = Join-Path $PSScriptRoot 'wsl-docker.ps1'
    if (-not (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslHelper -PathType Leaf)) { . $wslHelper }
    $resolvedDistro = if (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) { Resolve-WslDockerDistro -Distro $Distro } else { $Distro }
    $null = & wsl.exe -d $resolvedDistro -u root -- sh -c "pgrep -f 'sleep infinity' >/dev/null 2>&1 || nohup sleep infinity >/dev/null 2>&1 &" 2>$null
    if ($null -eq (Get-Process -Name 'wsl' -ErrorAction SilentlyContinue)) {
        Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '--', 'sleep', 'infinity') -WindowStyle Hidden | Out-Null
    }
}

function Start-SandalIpv6DockerBridge {
    param([string]$Distro = 'Ubuntu-24.04', [scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner -or -not $IsWindows) { return }
    $probe6 = [System.Net.Sockets.TcpClient]::new([System.Net.Sockets.AddressFamily]::InterNetworkV6)
    try {
        $iar6 = $probe6.BeginConnect([System.Net.IPAddress]::IPv6Loopback, 2375, $null, $null)
        if ($iar6.AsyncWaitHandle.WaitOne(100) -and $probe6.Connected) { return }
    } catch { } finally { $probe6.Dispose() }
    $wslHelper = Join-Path $PSScriptRoot 'wsl-docker.ps1'
    if (-not (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslHelper -PathType Leaf)) { . $wslHelper }
    $resolvedDistro = if (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) { Resolve-WslDockerDistro -Distro $Distro } else { $Distro }
    $rebindCmd = "mkdir -p /etc/systemd/system/docker.service.d && rm -f /etc/systemd/system/docker.service.d/override.conf && if ! grep -q '127.0.0.1:23750' /etc/systemd/system/docker.service.d/sandal-tcp.conf 2>/dev/null; then printf '[Service]\nExecStart=\nExecStart=/usr/bin/dockerd -H fd:// -H tcp://127.0.0.1:23750\n' > /etc/systemd/system/docker.service.d/sandal-tcp.conf && systemctl daemon-reload && systemctl restart docker; else systemctl start docker; fi"
    $null = & wsl.exe -d $resolvedDistro -u root -- sh -c $rebindCmd 2>$null
    $bridgeLines = @(
        '$ErrorActionPreference = ''SilentlyContinue''',
        '$l6 = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::IPv6Loopback, 2375); $l6.Server.DualMode = $false; $l6.Start()',
        '$l4 = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 2375); $l4.Start()',
        '$active = [System.Collections.Generic.List[object]]::new()',
        '$t6 = $l6.AcceptTcpClientAsync(); $t4 = $l4.AcceptTcpClientAsync()',
        'while ($true) {',
        '    [System.Threading.Tasks.Task[]]$waitTasks = @($t6, $t4) + @($active | ForEach-Object { $_.T1; $_.T2 })',
        '    [void][System.Threading.Tasks.Task]::WaitAny($waitTasks)',
        '    for ($i = $active.Count - 1; $i -ge 0; $i--) {',
        '        if ($active[$i].T1.IsCompleted -or $active[$i].T2.IsCompleted) { $active[$i].C.Dispose(); $active[$i].U.Dispose(); $active.RemoveAt($i) }',
        '    }',
        '    $accepted = [System.Collections.Generic.List[System.Net.Sockets.TcpClient]]::new()',
        '    if ($t6.IsCompleted) { $accepted.Add($t6.GetAwaiter().GetResult()); $t6 = $l6.AcceptTcpClientAsync() }',
        '    if ($t4.IsCompleted) { $accepted.Add($t4.GetAwaiter().GetResult()); $t4 = $l4.AcceptTcpClientAsync() }',
        '    foreach ($c in $accepted) {',
        '        $u = [System.Net.Sockets.TcpClient]::new()',
        '        try { $u.Connect(''127.0.0.1'', 23750) } catch {',
        '            $u.Dispose()',
        "            `$null = & wsl.exe -d '$resolvedDistro' -u root -- sh -c 'systemctl start docker.socket docker.service >/dev/null 2>&1 || service docker start >/dev/null 2>&1' 2>`$null",
        '            $u = [System.Net.Sockets.TcpClient]::new()',
        '            for ($r = 0; $r -lt 20; $r++) { try { $u.Connect(''127.0.0.1'', 23750); break } catch { Start-Sleep -Milliseconds 250 } }',
        '        }',
        '        if ($u.Connected) { $s1 = $c.GetStream(); $s2 = $u.GetStream(); $active.Add([pscustomobject]@{ C = $c; U = $u; T1 = $s1.CopyToAsync($s2); T2 = $s2.CopyToAsync($s1) }) } else { $u.Dispose(); $c.Dispose() }',
        '    }',
        '}'
    )
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes(($bridgeLines -join "`n")))
    Start-Process -FilePath (Get-Process -Id $PID).Path -ArgumentList @('-NoProfile', '-NonInteractive', '-WindowStyle', 'Hidden', '-EncodedCommand', $encoded) -WindowStyle Hidden | Out-Null
    for ($p = 0; $p -lt 10; $p++) {
        $chk = [System.Net.Sockets.TcpClient]::new([System.Net.Sockets.AddressFamily]::InterNetworkV6)
        try {
            $ar = $chk.BeginConnect([System.Net.IPAddress]::IPv6Loopback, 2375, $null, $null)
            if ($ar.AsyncWaitHandle.WaitOne(50) -and $chk.Connected) { break }
        } catch { } finally { $chk.Dispose() }
        Start-Sleep -Milliseconds 50
    }
}

function Install-SandalPowerShellDockerAutoWake {
    param([string]$Distro = 'Ubuntu-24.04', [string]$ProfilePath = $PROFILE.CurrentUserAllHosts, [scriptblock]$CommandRunner = $null)
    if ($null -ne $CommandRunner -or -not $IsWindows -or [string]::IsNullOrWhiteSpace($ProfilePath)) { return }
    $wslHelper = Join-Path $PSScriptRoot 'wsl-docker.ps1'
    if (-not (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslHelper -PathType Leaf)) { . $wslHelper }
    $resolvedDistro = if (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) { Resolve-WslDockerDistro -Distro $Distro } else { $Distro }
    $blockLines = @(
        '# >>> sandal-docker-autowake >>>',
        'function global:docker {',
        '    if ([string]::IsNullOrWhiteSpace($env:DOCKER_HOST) -or $env:DOCKER_HOST -eq ''tcp://[::1]:2375'') { $env:DOCKER_HOST = ''tcp://127.0.0.1:2375'' }',
        '    if ($env:DOCKER_HOST -eq ''tcp://127.0.0.1:2375'') {',
        '        $tcpOk = $false; $client = [System.Net.Sockets.TcpClient]::new()',
        '        try { $iar = $client.BeginConnect(''127.0.0.1'', 2375, $null, $null); if ($iar.AsyncWaitHandle.WaitOne(150) -and $client.Connected) { $tcpOk = $true } } catch { } finally { $client.Dispose() }',
        '        if (-not $tcpOk -and (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {',
        "            `$null = & wsl.exe -d '$resolvedDistro' -u root -- sh -c 'systemctl start docker.socket docker.service >/dev/null 2>&1 || service docker start >/dev/null 2>&1; pgrep -f ""sleep infinity"" >/dev/null 2>&1 || nohup sleep infinity >/dev/null 2>&1 &' 2>`$null",
        "            if (`$null -eq (Get-Process -Name 'wsl' -ErrorAction SilentlyContinue)) { Start-Process -FilePath 'wsl.exe' -ArgumentList @('-d', '$resolvedDistro', '--', 'sleep', 'infinity') -WindowStyle Hidden | Out-Null }",
        '        }',
        '    }',
        '    & docker.exe @args',
        '}',
        '# <<< sandal-docker-autowake <<<'
    )
    $blockText = $blockLines -join "`n"
    $parentDir = Split-Path -Path $ProfilePath -Parent
    if (-not [string]::IsNullOrWhiteSpace($parentDir) -and -not (Test-Path -LiteralPath $parentDir)) { $null = New-Item -ItemType Directory -Path $parentDir -Force }
    $existing = if (Test-Path -LiteralPath $ProfilePath -PathType Leaf) { [System.IO.File]::ReadAllText($ProfilePath, [System.Text.Encoding]::UTF8) } else { '' }
    $pattern = '(?s)# >>> sandal-docker-autowake >>>.*?# <<< sandal-docker-autowake <<<'
    $updated = if ($existing -match $pattern) { [regex]::Replace($existing, $pattern, $blockText) } elseif ([string]::IsNullOrWhiteSpace($existing)) { "$blockText`n" } else { $existing.TrimEnd() + "`n`n$blockText`n" }
    if ($updated -cne $existing) { [System.IO.File]::WriteAllText($ProfilePath, $updated, [System.Text.UTF8Encoding]::new($false)) }
}

function Complete-SandalWslDockerDaemonSetup {
    param([string]$Distro = 'Ubuntu-24.04', [scriptblock]$CommandRunner = $null)
    if ($IsWindows -and ($env:DOCKER_HOST -like '*:2375' -or [string]::IsNullOrWhiteSpace($env:DOCKER_HOST))) {
        $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
        Set-SandalUserDockerHost 'tcp://127.0.0.1:2375' -CommandRunner $CommandRunner
        Start-SandalWslKeepAlive -Distro $Distro -CommandRunner $CommandRunner
        Start-SandalIpv6DockerBridge -Distro $Distro -CommandRunner $CommandRunner
        Install-SandalPowerShellDockerAutoWake -Distro $Distro -CommandRunner $CommandRunner
        if ($null -eq $CommandRunner -and (Get-Command docker.exe -ErrorAction SilentlyContinue)) {
            $ctxVerb = if ((Invoke-SandalDaemonCommand -FilePath 'docker.exe' -ArgumentList @('context', 'inspect', 'wsl-docker')) -eq 0) { 'update' } else { 'create' }
            $null = Invoke-SandalDaemonCommand -FilePath 'docker.exe' -ArgumentList @('context', $ctxVerb, 'wsl-docker', '--docker', 'host=tcp://127.0.0.1:2375')
            $null = Invoke-SandalDaemonCommand -FilePath 'docker.exe' -ArgumentList @('context', 'use', 'wsl-docker')
        }
    }
    Sync-SandalWorldImageAlias -CommandRunner $CommandRunner
    Set-SandalWslComposeMountEnv
}

function Invoke-SandalWslDockerBootstrap {
    param([string]$Distro = 'Ubuntu-24.04', [scriptblock]$CommandRunner = $null)
    Write-SandalStep -Step 'WSL-Docker' -Message "Bootstrapping WSL2 Docker daemon in distro '$Distro'..." -Quiet:($null -ne $CommandRunner)
    Confirm-SandalWslConfig -CommandRunner $CommandRunner
    $wslHelper = Join-Path $PSScriptRoot 'wsl-docker.ps1'
    if (-not (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) -and (Test-Path -LiteralPath $wslHelper -PathType Leaf)) { . $wslHelper }
    $resolvedDistro = $Distro
    if (Get-Command Resolve-WslDockerDistro -ErrorAction SilentlyContinue) {
        $wslRunner = if ($null -ne $CommandRunner) { { param([string[]]$WslArgs) [pscustomobject]@{ ExitCode = (Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList $WslArgs -CommandRunner $CommandRunner); Output = '' } } } else { $null }
        $resolvedDistro = Resolve-WslDockerDistro -Distro $Distro -Runner $wslRunner
    }
    $wakeCmd = 'systemctl start docker.socket docker.service >/dev/null 2>&1 || service docker start >/dev/null 2>&1'
    $null = Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '-u', 'root', '--', 'sh', '-c', $wakeCmd) -CommandRunner $CommandRunner
    Start-SandalIpv6DockerBridge -Distro $resolvedDistro -CommandRunner $CommandRunner
    $ready = Wait-SandalWslTcpDockerReady -MaxAttempts $(if ($null -ne $CommandRunner) { 1 } else { 8 }) -DelayMs 500 -CommandRunner $CommandRunner
    $provScript = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'scripts\wsl\provision-docker.sh'
    if (-not $ready -and (Test-Path -LiteralPath $provScript -PathType Leaf)) {
        $wslProvPath = ConvertTo-SandalWslPath -WinPath $provScript -Distro $resolvedDistro -CommandRunner $CommandRunner
        $provExit = Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '-u', 'root', '--', 'bash', $wslProvPath) -CommandRunner $CommandRunner
        if ($provExit -eq 42) {
            Write-Host "INFO: Restarting WSL2 distro '$resolvedDistro' to activate systemd..."
            $null = Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList @('--terminate', $resolvedDistro) -CommandRunner $CommandRunner
            $null = Invoke-SandalDaemonCommand -FilePath 'wsl.exe' -ArgumentList @('-d', $resolvedDistro, '-u', 'root', '--', 'bash', $wslProvPath) -CommandRunner $CommandRunner
        }
        Start-SandalIpv6DockerBridge -Distro $resolvedDistro -CommandRunner $CommandRunner
        $ready = Wait-SandalWslTcpDockerReady -MaxAttempts $(if ($null -ne $CommandRunner) { 2 } else { 15 }) -DelayMs 1000 -CommandRunner $CommandRunner
    }
    if ($ready) { Complete-SandalWslDockerDaemonSetup -Distro $resolvedDistro -CommandRunner $CommandRunner }
    return [pscustomobject]@{ Ready = $ready; DryRun = $false; Distro = $resolvedDistro; Started = $true }
}

function Ensure-SandalDockerDaemon {
    [CmdletBinding()]
    param([string]$Distro = 'Ubuntu-24.04', [switch]$DryRun, [scriptblock]$CommandRunner = $null)
    if ($DryRun) { return [pscustomobject]@{ Ready = $true; DryRun = $true; Distro = $Distro; Started = $false } }
    if ($null -eq $CommandRunner -and $null -eq (Get-Command docker -ErrorAction SilentlyContinue)) {
        return [pscustomobject]@{ Ready = $false; DryRun = $false; Distro = $Distro; Started = $false }
    }
    Write-SandalStep -Step 'Docker' -Message 'Probing Docker daemon readiness...' -Quiet:($null -ne $CommandRunner)
    $initialProbe = Test-SandalDockerDaemonReady -CommandRunner $CommandRunner
    if ($initialProbe['Ready']) {
        if ($initialProbe['Mode'] -ne 'docker-desktop') {
            Complete-SandalWslDockerDaemonSetup -Distro $Distro -CommandRunner $CommandRunner
        } else {
            Sync-SandalWorldImageAlias -CommandRunner $CommandRunner; Set-SandalWslComposeMountEnv
        }
        return [pscustomobject]@{ Ready = $true; DryRun = $false; Distro = $Distro; Started = $false }
    }
    if (Wait-SandalWslTcpDockerReady -MaxAttempts 1 -DelayMs 0 -CommandRunner $CommandRunner) {
        Complete-SandalWslDockerDaemonSetup -Distro $Distro -CommandRunner $CommandRunner
        return [pscustomobject]@{ Ready = $true; DryRun = $false; Distro = $Distro; Started = $false }
    }
    if (Start-SandalDockerDesktopIfInstalled -CommandRunner $CommandRunner) {
        Sync-SandalWorldImageAlias -CommandRunner $CommandRunner; Set-SandalWslComposeMountEnv
        return [pscustomobject]@{ Ready = $true; DryRun = $false; Distro = $Distro; Started = $true }
    }
    if ($IsWindows -and ($null -ne $CommandRunner -or $null -ne (Get-Command wsl.exe -ErrorAction SilentlyContinue))) {
        return (Invoke-SandalWslDockerBootstrap -Distro $Distro -CommandRunner $CommandRunner)
    }
    $finalExit = Invoke-SandalDaemonCommand -FilePath 'docker' -ArgumentList @('info') -CommandRunner $CommandRunner
    Set-SandalWslComposeMountEnv
    return [pscustomobject]@{ Ready = ($finalExit -eq 0); DryRun = $false; Distro = $Distro; Started = $false }
}

function Ensure-SandalGameData {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepoRoot, [string]$GameRoot = '', [switch]$CheckOnly)
    $contentDb = Join-Path $RepoRoot 'dist\content\content.db'
    Write-SandalStep -Step 'GameData' -Message "Checking game data database ($contentDb)..."
    if ((Test-Path -LiteralPath $contentDb -PathType Leaf) -and ((Get-Item -LiteralPath $contentDb).Length -gt 0)) {
        return [pscustomobject]@{ Ready = $true; Extracted = $false; ContentDb = $contentDb }
    }
    $extractScript = Join-Path $RepoRoot 'scripts\extract_game_data.ps1'
    if (-not (Test-Path -LiteralPath $extractScript -PathType Leaf)) {
        return [pscustomobject]@{ Ready = $false; Extracted = $false; ContentDb = $contentDb }
    }
    $extractArgs = if (-not [string]::IsNullOrWhiteSpace($GameRoot)) { @('-GameRoot', $GameRoot) } else { @() }
    $clientAvailable = $false
    if ($CheckOnly) {
        try { $null = & $extractScript -CheckOnly @extractArgs *>$null; $clientAvailable = ($LASTEXITCODE -eq 0) } catch { $clientAvailable = $false }
    }
    if ($clientAvailable -or (-not $CheckOnly)) {
        Write-SandalStep -Step 'GameData' -Message 'Extracting game data from Ashes client pak files...'
        & $extractScript @extractArgs
        if ($LASTEXITCODE -ne 0) { throw "Game data extraction failed (exit code $LASTEXITCODE)." }
    }
    $ready = (Test-Path -LiteralPath $contentDb -PathType Leaf) -and ((Get-Item -LiteralPath $contentDb).Length -gt 0)
    return [pscustomobject]@{ Ready = $ready; Extracted = $ready; ContentDb = $contentDb }
}

Export-ModuleMember -Function @(
    'Write-SandalStep', 'New-SandalCsprngSecret', 'Get-SandalEnvMap', 'Set-SandalSecretAcl',
    'Initialize-SandalEnv', 'Sync-SandalDistStage', 'Sync-SandalDeployStage', 'Test-SandalDockerDaemonReady',
    'Start-SandalDockerDesktopIfInstalled', 'Set-SandalWslComposeMountEnv', 'Start-SandalWslKeepAlive',
    'Start-SandalIpv6DockerBridge', 'Install-SandalPowerShellDockerAutoWake', 'Complete-SandalWslDockerDaemonSetup',
    'Ensure-SandalDockerDaemon', 'Ensure-SandalGameData'
)
