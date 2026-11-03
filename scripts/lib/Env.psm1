#Requires -Version 7.2
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function New-SandalCsprngSecret {
    [CmdletBinding()]
    param([ValidateRange(16, 128)][int]$ByteLength = 32)
    $bytes = [System.Security.Cryptography.RandomNumberGenerator]::GetBytes($ByteLength)
    return [System.Convert]::ToHexString($bytes).ToLowerInvariant()
}

function Get-SandalEnvMap {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$EnvPath)
    $map = [ordered]@{}
    if (-not (Test-Path -LiteralPath $EnvPath -PathType Leaf)) {
        return $map
    }
    foreach ($line in [System.IO.File]::ReadAllLines($EnvPath, [System.Text.Encoding]::UTF8)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -gt 0 -and -not $trimmed.StartsWith('#') -and $trimmed -match '^([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$') {
            $map[$Matches[1]] = $Matches[2].Trim()
        }
    }
    return $map
}

function Set-SandalSecretAcl {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$LiteralPath)
    if ($IsWindows) {
        $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
        $null = & icacls $LiteralPath /inheritance:r /grant:r "${identity}:(F)" 2>&1
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to apply restrictive ACL on '$LiteralPath' (icacls exit $LASTEXITCODE)."
        }
    } else {
        $null = & chmod 0600 $LiteralPath
    }
    return [pscustomobject]@{ Path = $LiteralPath; Mode = '0600'; InheritanceDisabled = $true }
}

function Initialize-SandalEnv {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$EnvPath,
        [string]$TemplatePath = '',
        [string[]]$SecretKeys = @('POSTGRES_PASSWORD', 'AOC_SERVICE_TOKEN', 'SANDAL_SECRET_KEY'),
        [switch]$Force
    )
    $parent = Split-Path -Parent $EnvPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        $null = New-Item -ItemType Directory -Path $parent -Force
    }
    $sourcePath = if (Test-Path -LiteralPath $EnvPath -PathType Leaf) { $EnvPath } elseif ($TemplatePath -and (Test-Path -LiteralPath $TemplatePath -PathType Leaf)) { $TemplatePath } else { '' }
    $inLines = if ($sourcePath) { [System.IO.File]::ReadAllLines($sourcePath, [System.Text.Encoding]::UTF8) } else { @() }
    $outLines = [System.Collections.Generic.List[string]]::new()
    $seenSecrets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $generated = [System.Collections.Generic.List[string]]::new()

    foreach ($line in $inLines) {
        if ($line -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=(.*)$' -and ($Matches[1] -in $SecretKeys)) {
            $key = $Matches[1]
            $val = $Matches[2].Trim()
            $null = $seenSecrets.Add($key)
            if ($Force -or [string]::IsNullOrWhiteSpace($val) -or $val.StartsWith('CHANGE_ME')) {
                $val = New-SandalCsprngSecret -ByteLength 32
                $generated.Add($key)
            }
            $outLines.Add("${key}=${val}")
        } else {
            $outLines.Add($line)
        }
    }
    foreach ($key in $SecretKeys) {
        if (-not $seenSecrets.Contains($key)) {
            $outLines.Add("${key}=$(New-SandalCsprngSecret -ByteLength 32)")
            $generated.Add($key)
        }
    }
    $tmpPath = "${EnvPath}.tmp.${PID}"
    [System.IO.File]::WriteAllLines($tmpPath, $outLines, [System.Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tmpPath -Destination $EnvPath -Force
    $aclInfo = Set-SandalSecretAcl -LiteralPath $EnvPath
    return [pscustomobject]@{ EnvPath = $EnvPath; GeneratedKeys = $generated.ToArray(); Acl = $aclInfo }
}

Export-ModuleMember -Function @(
    'New-SandalCsprngSecret',
    'Get-SandalEnvMap',
    'Set-SandalSecretAcl',
    'Initialize-SandalEnv'
)
