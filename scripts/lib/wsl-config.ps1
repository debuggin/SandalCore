#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-SandalConfigValue {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Key,
        [Parameter(Mandatory)][AllowEmptyString()][string]$CurrentValue,
        [scriptblock]$PromptReader = $null
    )
    $promptText = "WSL2 $Key [current: $CurrentValue] -- press Enter to keep, or type new value:"
    $raw = if ($null -ne $PromptReader) {
        & $PromptReader $promptText $Key $CurrentValue
    } else {
        Read-Host -Prompt $promptText
    }
    if ($null -eq $raw -or [string]::IsNullOrWhiteSpace("$raw")) {
        return $CurrentValue
    }
    return ("$raw").Trim()
}

function Get-WslSectionState {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$ConfigPath
    )
    $exists = Test-Path -LiteralPath $ConfigPath -PathType Leaf
    $rawText = if ($exists) { [System.IO.File]::ReadAllText($ConfigPath, [System.Text.Encoding]::UTF8) } else { '' }
    $newline = if ($rawText.Contains("`r`n")) { "`r`n" } elseif ($rawText.Contains("`n")) { "`n" } else { [Environment]::NewLine }
    $hasTrailingNewline = if ($rawText.Length -gt 0) { $rawText.EndsWith("`n") } else { $true }
    $lines = if ($rawText.Length -eq 0) { @() } else { [regex]::Split($rawText, '\r?\n') }
    if ($hasTrailingNewline -and $lines.Count -gt 0 -and $lines[-1] -eq '') {
        $lines = if ($lines.Count -eq 1) { @() } else { $lines[0..($lines.Count - 2)] }
    }
    $sections = [hashtable]::new([System.StringComparer]::OrdinalIgnoreCase)
    $keys = [hashtable]::new([System.StringComparer]::OrdinalIgnoreCase)
    $activeSection = ''
    foreach ($line in $lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(?:[#;].*)?$') {
            $activeSection = $Matches[1].Trim()
            if (-not $sections.ContainsKey($activeSection)) {
                $sections[$activeSection] = [hashtable]::new([System.StringComparer]::OrdinalIgnoreCase)
            }
            continue
        }
        if ([string]::IsNullOrWhiteSpace($activeSection) -or $line -match '^\s*[#;]' -or $line -notmatch '^(\s*)([^=#;\s]+)(\s*=\s*)([^#;]*?)(\s*[#;].*)?$') {
            continue
        }
        $k = $Matches[2].Trim()
        $v = $Matches[4].Trim()
        $sections[$activeSection][$k] = $v
        $keys[$k] = $v
    }
    return [pscustomobject]@{
        Exists             = $exists
        Lines              = [string[]]$lines
        Keys               = $keys
        Sections           = $sections
        Newline            = $newline
        HasTrailingNewline = $hasTrailingNewline
    }
}

function Update-WslTargetSectionLines {
    param(
        [AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][string]$Section,
        [Parameter(Mandatory)][string]$Key,
        [Parameter(Mandatory)][string]$Value
    )
    $out = [System.Collections.Generic.List[string]]::new()
    $inTarget = $false
    $replaced = $false
    $indent = ''
    $eqSep = '='
    $lastContentIdx = -1
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(?:[#;].*)?$') {
            $inTarget = ($Matches[1].Trim() -ieq $Section)
            $out.Add($line)
            if ($inTarget) { $lastContentIdx = $out.Count - 1 }
            continue
        }
        if ($inTarget -and $line -notmatch '^\s*[#;]' -and $line -match '^(\s*)([^=#;\s]+)(\s*=\s*)([^#;]*?)(\s*[#;].*)?$') {
            $indent = $Matches[1]
            $eqSep = $Matches[3]
            if ($Matches[2].Trim() -ieq $Key) {
                if ($replaced) { continue }
                $curVal = $Matches[4].Trim()
                $commentSuffix = if ($null -ne $Matches[5]) { $Matches[5] } else { '' }
                $newLine = if ($curVal -ceq $Value) { $line } else { "$($Matches[1])$($Matches[2])$($Matches[3])$Value$commentSuffix" }
                $out.Add($newLine)
                $lastContentIdx = $out.Count - 1
                $replaced = $true
                continue
            }
        }
        $out.Add($line)
        if ($inTarget -and -not [string]::IsNullOrWhiteSpace($line)) {
            $lastContentIdx = $out.Count - 1
        }
    }
    if (-not $replaced -and $lastContentIdx -ge 0) {
        $out.Insert($lastContentIdx + 1, "$indent$Key$eqSep$Value")
    }
    return [string[]]$out.ToArray()
}

