#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$inPester = ($null -ne (Get-Module -Name Pester)) -and (
    @(Get-PSCallStack | Where-Object {
        ($null -ne $_.Command -and $_.Command -match 'Pester') -or
        ($null -ne $_.ScriptName -and $_.ScriptName -match 'Pester')
    }).Count -gt 0
)

if (-not $inPester) {
    function Describe([string]$Name, [scriptblock]$Fixture) {
        Write-Host "[Describe] $Name"
        & $Fixture
    }
    function Context([string]$Name, [scriptblock]$Fixture) {
        Write-Host "  [Context] $Name"
        & $Fixture
    }
    function BeforeAll([scriptblock]$Action) {
        & $Action
    }
    function It([string]$Name, [scriptblock]$Test) {
        & $Test
        Write-Host "    [PASS] $Name"
    }
}

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:SandalCli = Join-Path $script:RepoRoot 'sandal.ps1'
$script:WslInstallScript = Join-Path $script:RepoRoot 'scripts\sandal\Install-WslDocker.ps1'
$script:HeavyLockScript = Join-Path $script:RepoRoot 'scripts\checks\heavy_lock.ps1'
$script:PublicCheckScript = Join-Path $script:RepoRoot 'scripts\checks\public_check.ps1'
$script:ManifestPath = Join-Path $script:RepoRoot 'scripts\lib\SandalCore.psd1'
Import-Module $script:ManifestPath -Force -DisableNameChecking
. $script:HeavyLockScript
. $script:PublicCheckScript

