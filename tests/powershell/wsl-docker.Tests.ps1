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
$wslDockerLib = Join-Path $repoRoot 'scripts/lib/wsl-docker.ps1'
if (Test-Path $wslDockerLib) {
    . $wslDockerLib
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

Describe 'WSL2 & Linux Docker Engine Module (bd-ep04-04)' {
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
        $dkrPath = Join-Path $repoRoot 'scripts/lib/wsl-docker.ps1'
        if (Test-Path $dkrPath) {
            . $dkrPath
        }
    }

    Context 'Test-WslDockerReady warm fast-path probe (UX-07)' {
        It 'returns Ready=true, versions, and ElapsedMs < 500 when daemon and compose respond' {
            $runner = {
                param([string[]]$WslArgs)
                $joined = $WslArgs -join ' '
                if ($joined -match 'docker info') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                }
                if ($joined -match 'docker compose version') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '2.29.7' }
                }
                return [pscustomobject]@{ ExitCode = 1; Output = '' }
            }
            $res = Test-WslDockerReady -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Ready | Should -Be $true
            $res.ServerVersion | Should -Be '27.3.1'
            $res.ComposeVersion | Should -Be '2.29.7'
            $res.ElapsedMs | Should -BeLessThan 500
        }

        It 'returns Ready=false when wsl runner returns non-zero ExitCode' {
            $runner = {
                param([string[]]$WslArgs)
                return [pscustomobject]@{ ExitCode = 1; Output = '' }
            }
            $res = Test-WslDockerReady -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Ready | Should -Be $false
        }
    }

    Context 'Confirm-WslConfig non-destructive keep-or-change merge (UX-08)' {
        It 'preserves existing settings and returns Changed=false on Enter-to-keep' {
            $tmpFile = Join-Path ([System.IO.Path]::GetTempPath()) "wslconfig-test-$([guid]::NewGuid()).ini"
            try {
                $initial = "[wsl2]`nmemory=12GB`nprocessors=6`nnetworkingMode=mirrored`nhostAddressLoopback=true"
                Set-Content -LiteralPath $tmpFile -Value $initial -Encoding utf8
                $reader = { param($PromptText, $Key, $CurrentValue) return '' }
                $res = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader $reader
                $res.Changed | Should -Be $false
                $res.ShutdownAdvised | Should -Be $false
                $res.NetworkingMode | Should -Be 'mirrored'
                $res.HostAddressLoopback | Should -Be 'true'
                $raw = Get-Content -LiteralPath $tmpFile -Raw
                $raw | Should -Match 'memory=12GB'
                $raw | Should -Match 'processors=6'
            } finally {
                if (Test-Path -LiteralPath $tmpFile) {
                    Remove-Item -LiteralPath $tmpFile -Force
                }
            }
        }

        It 'merges missing keys non-destructively while preserving hardware limits' {
            $tmpFile = Join-Path ([System.IO.Path]::GetTempPath()) "wslconfig-test-$([guid]::NewGuid()).ini"
            try {
                Set-Content -LiteralPath $tmpFile -Value "[wsl2]`nmemory=16GB" -Encoding utf8
                $reader = {
                    param($PromptText, $Key, $CurrentValue)
                    if ($Key -eq 'networkingMode') { return 'nat' }
                    return ''
                }
                $res = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader $reader
                $res.Changed | Should -Be $true
                $res.ShutdownAdvised | Should -Be $true
                $res.NetworkingMode | Should -Be 'nat'
                $res.HostAddressLoopback | Should -Be 'true'
                $raw = Get-Content -LiteralPath $tmpFile -Raw
                $raw | Should -Match 'memory=16GB'
                $raw | Should -Match 'networkingMode=nat'
                $raw | Should -Match 'hostAddressLoopback=true'
            } finally {
                if (Test-Path -LiteralPath $tmpFile) {
                    Remove-Item -LiteralPath $tmpFile -Force
                }
            }
        }
    }

    Context 'Ensure-WslUbuntu and Ensure-LinuxDockerEngine idempotent reuse (UX-07, INFRA-02)' {
        It 'reuses existing Ubuntu-24.04 distro without reinstalling' {
            $runner = {
                param([string[]]$WslArgs)
                return [pscustomobject]@{ ExitCode = 0; Output = "Ubuntu-24.04`n" }
            }
            $res = Ensure-WslUbuntu -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Installed | Should -Be $true
            $res.Action | Should -Be 'Reused'
        }

        It 'reuses running Linux Docker Engine without re-provisioning' {
            $runner = {
                param([string[]]$WslArgs)
                $joined = $WslArgs -join ' '
                if ($joined -match 'docker info') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                }
                if ($joined -match 'docker compose version') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '2.29.7' }
                }
                return [pscustomobject]@{ ExitCode = 0; Output = '' }
            }
            $res = Ensure-LinuxDockerEngine -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Ready | Should -Be $true
            $res.Action | Should -Be 'Reused'
            $res.Provisioned | Should -Be $false
        }
    }

    Context 'Invoke-WslDocker Unix socket enforcement and TCP 2375 rejection (SEC-12)' {
        It 'throws when -Arguments contains prohibited TCP 2375 endpoint' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                $runner = { param([string[]]$WslArgs) return [pscustomobject]@{ ExitCode = 0; Output = 'ok' } }
                { Invoke-WslDocker -Arguments @('-H', 'tcp://127.0.0.1:2375', 'ps') -Runner $runner } | Should -Throw
                { Invoke-WslDocker -Arguments @('-H', 'tcp://0.0.0.0:2375', 'info') -Runner $runner } | Should -Throw
            } finally {
                if ($null -eq $prevHost) {
                    Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                } else {
                    $env:DOCKER_HOST = $prevHost
                }
            }
        }

        It 'throws when DOCKER_HOST environment variable points to TCP 2375' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $env:DOCKER_HOST = 'tcp://127.0.0.1:2375'
                $runner = { param([string[]]$WslArgs) return [pscustomobject]@{ ExitCode = 0; Output = 'ok' } }
                { Invoke-WslDocker -Arguments @('ps') -Runner $runner } | Should -Throw
            } finally {
                if ($null -eq $prevHost) {
                    Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                } else {
                    $env:DOCKER_HOST = $prevHost
                }
            }
        }

        It 'executes over /var/run/docker.sock when valid arguments are provided' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                $runner = {
                    param([string[]]$WslArgs)
                    return [pscustomobject]@{ ExitCode = 0; Output = 'sandal-postgres' }
                }
                $res = Invoke-WslDocker -Arguments @('ps', '--format', '{{.Names}}') -Runner $runner
                $res.ExitCode | Should -Be 0
                $res.Socket | Should -Be '/var/run/docker.sock'
                $res.Output | Should -Be 'sandal-postgres'
            } finally {
                if ($null -eq $prevHost) {
                    Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                } else {
                    $env:DOCKER_HOST = $prevHost
                }
            }
        }
    }
}