function Set-WslSectionKey {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][string]$Key,
        [Parameter(Mandatory)][string]$Value,
        [string]$Section = 'wsl2'
    )
    $secTrim = $Section.Trim()
    $hasSection = $false
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(?:[#;].*)?$' -and ($Matches[1].Trim() -ieq $secTrim)) {
            $hasSection = $true
            break
        }
    }
    if ($hasSection) {
        return Update-WslTargetSectionLines -Lines $Lines -Section $secTrim -Key $Key -Value $Value
    }
    $out = [System.Collections.Generic.List[string]]::new()
    if ($null -ne $Lines -and $Lines.Count -gt 0) {
        $out.AddRange([string[]]$Lines)
        if (-not [string]::IsNullOrWhiteSpace($out[$out.Count - 1])) {
            $out.Add('')
        }
    }
    $out.Add("[$secTrim]")
    $out.Add("$Key=$Value")
    return [string[]]$out.ToArray()
}

function Remove-WslSectionKey {
    param(
        [AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][string]$Section,
        [Parameter(Mandatory)][string]$Key
    )
    $out = [System.Collections.Generic.List[string]]::new()
    $inTarget = $false
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*(?:[#;].*)?$') {
            $inTarget = ($Matches[1].Trim() -ieq $Section)
            $out.Add($line)
            continue
        }
        if ($inTarget -and $line -notmatch '^\s*[#;]' -and $line -match '^(\s*)([^=#;\s]+)(\s*=\s*)([^#;]*?)(\s*[#;].*)?$' -and ($Matches[2].Trim() -ieq $Key)) {
            continue
        }
        $out.Add($line)
    }
    return [string[]]$out.ToArray()
}

function Get-WslSectionKeyValue {
    param([pscustomobject]$State, [string]$Section, [string]$Key, [string]$DefaultValue)
    if ($State.Sections.ContainsKey($Section) -and $State.Sections[$Section].ContainsKey($Key)) {
        return [string]$State.Sections[$Section][$Key]
    }
    return $DefaultValue
}

function Test-WslSectionKeyExact {
    param([pscustomobject]$State, [string]$Section, [string]$Key, [string]$ExpectedValue)
    return ($State.Sections.ContainsKey($Section) -and $State.Sections[$Section].ContainsKey($Key) -and ($State.Sections[$Section][$Key] -ceq $ExpectedValue))
}

