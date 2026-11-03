[CmdletBinding()]
param(
    [string]$Operation = 'build',
    [scriptblock]$Action,
    [switch]$NoWait
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-SandalHeavyLockFile {
    param([Parameter(Mandatory)][string]$LockFilePath)
    if (-not (Test-Path -LiteralPath $LockFilePath)) {
        return $null
    }
    try {
        $raw = Get-Content -LiteralPath $LockFilePath -Raw -ErrorAction Stop
        if ([string]::IsNullOrWhiteSpace($raw)) {
            return $null
        }
        return ($raw | ConvertFrom-Json -ErrorAction Stop)
    } catch {
        return $null
    }
}

function Test-SandalProcessAlive {
    param([Parameter(Mandatory)][int]$ProcessId)
    if ($ProcessId -le 0) {
        return $false
    }
    $proc = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    return ($null -ne $proc)
}

function Resolve-SandalStaleLock {
    param([Parameter(Mandatory)][string]$LockFilePath)
    $existing = Read-SandalHeavyLockFile -LockFilePath $LockFilePath
    if ($null -eq $existing) {
        return $null
    }
    $holderPid = 0
    if ($null -ne $existing.PSObject.Properties['pid']) {
        $holderPid = [int]$existing.pid
    }
    if ($holderPid -gt 0 -and -not (Test-SandalProcessAlive -ProcessId $holderPid)) {
        Remove-Item -LiteralPath $LockFilePath -Force -ErrorAction SilentlyContinue
        return $null
    }
    return $existing
}

function Write-SandalHeavyLockFile {
    param(
        [Parameter(Mandatory)][string]$LockFilePath,
        [Parameter(Mandatory)][string]$OperationName
    )
    $stampDir = Split-Path -Parent $LockFilePath
    if ($stampDir -and -not (Test-Path -LiteralPath $stampDir)) {
        $null = New-Item -ItemType Directory -Path $stampDir -Force
    }
    $payload = [ordered]@{
        pid            = $PID
        operation      = $OperationName
        started_at_utc = [datetime]::UtcNow.ToString('o')
        command_line   = [Environment]::CommandLine
    }
    $json = $payload | ConvertTo-Json -Depth 4
    Set-Content -LiteralPath $LockFilePath -Value $json -Encoding utf8 -NoNewline
}

function Format-SandalLockContention {
    param($HolderInfo)
    if ($null -eq $HolderInfo) {
        return 'Global\SandalCore_HeavyOperation_Lock is held by another process.'
    }
    $holderPid = $HolderInfo.pid
    $holderOp = $HolderInfo.operation
    $elapsedSec = 0.0
    try {
        $started = [datetime]::Parse([string]$HolderInfo.started_at_utc).ToUniversalTime()
        $elapsedSec = [math]::Round(([datetime]::UtcNow - $started).TotalSeconds, 1)
    } catch {
        $elapsedSec = 0.0
    }
    return "Heavy lock held by PID $holderPid (operation='$holderOp', elapsed=${elapsedSec}s)."
}

function Invoke-WithHeavyLock {
    [CmdletBinding()]
    param(
        [string]$Operation = 'build',
        [Parameter(Mandatory)][scriptblock]$Action,
        [switch]$NoWait,
        [string]$MutexName = 'Global\SandalCore_HeavyOperation_Lock'
    )
    $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    $lockFile = Join-Path $repoRoot 'data\.stamps\heavy_lock.json'
    $mutex = try {
        [System.Threading.Mutex]::new($false, $MutexName)
    } catch [System.UnauthorizedAccessException] {
        [System.Threading.Mutex]::new($false, ($MutexName -replace '^Global\\', 'Local\'))
    }
    $acquired = $false
    try {
        while (-not $acquired) {
            $holder = Resolve-SandalStaleLock -LockFilePath $lockFile
            $fileBlocked = ($null -ne $holder) -and ([int]$holder.pid -ne $PID)
            if (-not $fileBlocked) {
                try {
                    $acquired = $mutex.WaitOne(0)
                } catch [System.Threading.AbandonedMutexException] {
                    $acquired = $true
                }
            }
            if ($acquired) {
                $holderAfter = Resolve-SandalStaleLock -LockFilePath $lockFile
                if ($null -ne $holderAfter -and [int]$holderAfter.pid -ne $PID) {
                    try { $mutex.ReleaseMutex() } catch [System.ApplicationException] {}
                    $acquired = $false
                    $holder = $holderAfter
                }
            }
            if (-not $acquired) {
                $msg = Format-SandalLockContention -HolderInfo $holder
                if ($NoWait) {
                    throw $msg
                }
                Write-Host "[SandalCore] $msg Waiting 2.0s..."
                Start-Sleep -Milliseconds 2000
            }
        }
        Write-SandalHeavyLockFile -LockFilePath $lockFile -OperationName $Operation
        & $Action
    } finally {
        if ($acquired) {
            $current = Read-SandalHeavyLockFile -LockFilePath $lockFile
            if ($null -eq $current -or [int]$current.pid -eq $PID) {
                Remove-Item -LiteralPath $lockFile -Force -ErrorAction SilentlyContinue
            }
            try { $mutex.ReleaseMutex() } catch [System.ApplicationException] {}
        }
        $mutex.Dispose()
    }
}

if ($null -ne $Action) {
    Invoke-WithHeavyLock -Operation $Operation -Action $Action -NoWait:$NoWait
}
