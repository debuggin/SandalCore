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
    $exists = Test-Path -LiteralPath $ConfigPath
    $lines = if ($exists) { @(Get-Content -LiteralPath $ConfigPath -Encoding utf8) } else { @() }
    $keys = @{}
    $inWsl2 = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $inWsl2 = ($Matches[1].Trim().ToLowerInvariant() -eq 'wsl2')
            continue
        }
        if (-not $inWsl2 -or $line -match '^\s*[#;]' -or $line -notmatch '^\s*([^=#;\s]+)\s*=\s*(.*?)\s*$') {
            continue
        }
        $keys[$Matches[1].Trim()] = $Matches[2].Trim()
    }
    return [pscustomobject]@{
        Exists = $exists
        Lines  = [string[]]$lines
        Keys   = $keys
    }
}

function Set-WslSectionKey {
    [CmdletBinding()]
    param(
        [AllowEmptyCollection()][string[]]$Lines,
        [Parameter(Mandatory)][string]$Key,
        [Parameter(Mandatory)][string]$Value
    )
    $out = [System.Collections.Generic.List[string]]::new()
    $inWsl2 = $false
    $replaced = $false
    $pattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    foreach ($line in $Lines) {
        if ($inWsl2 -and -not $replaced -and $line -match '^\s*\[([^\]]+)\]\s*$') {
            $out.Add("$Key=$Value")
            $replaced = $true
        }
        if ($line -match '^\s*\[([^\]]+)\]\s*$') {
            $inWsl2 = ($Matches[1].Trim().ToLowerInvariant() -eq 'wsl2')
        }
        if ($inWsl2 -and $line -match $pattern) {
            $out.Add("$Key=$Value")
            $replaced = $true
            continue
        }
        $out.Add($line)
    }
    if (-not $replaced) {
        $out.Add("$Key=$Value")
    }
    return [string[]]$out.ToArray()
}

function Confirm-WslConfig {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$ConfigPath = (Join-Path $env:USERPROFILE '.wslconfig'),
        [scriptblock]$PromptReader = $null
    )
    $state = Get-WslSectionState -ConfigPath $ConfigPath
    $curNet = if ($state.Keys.ContainsKey('networkingMode')) { $state.Keys['networkingMode'] } else { 'mirrored' }
    $curLoop = if ($state.Keys.ContainsKey('hostAddressLoopback')) { $state.Keys['hostAddressLoopback'] } else { 'true' }
    $netVal = Read-SandalConfigValue -Key 'networkingMode' -CurrentValue $curNet -PromptReader $PromptReader
    $loopVal = Read-SandalConfigValue -Key 'hostAddressLoopback' -CurrentValue $curLoop -PromptReader $PromptReader
    $hadNet = $state.Keys.ContainsKey('networkingMode') -and ($state.Keys['networkingMode'] -ceq $netVal)
    $hadLoop = $state.Keys.ContainsKey('hostAddressLoopback') -and ($state.Keys['hostAddressLoopback'] -ceq $loopVal)
    $changed = (-not $state.Exists) -or (-not $hadNet) -or (-not $hadLoop)
    if ($changed -and $PSCmdlet.ShouldProcess($ConfigPath, 'Merge [wsl2] configuration')) {
        $lines = $state.Lines
        if (-not ($lines | Where-Object { $_ -match '^\s*\[wsl2\]\s*$' })) {
            $lines = @($lines) + @('[wsl2]')
        }
        $lines = Set-WslSectionKey -Lines $lines -Key 'networkingMode' -Value $netVal
        $lines = Set-WslSectionKey -Lines $lines -Key 'hostAddressLoopback' -Value $loopVal
        $parentDir = Split-Path -Path $ConfigPath -Parent
        if (-not [string]::IsNullOrWhiteSpace($parentDir) -and -not (Test-Path -LiteralPath $parentDir)) {
            $null = New-Item -ItemType Directory -Path $parentDir -Force
        }
        $tmpPath = "$ConfigPath.tmp.$PID"
        Set-Content -LiteralPath $tmpPath -Value $lines -Encoding utf8
        Move-Item -LiteralPath $tmpPath -Destination $ConfigPath -Force
    }
    return [pscustomobject]@{
        Changed             = [bool]$changed
        NetworkingMode      = [string]$netVal
        HostAddressLoopback = [string]$loopVal
        Path                = [string]$ConfigPath
        ShutdownAdvised     = [bool]$changed
    }
}
