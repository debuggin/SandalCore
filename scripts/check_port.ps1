#Requires -Version 7.0
param(
    [Parameter(Mandatory = $true)] [string]$Protocol,
    [Parameter(Mandatory = $true)] [int]$Port,
    [Parameter(Mandatory = $true)] [string]$ServiceName,
    [Parameter(Mandatory = $true)] [int]$TimeoutSeconds,
    [Parameter(Mandatory = $true)] [string]$RepoRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $RepoRoot.TrimEnd('\') + '\'
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)

Write-Host "Checking $Protocol port $Port for service '$ServiceName'..." -ForegroundColor Cyan

while ((Get-Date) -lt $deadline) {
    if ($Protocol -eq 'UDP') {
        $endpoints = @(Get-NetUDPEndpoint -LocalPort $Port -ErrorAction SilentlyContinue)
    } else {
        $endpoints = @(Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue)
    }

    $pids = @($endpoints | Where-Object { $null -ne $_ } | Select-Object -ExpandProperty OwningProcess -Unique)

    if ($pids.Count -gt 0) {
        foreach ($pidToTest in $pids) {
            $proc = Get-Process -Id $pidToTest -ErrorAction SilentlyContinue
            if (-not $proc) { continue }

            $pName = $proc.ProcessName
            Write-Host "  -> Port $Port detected on PID $pidToTest ($pName)" -ForegroundColor Gray

            if ($pName -eq $ServiceName) {
                Write-Host "  [SUCCESS] Port $Port bound by $ServiceName (PID $pidToTest)" -ForegroundColor Green
                exit 0
            }

            $wmiProc = Get-CimInstance Win32_Process -Filter "ProcessId = $pidToTest" -ErrorAction SilentlyContinue
            if ($wmiProc -and ($wmiProc.CommandLine -like "*$ServiceName.exe*" -or $wmiProc.ExecutablePath -like "*$ServiceName.exe*")) {
                Write-Host "  [SUCCESS] Port $Port bound by $ServiceName via WMI wrapper (PID $pidToTest)" -ForegroundColor Green
                exit 0
            }

            if ($pName -eq 'cmd') {
                $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $pidToTest" -ErrorAction SilentlyContinue)
                foreach ($child in $children) {
                    $cProc = Get-Process -Id $child.ProcessId -ErrorAction SilentlyContinue
                    if ($cProc -and $cProc.ProcessName -eq $ServiceName) {
                        Write-Host "  [SUCCESS] Port $Port bound by child $ServiceName (PID $($child.ProcessId))" -ForegroundColor Green
                        exit 0
                    }
                }
            }

            if ($proc.Path -and $proc.Path.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) {
                Write-Host "  [SUCCESS] Port $Port is active from repository checkout ($($proc.Path))" -ForegroundColor Green
                exit 0
            }
        }
    }

    Start-Sleep -Milliseconds 500
}

Write-Host "  [ERROR] $ServiceName failed to bind $Protocol port $Port within $TimeoutSeconds seconds." -ForegroundColor Red
exit 1