Describe 'SandalCore CLI Dispatcher, WSL Docker Preflight, Heavy Lock & Legacy Script Retirement' {
    BeforeAll {
        Import-Module $script:ManifestPath -Force -DisableNameChecking
        . $script:HeavyLockScript
        . $script:PublicCheckScript
    }

    Context 'Case 1: sandal.ps1 Command Dispatcher, Help, DryRun JSON Envelopes & Exit Codes' {
        It 'dispatches help, doctor, status, lifecycle DryRun JSON commands (Exit 0), and rejects unknown verbs (Exit 2)' {
            $helpOut = (@(& pwsh -NoProfile -File $script:SandalCli help) -join "`n")
            if ($LASTEXITCODE -ne 0 -or $helpOut -notmatch 'Usage: pwsh \./sandal\.ps1') {
                throw "Expected sandal.ps1 help to exit 0 with usage banner, got exit=$LASTEXITCODE out=$helpOut"
            }

            $docJson = (@(& pwsh -NoProfile -File $script:SandalCli doctor -DryRun -Json) -join "`n") | ConvertFrom-Json
            if ($LASTEXITCODE -ne 0 -or -not $docJson.ok -or $docJson.command -ne 'doctor' -or -not $docJson.dry_run -or $docJson.tier_count -ne 6) {
                throw "Unexpected doctor -DryRun -Json payload: $($docJson | ConvertTo-Json -Compress)"
            }
            if (@($docJson.toolchains).Count -ne 5 -or @($docJson.ports).Count -ne 5) {
                throw "Expected 5 toolchains and 5 port checks in doctor output."
            }

            foreach ($verb in @('up', 'down', 'extract', 'certs', 'build', 'test')) {
                $envOut = (@(& pwsh -NoProfile -File $script:SandalCli $verb -DryRun -Json) -join "`n") | ConvertFrom-Json
                if ($LASTEXITCODE -ne 0 -or -not $envOut.ok -or $envOut.command -ne $verb -or -not $envOut.dry_run) {
                    throw "Expected sandal.ps1 $verb -DryRun -Json to exit 0 with dry_run=true, got: $($envOut | ConvertTo-Json -Compress)"
                }
            }

            $badOut = (@(& pwsh -NoProfile -File $script:SandalCli invalid-subcommand 2>&1 | ForEach-Object { "$_" }) -join "`n")
            if ($LASTEXITCODE -ne 2 -or $badOut -notmatch 'What:.*Why:.*How-to-fix:.*\(Exit 2\)') {
                throw "Expected unknown subcommand to exit 2 with What/Why/How-to-fix diagnostic, got exit=$LASTEXITCODE out=$badOut"
            }
        }
    }

    Context 'Case 2: Install-WslDocker.ps1 DryRun Preflight & SEC-12 TCP 2375 Guard' {
        It 'returns a deterministic DryRun preflight plan without mutating host WSL/Docker state and blocks TCP 2375' {
            $planRaw = (@(& pwsh -NoProfile -File $script:WslInstallScript -DryRun -Json) -join "`n")
            if ($LASTEXITCODE -ne 0) {
                throw "Expected Install-WslDocker.ps1 -DryRun -Json to exit 0, got $LASTEXITCODE"
            }
            $plan = $planRaw | ConvertFrom-Json
            if (-not $plan.ok -or $plan.command -ne 'install-wsl-docker' -or -not $plan.dry_run) {
                throw "Unexpected Install-WslDocker DryRun envelope: $planRaw"
            }
            if ($plan.distro -ne 'Ubuntu-24.04' -or $plan.socket -ne '/var/run/docker.sock' -or $plan.tcp_2375 -ne $false) {
                throw "Expected Ubuntu-24.04 and unix socket /var/run/docker.sock with tcp_2375=false."
            }
            if ($plan.wsl_config.Action -ne 'DryRun' -or $plan.engine.Action -ne 'DryRun') {
                throw "Expected wsl_config.Action and engine.Action to be DryRun, got wsl=$($plan.wsl_config.Action) engine=$($plan.engine.Action)"
            }

            $threwTcp = $false
            try {
                & $script:WslInstallScript -DryRun -Json -Arguments @('-H', 'tcp://127.0.0.1:2375') *>$null
            } catch {
                if ($_.Exception.Message -match 'SEC-12') {
                    $threwTcp = $true
                }
            }
            if (-not $threwTcp) {
                throw 'Expected Install-WslDocker.ps1 to throw SEC-12 exception when TCP 2375 endpoint is passed.'
            }
        }
    }

    Context 'Case 3: heavy_lock.ps1 Mutex Acquisition, Inner Return Value & Contention Timeout' {
        It 'acquires mutex, returns inner scriptblock result, and throws deterministically when contended' {
            $mutexName = 'Global\SandalCore_TestLock_' + [guid]::NewGuid().ToString('N')
            $lockFile = Join-Path $script:RepoRoot 'data\.stamps\heavy_lock.json'
            $result = Invoke-WithHeavyLock -Operation 'test-heavy-lock' -MutexName $mutexName -NoWait -Action {
                if (-not (Test-Path -LiteralPath $lockFile)) {
                    throw 'Expected heavy_lock.json to exist while lock is held.'
                }
                'lock-acquired-ok'
            }
            if ($result -ne 'lock-acquired-ok') {
                throw "Expected inner scriptblock return value 'lock-acquired-ok', got '$result'"
            }

            if (-not ('SandalCoreLockContentionHelper' -as [type])) {
                Add-Type -TypeDefinition @'
using System.Threading;
public static class SandalCoreLockContentionHelper {
    public static Thread HoldMutex(string name, ManualResetEventSlim acquired, ManualResetEventSlim release) {
        var thread = new Thread(() => {
            using (var mutex = new Mutex(false, name)) {
                mutex.WaitOne(5000);
                acquired.Set();
                release.Wait(5000);
                mutex.ReleaseMutex();
            }
        }) { IsBackground = true };
        thread.Start();
        return thread;
    }
}
'@
            }

            $acquiredEvent = [System.Threading.ManualResetEventSlim]::new($false)
            $releaseEvent = [System.Threading.ManualResetEventSlim]::new($false)
            $holder = [SandalCoreLockContentionHelper]::HoldMutex($mutexName, $acquiredEvent, $releaseEvent)
            $null = $acquiredEvent.Wait(5000)
            try {
                $noWaitThrew = $false
                try {
                    Invoke-WithHeavyLock -Operation 'contended-nowait' -MutexName $mutexName -NoWait -Action { 'unreachable' }
                } catch {
                    $noWaitThrew = ($_.Exception.Message -match 'Heavy lock held|held by another process')
                }
                if (-not $noWaitThrew) {
                    throw 'Expected Invoke-WithHeavyLock -NoWait to throw when mutex is held by competing thread.'
                }

                $timeoutThrew = $false
                try {
                    Invoke-WithHeavyOperationLock -Operation 'contended-timeout' -MutexName $mutexName -TimeoutMs 50 -ScriptBlock { 'unreachable' }
                } catch [System.TimeoutException] {
                    $timeoutThrew = $true
                }
                if (-not $timeoutThrew) {
                    throw 'Expected Invoke-WithHeavyOperationLock to throw TimeoutException on contention.'
                }
            } finally {
                $releaseEvent.Set()
                $null = $holder.Join(5000)
                $acquiredEvent.Dispose()
                $releaseEvent.Dispose()
            }
        }
    }

    Context 'Case 4: Test-SandalRetiredLegacyScripts Positive & Negative-Space Fixture' {
        It 'records 0 violations on a clean root and all 4 violations when retired scripts exist' {
            $tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ('sandal-retired-' + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-Item -ItemType Directory -Path (Join-Path $tmpRoot 'scripts') -Force
                $cleanViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot -Violations $cleanViolations
                if ($cleanViolations.Count -ne 0) {
                    throw "Expected 0 violations on clean root, got $($cleanViolations.Count)"
                }

                $retiredRelPaths = @('build_launcher.bat', 'build_launcher.ps1', 'scripts/save_run_logs.bat', 'scripts/save_run_logs.ps1')
                foreach ($rel in $retiredRelPaths) {
                    Set-Content -LiteralPath (Join-Path $tmpRoot $rel) -Value 'rem retired' -Encoding utf8
                }

                $explicitViolations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot -Violations $explicitViolations
                if ($explicitViolations.Count -ne 4) {
                    throw "Expected 4 violations for retired scripts, got $($explicitViolations.Count): $($explicitViolations -join '; ')"
                }

                $violations = [System.Collections.Generic.List[string]]::new()
                Test-SandalRetiredLegacyScripts -Root $tmpRoot
                if ($violations.Count -ne 4) {
                    throw "Expected 4 violations via caller-scope fallback, got $($violations.Count)"
                }
            } finally {
                if (Test-Path -LiteralPath $tmpRoot) {
                    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }
    }
}
