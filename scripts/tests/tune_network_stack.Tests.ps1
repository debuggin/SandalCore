#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($null -ne (Get-Module Pester) -and (Get-Module Pester).Version.Major -lt 5) {
    function Should {
        if ($args.Count -ge 1 -and "$($args[0])" -match '^-?Throw$') {
            $threw = $false
            foreach ($sbItem in $input) {
                try { & $sbItem *>$null } catch { $threw = $true }
            }
            if (-not $threw) { throw 'Expected expression to throw an exception.' }
            return
        }
        $mapped = [System.Collections.Generic.List[object]]::new()
        foreach ($a in $args) {
            if ($a -is [string] -and $a -eq '-BeTrue') {
                $mapped.AddRange([object[]]@('Be', $true))
            } elseif ($a -is [string] -and $a -eq '-BeFalse') {
                $mapped.AddRange([object[]]@('Be', $false))
            } elseif ($a -is [string] -and $a.StartsWith('-')) {
                $mapped.Add($a.Substring(1))
            } else {
                $mapped.Add($a)
            }
        }
        $input | Pester\Should @mapped
    }
}

Describe 'SandalCore Network Stack Optimizer -Revert vs -Rollback (tune_network_stack.ps1)' {
    BeforeAll {
        $script:ScriptPath = Join-Path $PSScriptRoot '../tune_network_stack.ps1'
        $rawText = Get-Content -LiteralPath $script:ScriptPath -Raw
        if ($rawText -match '\$MyInvocation\.InvocationName') {
            . $script:ScriptPath
        }

        function Invoke-TuneScriptCli([string[]]$CliArgs) {
            $prevEap = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $out = @(& pwsh -NoProfile -File $script:ScriptPath @CliArgs 2>&1 | ForEach-Object { "$_" })
                return [pscustomobject]@{
                    ExitCode = $LASTEXITCODE
                    Output   = ($out -join "`n")
                }
            } finally {
                $ErrorActionPreference = $prevEap
            }
        }

        $script:RevertDryRun = Invoke-TuneScriptCli -CliArgs @('-Revert', '-DryRun')
        $script:RollbackDryRun = Invoke-TuneScriptCli -CliArgs @('-Rollback', '-DryRun', '-SkipNicTuning', '-SkipDockerWslTuning')
        $script:RestoreDryRun = Invoke-TuneScriptCli -CliArgs @('-Restore', '-DryRun', '-SkipNicTuning', '-SkipDockerWslTuning')
    }

    Context 'CLI Switch Contract, Aliases & Dot-Sourcing Guard (WRT-01, HH-01)' {
        It 'Exposes -Status, -Rollback (-Restore), -Revert (-Default/-Defaults/-ResetDefaults), and -DryRun' {
            $cmd = Get-Command -Name $script:ScriptPath
            $cmd.Parameters.ContainsKey('Status') | Should -BeTrue
            $cmd.Parameters.ContainsKey('Rollback') | Should -BeTrue
            $cmd.Parameters.ContainsKey('Revert') | Should -BeTrue
            $cmd.Parameters.ContainsKey('DryRun') | Should -BeTrue
            $cmd.Parameters.ContainsKey('Force') | Should -BeTrue
            $cmd.Parameters.ContainsKey('SkipNicTuning') | Should -BeTrue
            $cmd.Parameters.ContainsKey('SkipDockerWslTuning') | Should -BeTrue
            ($cmd.Parameters['Rollback'].Aliases -contains 'Restore') | Should -BeTrue
            ($cmd.Parameters['Revert'].Aliases -contains 'Default') | Should -BeTrue
            ($cmd.Parameters['Revert'].Aliases -contains 'Defaults') | Should -BeTrue
            ($cmd.Parameters['Revert'].Aliases -contains 'ResetDefaults') | Should -BeTrue
        }

        It 'Loads parameter tables and helper functions via dot-sourcing without running the interactive menu' {
            $rawScript = Get-Content -LiteralPath $script:ScriptPath -Raw
            $rawScript | Should -Match '\$MyInvocation\.InvocationName\s*-ne\s*''\.'''
            $script:AbsentSentinel | Should -Be '__ABSENT__'
            $script:DesiredTcpipParams.Count | Should -Be 10
            $script:DesiredAfdParams.Count | Should -Be 7
            foreach ($fn in @(
                'Restore-RegistryMap',
                'Remove-SandalCoreRegistryOverrides',
                'Restore-NetshDefaults',
                'Get-StockNicPropertyValue',
                'Get-OptimalNicPropertyValue',
                'Invoke-NetworkRevert',
                'Invoke-NetworkRollback'
            )) {
                ($null -ne (Get-Command -Name $fn -ErrorAction SilentlyContinue)) | Should -BeTrue
            }
        }
    }

    Context 'Mutual Exclusion of -Rollback (-Restore) and -Revert (TS-02, HH-01)' {
        It 'Exits with code 1 and reports mutual exclusion when both -Rollback and -Revert are supplied' {
            $res = Invoke-TuneScriptCli -CliArgs @('-Rollback', '-Revert', '-DryRun')
            $res.ExitCode | Should -Be 1
            $res.Output | Should -Match '-Rollback'
            $res.Output | Should -Match '-Revert'
        }

        It 'Exits with code 1 when -Restore and -Default aliases are combined' {
            $res = Invoke-TuneScriptCli -CliArgs @('-Restore', '-Default', '-DryRun')
            $res.ExitCode | Should -Be 1
            $res.Output | Should -Match '-Rollback'
            $res.Output | Should -Match '-Revert'
        }
    }

    Context '-Revert -DryRun Stock Windows 10/11 Netsh & Port Restoration (QAT-01, HH-01)' {
        It 'Runs baseline netsh resets (int ip, int ipv6, int tcp, winsock), resets dynamic ports, and deletes port exclusions' {
            $script:RevertDryRun.ExitCode | Should -Be 0
            $out = $script:RevertDryRun.Output
            $out | Should -Match '\[DRY-RUN\]'
            $out | Should -Match 'int\s+ip\s+reset'
            $out | Should -Match 'int\s+ipv6\s+reset'
            $out | Should -Match 'int\s+tcp\s+reset'
            $out | Should -Match 'winsock\s+reset'
            $out | Should -Match 'dynamicport\s+tcp\s+start=49152\s+num=16384\s+store=persistent'
            $out | Should -Match 'dynamicport\s+tcp\s+start=49152\s+num=16384\s+store=active'
            $out | Should -Match 'dynamicport\s+udp\s+start=49152\s+num=16384\s+store=persistent'
            $out | Should -Match 'dynamicport\s+udp\s+start=49152\s+num=16384\s+store=active'
            $out | Should -Match 'delete\s+excludedportrange'
        }

        It 'Resets IPv4/IPv6 loopback, TCP global/supplemental, and UDP global parameters to stock defaults' {
            $out = $script:RevertDryRun.Output
            $out | Should -Match 'loopbackexecutionmode=inline'
            $out | Should -Match 'defaultcurhoplimit=128'
            $out | Should -Match 'initialrto=3000'
            $out | Should -Match 'rsc=enabled'
            $out | Should -Match 'minrto=300'
            $out | Should -Match 'icw=10'
            $out | Should -Match 'taill?ossprobe=enabled'
            $out | Should -Match 'uro=enabled'
        }
    }

    Context '-Revert -DryRun Removal of Newly Added Registry Overrides (QAT-01, QAT-02, HH-01)' {
        It 'Removes all 10 Tcpip\Parameters keys, Tcpip6 MaxLoopbackQueueLength, interface ACK keys, and all 7 AFD keys' {
            $out = $script:RevertDryRun.Output
            $out | Should -Match 'Remove-ItemProperty'
            foreach ($tcpKey in @(
                'MaxUserPort', 'TcpTimedWaitDelay', 'TcpFinWait2Delay', 'TcpMaxDataRetransmissions',
                'KeepAliveTime', 'KeepAliveInterval', 'EnablePMTUDiscovery', 'DisableTaskOffload',
                'Tcp1323Opts', 'MaxLoopbackQueueLength'
            )) {
                $out | Should -Match ([regex]::Escape($tcpKey))
            }
            $out | Should -Match 'Tcpip6\\Parameters'
            $out | Should -Match 'TcpAckFrequency'
            $out | Should -Match 'TcpDelAckTicks'
            foreach ($afdKey in @(
                'FastSendDatagramThreshold', 'DefaultReceiveWindow', 'DefaultSendWindow',
                'DynamicSendBufferDisable', 'MaxFastTransmit', 'MaxFastCopyTransmit', 'DoNotHoldNICBuffers'
            )) {
                $out | Should -Match ([regex]::Escape($afdKey))
            }
        }
    }

    Context '-Rollback / -Restore Alias & Hermetic Helper Verification (QAT-01, QAT-02, QAT-11)' {
        It 'Executes -Rollback -DryRun and -Restore -DryRun cleanly with exit code 0' {
            $script:RollbackDryRun.ExitCode | Should -Be 0
            $script:RollbackDryRun.Output | Should -Match '\[DRY-RUN\]'
            $script:RestoreDryRun.ExitCode | Should -Be 0
            $script:RestoreDryRun.Output | Should -Match '\[DRY-RUN\]'
        }

        It 'Restores saved registry values in Restore-RegistryMap and removes overrides in Remove-SandalCoreRegistryOverrides on HKCU' {
            $testRegRoot = "HKCU:\Software\SandalCoreTest_$([guid]::NewGuid().ToString('N'))"
            $prevTcpip = $script:TcpipPath
            $prevTcpip6 = $script:Tcpip6Path
            $prevAfd = $script:AfdPath
            $prevIface = $script:IfaceRoot
            try {
                $script:DryRun = $false
                $script:TcpipPath = "$testRegRoot\Tcpip"
                $script:Tcpip6Path = "$testRegRoot\Tcpip6"
                $script:AfdPath = "$testRegRoot\Afd"
                $script:IfaceRoot = "$testRegRoot\Interfaces"
                foreach ($p in @($script:TcpipPath, $script:Tcpip6Path, $script:AfdPath, "$script:IfaceRoot\eth0")) {
                    $null = New-Item -Path $p -Force
                }
                $null = New-ItemProperty -LiteralPath $script:TcpipPath -Name 'MaxUserPort' -Value 65534 -PropertyType DWord -Force
                $null = New-ItemProperty -LiteralPath $script:TcpipPath -Name 'TcpTimedWaitDelay' -Value 30 -PropertyType DWord -Force
                $savedMap = [pscustomobject]@{
                    MaxUserPort       = $script:AbsentSentinel
                    TcpTimedWaitDelay = 120
                }
                Restore-RegistryMap $script:TcpipPath $savedMap @('MaxUserPort', 'TcpTimedWaitDelay') *>$null
                $props = Get-ItemProperty -LiteralPath $script:TcpipPath
                ($null -eq $props.PSObject.Properties['MaxUserPort']) | Should -BeTrue
                [int]$props.TcpTimedWaitDelay | Should -Be 120

                $null = New-ItemProperty -LiteralPath $script:AfdPath -Name 'DefaultReceiveWindow' -Value 16777216 -PropertyType DWord -Force
                $null = New-ItemProperty -LiteralPath "$script:IfaceRoot\eth0" -Name 'TcpAckFrequency' -Value 1 -PropertyType DWord -Force
                Remove-SandalCoreRegistryOverrides *>$null
                $propsAfter = Get-ItemProperty -LiteralPath $script:TcpipPath
                $afdAfter = Get-ItemProperty -LiteralPath $script:AfdPath
                $ifAfter = Get-ItemProperty -LiteralPath "$script:IfaceRoot\eth0"
                ($null -eq $propsAfter.PSObject.Properties['TcpTimedWaitDelay']) | Should -BeTrue
                ($null -eq $afdAfter.PSObject.Properties['DefaultReceiveWindow']) | Should -BeTrue
                ($null -eq $ifAfter.PSObject.Properties['TcpAckFrequency']) | Should -BeTrue
            } finally {
                $script:TcpipPath = $prevTcpip
                $script:Tcpip6Path = $prevTcpip6
                $script:AfdPath = $prevAfd
                $script:IfaceRoot = $prevIface
                if (Test-Path -LiteralPath $testRegRoot) {
                    Remove-Item -LiteralPath $testRegRoot -Recurse -Force -ErrorAction SilentlyContinue
                }
            }
        }

        It 'Computes stock and optimal NIC property values via Get-StockNicPropertyValue and Get-OptimalNicPropertyValue' {
            $propWithDefault = [pscustomobject]@{
                RegistryKeyword          = '*ReceiveBuffers'
                RegistryValue            = @('4096')
                DefaultRegistryValue     = '512'
                NumericParameterMinValue = 64
                NumericParameterMaxValue = 4096
                NumericParameterStepValue = 64
                ValidRegistryValues      = $null
            }
            [string](Get-StockNicPropertyValue $propWithDefault) | Should -Be '512'
            [string](Get-OptimalNicPropertyValue $propWithDefault 8) | Should -Be '4096'

            $rscProp = [pscustomobject]@{
                RegistryKeyword          = '*RscIPv4'
                RegistryValue            = @('0')
                DefaultRegistryValue     = $null
                NumericParameterMinValue = $null
                NumericParameterMaxValue = $null
                NumericParameterStepValue = $null
                ValidRegistryValues      = @('0', '1')
            }
            [string](Get-StockNicPropertyValue $rscProp) | Should -Be '1'
            [string](Get-OptimalNicPropertyValue $rscProp 8) | Should -Be '0'
        }
    }
}
