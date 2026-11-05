#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = $PSScriptRoot
while ($repoRoot -and -not (Test-Path -LiteralPath (Join-Path $repoRoot 'sandal.ps1'))) {
    $parent = Split-Path -Parent $repoRoot
    if ($parent -eq $repoRoot) { break }
    $repoRoot = $parent
}
$wslConfigLib = Join-Path $repoRoot 'scripts/lib/wsl-config.ps1'
if (Test-Path $wslConfigLib) {
    . $wslConfigLib
}

if ($null -ne (Get-Module Pester) -and (Get-Module Pester).Version.Major -lt 5) {
    InModuleScope Pester {
        function script:Get-DoMessagesMatch($value, $expected) {
            if ($null -eq $expected) { return $true }
            if ($expected -eq '') { return $false }
            return $value.IndexOf([string]$expected, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        }
        function script:Parse-ShouldArgs([array]$shouldArgs) {
            if ($null -eq $shouldArgs) { $shouldArgs = @() }
            [array]$shouldArgs = @($shouldArgs | ForEach-Object {
                if ($_ -is [string] -and $_ -match '^-(Not|Be|BeExactly|BeGreaterThan|BeLessThan|BeLike|BeLikeExactly|BeNullOrEmpty|BeOfType|Contain|ContainExactly|Exist|Match|MatchExactly|Throw)$') {
                    $Matches[1]
                } else {
                    $_
                }
            })
            $parsedArgs = @{ PositiveAssertion = $true; ExpectedValue = $null }
            $assertionMethodIndex = 0
            $expectedValueIndex = 1
            if ($shouldArgs.Count -gt 0 -and "$($shouldArgs[0])".ToLower() -eq 'not') {
                $parsedArgs.PositiveAssertion = $false
                $assertionMethodIndex += 1
                $expectedValueIndex += 1
            }
            if ($assertionMethodIndex -lt $shouldArgs.Count) {
                $parsedArgs.AssertionMethod = "Pester$($shouldArgs[$assertionMethodIndex])"
            } else {
                throw 'You cannot call Should without specifying an assertion method.'
            }
            if ($expectedValueIndex -lt $shouldArgs.Count) {
                $parsedArgs.ExpectedValue = $shouldArgs[$expectedValueIndex]
            }
            return $parsedArgs
        }
    }
}

Describe 'WSL2 .wslconfig Schema & Windows TCP 127.0.0.1:2375 Auto-Wake (SandalCore-27)' {
    BeforeAll {
        $repoRoot = $PSScriptRoot
        while ($repoRoot -and -not (Test-Path -LiteralPath (Join-Path $repoRoot 'sandal.ps1'))) {
            $parent = Split-Path -Parent $repoRoot
            if ($parent -eq $repoRoot) { break }
            $repoRoot = $parent
        }
        $cfgPath = Join-Path $repoRoot 'scripts/lib/wsl-config.ps1'
        if (Test-Path $cfgPath) {
            . $cfgPath
        }
        Import-Module (Join-Path $repoRoot 'scripts/lib/Env.psm1') -Force -DisableNameChecking
    }

    Context 'Confirm-WslConfig [experimental] hostAddressLoopback placement & stale [wsl2] key removal' {
        It 'writes hostAddressLoopback=true under [experimental] and not [wsl2] on new config' {
            $tmpFile = Join-Path ([System.IO.Path]::GetTempPath()) "wslconfig-exp-$([guid]::NewGuid()).ini"
            try {
                Set-Content -LiteralPath $tmpFile -Value "[wsl2]`nmemory=16GB" -Encoding utf8
                $res = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader { return '' }
                $res.Changed | Should -Be $true
                $res.InstanceIdleTimeout | Should -Be '-1'
                $res.NetworkingMode | Should -Be 'mirrored'
                $res.HostAddressLoopback | Should -Be 'true'
                $res.VmIdleTimeout | Should -Be '-1'
                $state = Get-WslSectionState -ConfigPath $tmpFile
                $state.Sections['experimental']['hostAddressLoopback'] | Should -Be 'true'
                $state.Sections['wsl2'].ContainsKey('hostAddressLoopback') | Should -Be $false
            } finally {
                if (Test-Path -LiteralPath $tmpFile) { Remove-Item -LiteralPath $tmpFile -Force }
            }
        }

        It 'migrates stale [wsl2] hostAddressLoopback=true to [experimental] and preserves no-op on subsequent run' {
            $tmpFile = Join-Path ([System.IO.Path]::GetTempPath()) "wslconfig-migrate-$([guid]::NewGuid()).ini"
            try {
                $stale = "[general]`ninstanceIdleTimeout=-1`n`n[wsl2]`nmemory=12GB`nnetworkingMode=mirrored`nhostAddressLoopback=true`nvmIdleTimeout=-1`n"
                [System.IO.File]::WriteAllText($tmpFile, $stale, [System.Text.UTF8Encoding]::new($false))
                $res1 = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader { return '' }
                $res1.Changed | Should -Be $true
                $stateAfter = Get-WslSectionState -ConfigPath $tmpFile
                $stateAfter.Sections['experimental']['hostAddressLoopback'] | Should -Be 'true'
                $stateAfter.Sections['wsl2'].ContainsKey('hostAddressLoopback') | Should -Be $false

                $res2 = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader { return '' }
                $res2.Changed | Should -Be $false
            } finally {
                if (Test-Path -LiteralPath $tmpFile) { Remove-Item -LiteralPath $tmpFile -Force }
            }
        }
    }

    Context 'Windows TCP 127.0.0.1:2375 auto-healing & PowerShell auto-wake profile block' {
        It 'rewrites stale tcp://[::1]:2375 to tcp://127.0.0.1:2375 in Test-SandalDockerDaemonReady and writes idempotent profile block' {
            $tmpProfile = Join-Path ([System.IO.Path]::GetTempPath()) "profile-autowake-$([guid]::NewGuid()).ps1"
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $probe = Test-SandalDockerDaemonReady -CommandRunner { return 0 }
                $probe['Ready'] | Should -Be $true
                $env:DOCKER_HOST | Should -Be 'tcp://127.0.0.1:2375'

                Install-SandalPowerShellDockerAutoWake -Distro 'Ubuntu' -ProfilePath $tmpProfile
                $raw1 = Get-Content -LiteralPath $tmpProfile -Raw
                $raw1 | Should -Match '# >>> sandal-docker-autowake >>>'
                $raw1 | Should -Match 'function global:docker'
                $raw1 | Should -Match 'tcp://127\.0\.0\.1:2375'

                Install-SandalPowerShellDockerAutoWake -Distro 'Ubuntu' -ProfilePath $tmpProfile
                $raw2 = Get-Content -LiteralPath $tmpProfile -Raw
                ([regex]::Matches($raw2, '# >>> sandal-docker-autowake >>>').Count) | Should -Be 1
            } finally {
                if (Test-Path -LiteralPath $tmpProfile) { Remove-Item -LiteralPath $tmpProfile -Force }
                if ($null -eq $prevHost) { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }
                else { $env:DOCKER_HOST = $prevHost }
            }
        }

        It 'exports Start-SandalIpv6DockerBridge and Complete-SandalWslDockerDaemonSetup and completes fast-path setup in Ensure-SandalDockerDaemon' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                (Get-Command Start-SandalIpv6DockerBridge -ErrorAction SilentlyContinue) | Should -Not -BeNullOrEmpty
                (Get-Command Complete-SandalWslDockerDaemonSetup -ErrorAction SilentlyContinue) | Should -Not -BeNullOrEmpty
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $res = Ensure-SandalDockerDaemon -Distro 'Ubuntu' -CommandRunner { return 0 }
                $res.Ready | Should -Be $true
                $env:DOCKER_HOST | Should -Be 'tcp://127.0.0.1:2375'
            } finally {
                if ($null -eq $prevHost) { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }
                else { $env:DOCKER_HOST = $prevHost }
            }
        }
    }
}
