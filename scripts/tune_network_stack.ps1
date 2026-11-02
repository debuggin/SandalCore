<#
.SYNOPSIS
    Configures Windows 10/11 & Server TCP/IP, UDP, Winsock (afd.sys), loopback, Docker/WSL2, and multi-vendor NIC settings for SandalCore load testing.
#>
[CmdletBinding()]
param(
    [Alias('Restore')][switch]$Rollback,
    [Alias('Default', 'Defaults', 'ResetDefaults')][switch]$Revert,
    [switch]$Status, [switch]$DryRun, [switch]$SkipNicTuning, [switch]$SkipDockerWslTuning, [switch]$AllAdapters,
    [ValidateNotNullOrEmpty()][ValidateLength(1, 128)][ValidatePattern('^[A-Za-z0-9 _\-\(\)\.#/]+$')][string[]]$AdapterName,
    [ValidateSet('Off', 'Low')][string]$InterruptModerationMode = 'Off',
    [Alias('NoPause')][switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:System32Dir = [Environment]::GetFolderPath([Environment+SpecialFolder]::System)
$script:NetshExe = Join-Path $script:System32Dir 'netsh.exe'
if (-not (Test-Path -LiteralPath $script:NetshExe -PathType Leaf)) { throw "Required binary not found: $script:NetshExe" }
$script:BackupDir = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)) 'SandalCore'
$script:BackupFile = Join-Path $script:BackupDir 'network_stack_backup.json'
$script:AbsentSentinel = '__ABSENT__'
$script:TcpipPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters'
$script:Tcpip6Path = 'HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip6\Parameters'
$script:AfdPath = 'HKLM:\SYSTEM\CurrentControlSet\Services\AFD\Parameters'
$script:IfaceRoot = 'HKLM:\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces'

$script:DesiredTcpipParams = [ordered]@{
    'MaxUserPort' = [uint32]65534; 'TcpTimedWaitDelay' = [uint32]30; 'TcpFinWait2Delay' = [uint32]30; 'TcpMaxDataRetransmissions' = [uint32]3
    'KeepAliveTime' = [uint32]300000; 'KeepAliveInterval' = [uint32]1000; 'EnablePMTUDiscovery' = [uint32]1; 'DisableTaskOffload' = [uint32]0
    'Tcp1323Opts' = [uint32]3; 'MaxLoopbackQueueLength' = [uint32]65536
}
$script:DesiredAfdParams = [ordered]@{
    'FastSendDatagramThreshold' = [uint32]65536; 'DefaultReceiveWindow' = [uint32]16777216; 'DefaultSendWindow' = [uint32]8388608
    'DynamicSendBufferDisable' = [uint32]0; 'MaxFastTransmit' = [uint32]65536; 'MaxFastCopyTransmit' = [uint32]1500; 'DoNotHoldNICBuffers' = [uint32]1
}
$script:RingBufferKeywords = @('*ReceiveBuffers', '*TransmitBuffers', 'RxRingSize', 'TxRingSize', 'RxStdDescCnt', 'TxDescCnt', 'NumRfd', 'NumTcb')
$script:ZeroCoalesceAndPowerKeywords = @(
    '*RscIPv4', '*RscIPv6', '*UdpRsc', '*FlowControl', '*EEE', '*SelectiveSuspend', '*WakeOnMagicPacket', '*WakeOnPattern', '*PMARPOffload', '*PMNSOffload',
    'EEELinkAdvertisement', 'ReduceSpeedOnPowerDown', 'ULPMode', 'DMACoalescing', 'EnablePME', 'AdvancedEEE', 'EnableGreenEthernet', 'GigaLite',
    'PowerSavingMode', 'PowerDownPll', 'EEEPlus', 'AutoDisableGigabit', 'EeePhyEnable', 'GreenEthernet', 'SelectiveSuspend', 'EnablePowerManagement', 'AdaptiveRxCoal'
)
$script:InterruptModKeywords = @('*InterruptModeration', '*ITR', 'ITR', '*InterruptModerationRate', 'RxIntModeration', 'TxIntModeration')
$script:RssKeywords = @('*RSS', '*NumRSSQueues', '*MaxRssProcessors')
$script:StockOneKeywords = @('*RscIPv4', '*RscIPv6', '*UdpRsc', '*PMARPOffload', '*PMNSOffload', '*WakeOnMagicPacket', '*WakeOnPattern', '*EEE', 'EnablePME', 'EnablePowerManagement', '*RSS')
$script:ManagedNicKeywords = @($script:RingBufferKeywords + $script:ZeroCoalesceAndPowerKeywords + $script:InterruptModKeywords + $script:RssKeywords) | Select-Object -Unique

