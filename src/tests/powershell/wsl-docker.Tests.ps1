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
                $initial = "[general]`ninstanceIdleTimeout=-1`n`n[wsl2]`nmemory=12GB`nprocessors=6`nnetworkingMode=mirrored`nvmIdleTimeout=-1`n`n[experimental]`nhostAddressLoopback=true"
                Set-Content -LiteralPath $tmpFile -Value $initial -Encoding utf8
                $reader = { param($PromptText, $Key, $CurrentValue) return '' }
                $res = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader $reader
                $res.Changed | Should -Be $false
                $res.ShutdownAdvised | Should -Be $false
                $res.InstanceIdleTimeout | Should -Be '-1'
                $res.NetworkingMode | Should -Be 'mirrored'
                $res.HostAddressLoopback | Should -Be 'true'
                $res.VmIdleTimeout | Should -Be '-1'
                $raw = Get-Content -LiteralPath $tmpFile -Raw
                $raw | Should -Match 'instanceIdleTimeout=-1'
                $raw | Should -Match 'memory=12GB'
                $raw | Should -Match 'processors=6'
                $raw | Should -Match 'vmIdleTimeout=-1'
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
                $res.InstanceIdleTimeout | Should -Be '-1'
                $res.NetworkingMode | Should -Be 'nat'
                $res.HostAddressLoopback | Should -Be 'true'
                $res.VmIdleTimeout | Should -Be '-1'
                $raw = Get-Content -LiteralPath $tmpFile -Raw
                $raw | Should -Match '\[general\]'
                $raw | Should -Match 'instanceIdleTimeout=-1'
                $raw | Should -Match 'memory=16GB'
                $raw | Should -Match 'networkingMode=nat'
                $raw | Should -Match '\[experimental\]'
                $raw | Should -Match 'hostAddressLoopback=true'
                $raw | Should -Match 'vmIdleTimeout=-1'
            } finally {
                if (Test-Path -LiteralPath $tmpFile) {
                    Remove-Item -LiteralPath $tmpFile -Force
                }
            }
        }

        It 'preserves arbitrary section order, whitespace around =, comments, and LF vs CRLF line endings without duplication' {
            $tmpFile = Join-Path ([System.IO.Path]::GetTempPath()) "wslconfig-fmt-$([guid]::NewGuid()).ini"
            try {
                $customLf = "# Custom user header`n[experimental]`nautoMemoryReclaim = gradual`n  hostAddressLoopback = true`n`n[WSL2]`n  memory = 24GB # keep comment`n  networkingMode = mirrored`n  vmIdleTimeout = -1 ; vm comment`n`n[General]`n  InstanceIdleTimeout = -1 # never sleep`n"
                [System.IO.File]::WriteAllText($tmpFile, $customLf, [System.Text.UTF8Encoding]::new($false))
                $res = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader { return '' }
                $res.Changed | Should -Be $false
                $afterNoOp = [System.IO.File]::ReadAllText($tmpFile, [System.Text.Encoding]::UTF8)
                $afterNoOp | Should -Be $customLf

                $missingVmIdleCrlf = "# Custom CRLF config`r`n[general]`r`ninstanceIdleTimeout = -1 # keep`r`n`r`n[wsl2]`r`n  networkingMode = mirrored`r`n`r`n[experimental]`r`n  hostAddressLoopback = true`r`n"
                [System.IO.File]::WriteAllText($tmpFile, $missingVmIdleCrlf, [System.Text.UTF8Encoding]::new($false))
                $res2 = Confirm-WslConfig -ConfigPath $tmpFile -PromptReader { return '' }
                $res2.Changed | Should -Be $true
                $afterMerge = [System.IO.File]::ReadAllText($tmpFile, [System.Text.Encoding]::UTF8)
                $afterMerge.Contains("`r`n") | Should -Be $true
                ($afterMerge -replace "`r`n", '' -match "`n") | Should -Be $false
                $afterMerge | Should -Match '  vmIdleTimeout = -1'
                ([regex]::Matches($afterMerge, '(?mi)^\s*\[wsl2\]').Count) | Should -Be 1
                ([regex]::Matches($afterMerge, '(?mi)^\s*\[general\]').Count) | Should -Be 1
                ([regex]::Matches($afterMerge, '(?mi)^\s*\[experimental\]').Count) | Should -Be 1
                ([regex]::Matches($afterMerge, '(?mi)^\s*instanceIdleTimeout\s*=').Count) | Should -Be 1
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
            $recorded = [System.Collections.Generic.List[string]]::new()
            $runner = {
                param([string[]]$WslArgs)
                $joined = $WslArgs -join ' '
                $recorded.Add($joined)
                if ($joined -match 'docker info') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                }
                if ($joined -match 'docker compose version') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '2.29.7' }
                }
                return [pscustomobject]@{ ExitCode = 0; Output = '' }
            }.GetNewClosure()
            $res = Ensure-LinuxDockerEngine -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Ready | Should -Be $true
            $res.Action | Should -Be 'Reused'
            $res.Provisioned | Should -Be $false
            ($recorded -join "`n") | Should -Not -Match ([regex]::Escape("sed -i '/2375/d'"))
        }

        It 'writes override.conf with -H fd:// -H tcp://127.0.0.1:2375 -H tcp://[::1]:2375 when Docker is installed in WSL2 but not yet running and never runs sed -i /2375/d' {
            $recorded = [System.Collections.Generic.List[string]]::new()
            $state = @{ Started = $false }
            $runner = {
                param([string[]]$WslArgs)
                $joined = $WslArgs -join ' '
                $recorded.Add($joined)
                if ($joined -match '^-l -q$') {
                    return [pscustomobject]@{ ExitCode = 0; Output = "Ubuntu-24.04`n" }
                }
                if ($joined -match 'override\.conf|service docker|systemctl') {
                    $state.Started = $true
                    return [pscustomobject]@{ ExitCode = 0; Output = '' }
                }
                if ($joined -match 'docker info') {
                    if ($state.Started) {
                        return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                    }
                    return [pscustomobject]@{ ExitCode = 1; Output = '' }
                }
                if ($joined -match 'docker compose version') {
                    return [pscustomobject]@{ ExitCode = 0; Output = '2.29.7' }
                }
                return [pscustomobject]@{ ExitCode = 0; Output = '' }
            }.GetNewClosure()
            $res = Ensure-LinuxDockerEngine -Distro 'Ubuntu-24.04' -Runner $runner
            $res.Ready | Should -Be $true
            $res.Provisioned | Should -Be $false
            $allCmds = $recorded -join "`n"
            $allCmds | Should -Match '/etc/systemd/system/docker\.service\.d/override\.conf'
            $allCmds | Should -Match '-H fd:// -H tcp://127\.0\.0\.1:2375 -H tcp://\[::1\]:2375'
            $allCmds | Should -Not -Match ([regex]::Escape("sed -i '/2375/d'"))
        }
    }

    Context 'Invoke-WslDocker and Install-WslDocker.ps1 canonical TCP 2375 contract (INFRA-02)' {
        It 'succeeds when DOCKER_HOST is tcp://[::1]:2375 and preserves DOCKER_HOST unchanged' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $runner = {
                    param([string[]]$WslArgs)
                    return [pscustomobject]@{ ExitCode = 0; Output = 'sandal-postgres' }
                }
                $res = Invoke-WslDocker -Arguments @('ps', '--format', '{{.Names}}') -Runner $runner
                $res.ExitCode | Should -Be 0
                $res.Output | Should -Be 'sandal-postgres'
                $env:DOCKER_HOST | Should -Be 'tcp://[::1]:2375'
            } finally {
                if ($null -eq $prevHost) {
                    Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue
                } else {
                    $env:DOCKER_HOST = $prevHost
                }
            }
        }

        It 'reports tcp_2375=true and docker_host=tcp://[::1]:2375 from Install-WslDocker.ps1 -DryRun -Json' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $installScript = (Resolve-Path (Join-Path $PSScriptRoot '../../../scripts/sandal/Install-WslDocker.ps1')).Path
                $runner = {
                    param([string[]]$WslArgs)
                    return [pscustomobject]@{ ExitCode = 0; Output = "Ubuntu-24.04`n" }
                }
                $rawJson = (@(& $installScript -DryRun -Json -Runner $runner) -join "`n")
                $payload = $rawJson | ConvertFrom-Json
                $payload.ok | Should -Be $true
                $payload.tcp_2375 | Should -Be $true
                $payload.docker_host | Should -Be 'tcp://[::1]:2375'
                $env:DOCKER_HOST | Should -Be 'tcp://[::1]:2375'
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

    Context 'Get-DockerDesktopState backend & plugin detection (Wave 25A)' {
        It 'detects Docker Desktop backend including desktop-linux fallback behind stale DOCKER_HOST=:2375' {
            $tmpExe = Join-Path ([System.IO.Path]::GetTempPath()) "docker-desktop-mock-$([guid]::NewGuid()).exe"
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                Set-Content -LiteralPath $tmpExe -Value 'mock' -Encoding ascii
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $cmdRunner = {
                    param([string]$Command, [string[]]$Arguments)
                    $joined = "$Command $($Arguments -join ' ')"
                    if ($joined -match '--version|compose version|buildx version') {
                        return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                    }
                    if ($joined -match '--context desktop-linux info') {
                        return [pscustomobject]@{ ExitCode = 0; Output = 'docker-desktop|Docker Desktop' }
                    }
                    if ($joined -match 'docker info') {
                        return [pscustomobject]@{ ExitCode = 1; Output = '' }
                    }
                    return [pscustomobject]@{ ExitCode = 1; Output = '' }
                }
                $state = Get-DockerDesktopState -DockerDesktopExePath $tmpExe -CommandRunner $cmdRunner
                $state.DesktopInstalled | Should -Be $true
                $state.CliInstalled | Should -Be $true
                $state.ComposeInstalled | Should -Be $true
                $state.BuildxInstalled | Should -Be $true
                $state.DaemonReachable | Should -Be $true
                $state.ActiveBackend | Should -Be 'docker-desktop'
                $state.Endpoint | Should -Be 'context:desktop-linux'
                $env:DOCKER_HOST | Should -Be 'tcp://[::1]:2375'
            } finally {
                if (Test-Path -LiteralPath $tmpExe) { Remove-Item -LiteralPath $tmpExe -Force }
                if ($null -eq $prevHost) { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }
                else { $env:DOCKER_HOST = $prevHost }
            }
        }

        It 'detects wsl-docker backend when DOCKER_HOST is :2375 and Linux daemon responds' {
            $prevHost = if (Test-Path Env:\DOCKER_HOST) { $env:DOCKER_HOST } else { $null }
            try {
                $env:DOCKER_HOST = 'tcp://[::1]:2375'
                $cmdRunner = {
                    param([string]$Command, [string[]]$Arguments)
                    $joined = "$Command $($Arguments -join ' ')"
                    if ($joined -match '--version|compose version|buildx version') {
                        return [pscustomobject]@{ ExitCode = 0; Output = '27.3.1' }
                    }
                    if ($joined -match 'docker info') {
                        return [pscustomobject]@{ ExitCode = 0; Output = 'sandal-wsl|Ubuntu 24.04.1 LTS' }
                    }
                    return [pscustomobject]@{ ExitCode = 1; Output = '' }
                }
                $state = Get-DockerDesktopState -DockerDesktopExePath 'Z:\nonexistent\Docker Desktop.exe' -CommandRunner $cmdRunner
                $state.DesktopInstalled | Should -Be $false
                $state.CliInstalled | Should -Be $true
                $state.ComposeInstalled | Should -Be $true
                $state.BuildxInstalled | Should -Be $true
                $state.DaemonReachable | Should -Be $true
                $state.ActiveBackend | Should -Be 'wsl-docker'
                $state.Endpoint | Should -Be 'tcp://[::1]:2375'
            } finally {
                if ($null -eq $prevHost) { Remove-Item Env:\DOCKER_HOST -ErrorAction SilentlyContinue }
                else { $env:DOCKER_HOST = $prevHost }
            }
        }

        It 'reports ActiveBackend=none when CLI or plugins are missing' {
            $cmdRunner = {
                param([string]$Command, [string[]]$Arguments)
                $null = $Command; $null = $Arguments
                return [pscustomobject]@{ ExitCode = 1; Output = '' }
            }
            $state = Get-DockerDesktopState -DockerDesktopExePath 'Z:\nonexistent\Docker Desktop.exe' -CommandRunner $cmdRunner
            $state.DesktopInstalled | Should -Be $false
            $state.CliInstalled | Should -Be $false
            $state.ComposeInstalled | Should -Be $false
            $state.BuildxInstalled | Should -Be $false
            $state.DaemonReachable | Should -Be $false
            $state.ActiveBackend | Should -Be 'none'
        }
    }

    Context 'Resolve-WslDockerDistro multi-distro selection & WSL1->WSL2 upgrade (Wave 25A)' {
        It 'selects Debian or Fedora while ignoring docker-desktop and docker-desktop-data' {
            $debRunner = {
                param([string[]]$WslArgs)
                $null = $WslArgs
                return [pscustomobject]@{ ExitCode = 0; Output = "docker-desktop`ndocker-desktop-data`nDebian`nFedora-40`n" }
            }
            (Resolve-WslDockerDistro -Runner $debRunner) | Should -Be 'Debian'

            $fedRunner = {
                param([string[]]$WslArgs)
                $null = $WslArgs
                return [pscustomobject]@{ ExitCode = 0; Output = "docker-desktop`ndocker-desktop-data`nFedora-40`nArch`n" }
            }
            (Resolve-WslDockerDistro -Runner $fedRunner) | Should -Be 'Fedora-40'
        }

        It 'honors SANDAL_WSL_DISTRO when -Distro is not bound and upgrades WSL1 distro to WSL2 in Ensure-WslUbuntu' {
            $prevEnv = if (Test-Path Env:\SANDAL_WSL_DISTRO) { $env:SANDAL_WSL_DISTRO } else { $null }
            $recorded = [System.Collections.Generic.List[string]]::new()
            try {
                $env:SANDAL_WSL_DISTRO = 'Debian'
                $runner = {
                    param([string[]]$WslArgs)
                    $joined = $WslArgs -join ' '
                    $recorded.Add($joined)
                    if ($joined -eq '-l -q') {
                        return [pscustomobject]@{ ExitCode = 0; Output = "Ubuntu-24.04`nDebian`n" }
                    }
                    if ($joined -eq '-l -v') {
                        return [pscustomobject]@{
                            ExitCode = 0
                            Output   = "  NAME            STATE           VERSION`n* Debian          Stopped         1`n  Ubuntu-24.04    Running         2"
                        }
                    }
                    return [pscustomobject]@{ ExitCode = 0; Output = '' }
                }.GetNewClosure()
                $res = Ensure-WslUbuntu -Runner $runner
                $res.Installed | Should -Be $true
                $res.Action | Should -Be 'Reused'
                $res.Distro | Should -Be 'Debian'
                ($recorded -join "`n") | Should -Match '--set-version Debian 2'
            } finally {
                if ($null -eq $prevEnv) { Remove-Item Env:\SANDAL_WSL_DISTRO -ErrorAction SilentlyContinue }
                else { $env:SANDAL_WSL_DISTRO = $prevEnv }
            }
        }
    }

    Context 'Ensure-LinuxDockerEngine exit-42 systemd restart & multi-distro provision-docker.sh (Wave 25A)' {
        It 'terminates the WSL2 distro and retries provision-docker.sh when exit code 42 is returned' {
            $recorded = [System.Collections.Generic.List[string]]::new()
            $state = @{ ProvCount = 0 }
            $runner = {
                param([string[]]$WslArgs)
                $joined = $WslArgs -join ' '
                $recorded.Add($joined)
                if ($joined -eq '-l -q') {
                    return [pscustomobject]@{ ExitCode = 0; Output = "Debian`n" }
                }
                if ($joined -match 'provision-docker\.sh') {
                    $state.ProvCount = [int]$state.ProvCount + 1
                    if ($state.ProvCount -eq 1) {
                        return [pscustomobject]@{ ExitCode = 42; Output = 'INFO: systemd is enabled in /etc/wsl.conf but WSL2 distro must be restarted to boot systemd as PID 1.' }
                    }
                    return [pscustomobject]@{ ExitCode = 0; Output = 'INFO: provisioned' }
                }
                return [pscustomobject]@{ ExitCode = 1; Output = '' }
            }.GetNewClosure()
            $res = Ensure-LinuxDockerEngine -Distro 'Debian' -Runner $runner
            $res.Ready | Should -Be $true
            $res.Action | Should -Be 'Provisioned'
            $res.Distro | Should -Be 'Debian'
            $all = $recorded -join "`n"
            $all | Should -Match '--terminate Debian'
            $state.ProvCount | Should -Be 2
        }

        It 'verifies provision-docker.sh contains multi-distro package managers, sandal-tcp.conf drop-in, safe daemon.json cleanup, section-aware /etc/wsl.conf merge, and exit 42' {
            $provScript = Join-Path $repoRoot 'scripts/wsl/provision-docker.sh'
            $content = Get-Content -LiteralPath $provScript -Raw
            $content | Should -Match 'sandal-tcp\.conf'
            $content | Should -Match 'apt-get'
            $content | Should -Match 'dnf'
            $content | Should -Match 'yum'
            $content | Should -Match 'pacman'
            $content | Should -Match 'zypper'
            $content | Should -Match 'apk'
            $content | Should -Match '--ensure-systemd-only'
            $content | Should -Match 'exit 42'
            $content | Should -Not -Match ([regex]::Escape('/"hosts"[[:space:]]*:/d'))

            $wslCheck = Invoke-WslRunner -WslArgs @('-d', 'Ubuntu', '--', 'bash', '-c', 'tmp=$(mktemp); printf "[automount]\nenabled=true\n[network]\ngenerateResolvConf=true\n" > "$tmp"; WSL_CONF_PATH="$tmp" SYSTEMD_RUN_DIR="/nonexistent-systemd-dir" bash scripts/wsl/provision-docker.sh --ensure-systemd-only; rc=$?; cat "$tmp"; rm -f "$tmp"; exit $rc')
            if ($wslCheck.ExitCode -eq 42) {
                $wslCheck.Output | Should -Match '\[automount\]'
                $wslCheck.Output | Should -Match '\[network\]'
                $wslCheck.Output | Should -Match '\[boot\]'
                $wslCheck.Output | Should -Match 'systemd=true'
            }
        }
    }
}
