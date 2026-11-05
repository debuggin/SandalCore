#Requires -Version 7.2
# SPDX-License-Identifier: AGPL-3.0-only
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inPester = ($null -ne (Get-Module -Name Pester)) -and (
    @(Get-PSCallStack | Where-Object {
        ($null -ne $_.Command -and $_.Command -match 'Pester') -or
        ($null -ne $_.ScriptName -and $_.ScriptName -match 'Pester')
    }).Count -gt 0
)

if (-not $inPester) {
    function Describe([string]$Name, [scriptblock]$Fixture) { Write-Host "[Describe] $Name"; & $Fixture }
    function Context([string]$Name, [scriptblock]$Fixture) { Write-Host "  [Context] $Name"; & $Fixture }
    function BeforeAll([scriptblock]$Action) { & $Action }
    function It([string]$Name, [scriptblock]$Test) { & $Test; Write-Host "    [PASS] $Name" }
}

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$script:JourneyScript = Join-Path $script:RepoRoot 'scripts/e2e_user_journey.ps1'
$script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
$script:DefaultReportPath = Join-Path $script:RepoRoot 'dist/logs/e2e_journey_latest.json'

Describe '6-Phase E2E Operator Lifecycle Journey (start.bat -> launcher -> game -> login -> close -> stop.bat)' {
    BeforeAll {
        $script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
        $script:JourneyScript = Join-Path $script:RepoRoot 'scripts/e2e_user_journey.ps1'
        $script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
        $script:DefaultReportPath = Join-Path $script:RepoRoot 'dist/logs/e2e_journey_latest.json'
    }

    Context 'CLI Wiring for sandal run -SpawnLauncher -AutomationPort and sandal down' {
        It 'wires -SpawnLauncher, -NoLauncher, default interactive spawn, and step-by-step progress output' {
            $humanOut = (& pwsh -NoProfile -File $script:SandalCli run -DryRun) -join "`n"
            if ($LASTEXITCODE -ne 0) {
                throw "Expected sandal.ps1 run -DryRun to exit 0, got $LASTEXITCODE"
            }
            if ($humanOut -notmatch '\[SandalCore\]\s*\[Step 1/6\]' -or $humanOut -notmatch '\[SandalCore\]\s*\[Step 6/6\].*spawn=True') {
                throw "Expected step-by-step progress lines [Step 1/6]..[Step 6/6] with default spawn=True, got:`n$humanOut"
            }

            $noLaunchRaw = (& pwsh -NoProfile -File $script:SandalCli run -NoLauncher -DryRun -Json) -join "`n"
            $noLaunchObj = $noLaunchRaw | ConvertFrom-Json
            if ($noLaunchObj.spawn_launcher) {
                throw "Expected spawn_launcher=false when -NoLauncher is specified, got: $noLaunchRaw"
            }

            $runRaw = (& pwsh -NoProfile -File $script:SandalCli run -SpawnLauncher -AutomationPort 19876 -DryRun -Json) -join "`n"
            if ($LASTEXITCODE -ne 0) {
                throw "Expected sandal.ps1 run -DryRun -Json to exit 0, got $LASTEXITCODE"
            }
            $runObj = $runRaw | ConvertFrom-Json
            if (-not $runObj.ok -or -not $runObj.spawn_launcher -or [int]$runObj.automation_port -ne 19876) {
                throw "Expected spawn_launcher=true and automation_port=19876, got: $runRaw"
            }

            $downRaw = (& pwsh -NoProfile -File $script:SandalCli down -DryRun -Json) -join "`n"
            if ($LASTEXITCODE -ne 0) {
                throw "Expected sandal.ps1 down -DryRun to exit 0, got $LASTEXITCODE"
            }
            $downObj = $downRaw | ConvertFrom-Json
            if (-not $downObj.ok -or $downObj.command -ne 'down') {
                throw "Expected down receipt ok=true, got: $downRaw"
            }
        }
    }

    Context 'Full 6-Phase E2E Journey Execution & Structured Telemetry Report' {
        It 'executes all 6 lifecycle phases in order and writes dist/logs/e2e_journey_latest.json' {
            $outRaw = (& pwsh -NoProfile -File $script:JourneyScript -Hermetic -IncludeDiagnostics -Json) -join "`n"
            if ($LASTEXITCODE -ne 0) {
                throw "Expected e2e_user_journey.ps1 -Hermetic to exit 0, got $LASTEXITCODE`n$outRaw"
            }
            if (-not (Test-Path -LiteralPath $script:DefaultReportPath -PathType Leaf)) {
                throw "Expected report file at $($script:DefaultReportPath)"
            }
            $report = (Get-Content -LiteralPath $script:DefaultReportPath -Raw) | ConvertFrom-Json
            if (-not $report.ok -or [int]$report.phase_count -ne 6) {
                throw "Expected report.ok=true and phase_count=6, got ok=$($report.ok) count=$($report.phase_count)"
            }

            $expectedPhases = @(
                'launcher_loaded',
                'game_started',
                'game_logged_in',
                'game_closed',
                'launcher_closed',
                'click_stop'
            )
            for ($i = 0; $i -lt $expectedPhases.Count; $i++) {
                $p = $report.phases[$i]
                if ($p.phase -ne $expectedPhases[$i] -or $p.status -ne 'PASS') {
                    throw "Phase index $i expected $($expectedPhases[$i])=PASS, got $($p.phase)=$($p.status)"
                }
            }

            if ([int]$report.phases[0].observed_state.launcherPid -le 0) {
                throw "Phase 1 expected launcherPid > 0"
            }
            if (-not $report.phases[1].observed_state.ashesProcessRunning -or [int]$report.phases[1].observed_state.ashesPid -le 0) {
                throw "Phase 2 expected ashesProcessRunning=true and ashesPid > 0"
            }
            $p3 = $report.phases[2].observed_state
            if (-not $p3.isAuthenticated -or $p3.selectedCharacterId -notmatch '^[0-9a-fA-F]{32}$' -or $p3.lastJoinTicket -notmatch '^[0-9a-fA-F]{32}$' -or $p3.worldEndpoint -notin @('127.0.0.1:7777', 'world:7777')) {
                throw "Phase 3 login/ticket assertions failed: $($p3 | ConvertTo-Json -Compress)"
            }
            if ($report.phases[3].observed_state.ashesProcessRunning -or -not $report.phases[3].observed_state.osExited) {
                throw "Phase 4 expected ashesProcessRunning=false and osExited=true"
            }
            if (-not $report.phases[4].observed_state.launcherExited) {
                throw "Phase 5 expected launcherExited=true"
            }
            if (-not $report.phases[5].observed_state.stopCommandOk -or [int]$report.phases[5].observed_state.orphanProcesses -ne 0) {
                throw "Phase 6 expected stopCommandOk=true and orphanProcesses=0"
            }
        }
    }

    Context 'Failure Diagnostics Collection on Phase Fault' {
        It 'captures rich diagnostic telemetry when a phase fails' {
            $tmpReport = Join-Path ([System.IO.Path]::GetTempPath()) ('e2e-fail-' + [guid]::NewGuid().ToString('N') + '.json')
            try {
                $failRaw = (& pwsh -NoProfile -File $script:JourneyScript -Hermetic -SimulateFailurePhase 'game_logged_in' -ReportPath $tmpReport -Json) -join "`n"
                $exitCode = $LASTEXITCODE
                if ($exitCode -eq 0) {
                    throw "Expected non-zero exit code on simulated failure, got 0"
                }
                $failReport = $failRaw | ConvertFrom-Json
                if ($failReport.ok -or $failReport.failed_phase -ne 'game_logged_in') {
                    throw "Expected ok=false and failed_phase='game_logged_in', got ok=$($failReport.ok) failed_phase=$($failReport.failed_phase)"
                }
                if ($null -eq $failReport.diagnostics -or $null -eq $failReport.diagnostics.listening_ports -or $null -eq $failReport.diagnostics.http_responses) {
                    throw "Expected diagnostics bundle with listening_ports and http_responses on failure"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpReport) {
                    Remove-Item -LiteralPath $tmpReport -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}