function Write-Section([string]$Title) {
    $suffix = if ($DryRun) { ' [DRY-RUN]' } else { '' }
    Write-Host "`n================================================================================" -ForegroundColor Cyan
    Write-Host "  $Title$suffix`n================================================================================" -ForegroundColor Cyan
}
function Write-Step([string]$Name, [string]$Value, [string]$Status = 'OK') {
    $color = switch ($Status) { 'OK' { 'Green' } 'SKIP' { 'Yellow' } 'WARN' { 'Yellow' } 'ERR' { 'Red' } default { 'Gray' } }
    Write-Host ("  [{0,-4}] {1,-44} -> {2}" -f $Status, $Name, $Value) -ForegroundColor $color
}
function Write-Err([string]$Message) { Write-Step 'Error' $Message 'ERR' }
function Test-IsAdministrator { return ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
function Assert-Administrator([string]$OperationName) {
    if (-not $DryRun -and -not (Test-IsAdministrator)) { throw "$OperationName requires an elevated Administrator PowerShell session. Run as Administrator, or use -Status to inspect without elevation." }
}
function Test-IsInteractiveSession { try { return ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) } catch { return $false } }
function Get-JsonProp($Obj, [string]$Name) {
    if ($null -eq $Obj) { return $null }
    if ($Obj -is [System.Collections.IDictionary]) { return $(if ($Obj.Contains($Name)) { $Obj[$Name] } else { $null }) }
    $m = $Obj.PSObject.Properties.Match($Name)
    return $(if ($null -ne $m -and $m.Count -gt 0) { $Obj.PSObject.Properties[$Name].Value } else { $null })
}
function Get-ScalarRegistryValue($Prop) {
    if ($null -eq $Prop) { return $null }
    $raw = @(Get-JsonProp $Prop 'RegistryValue' | Where-Object { -not [string]::IsNullOrWhiteSpace("$_") })
    return $(if ($raw.Count -gt 0) { [string]$raw[0] } else { $null })
}
function Invoke-NetshCapture([string[]]$ArgList) {
    $prevEap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $out = @(& $script:NetshExe @ArgList 2>&1 | ForEach-Object { "$_" }); return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n").Trim() } }
    finally { $ErrorActionPreference = $prevEap }
}
function Invoke-Netsh([string[]]$ArgList) {
    if ($DryRun) { Write-Step 'DryRun netsh' ($ArgList -join ' ') 'OK'; return $true }
    $res = Invoke-NetshCapture -ArgList $ArgList
    if ($res.ExitCode -ne 0) {
        if ($res.Output -notmatch 'already exists|object was not found|element not found|0x80070490') { Write-Warning "netsh $($ArgList -join ' ') exited $($res.ExitCode): $($res.Output)" }
        return $false
    }
    return $true
}
function Invoke-NetshKeyValues([string[]]$PrefixArgs, [array]$Items, [System.Collections.Generic.HashSet[string]]$SupportedTags, [string[]]$SuffixArgs = @()) {
    $validPairs = @($Items | Where-Object { $SupportedTags.Count -eq 0 -or $SupportedTags.Contains($_.Key) } | ForEach-Object { "$($_.Key)=$($_.Val)" })
    if ($validPairs.Count -eq 0) { return }
    if ($DryRun) { foreach ($pair in $validPairs) { Write-Step ($PrefixArgs -join ' ') "$pair $($SuffixArgs -join ' ') (DryRun)" 'OK' }; return }
    if ((Invoke-NetshCapture -ArgList (@($PrefixArgs) + $validPairs + @($SuffixArgs))).ExitCode -eq 0) {
        foreach ($pair in $validPairs) { Write-Step ($PrefixArgs -join ' ') "$pair $($SuffixArgs -join ' ')" 'OK' }; return
    }
    foreach ($pair in $validPairs) {
        $singleRes = Invoke-NetshCapture -ArgList (@($PrefixArgs) + @($pair) + @($SuffixArgs))
        Write-Step ($PrefixArgs -join ' ') "$pair $($SuffixArgs -join ' ')" ($(if ($singleRes.ExitCode -eq 0) { 'OK' } else { 'SKIP' }))
    }
}
function Get-NetshSupportedTags([string[]]$HelpArgs) {
    $tags = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($m in [regex]::Matches((Invoke-NetshCapture -ArgList $HelpArgs).Output, '\b([a-zA-Z0-9_]+)\s*=')) { [void]$tags.Add($m.Groups[1].Value) }
    return ,$tags
}
function Get-DriverSupportedParameters([string[]]$DriverPaths, [string[]]$CandidateNames) {
    $supported = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    foreach ($path in $DriverPaths) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $rawText = $latin1.GetString([System.IO.File]::ReadAllBytes($path))
        foreach ($name in $CandidateNames) {
            $utf16 = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($name))
            if ($rawText.IndexOf($utf16, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or $rawText.IndexOf($name, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) { [void]$supported.Add($name) }
        }
    }
    return ,$supported
}
function Get-DynamicPortRange([ValidateSet('ipv4', 'ipv6')][string]$IpVersion, [ValidateSet('tcp', 'udp')][string]$Protocol) {
    $m = [regex]::Matches((Invoke-NetshCapture -ArgList @('int', $IpVersion, 'show', 'dynamicport', $Protocol)).Output, ':\s*(\d+)')
    return [pscustomobject]@{ StartPort = $(if ($m.Count -ge 1) { [int]$m[0].Groups[1].Value } else { 49152 }); NumberOfPorts = $(if ($m.Count -ge 2) { [int]$m[1].Groups[1].Value } else { 16384 }) }
}
function Get-ExcludedPortRanges {
    [CmdletBinding()][Alias('Get-ExistingExcludedRanges')]
    param([ValidateSet('ipv4', 'ipv6')][string]$IpVersion, [ValidateSet('tcp', 'udp')][string]$Protocol)
    $res = Invoke-NetshCapture -ArgList @('int', $IpVersion, 'show', 'excludedportrange', "protocol=$Protocol")
    $ranges = [System.Collections.Generic.List[pscustomobject]]::new()
    foreach ($line in ($res.Output -split "`r?`n")) {
        if ($line -match '^\s*(\d+)\s+(\d+)\b') { $s = [int]$Matches[1]; $e = [int]$Matches[2]; $ranges.Add([pscustomobject]@{ StartPort = $s; EndPort = $e; NumberOfPorts = ($e - $s + 1) }) }
    }
    return $ranges.ToArray()
}
function Get-SandalCorePortExclusions([int]$EphemeralStart = 10000) {
    $baseRanges = @(@{ StartPort = 50000; NumberOfPorts = 60; Protocols = @('tcp', 'udp') }, @{ StartPort = 19021; NumberOfPorts = 1; Protocols = @('tcp', 'udp') })
    foreach ($rel in @('config\server.json', 'dist\Release\config\server.json')) {
        $cfgPath = Join-Path $script:RepoRoot $rel
        if (-not (Test-Path -LiteralPath $cfgPath -PathType Leaf)) { continue }
        try {
            $cfg = Get-Content -LiteralPath $cfgPath -Raw -Encoding UTF8 | ConvertFrom-Json
            foreach ($propName in @('tether_port', 'game_port', 'auth_server_port', 'ics_port')) {
                $val = Get-JsonProp $cfg $propName
                if ($null -ne $val -and [int]$val -ge $EphemeralStart -and [int]$val -le 65534) { $baseRanges += @{ StartPort = [int]$val; NumberOfPorts = 1; Protocols = @('tcp', 'udp') } }
            }
        } catch { Write-Verbose "Skipped config file: $cfgPath" }
    }
    $specs = [System.Collections.Generic.List[pscustomobject]]::new()
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($ipVer in @('ipv4', 'ipv6')) {
        foreach ($entry in $baseRanges) {
            foreach ($proto in $entry.Protocols) {
                if ($seen.Add("$ipVer|$proto|$($entry.StartPort)|$($entry.NumberOfPorts)")) { $specs.Add([pscustomobject]@{ IpVersion = $ipVer; Protocol = $proto; StartPort = [int]$entry.StartPort; NumberOfPorts = [int]$entry.NumberOfPorts }) }
            }
        }
    }
    return $specs.ToArray()
}
function Get-TargetPhysicalAdapters {
    [CmdletBinding()][Alias('Get-CandidateAdapters')]
    param()
    $nics = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' })
    if ($nics.Count -eq 0) { $nics = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' -and $_.InterfaceDescription -notmatch 'Loopback' }) }
    if ($null -ne $AdapterName -and $AdapterName.Count -gt 0 -and $AdapterName -notcontains 'All') { $nics = @($nics | Where-Object { $AdapterName -icontains $_.Name }) }
    return $nics
}
function Resolve-MenuAdapterSelection([string]$Choice, [array]$Candidates) {
    $selected = foreach ($token in ($Choice -split '[,;\s]+' | Where-Object { $_ -ne '' } | Select-Object -Unique)) {
        $idx = 0; if ([int]::TryParse($token, [ref]$idx) -and $idx -ge 1 -and $idx -le $Candidates.Count) { $Candidates[$idx - 1] } else { $Candidates | Where-Object { $_.Name -ieq $token } }
    }
    return @($selected | Select-Object -Unique)
}
function Invoke-InteractiveMenu {
    [CmdletBinding()][Alias('Show-InteractiveMenu')]
    param([array]$Candidates = @(Get-TargetPhysicalAdapters), [switch]$DeferExecution)
    Write-Section 'SandalCore Network Stack Optimizer - Interactive Menu'
    $hasBackup = Test-Path -LiteralPath $script:BackupFile -PathType Leaf
    Write-Host ("  Backup Snapshot : {0}`n" -f ($(if ($hasBackup) { "Present ($script:BackupFile)" } else { "Not yet created" }))) -ForegroundColor Gray
    for ($i = 0; $i -lt $Candidates.Count; $i++) { $n = $Candidates[$i]; Write-Host ("    [{0}] {1,-22} | {2,-38} | {3}" -f ($i + 1), $n.Name, $n.InterfaceDescription, $n.LinkSpeed) -ForegroundColor Cyan }
    Write-Host "`n    [A] Apply Optimal Stack + Tune ALL listed physical adapters (Recommended)" -ForegroundColor Green
    Write-Host '    [1..N] Apply Optimal Stack + Tune specific adapter(s) (e.g. 1 or 1,2)' -ForegroundColor Green
    Write-Host '    [S] Apply Optimal OS & Docker/WSL2 Stack ONLY (Skip physical NIC tuning)' -ForegroundColor Yellow
    Write-Host '    [D] Status / Dry-Run Audit (Inspect current vs. target settings, 0 changes)' -ForegroundColor Cyan
    Write-Host '    [R] Rollback / Restore from pre-tuning backup' -ForegroundColor Magenta
    Write-Host '    [V] Revert to stock Windows defaults (remove added keys & reset defaults)' -ForegroundColor Magenta
    Write-Host '    [Q] Quit / Do Nothing (Exit immediately without making any changes)' -ForegroundColor Gray
    $choice = (Read-Host "`n  Select option [A / 1..N / S / D / R / V / Q] (Default: A)").Trim()
    switch -Regex ($choice) {
        '^(?i:|a|all)$'                    { return [pscustomobject]@{ Action = 'Apply';    Adapters = $Candidates } }
        '^(?i:q|quit|exit|n|none|0)$'      { return [pscustomobject]@{ Action = 'Quit';     Adapters = @() } }
        '^(?i:r|rollback|restore)$'        { if (-not $DeferExecution) { Invoke-NetworkRollback }; return [pscustomobject]@{ Action = 'Rollback'; Adapters = $Candidates } }
        '^(?i:v|revert|default|defaults)$' { if (-not $DeferExecution) { Invoke-NetworkRevert };   return [pscustomobject]@{ Action = 'Revert';   Adapters = $Candidates } }
        '^(?i:d|status|dryrun|check)$'     { if (-not $DeferExecution) { Show-NetworkStatus -AdaptersToInspect $Candidates }; return [pscustomobject]@{ Action = 'Status'; Adapters = $Candidates } }
        '^(?i:s|skip)$'                    { return [pscustomobject]@{ Action = 'Apply';    Adapters = @() } }
    }
    return [pscustomobject]@{ Action = 'Apply'; Adapters = @(Resolve-MenuAdapterSelection -Choice $choice -Candidates $Candidates) }
}
function Resolve-RequestedExecutionPlan {
    $candidates = @(Get-TargetPhysicalAdapters)
    if ($Status)        { return [pscustomobject]@{ Action = 'Status';   Adapters = $candidates } }
    if ($Revert)        { return [pscustomobject]@{ Action = 'Revert';   Adapters = $candidates } }
    if ($Rollback)      { return [pscustomobject]@{ Action = 'Rollback'; Adapters = $candidates } }
    if ($SkipNicTuning) { return [pscustomobject]@{ Action = 'Apply';    Adapters = @() } }
    if ($AllAdapters)   { return [pscustomobject]@{ Action = 'Apply';    Adapters = $candidates } }
    if ($null -ne $AdapterName -and $AdapterName.Count -gt 0) {
        if ($AdapterName -contains 'All') { return [pscustomobject]@{ Action = 'Apply'; Adapters = $candidates } }
        $allHostNics = @(Get-NetAdapter -ErrorAction SilentlyContinue)
        $matched = foreach ($req in $AdapterName) {
            $found = @($allHostNics | Where-Object { $_.Name -ieq $req })
            if ($found.Count -eq 0) { Write-Warning "Adapter '$req' not found on this machine." }; $found
        }
        return [pscustomobject]@{ Action = 'Apply'; Adapters = @($matched | Select-Object -Unique) }
    }
    if (-not (Test-IsInteractiveSession)) { Write-Step 'NIC Selection (Non-Interactive)' 'Defaulting to OS/Docker tuning only (-SkipNicTuning)' 'SKIP'; return [pscustomobject]@{ Action = 'Apply'; Adapters = @() } }
    return Invoke-InteractiveMenu -Candidates $candidates -DeferExecution
}
function Assert-SafeBackupPath {
    if (Test-Path -LiteralPath $script:BackupDir) {
        if (((Get-Item -LiteralPath $script:BackupDir -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Security violation: Backup directory '$script:BackupDir' is a reparse point." }
    } else { [void](New-Item -ItemType Directory -Path $script:BackupDir -Force) }
    if ((Test-Path -LiteralPath $script:BackupFile) -and (((Get-Item -LiteralPath $script:BackupFile -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) { throw "Security violation: Backup file '$script:BackupFile' is a reparse point." }
    $acl = New-Object System.Security.AccessControl.DirectorySecurity; $acl.SetAccessRuleProtection($true, $false)
    $inh = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit, ObjectInherit'; $prp = [System.Security.AccessControl.PropagationFlags]::None
    foreach ($rule in @(
        @{ Sid = [System.Security.Principal.WellKnownSidType]::BuiltinAdministratorsSid; Rights = 'FullControl' },
        @{ Sid = [System.Security.Principal.WellKnownSidType]::LocalSystemSid;           Rights = 'FullControl' },
        @{ Sid = [System.Security.Principal.WellKnownSidType]::BuiltinUsersSid;          Rights = 'ReadAndExecute' }
    )) {
        $sidObj = New-Object System.Security.Principal.SecurityIdentifier($rule.Sid, $null)
        $acl.AddAccessRule((New-Object System.Security.AccessControl.FileSystemAccessRule($sidObj, $rule.Rights, $inh, $prp, 'Allow')))
    }
    Set-Acl -LiteralPath $script:BackupDir -AclObject $acl
}
function Write-AtomicJsonFile([string]$Path, $ObjectToSerialize) {
    Assert-SafeBackupPath; $tmpPath = "$Path.tmp.$PID"
    [System.IO.File]::WriteAllText($tmpPath, ($ObjectToSerialize | ConvertTo-Json -Depth 8), [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tmpPath -Destination $Path -Force
}
function Read-RegistryMap([string]$RegPath, [string[]]$Names) {
    $map = [ordered]@{}; $props = if (Test-Path -LiteralPath $RegPath) { Get-ItemProperty -LiteralPath $RegPath -ErrorAction SilentlyContinue } else { $null }
    foreach ($name in $Names) { $val = Get-JsonProp $props $name; $map[$name] = if ($null -ne $val) { [int64]$val } else { $script:AbsentSentinel } }
    return $map
}
function Get-NicBackupRecord($Nic) {
    $advMap = [ordered]@{}
    foreach ($p in @(Get-NetAdapterAdvancedProperty -Name $Nic.Name -AllProperties -ErrorAction SilentlyContinue)) {
        if ($null -ne $p.RegistryKeyword -and $script:ManagedNicKeywords -contains $p.RegistryKeyword) { $scalar = Get-ScalarRegistryValue $p; if ($null -ne $scalar) { $advMap[$p.RegistryKeyword] = $scalar } }
    }
    $rssState = $null
    try { $rss = Get-NetAdapterRss -Name $Nic.Name -ErrorAction Stop; $rssState = [ordered]@{ Enabled = [bool]$rss.Enabled; Profile = [string]$rss.Profile; BaseProcessorNumber = [int]$rss.BaseProcessorNumber; MaxProcessors = [int]$rss.MaxProcessors } } catch { }
    return [ordered]@{ Advanced = $advMap; Rss = $rssState }
}
function Save-NetworkBackup([array]$TargetAdapters) {
    Assert-SafeBackupPath
    $allNics = @(@(Get-CandidateAdapters) + @($TargetAdapters)) | Where-Object { $null -ne $_ }
    if (Test-Path -LiteralPath $script:BackupFile -PathType Leaf) {
        try {
            $existing = Get-Content -LiteralPath $script:BackupFile -Raw -Encoding UTF8 | ConvertFrom-Json
            $adaptersObj = Get-JsonProp $existing 'Adapters'; $updated = $false
            foreach ($nic in $allNics) { if ($null -eq (Get-JsonProp $adaptersObj $nic.Name)) { $adaptersObj | Add-Member -NotePropertyName $nic.Name -NotePropertyValue (Get-NicBackupRecord $nic) -Force; $updated = $true } }
            if ($updated) { Write-AtomicJsonFile -Path $script:BackupFile -ObjectToSerialize $existing }
            Write-Step 'State Backup' "Verified at $script:BackupFile" 'OK'; return
        } catch { Write-Warning 'Existing backup was unreadable; capturing fresh baseline.' }
    }
    $nicSnap = [ordered]@{}; foreach ($nic in $allNics) { if (-not $nicSnap.Contains($nic.Name)) { $nicSnap[$nic.Name] = Get-NicBackupRecord $nic } }
    $ifaceSnap = [ordered]@{}
    if (Test-Path -LiteralPath $script:IfaceRoot) { foreach ($sub in @(Get-ChildItem -LiteralPath $script:IfaceRoot -ErrorAction SilentlyContinue)) { $ifaceSnap[$sub.PSChildName] = Read-RegistryMap -RegPath $sub.PSPath -Names @('TcpAckFrequency', 'TcpDelAckTicks') } }
    $snapshot = [ordered]@{
        Timestamp = (Get-Date -Format 'o'); WindowsBuild = [Environment]::OSVersion.Version.ToString()
        DynamicPorts = [ordered]@{ IPv4Tcp = Get-DynamicPortRange 'ipv4' 'tcp'; IPv4Udp = Get-DynamicPortRange 'ipv4' 'udp'; IPv6Tcp = Get-DynamicPortRange 'ipv6' 'tcp'; IPv6Udp = Get-DynamicPortRange 'ipv6' 'udp' }
        AddedPortExclusions = @(); Adapters = $nicSnap
        Registry = [ordered]@{ Tcpip = Read-RegistryMap $script:TcpipPath @($script:DesiredTcpipParams.Keys); Tcpip6 = Read-RegistryMap $script:Tcpip6Path @('MaxLoopbackQueueLength'); Afd = Read-RegistryMap $script:AfdPath @($script:DesiredAfdParams.Keys); Interfaces = $ifaceSnap }
    }
    Write-AtomicJsonFile -Path $script:BackupFile -ObjectToSerialize $snapshot; Write-Step 'State Backup Created' $script:BackupFile 'OK'
}
function Add-TrackedPortExclusionsToBackup([array]$NewlyAdded) {
    if ($NewlyAdded.Count -eq 0 -or -not (Test-Path -LiteralPath $script:BackupFile -PathType Leaf)) { return }
    $snap = Get-Content -LiteralPath $script:BackupFile -Raw -Encoding UTF8 | ConvertFrom-Json
    $snap | Add-Member -NotePropertyName 'AddedPortExclusions' -NotePropertyValue @(@(Get-JsonProp $snap 'AddedPortExclusions') + $NewlyAdded | Where-Object { $null -ne $_ }) -Force
    Write-AtomicJsonFile -Path $script:BackupFile -ObjectToSerialize $snap
}
function Restore-RegistryMap {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RegPath, [Alias('SavedMap')]$Map = $null, [string[]]$ManagedNames = @())
    if ($null -eq $Map) { return }
    if ($ManagedNames.Count -eq 0) { $ManagedNames = if ($Map -is [System.Collections.IDictionary]) { @($Map.Keys) } else { @($Map.PSObject.Properties | ForEach-Object { $_.Name }) } }
    if (-not $DryRun -and -not (Test-Path -LiteralPath $RegPath)) { [void](New-Item -Path $RegPath -Force) }
    $regLabel = $RegPath -replace '^HKLM:\\SYSTEM\\CurrentControlSet\\Services\\', ''
    foreach ($name in $ManagedNames) {
        $prev = Get-JsonProp $Map $name; if ($null -eq $prev) { continue }
        if ([string]$prev -eq $script:AbsentSentinel) {
            if (-not $DryRun) { Remove-ItemProperty -LiteralPath $RegPath -Name $name -ErrorAction SilentlyContinue }
            Write-Step "$regLabel\$name" 'Remove-ItemProperty (absent in stock defaults)' 'OK'
        } else {
            if (-not $DryRun) { [void](New-ItemProperty -LiteralPath $RegPath -Name $name -Value ([int32]([int64]$prev -band 0xFFFFFFFFL)) -PropertyType DWord -Force) }
            Write-Step "$regLabel\$name" "Restored to $prev" 'OK'
        }
    }
}
function Remove-SandalCoreRegistryOverrides {
    $tcpipAbsent = [ordered]@{}; foreach ($k in $script:DesiredTcpipParams.Keys) { $tcpipAbsent[$k] = $script:AbsentSentinel }
    Restore-RegistryMap -RegPath $script:TcpipPath -Map $tcpipAbsent
    Restore-RegistryMap -RegPath $script:Tcpip6Path -Map @{ MaxLoopbackQueueLength = $script:AbsentSentinel }
    $afdAbsent = [ordered]@{}; foreach ($k in $script:DesiredAfdParams.Keys) { $afdAbsent[$k] = $script:AbsentSentinel }
    Restore-RegistryMap -RegPath $script:AfdPath -Map $afdAbsent
    if (Test-Path -LiteralPath $script:IfaceRoot) {
        foreach ($iface in @(Get-ChildItem -LiteralPath $script:IfaceRoot -ErrorAction SilentlyContinue)) { Restore-RegistryMap -RegPath $iface.PSPath -Map @{ TcpAckFrequency = $script:AbsentSentinel; TcpDelAckTicks = $script:AbsentSentinel } }
    }
}
function Restore-SingleAdapterProperties($Nic, $AdvObj) {
    $byKw = @{}; foreach ($cp in @(Get-NetAdapterAdvancedProperty -Name $Nic.Name -AllProperties -ErrorAction SilentlyContinue)) { if ($null -ne $cp.RegistryKeyword) { $byKw[$cp.RegistryKeyword] = $cp } }
    $changed = $false
    foreach ($kwProp in $AdvObj.PSObject.Properties) {
        $kw = $kwProp.Name; $val = [string]@($kwProp.Value)[0]
        if ($script:ManagedNicKeywords -notcontains $kw -or -not $byKw.ContainsKey($kw) -or [string]::IsNullOrWhiteSpace($val)) { continue }
        if (-not $DryRun -and (Get-ScalarRegistryValue $byKw[$kw]) -ne $val) { try { Set-NetAdapterAdvancedProperty -InputObject $byKw[$kw] -RegistryValue $val -NoRestart -ErrorAction Stop; $changed = $true } catch { } }
    }
    if ($changed -and -not $DryRun) { Restart-NetAdapter -Name $Nic.Name -ErrorAction SilentlyContinue }
}
function Restore-AdapterSettings($SnapshotAdapters) {
    if ($null -eq $SnapshotAdapters -or $SkipNicTuning) { return }
    $targetNics = @(Get-TargetPhysicalAdapters)
    foreach ($nicProp in $SnapshotAdapters.PSObject.Properties) {
        $nic = @($targetNics | Where-Object { $_.Name -ieq $nicProp.Name } | Select-Object -First 1); if ($nic.Count -eq 0) { continue }
        $advObj = Get-JsonProp $nicProp.Value 'Advanced'
        Restore-SingleAdapterProperties -Nic $nic[0] -AdvObj ($(if ($null -ne $advObj) { $advObj } else { $nicProp.Value }))
        Write-Step "NIC '$($nicProp.Name)'" 'Restored saved driver properties' 'OK'
    }
}
function Remove-RevertPortExclusions($Snapshot, [switch]$RevertToStock) {
    $deleted = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($ex in @(Get-JsonProp $Snapshot 'AddedPortExclusions')) {
        if ($null -eq $ex) { continue }
        [void]$deleted.Add("$($ex.IpVersion)|$($ex.Protocol)|$($ex.StartPort)|$($ex.NumberOfPorts)")
        [void](Invoke-Netsh @('int', [string]$ex.IpVersion, 'delete', 'excludedportrange', "protocol=$($ex.Protocol)", "startport=$($ex.StartPort)", "numberofports=$($ex.NumberOfPorts)", 'store=persistent'))
        Write-Step "Removed Port Exclusion ($($ex.IpVersion)/$($ex.Protocol))" "$($ex.StartPort) (+$($ex.NumberOfPorts))" 'OK'
    }
    if (-not $RevertToStock) { return }
    $existingMap = @{}; foreach ($af in @('ipv4', 'ipv6')) { foreach ($proto in @('tcp', 'udp')) { $existingMap["$af|$proto"] = @(Get-ExcludedPortRanges -IpVersion $af -Protocol $proto) } }
    foreach ($req in @(Get-SandalCorePortExclusions -EphemeralStart 1024)) {
        if ($deleted.Contains("$($req.IpVersion)|$($req.Protocol)|$($req.StartPort)|$($req.NumberOfPorts)")) { continue }
        $curList = $existingMap["$($req.IpVersion)|$($req.Protocol)"]
        $present = @($curList | Where-Object { $_.StartPort -eq $req.StartPort -and $_.NumberOfPorts -eq $req.NumberOfPorts }).Count -gt 0
        if ($DryRun -or $curList.Count -eq 0 -or $present) {
            [void](Invoke-Netsh @('int', $req.IpVersion, 'delete', 'excludedportrange', "protocol=$($req.Protocol)", "startport=$($req.StartPort)", "numberofports=$($req.NumberOfPorts)", 'store=persistent'))
            Write-Step "Removed Port Exclusion ($($req.IpVersion)/$($req.Protocol))" "$($req.StartPort) (+$($req.NumberOfPorts))" 'OK'
        }
    }
}
function Restore-NetshDefaults($Snapshot = $null, [switch]$RevertToStock) {
    if ($RevertToStock) {
        Invoke-Netsh @('int', 'ip', 'reset') | Out-Null; Invoke-Netsh @('int', 'ipv6', 'reset') | Out-Null
        Invoke-Netsh @('int', 'tcp', 'reset') | Out-Null; Invoke-Netsh @('winsock', 'reset') | Out-Null
    }
    $dynMap = @{ IPv4Tcp = @('ipv4', 'tcp'); IPv4Udp = @('ipv4', 'udp'); IPv6Tcp = @('ipv6', 'tcp'); IPv6Udp = @('ipv6', 'udp') }
    $savedDyn = if ($RevertToStock) { $null } else { Get-JsonProp $Snapshot 'DynamicPorts' }
    foreach ($key in $dynMap.Keys) {
        $ipVer = $dynMap[$key][0]; $proto = $dynMap[$key][1]; $entry = Get-JsonProp $savedDyn $key
        $start = if ($null -ne $entry -and [int]$entry.StartPort -ge 1024) { [int]$entry.StartPort } else { 49152 }
        $count = if ($null -ne $entry -and [int]$entry.NumberOfPorts -ge 1) { [int]$entry.NumberOfPorts } else { 16384 }
        [void](Invoke-Netsh @('int', $ipVer, 'set', 'dynamicport', $proto, "start=$start", "num=$count", 'store=persistent'))
        [void](Invoke-Netsh @('int', $ipVer, 'set', 'dynamicport', $proto, "start=$start", "num=$count", 'store=active'))
        Write-Step "Dynamic Ports ($ipVer/$proto)" "start=$start num=$count" 'OK'
    }
    Remove-RevertPortExclusions -Snapshot $Snapshot -RevertToStock:$RevertToStock
    $workerDef = if ($RevertToStock) { '2' } else { '8' }
    foreach ($af in @('ipv4', 'ipv6')) {
        $tags = Get-NetshSupportedTags @('int', $af, 'set', 'global', '?')
        $ipItems = @(@{ Key = 'loopbackexecutionmode'; Val = 'inline' }, @{ Key = 'loopbackworkercount'; Val = $workerDef }, @{ Key = 'neighborcachelimit'; Val = '256' }, @{ Key = 'routecachelimit'; Val = '4096' }, @{ Key = 'defaultcurhoplimit'; Val = '128' })
        Invoke-NetshKeyValues -PrefixArgs @('int', $af, 'set', 'global') -Items $ipItems -SupportedTags $tags -SuffixArgs @('store=persistent')
        Invoke-NetshKeyValues -PrefixArgs @('int', $af, 'set', 'global') -Items @($ipItems | Where-Object { $_.Key -ne 'loopbackworkercount' }) -SupportedTags $tags -SuffixArgs @('store=active')
    }
    $tcpDefaults = @(
        @{ Key = 'autotuninglevel'; Val = 'normal' }, @{ Key = 'rss'; Val = 'enabled' }, @{ Key = 'rsc'; Val = 'enabled' }, @{ Key = 'ecncapability'; Val = 'disabled' }, @{ Key = 'timestamps'; Val = 'disabled' },
        @{ Key = 'initialrto'; Val = '3000' }, @{ Key = 'maxsynretransmissions'; Val = '2' }, @{ Key = 'nonsackrttresiliency'; Val = 'disabled' }, @{ Key = 'fastopen'; Val = 'enabled' },
        @{ Key = 'fastopenfallback'; Val = 'enabled' }, @{ Key = 'hystart'; Val = 'enabled' }, @{ Key = 'prr'; Val = 'enabled' }, @{ Key = 'pacingprofile'; Val = 'off' }
    )
    Invoke-NetshKeyValues -PrefixArgs @('int', 'tcp', 'set', 'global') -Items $tcpDefaults -SupportedTags (Get-NetshSupportedTags @('int', 'tcp', 'set', 'global', '?'))
    $suppOut = (Invoke-NetshCapture @('int', 'tcp', 'show', 'supplemental')).Output
    $tpl = if ($suppOut -match '\b(internetcustom|datacentercustom|internet|datacenter|compat|custom)\b') { $Matches[1] } else { 'internet' }
    $suppDefaults = @(@{ Key = 'minrto'; Val = '300' }, @{ Key = 'icw'; Val = '10' }, @{ Key = 'delayedacktimeout'; Val = '40' }, @{ Key = 'delayedackfrequency'; Val = '2' }, @{ Key = 'congestionprovider'; Val = 'cubic' }, @{ Key = 'enablecwndrestart'; Val = 'disabled' }, @{ Key = 'rack'; Val = 'enabled' }, @{ Key = 'taillossprobe'; Val = 'enabled' })
    Invoke-NetshKeyValues -PrefixArgs @('int', 'tcp', 'set', 'supplemental', "template=$tpl") -Items $suppDefaults -SupportedTags (Get-NetshSupportedTags @('int', 'tcp', 'set', 'supplemental', '?'))
    Invoke-NetshKeyValues -PrefixArgs @('int', 'udp', 'set', 'global') -Items @(@{ Key = 'uro'; Val = 'enabled' }, @{ Key = 'uso'; Val = 'enabled' }) -SupportedTags (Get-NetshSupportedTags @('int', 'udp', 'set', 'global', '?'))
}
function Invoke-WslSysctl([string]$SysctlCommand, [string]$SummaryText) {
    $wslExe = Join-Path $script:System32Dir 'wsl.exe'
    if ($DryRun -or -not (Test-Path -LiteralPath $wslExe -PathType Leaf)) { return }
    $prevEap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try {
        foreach ($d in @(& $wslExe --list --running --quiet 2>$null | ForEach-Object { ("$_" -replace "`0", '').Trim() } | Where-Object { $_ -ne '' })) {
            & $wslExe -d $d -u root -- sh -c $SysctlCommand 2>$null
            if ($LASTEXITCODE -eq 0) { Write-Step "WSL2 Distro '$d' sysctl" $SummaryText 'OK' }
        }
    } finally { $ErrorActionPreference = $prevEap }
}
function Reset-VirtualSwitchAndWslDefaults {
    if ($SkipDockerWslTuning) { Write-Step 'Docker & WSL2 Reset' 'Skipped via -SkipDockerWslTuning' 'SKIP'; return }
    foreach ($vNic in @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' -and ($_.Name -match '^vEthernet|WSL' -or $_.InterfaceDescription -match 'Hyper-V Virtual Ethernet|WSL') })) {
        if (-not $DryRun) {
            try { Enable-NetAdapterRsc -Name $vNic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue } catch { }
            foreach ($vp in @(Get-NetAdapterAdvancedProperty -Name $vNic.Name -AllProperties -ErrorAction SilentlyContinue)) {
                if ($vp.RegistryKeyword -in @('*RscIPv4', '*RscIPv6') -and (Get-ScalarRegistryValue $vp) -ne '1') { try { Set-NetAdapterAdvancedProperty -InputObject $vp -RegistryValue '1' -NoRestart -ErrorAction Stop } catch { } }
            }
        }
        Write-Step "Virtual Switch '$($vNic.Name)'" 'Restored stock RSC coalescing (enabled)' 'OK'
    }
    Invoke-WslSysctl -SysctlCommand 'sysctl -w net.ipv4.ip_local_port_range="32768 60999" net.ipv4.tcp_tw_reuse=2 net.ipv4.tcp_fin_timeout=60 net.core.somaxconn=4096 net.core.netdev_max_backlog=1000 net.core.rmem_max=212992 net.core.wmem_max=212992 >/dev/null 2>&1' -SummaryText 'Restored stock Linux kernel defaults'
}
function Get-StockNicPropertyValue {
    [CmdletBinding()]
    param([Parameter(Mandatory)][Alias('Prop')]$Property)
    $defArr = @(Get-JsonProp $Property 'DefaultRegistryValue' | Where-Object { -not [string]::IsNullOrWhiteSpace("$_") })
    if ($defArr.Count -gt 0) { return [string]$defArr[0] }
    $kw = [string]$Property.RegistryKeyword
    $valid = @(Get-JsonProp $Property 'ValidRegistryValues' | Where-Object { $null -ne $_ })
    $ceil = if ($kw -in $script:RingBufferKeywords) { 512 } elseif ($kw -in @('*NumRSSQueues', '*MaxRssProcessors')) { 8 } else { 0 }
    if ($ceil -gt 0) { return [string]((Resolve-OptimalRingBufferValue -Prop $Property -Ceiling $ceil) ?? $ceil) }
    if ($kw -in @('ITR', '*ITR', '*InterruptModerationRate')) { return $(if ($valid.Count -gt 0 -and $valid -notcontains '65535') { '1' } else { '65535' }) }
    if ($kw -in ($script:InterruptModKeywords + $script:StockOneKeywords)) { return '1' }
    if ($kw -eq '*FlowControl') { return $(if ($valid.Count -gt 0 -and $valid -notcontains '3') { '1' } else { '3' }) }
    if ($kw -in $script:ZeroCoalesceAndPowerKeywords) { return '0' }
    return $null
}
function Reset-AdapterStockDefaults([array]$Adapters = @(Get-TargetPhysicalAdapters)) {
    if ($SkipNicTuning) { Write-Step 'Physical NIC Reset' 'Skipped via -SkipNicTuning' 'SKIP'; return }
    foreach ($nic in $Adapters) {
        if ($DryRun) { Write-Step "NIC '$($nic.Name)'" 'DryRun: Reset to stock Windows / INF defaults' 'OK'; continue }
        try {
            Enable-NetAdapterRsc -Name $nic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue
            Enable-NetAdapterPowerManagement -Name $nic.Name -NoRestart -ErrorAction SilentlyContinue
            Enable-NetAdapterChecksumOffload -Name $nic.Name -IpIPv4 -TcpIPv4 -TcpIPv6 -UdpIPv4 -UdpIPv6 -NoRestart -ErrorAction SilentlyContinue
            if (Get-Command 'Enable-NetAdapterUso' -ErrorAction SilentlyContinue) { Enable-NetAdapterUso -Name $nic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue }
            if (Get-Command 'Enable-NetAdapterUro' -ErrorAction SilentlyContinue) { Enable-NetAdapterUro -Name $nic.Name -NoRestart -ErrorAction SilentlyContinue }
            Enable-NetAdapterRss -Name $nic.Name -NoRestart -ErrorAction SilentlyContinue
            Set-NetAdapterRss -Name $nic.Name -Enabled $true -Profile Closest -BaseProcessorNumber 0 -NoRestart -ErrorAction SilentlyContinue
        } catch { }
        $changed = $false
        foreach ($prop in @(Get-NetAdapterAdvancedProperty -Name $nic.Name -AllProperties -ErrorAction SilentlyContinue)) {
            if ($null -eq $prop.RegistryKeyword -or $script:ManagedNicKeywords -notcontains $prop.RegistryKeyword) { continue }
            $stockVal = Get-StockNicPropertyValue -Property $prop
            if ($null -ne $stockVal -and (Get-ScalarRegistryValue $prop) -ne $stockVal) {
                try { Set-NetAdapterAdvancedProperty -InputObject $prop -RegistryValue $stockVal -NoRestart -ErrorAction Stop; $changed = $true } catch { }
            }
        }
        if ($changed) { Restart-NetAdapter -Name $nic.Name -ErrorAction SilentlyContinue }
        Write-Step "NIC '$($nic.Name)'" 'Reset to stock Windows / INF defaults' 'OK'
    }
}
function Invoke-NetworkRollback {
    Assert-Administrator -OperationName 'Network Stack Rollback'
    Write-Section 'Rolling Back Network Stack to Pre-Tuning Backup'
    if (-not $DryRun) { Assert-SafeBackupPath }
    $snapshot = if (Test-Path -LiteralPath $script:BackupFile -PathType Leaf) {
        Write-Step 'Loaded Backup Snapshot' $script:BackupFile 'OK'
        Get-Content -LiteralPath $script:BackupFile -Raw -Encoding UTF8 | ConvertFrom-Json
    } else {
        Write-Step 'Backup Snapshot Not Found' 'Applying stock Windows defaults for netsh/ports only' 'WARN'
        $null
    }
    Restore-NetshDefaults -Snapshot $snapshot
    $savedReg = Get-JsonProp $snapshot 'Registry'
    Restore-RegistryMap -RegPath $script:TcpipPath -SavedMap (Get-JsonProp $savedReg 'Tcpip') -ManagedNames @($script:DesiredTcpipParams.Keys)
    Restore-RegistryMap -RegPath $script:Tcpip6Path -SavedMap (Get-JsonProp $savedReg 'Tcpip6') -ManagedNames @('MaxLoopbackQueueLength')
    Restore-RegistryMap -RegPath $script:AfdPath -SavedMap (Get-JsonProp $savedReg 'Afd') -ManagedNames @($script:DesiredAfdParams.Keys)
    $savedIfaces = Get-JsonProp $savedReg 'Interfaces'
    if ($null -ne $savedIfaces) {
        foreach ($ifProp in $savedIfaces.PSObject.Properties) {
            Restore-RegistryMap -RegPath "$script:IfaceRoot\$($ifProp.Name)" -SavedMap $ifProp.Value -ManagedNames @('TcpAckFrequency', 'TcpDelAckTicks')
        }
    }
    Restore-AdapterSettings -SnapshotAdapters (Get-JsonProp $snapshot 'Adapters')
    if (-not $DryRun -and -not $SkipNicTuning -and ($null -eq $AdapterName -or $AdapterName.Count -eq 0) -and (Test-Path -LiteralPath $script:BackupFile -PathType Leaf)) {
        Remove-Item -LiteralPath $script:BackupFile -Force -ErrorAction SilentlyContinue
        Write-Step 'Cleaned Up Backup File' $script:BackupFile 'OK'
    }
    Write-Host "`nRollback complete. Reboot is recommended to finalize AFD/TCPIP registry restoration." -ForegroundColor Green
}
function Invoke-NetworkRevert {
    Assert-Administrator -OperationName 'Network Stack Revert to Stock Windows Defaults'
    Write-Section 'Reverting Network Stack to Stock Windows 10/11 Defaults'
    $snapshot = if (Test-Path -LiteralPath $script:BackupFile -PathType Leaf) {
        try { Get-Content -LiteralPath $script:BackupFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null }
    } else { $null }
    Restore-NetshDefaults -Snapshot $snapshot -RevertToStock
    Remove-SandalCoreRegistryOverrides
    Reset-VirtualSwitchAndWslDefaults
    Reset-AdapterStockDefaults -Adapters @(Get-TargetPhysicalAdapters)
    if (-not $DryRun -and -not $SkipNicTuning -and ($null -eq $AdapterName -or $AdapterName.Count -eq 0) -and (Test-Path -LiteralPath $script:BackupFile -PathType Leaf)) {
        Remove-Item -LiteralPath $script:BackupFile -Force -ErrorAction SilentlyContinue
        Write-Step 'Cleaned Up Backup File' $script:BackupFile 'OK'
    }
    Write-Host "`nRevert to stock Windows 10/11 defaults complete (netsh int ip/ipv6/tcp reset & netsh winsock reset executed)." -ForegroundColor Green
    Write-Host "A Windows reboot is recommended for full Winsock catalog and AFD/TCPIP kernel reset to take effect." -ForegroundColor Yellow
}
function Get-MissingPortExclusions([array]$RequiredExclusions) {
    $existingByAfProto = @{}
    foreach ($af in @('ipv4', 'ipv6')) {
        foreach ($proto in @('tcp', 'udp')) { $existingByAfProto["$af|$proto"] = @(Get-ExcludedPortRanges -IpVersion $af -Protocol $proto) }
    }
    $missing = [System.Collections.Generic.List[pscustomobject]]::new()
    foreach ($req in $RequiredExclusions) {
        $reqEnd = $req.StartPort + $req.NumberOfPorts - 1
        $covered = $false
        foreach ($ex in $existingByAfProto["$($req.IpVersion)|$($req.Protocol)"]) {
            if ($ex.StartPort -le $req.StartPort -and $ex.EndPort -ge $reqEnd) { $covered = $true; break }
        }
        if ($covered) { Write-Step "Port Exclusion ($($req.IpVersion)/$($req.Protocol))" "$($req.StartPort)..$reqEnd (already reserved)" 'OK' }
        else { $missing.Add($req) }
    }
    return $missing.ToArray()
}
function Add-MissingPortExclusions([array]$MissingExclusions) {
    $newlyAdded = [System.Collections.Generic.List[pscustomobject]]::new()
    $needWinnatCycle = $false
    foreach ($req in $MissingExclusions) {
        $reqEnd = $req.StartPort + $req.NumberOfPorts - 1
        foreach ($ex in @(Get-ExcludedPortRanges -IpVersion $req.IpVersion -Protocol $req.Protocol)) {
            if ($ex.StartPort -le $reqEnd -and $ex.EndPort -ge $req.StartPort) {
                [void](Invoke-Netsh @('int', $req.IpVersion, 'delete', 'excludedportrange', "protocol=$($req.Protocol)", "startport=$($ex.StartPort)", "numberofports=$($ex.NumberOfPorts)", 'store=persistent'))
            }
        }
        if (Invoke-Netsh @('int', $req.IpVersion, 'add', 'excludedportrange', "protocol=$($req.Protocol)", "startport=$($req.StartPort)", "numberofports=$($req.NumberOfPorts)", 'store=persistent')) {
            $newlyAdded.Add($req)
            Write-Step "Reserved Ports ($($req.IpVersion)/$($req.Protocol))" "$($req.StartPort) (+$($req.NumberOfPorts))" 'OK'
        } else { $needWinnatCycle = $true }
    }
    if ($needWinnatCycle -and -not $DryRun) {
        $winnat = Get-Service -Name 'winnat' -ErrorAction SilentlyContinue
        $wasRunning = ($null -ne $winnat -and $winnat.Status -eq 'Running')
        try {
            if ($wasRunning) { Stop-Service -Name 'winnat' -Force -ErrorAction Stop; Write-Step 'WinNAT Service' 'Paused to clear overlapping port reservations' 'OK' }
            foreach ($req in $MissingExclusions) {
                if ($newlyAdded.Contains($req)) { continue }
                if (Invoke-Netsh @('int', $req.IpVersion, 'add', 'excludedportrange', "protocol=$($req.Protocol)", "startport=$($req.StartPort)", "numberofports=$($req.NumberOfPorts)", 'store=persistent')) {
                    $newlyAdded.Add($req)
                    Write-Step "Reserved Ports ($($req.IpVersion)/$($req.Protocol))" "$($req.StartPort) (+$($req.NumberOfPorts))" 'OK'
                }
            }
        } finally {
            if ($wasRunning) { Start-Service -Name 'winnat' -ErrorAction Continue; Write-Step 'WinNAT Service' 'Restarted' 'OK' }
        }
    }
    if (-not $DryRun) { Add-TrackedPortExclusionsToBackup -NewlyAdded $newlyAdded.ToArray() }
}
function Set-PortExclusionsAndDynamicRanges {
    Write-Section '1. Ephemeral Port Pool (10000..65534) & SandalCore Service Port Reservations'
    $missing = @(Get-MissingPortExclusions -RequiredExclusions @(Get-SandalCorePortExclusions -EphemeralStart 10000))
    $needsDynUpdate = $false
    foreach ($af in @('ipv4', 'ipv6')) {
        foreach ($proto in @('tcp', 'udp')) {
            $curDyn = Get-DynamicPortRange -IpVersion $af -Protocol $proto
            if ($curDyn.StartPort -ne 10000 -or $curDyn.NumberOfPorts -ne 55535) { $needsDynUpdate = $true }
        }
    }
    if ($missing.Count -eq 0 -and -not $needsDynUpdate) {
        Write-Step 'Ephemeral Port Pool (IPv4/IPv6 TCP/UDP)' 'start=10000 num=55535 (already active; 0 WinNAT disruption)' 'OK'
        return
    }
    if ($missing.Count -gt 0) { Add-MissingPortExclusions -MissingExclusions $missing }
    foreach ($af in @('ipv4', 'ipv6')) {
        foreach ($proto in @('tcp', 'udp')) {
            [void](Invoke-Netsh @('int', $af, 'set', 'dynamicport', $proto, 'start=10000', 'num=55535', 'store=persistent'))
            [void](Invoke-Netsh @('int', $af, 'set', 'dynamicport', $proto, 'start=10000', 'num=55535', 'store=active'))
            Write-Step "Dynamic Port Pool ($af/$proto)" '10000..65534 (55,535 ephemeral ports)' 'OK'
        }
    }
}
function Set-LoopbackAndTcpUdpGlobals([int]$LoopbackWorkers) {
    Write-Section '2. Loopback Fast-Path, TCP Supplemental & UDP Global Offloads'
    foreach ($af in @('ipv4', 'ipv6')) {
        $tags = Get-NetshSupportedTags @('int', $af, 'set', 'global', '?')
        $ipItems = @(@{ Key = 'loopbackexecutionmode'; Val = 'adaptive' }, @{ Key = 'loopbackworkercount'; Val = "$LoopbackWorkers" }, @{ Key = 'neighborcachelimit'; Val = '4096' }, @{ Key = 'routecachelimit'; Val = '16384' }, @{ Key = 'reassemblylimit'; Val = '67108864' }, @{ Key = 'defaultcurhoplimit'; Val = '64' })
        Invoke-NetshKeyValues -PrefixArgs @('int', $af, 'set', 'global') -Items $ipItems -SupportedTags $tags -SuffixArgs @('store=persistent')
        Invoke-NetshKeyValues -PrefixArgs @('int', $af, 'set', 'global') -Items @($ipItems | Where-Object { $_.Key -ne 'loopbackworkercount' }) -SupportedTags $tags -SuffixArgs @('store=active')
    }
    $tcpGlobalTags = Get-NetshSupportedTags @('int', 'tcp', 'set', 'global', '?')
    $tcpGlobalItems = @(@{ Key = 'autotuninglevel'; Val = 'normal' }, @{ Key = 'rss'; Val = 'enabled' }, @{ Key = 'rsc'; Val = 'disabled' }, @{ Key = 'ecncapability'; Val = 'disabled' }, @{ Key = 'timestamps'; Val = 'disabled' }, @{ Key = 'initialrto'; Val = '300' }, @{ Key = 'maxsynretransmissions'; Val = '2' }, @{ Key = 'nonsackrttresiliency'; Val = 'disabled' }, @{ Key = 'fastopen'; Val = 'enabled' }, @{ Key = 'fastopenfallback'; Val = 'enabled' }, @{ Key = 'hystart'; Val = 'enabled' }, @{ Key = 'prr'; Val = 'enabled' }, @{ Key = 'pacingprofile'; Val = 'off' })
    Invoke-NetshKeyValues -PrefixArgs @('int', 'tcp', 'set', 'global') -Items $tcpGlobalItems -SupportedTags $tcpGlobalTags
    $suppOut = (Invoke-NetshCapture @('int', 'tcp', 'show', 'supplemental')).Output
    $tpl = if ($suppOut -match '\b(internetcustom|datacentercustom|internet|datacenter|compat|custom)\b') { $Matches[1] } else { 'internet' }
    $tcpSuppTags = Get-NetshSupportedTags @('int', 'tcp', 'set', 'supplemental', '?')
    $suppItems = @(@{ Key = 'delayedacktimeout'; Val = '10' }, @{ Key = 'delayedackfrequency'; Val = '1' }, @{ Key = 'minrto'; Val = '20' }, @{ Key = 'icw'; Val = '16' }, @{ Key = 'congestionprovider'; Val = 'cubic' }, @{ Key = 'enablecwndrestart'; Val = 'enabled' }, @{ Key = 'rack'; Val = 'enabled' }, @{ Key = 'taillossprobe'; Val = 'enabled' })
    Invoke-NetshKeyValues -PrefixArgs @('int', 'tcp', 'set', 'supplemental', "template=$tpl") -Items $suppItems -SupportedTags $tcpSuppTags
    $udpTags = Get-NetshSupportedTags @('int', 'udp', 'set', 'global', '?')
    Invoke-NetshKeyValues -PrefixArgs @('int', 'udp', 'set', 'global') -Items @(@{ Key = 'uro'; Val = 'disabled' }, @{ Key = 'uso'; Val = 'enabled' }) -SupportedTags $udpTags
}
function Set-VerifiedRegistryMap([string]$RegPath, [string]$LabelPrefix, [string]$DriverLabel, [System.Collections.IDictionary]$DesiredMap, $SupportedSet) {
    foreach ($kv in $DesiredMap.GetEnumerator()) {
        if ($SupportedSet.Contains($kv.Key)) {
            if (-not $DryRun) { [void](New-ItemProperty -LiteralPath $RegPath -Name $kv.Key -Value ([uint32]$kv.Value) -PropertyType DWord -Force) }
            Write-Step "$LabelPrefix\$($kv.Key)" "$($kv.Value) (verified in $DriverLabel)" 'OK'
        } else { Write-Step "$LabelPrefix\$($kv.Key)" 'Not found in driver binary; skipped' 'SKIP' }
    }
}
function Set-KernelDriverRegistrySettings {
    Write-Section '3. Kernel Registry Parameters (tcpip.sys, tcpipreg.sys & afd.sys)'
    $tcpipDrivers = @((Join-Path $script:System32Dir 'drivers\tcpip.sys'), (Join-Path $script:System32Dir 'drivers\tcpipreg.sys'))
    $afdDriver = Join-Path $script:System32Dir 'drivers\afd.sys'
    $tcpipSupported = Get-DriverSupportedParameters -DriverPaths $tcpipDrivers -CandidateNames (@($script:DesiredTcpipParams.Keys) + @('TcpAckFrequency', 'TcpDelAckTicks'))
    $afdSupported = Get-DriverSupportedParameters -DriverPaths @($afdDriver) -CandidateNames @($script:DesiredAfdParams.Keys)
    if (-not $DryRun) { foreach ($p in @($script:TcpipPath, $script:Tcpip6Path, $script:AfdPath)) { if (-not (Test-Path -LiteralPath $p)) { [void](New-Item -Path $p -Force) } } }
    Set-VerifiedRegistryMap -RegPath $script:TcpipPath -LabelPrefix 'Tcpip\Parameters' -DriverLabel 'tcpip.sys' -DesiredMap $script:DesiredTcpipParams -SupportedSet $tcpipSupported
    if ($tcpipSupported.Contains('MaxLoopbackQueueLength')) {
        if (-not $DryRun) { [void](New-ItemProperty -LiteralPath $script:Tcpip6Path -Name 'MaxLoopbackQueueLength' -Value ([uint32]65536) -PropertyType DWord -Force) }
        Write-Step 'Tcpip6\Parameters\MaxLoopbackQueueLength' '65536' 'OK'
    }
    if ($tcpipSupported.Contains('TcpAckFrequency') -and (Test-Path -LiteralPath $script:IfaceRoot)) {
        if (-not $DryRun) {
            foreach ($sub in @(Get-ChildItem -LiteralPath $script:IfaceRoot -ErrorAction SilentlyContinue)) {
                [void](New-ItemProperty -LiteralPath $sub.PSPath -Name 'TcpAckFrequency' -Value ([uint32]1) -PropertyType DWord -Force)
                [void](New-ItemProperty -LiteralPath $sub.PSPath -Name 'TcpDelAckTicks' -Value ([uint32]0) -PropertyType DWord -Force)
            }
        }
        Write-Step 'Tcpip\Interfaces\*\TcpAckFrequency & TcpDelAckTicks' '1 & 0 (no 40ms Nagle+Delayed-ACK stalls)' 'OK'
    }
    $tcpipregSvc = Get-Service -Name 'tcpipreg' -ErrorAction SilentlyContinue
    if ($null -ne $tcpipregSvc -and -not $DryRun) {
        if ($tcpipregSvc.StartType -ne 'Automatic') { Set-Service -Name 'tcpipreg' -StartupType Automatic }
        if ($tcpipregSvc.Status -ne 'Running')      { Start-Service -Name 'tcpipreg' }
        Write-Step 'Service tcpipreg' 'Running (Automatic)' 'OK'
    }
    Set-VerifiedRegistryMap -RegPath $script:AfdPath -LabelPrefix 'AFD\Parameters' -DriverLabel 'afd.sys' -DesiredMap $script:DesiredAfdParams -SupportedSet $afdSupported
}
function Set-DockerAndWslNetworkOptimizations {
    Write-Section '4. Docker & WSL2 Virtual Switch (vEthernet) and Linux Kernel Sysctl Tuning'
    if ($SkipDockerWslTuning) { Write-Step 'Docker & WSL2 Network Tuning' 'Skipped via -SkipDockerWslTuning' 'SKIP'; return }
    $vSwitches = @(Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' -and ($_.Name -match '^vEthernet|WSL' -or $_.InterfaceDescription -match 'Hyper-V Virtual Ethernet|WSL') })
    foreach ($vNic in $vSwitches) {
        if (-not $DryRun) {
            try { Disable-NetAdapterRsc -Name $vNic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue } catch { }
            foreach ($vp in @(Get-NetAdapterAdvancedProperty -Name $vNic.Name -AllProperties -ErrorAction SilentlyContinue)) {
                if ($vp.RegistryKeyword -in @('*RscIPv4', '*RscIPv6') -and (Get-ScalarRegistryValue $vp) -ne '0') {
                    try { Set-NetAdapterAdvancedProperty -InputObject $vp -RegistryValue '0' -NoRestart -ErrorAction Stop } catch { }
                }
            }
        }
        Write-Step "Virtual Switch '$($vNic.Name)'" 'Disabled RSC coalescing for low-latency Docker/WSL2 bridge' 'OK'
    }
    $sysctlCmd = 'sysctl -w net.ipv4.ip_local_port_range="10000 65534" net.ipv4.tcp_tw_reuse=1 net.ipv4.tcp_fin_timeout=15 net.core.somaxconn=65535 net.core.netdev_max_backlog=65536 net.core.rmem_max=16777216 net.core.wmem_max=16777216 >/dev/null 2>&1'
    Invoke-WslSysctl -SysctlCommand $sysctlCmd -SummaryText 'ports=10000..65534, tw_reuse=1, somaxconn=65535, rmem/wmem=16MB'
}
function Resolve-OptimalRingBufferValue($Prop, [int64]$Ceiling = 4096) {
    if ($null -ne $Prop.NumericParameterMaxValue -and [int64]$Prop.NumericParameterMaxValue -gt 0) {
        $maxVal = [int64]$Prop.NumericParameterMaxValue
        $minVal = if ($null -ne $Prop.NumericParameterMinValue) { [int64]$Prop.NumericParameterMinValue } else { 0L }
        $step   = if ($null -ne $Prop.NumericParameterStepValue -and [int64]$Prop.NumericParameterStepValue -gt 1) { [int64]$Prop.NumericParameterStepValue } else { 1L }
        $target = [Math]::Max($minVal, [Math]::Min($Ceiling, $maxVal))
        if ($step -gt 1 -and $target -gt $minVal) { $target = $minVal + [int64]([Math]::Floor(($target - $minVal) / $step) * $step) }
        return $target
    }
    if ($null -ne $Prop.ValidRegistryValues) {
        $nums = @($Prop.ValidRegistryValues | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [int64]$_ } | Sort-Object)
        if ($nums.Count -gt 0) {
            $le = @($nums | Where-Object { $_ -le $Ceiling })
            return $(if ($le.Count -gt 0) { $le[-1] } else { $nums[0] })
        }
    }
    return $null
}
function Get-OptimalNicPropertyValue($Prop, [int]$RssMaxProc = 8) {
    $kw = [string]$Prop.RegistryKeyword
    $desired = $null
    if ($kw -in ($script:RingBufferKeywords + @('*NumRSSQueues', '*MaxRssProcessors'))) {
        $ceiling = if ($kw -in $script:RingBufferKeywords) { 4096L } else { [int64]$RssMaxProc }
        $targetVal = Resolve-OptimalRingBufferValue -Prop $Prop -Ceiling $ceiling
        if ($null -ne $targetVal) { $desired = "$targetVal" }
    } elseif ($kw -in $script:ZeroCoalesceAndPowerKeywords) {
        if ([int64]($Prop.NumericParameterMinValue ?? 0) -le 0) { $desired = '0' }
    } elseif ($kw -in $script:InterruptModKeywords) {
        $desired = if ($InterruptModerationMode -eq 'Off') { '0' } elseif ($kw -in @('ITR', '*ITR', '*InterruptModerationRate')) { '200' } else { '1' }
    }
    if ($null -eq $desired) { return $null }
    if ($null -ne $Prop.ValidRegistryValues -and $Prop.ValidRegistryValues -notcontains $desired) { return $null }
    return $desired
}
function Set-NicAdvancedProperties([string]$NicName, [hashtable]$ByKw, [int]$RssMaxProc) {
    $changed = $false
    $ceilKws = $script:RingBufferKeywords + @('*NumRSSQueues', '*MaxRssProcessors')
    foreach ($kw in ($ceilKws + $script:ZeroCoalesceAndPowerKeywords + $script:InterruptModKeywords)) {
        if (-not $ByKw.ContainsKey($kw)) { continue }
        $prop = $ByKw[$kw]; $curStr = Get-ScalarRegistryValue $prop
        $desired = Get-OptimalNicPropertyValue -Prop $prop -RssMaxProc $RssMaxProc
        $skip = ($null -eq $desired) -or ($desired -eq $curStr) -or ($kw -in $ceilKws -and ($desired -as [int64]) -le ($curStr -as [int64]))
        if (-not $skip) {
            try { Set-NetAdapterAdvancedProperty -InputObject $prop -RegistryValue $desired -NoRestart -ErrorAction Stop; $changed = $true; Write-Step "NIC '$NicName' $kw" "$curStr -> $desired" 'OK' } catch { }
        } elseif ($kw -in $script:RingBufferKeywords) {
            Write-Step "NIC '$NicName' $kw" "$curStr (already optimal)" 'OK'
        }
    }
    return $changed
}
function Set-SingleAdapterOptimalSettings($Nic, [int]$LogicalCores) {
    if ($DryRun) { Write-Step "NIC '$($Nic.Name)'" 'DryRun: Apply optimal offloads, RSS & ring buffer properties' 'OK'; return }
    try {
        Disable-NetAdapterRsc -Name $Nic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue
        Disable-NetAdapterPowerManagement -Name $Nic.Name -NoRestart -ErrorAction SilentlyContinue
        Enable-NetAdapterChecksumOffload -Name $Nic.Name -IpIPv4 -TcpIPv4 -TcpIPv6 -UdpIPv4 -UdpIPv6 -NoRestart -ErrorAction SilentlyContinue
        if (Get-Command 'Enable-NetAdapterUso' -ErrorAction SilentlyContinue)  { Enable-NetAdapterUso -Name $Nic.Name -IPv4 -IPv6 -NoRestart -ErrorAction SilentlyContinue }
        if (Get-Command 'Disable-NetAdapterUro' -ErrorAction SilentlyContinue) { Disable-NetAdapterUro -Name $Nic.Name -NoRestart -ErrorAction SilentlyContinue }
        $rssMaxProc = [Math]::Min(16, [Math]::Max(2, $LogicalCores))
        $rssBaseProc = if ($LogicalCores -ge 8) { 2 } else { 0 }
        Enable-NetAdapterRss -Name $Nic.Name -NoRestart -ErrorAction SilentlyContinue
        Set-NetAdapterRss -Name $Nic.Name -Enabled $true -Profile ClosestStatic -BaseProcessorNumber $rssBaseProc -MaxProcessors $rssMaxProc -NoRestart -ErrorAction SilentlyContinue
    } catch { }
    $byKw = @{}
    foreach ($p in @(Get-NetAdapterAdvancedProperty -Name $Nic.Name -AllProperties -ErrorAction SilentlyContinue)) {
        if ($null -ne $p.RegistryKeyword) { $byKw[$p.RegistryKeyword] = $p }
    }
    if (Set-NicAdvancedProperties -NicName $Nic.Name -ByKw $byKw -RssMaxProc ([Math]::Min(16, [Math]::Max(2, $LogicalCores)))) {
        Restart-NetAdapter -Name $Nic.Name -ErrorAction SilentlyContinue
        Write-Step "NIC '$($Nic.Name)' Restarted" 'Applied hardware ring/coalescing/RSS changes' 'OK'
    } else {
        Write-Step "NIC '$($Nic.Name)'" 'All supported hardware properties already optimal' 'OK'
    }
}
function Show-NetworkStatus([array]$AdaptersToInspect = @(Get-TargetPhysicalAdapters)) {
    Write-Section 'Current Network Stack Status vs. SandalCore Optimal Targets'
    foreach ($af in @('ipv4', 'ipv6')) {
        foreach ($proto in @('tcp', 'udp')) {
            $r = Get-DynamicPortRange -IpVersion $af -Protocol $proto
            $st = if ($r.StartPort -eq 10000 -and $r.NumberOfPorts -eq 55535) { 'OK' } else { 'WARN' }
            Write-Step "$($af.ToUpper()) $($proto.ToUpper()) Ephemeral Ports" "$($r.StartPort) .. $($r.StartPort + $r.NumberOfPorts - 1) ($($r.NumberOfPorts) ports; target: 10000..65534)" $st
        }
    }
    foreach ($sec in @(@{ Label = 'Tcpip'; Path = $script:TcpipPath; Map = $script:DesiredTcpipParams }, @{ Label = 'AFD'; Path = $script:AfdPath; Map = $script:DesiredAfdParams })) {
        $props = Get-ItemProperty -LiteralPath $sec.Path -ErrorAction SilentlyContinue
        foreach ($kv in $sec.Map.GetEnumerator()) {
            $cur = Get-JsonProp $props $kv.Key
            Write-Step "$($sec.Label)\$($kv.Key)" ("Current: {0} | Target: {1}" -f ($(if ($null -ne $cur) { $cur } else { '<default>' })), $kv.Value) ($(if ($null -ne $cur -and [int64]$cur -eq [int64]$kv.Value) { 'OK' } else { 'WARN' }))
        }
    }
    foreach ($nic in $AdaptersToInspect) {
        $parts = [System.Collections.Generic.List[string]]::new()
        foreach ($p in @(Get-NetAdapterAdvancedProperty -Name $nic.Name -AllProperties -ErrorAction SilentlyContinue)) {
            if ($p.RegistryKeyword -in @('*ReceiveBuffers', '*TransmitBuffers', '*InterruptModeration', '*RscIPv4', '*FlowControl', '*EEE', 'PowerDownPll')) {
                $parts.Add("$($p.RegistryKeyword)=$(Get-ScalarRegistryValue $p)")
            }
        }
        Write-Step "NIC '$($nic.Name)' ($($nic.LinkSpeed))" ($parts -join ', ') 'OK'
    }
    Write-Host "`n  Tip: Use -Status, -DryRun, -Restore (-Rollback), or -Revert to inspect, preview, restore backup, or reset to stock Windows defaults." -ForegroundColor Gray
}

if ($MyInvocation.InvocationName -ne '.') {
    if ($Rollback -and $Revert) {
        Write-Err "Cannot specify both -Rollback (-Restore) and -Revert simultaneously."
        exit 1
    }
    $plan = Resolve-RequestedExecutionPlan
    switch ($plan.Action) {
        'Quit'     { Write-Host "`n  Exited without making any changes." -ForegroundColor Gray; return }
        'Status'   { Show-NetworkStatus -AdaptersToInspect $plan.Adapters; return }
        'Rollback' { Invoke-NetworkRollback; return }
        'Revert'   { Invoke-NetworkRevert; return }
        'Apply' {
            Assert-Administrator -OperationName 'Network Stack Tuning'
            if (-not $DryRun) { Save-NetworkBackup -TargetAdapters $plan.Adapters }
            $logicalCores = [Environment]::ProcessorCount
            $loopbackWorkers = [Math]::Min(16, [Math]::Max(4, [int][Math]::Floor($logicalCores / 2)))
            Set-PortExclusionsAndDynamicRanges
            Set-LoopbackAndTcpUdpGlobals -LoopbackWorkers $loopbackWorkers
            Set-KernelDriverRegistrySettings
            Set-DockerAndWslNetworkOptimizations
            Write-Section '5. Multi-Vendor Physical NIC Optimization'
            if ($plan.Adapters.Count -eq 0) { Write-Step 'Physical NIC Tuning' 'Skipped (OS & Docker/WSL2 stack tuned only)' 'SKIP' }
            else { foreach ($nic in $plan.Adapters) { Set-SingleAdapterOptimalSettings -Nic $nic -LogicalCores $logicalCores } }
            Show-NetworkStatus -AdaptersToInspect $plan.Adapters
            Write-Host "`n[NOTE] Ephemeral port ranges (55,535 ports), port exclusions, active IP caches, TCP globals, and NIC properties are active immediately." -ForegroundColor Green
            Write-Host "       Reboot once before peak multi-thousand-bot benchmarks so afd.sys and tcpip.sys boot-time registry allocations take full effect." -ForegroundColor Yellow
            Write-Host "       To restore backup: .\scripts\tune_network_stack.ps1 -Restore (-Rollback) | To reset to stock Windows defaults: -Revert`n" -ForegroundColor Gray
            if (-not $Force -and (Test-IsInteractiveSession)) { [void](Read-Host 'Press Enter to exit') }
        }
    }
}