function Confirm-WslConfig {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$ConfigPath = (Join-Path $(if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }) '.wslconfig'),
        [scriptblock]$PromptReader = $null
    )
    $state = Get-WslSectionState -ConfigPath $ConfigPath
    $curInst = Get-WslSectionKeyValue -State $state -Section 'general' -Key 'instanceIdleTimeout' -DefaultValue '-1'
    $curNet = Get-WslSectionKeyValue -State $state -Section 'wsl2' -Key 'networkingMode' -DefaultValue 'mirrored'
    $curLoop = Get-WslSectionKeyValue -State $state -Section 'experimental' -Key 'hostAddressLoopback' -DefaultValue (
        Get-WslSectionKeyValue -State $state -Section 'wsl2' -Key 'hostAddressLoopback' -DefaultValue 'true'
    )
    $curIdle = Get-WslSectionKeyValue -State $state -Section 'wsl2' -Key 'vmIdleTimeout' -DefaultValue '-1'
    $instVal = Read-SandalConfigValue -Key 'instanceIdleTimeout' -CurrentValue $curInst -PromptReader $PromptReader
    $netVal = Read-SandalConfigValue -Key 'networkingMode' -CurrentValue $curNet -PromptReader $PromptReader
    $loopVal = Read-SandalConfigValue -Key 'hostAddressLoopback' -CurrentValue $curLoop -PromptReader $PromptReader
    $idleVal = Read-SandalConfigValue -Key 'vmIdleTimeout' -CurrentValue $curIdle -PromptReader $PromptReader
    $specs = @(
        @{ Section = 'general';      Key = 'instanceIdleTimeout'; Value = $instVal; Reason = 'disable WSL2 distro idle shutdown' },
        @{ Section = 'wsl2';         Key = 'networkingMode';      Value = $netVal;  Reason = 'enable WSL2 mirrored localhost networking' },
        @{ Section = 'wsl2';         Key = 'vmIdleTimeout';       Value = $idleVal; Reason = 'disable WSL2 utility VM idle shutdown' },
        @{ Section = 'experimental'; Key = 'hostAddressLoopback'; Value = $loopVal; Reason = 'enable host-to-container loopback routing' }
    )
    $hasStaleWsl2Loopback = $state.Sections.ContainsKey('wsl2') -and $state.Sections['wsl2'].ContainsKey('hostAddressLoopback')
    $pending = @($specs | Where-Object { -not (Test-WslSectionKeyExact -State $state -Section $_.Section -Key $_.Key -ExpectedValue $_.Value) })
    $changed = (-not $state.Exists) -or ($pending.Count -gt 0) -or $hasStaleWsl2Loopback
    if (-not $changed) {
        Write-Information "INFO: WSL2 config ($ConfigPath) already has [general] instanceIdleTimeout=$instVal, [wsl2] networkingMode=$netVal, vmIdleTimeout=$idleVal, and [experimental] hostAddressLoopback=$loopVal; keeping existing file unchanged." -InformationAction Continue
    } elseif ($PSCmdlet.ShouldProcess($ConfigPath, 'Merge [general], [wsl2], and [experimental] configuration')) {
        $lines = $state.Lines
        if ($hasStaleWsl2Loopback) {
            Write-Information "INFO: Removing deprecated [wsl2] hostAddressLoopback from $ConfigPath (migrated to [experimental])" -InformationAction Continue
            $lines = Remove-WslSectionKey -Lines $lines -Section 'wsl2' -Key 'hostAddressLoopback'
        }
        foreach ($item in $pending) {
            Write-Information "INFO: Configuring $ConfigPath -> [$($item.Section)] $($item.Key)=$($item.Value) ($($item.Reason))" -InformationAction Continue
            $lines = Set-WslSectionKey -Lines $lines -Section $item.Section -Key $item.Key -Value $item.Value
        }
        $parentDir = Split-Path -Path $ConfigPath -Parent
        if (-not [string]::IsNullOrWhiteSpace($parentDir) -and -not (Test-Path -LiteralPath $parentDir)) {
            $null = New-Item -ItemType Directory -Path $parentDir -Force
        }
        $body = ($lines -join $state.Newline) + $(if ($state.HasTrailingNewline) { $state.Newline } else { '' })
        $tmpPath = "$ConfigPath.tmp.$PID"
        [System.IO.File]::WriteAllText($tmpPath, $body, [System.Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $tmpPath -Destination $ConfigPath -Force
    }
    return [pscustomobject]@{
        Changed             = [bool]$changed
        InstanceIdleTimeout = [string]$instVal
        NetworkingMode      = [string]$netVal
        HostAddressLoopback = [string]$loopVal
        VmIdleTimeout       = [string]$idleVal
        Path                = [string]$ConfigPath
        ShutdownAdvised     = [bool]$changed
    }
}
