#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-SandalStageGraph {
    [CmdletBinding()]
    param()
    $schema = @(
        'src/tools/schemagen/gen_zoi_surface.ps1', 'src/tools/schemagen/gen_region_rects.ps1',
        'src/tools/schemagen/gen_region.ps1', 'src/tools/schemagen/gen_gm_garden.ps1',
        'src/tools/schemagen/gen_tooltips.ps1', 'src/tools/schemagen/gen_gear_fixture.ps1',
        'src/tools/schemagen/gen_talent_fixture.ps1'
    )
    return [ordered]@{
        'config'       = @{ Upstream = @(); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc') }
        'curves'       = @{ Upstream = @(); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc') }
        'names'        = @{ Upstream = @(); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc', 'src/tools/itemnames') }
        'database'     = @{ Upstream = @('config', 'curves', 'names'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/itemnames', 'src/server/services/cmd/contentc', 'src/server/services/internal/content', 'src/server/services/internal/designdb', 'src/server/services/internal/usmap', 'src/server/services/internal/aocexpr', 'src/server/services/internal/chardef', 'src/server/services/internal/statprog', 'src/server/services/go.mod', 'src/server/services/go.sum') }
        'region'       = @{ Upstream = @('database'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc') + $schema }
        'headers'      = @{ Upstream = @('config', 'region'); Inputs = $schema }
        'fixtures'     = @{ Upstream = @('database'); Inputs = @('src/tools/schemagen/gen_talent_fixture.ps1', 'src/tools/schemagen/gen_gear_fixture.ps1', 'src/tools/schemagen/gen_tooltips.ps1', 'src/server/services/internal/statprog') }
        'navbuild'     = @{ Upstream = @('headers'); Inputs = @('src/libs/aocnav', 'src/tools/navbuild', 'build/third_party/recastnavigation', 'src/vcpkg.json') }
        'roads'        = @{ Upstream = @('region'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc', 'src/server/services/cmd/contentc', 'src/server/services/internal/content') }
        'townfloors'   = @{ Upstream = @(); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc') }
        'townlayouts'  = @{ Upstream = @('database'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc', 'src/tools/schemagen/gen_town_layouts.ps1', 'src/config/seeds/citynode_seed.json') }
        'npcabilities' = @{ Upstream = @('database'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc', 'src/tools/npcabilities') }
        'navmesh'      = @{ Upstream = @('region', 'navbuild', 'townfloors', 'townlayouts', 'database'); Inputs = @('src/config/layouts/aoc.usmap', 'src/tools/mapc', 'scripts/nav_inputs.ps1') }
    }
}

function Get-SandalStageOrder {
    [CmdletBinding()]
    param([switch]$All)
    $keys = [string[]]@(Get-SandalStageGraph).Keys
    if ($All) { return $keys }
    return [string[]]@($keys | Where-Object { $_ -notin @('names', 'npcabilities') })
}

function Test-StageInputFileIncluded {
    param([System.IO.FileInfo]$File)
    if ($File.FullName -match '[\\/](bin|obj|__pycache__|\.git)[\\/]') { return $false }
    return ($File.Name -notlike '*_test.go' -and $File.Name -notlike '*.md' -and $File.Name -notlike '*.pyc' -and $File.Name -notin @('CMakeLists.txt', 'region_rects.tsv'))
}

function Add-StageInputHashes {
    param([string]$RepoRoot, [string]$RelPath, [System.Collections.Generic.List[string]]$Records)
    $target = Join-Path $RepoRoot ($RelPath -replace '/', [System.IO.Path]::DirectorySeparatorChar)
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $Records.Add("FILE:${RelPath}=$((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant())")
        return
    }
    if (-not (Test-Path -LiteralPath $target -PathType Container)) {
        $Records.Add("MISSING:${RelPath}")
        return
    }
    $files = Get-ChildItem -LiteralPath $target -Recurse -File | Where-Object { Test-StageInputFileIncluded -File $_ }
    foreach ($f in ($files | Sort-Object FullName)) {
        $rel = [System.IO.Path]::GetRelativePath($RepoRoot, $f.FullName).Replace('\', '/')
        $Records.Add("FILE:${rel}=$((Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant())")
    }
}

function Compute-SandalStageHash {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$RepoRoot, [hashtable]$Salt = @{})
    $graph = Get-SandalStageGraph
    if (-not $graph.Contains($Stage)) { throw [System.ArgumentException]::new("Unknown SandalCore stage '$Stage'.") }
    $node = $graph[$Stage]
    $records = [System.Collections.Generic.List[string]]::new()
    foreach ($dep in $node.Upstream) {
        $records.Add("UPSTREAM:${dep}=$(Compute-SandalStageHash -Stage $dep -RepoRoot $RepoRoot -Salt $Salt)")
    }
    foreach ($relPath in ($node.Inputs | Sort-Object -Unique)) {
        Add-StageInputHashes -RepoRoot $RepoRoot -RelPath $relPath -Records $records
    }
    foreach ($key in ($Salt.Keys | Sort-Object)) { $records.Add("SALT:${key}=$($Salt[$key])") }
    $payload = [System.Text.Encoding]::UTF8.GetBytes("STAGE:${Stage}`n" + ($records -join "`n"))
    return [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($payload)).ToLowerInvariant()
}

function Read-SandalStageStamp {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$RepoRoot)
    $jsonPath = Join-Path $RepoRoot "dist/.stamps/${Stage}.json"
    if (-not (Test-Path -LiteralPath $jsonPath -PathType Leaf)) { return $null }
    return ([System.IO.File]::ReadAllText($jsonPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json)
}

function Write-SandalStageStamp {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$RepoRoot, [string]$InputHash = '')
    if ([string]::IsNullOrWhiteSpace($InputHash)) { $InputHash = Compute-SandalStageHash -Stage $Stage -RepoRoot $RepoRoot }
    $stampDir = Join-Path $RepoRoot 'dist/.stamps'
    if (-not (Test-Path -LiteralPath $stampDir)) { $null = New-Item -ItemType Directory -Path $stampDir -Force }
    $stamp = [pscustomobject]@{ stage = $Stage; input_hash = $InputHash; updated_at = [DateTime]::UtcNow.ToString('o') }
    [System.IO.File]::WriteAllText((Join-Path $stampDir "${Stage}.json"), ($stamp | ConvertTo-Json -Compress), [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText((Join-Path $stampDir "${Stage}.sha256"), "$InputHash`n", [System.Text.UTF8Encoding]::new($false))
    return $stamp
}

function Test-SandalStageUpToDate {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$Stage, [Parameter(Mandatory)][string]$RepoRoot)
    $stamp = Read-SandalStageStamp -Stage $Stage -RepoRoot $RepoRoot
    if ($null -eq $stamp) { return $false }
    return [bool]($stamp.input_hash -eq (Compute-SandalStageHash -Stage $Stage -RepoRoot $RepoRoot))
}

function Open-HeavyMutex {
    param([string]$MutexName)
    try { return [System.Threading.Mutex]::new($false, $MutexName) }
    catch { return [System.Threading.Mutex]::new($false, ($MutexName -replace '^Global\\', 'Local\')) }
}

function Invoke-WithHeavyOperationLock {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Operation,
        [Parameter(Mandatory)][scriptblock]$ScriptBlock,
        [int]$TimeoutMs = 1800000,
        [string]$MutexName = 'Global\SandalCore_HeavyOperation_Lock',
        [string]$LockFilePath = ''
    )
    $mutex = Open-HeavyMutex -MutexName $MutexName
    $acquired = $false
    try {
        try { $acquired = $mutex.WaitOne($TimeoutMs) }
        catch [System.Threading.AbandonedMutexException] { $acquired = $true }
        if (-not $acquired) { throw [System.TimeoutException]::new("Timed out after ${TimeoutMs}ms waiting for mutex '$MutexName' ($Operation).") }
        if ($LockFilePath) {
            $meta = @{ pid = $PID; operation = $Operation; acquired_at = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Compress
            [System.IO.File]::WriteAllText($LockFilePath, $meta, [System.Text.UTF8Encoding]::new($false))
        }
        return (& $ScriptBlock)
    } finally {
        if ($acquired) {
            if ($LockFilePath -and (Test-Path -LiteralPath $LockFilePath)) { Remove-Item -LiteralPath $LockFilePath -Force -ErrorAction SilentlyContinue }
            try { $mutex.ReleaseMutex() } catch {}
        }
        $mutex.Dispose()
    }
}

Export-ModuleMember -Function @(
    'Get-SandalStageGraph', 'Get-SandalStageOrder', 'Compute-SandalStageHash',
    'Read-SandalStageStamp', 'Write-SandalStageStamp', 'Test-SandalStageUpToDate',
    'Invoke-WithHeavyOperationLock', 'New-SandalCsprngSecret', 'Get-SandalEnvMap',
    'Set-SandalSecretAcl', 'Initialize-SandalEnv', 'Sync-SandalDistStage',
    'Sync-SandalDeployStage', 'Ensure-SandalGameData'
)
