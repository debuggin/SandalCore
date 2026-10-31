Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Get-Command Confirm-SandalCerts -ErrorAction SilentlyContinue)) {
    $certsCore = Join-Path $PSScriptRoot 'certs.ps1'
    if (Test-Path -LiteralPath $certsCore) {
        . $certsCore
    }
}

function Build-SandalSanString {
    [CmdletBinding()]
    param(
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = ''
    )
    $san = 'DNS:localhost,DNS:*.localhost,IP:127.0.0.1'
    if (-not [string]::IsNullOrWhiteSpace($AdvertiseIp) -and $AdvertiseIp -ne '127.0.0.1') {
        $san += ",IP:$AdvertiseIp"
    }
    if (-not [string]::IsNullOrWhiteSpace($CustomDomain)) {
        $san += ",DNS:$CustomDomain"
    }
    return $san
}

function Protect-SandalPrivateKey {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$KeyPath
    )
    if (-not (Test-Path -LiteralPath $KeyPath)) {
        return
    }
    if (-not $IsWindows) {
        & chmod 0600 $KeyPath 2>$null
        return
    }
    try {
        $acl = Get-Acl -LiteralPath $KeyPath -ErrorAction Stop
        $acl.SetAccessRuleProtection($false, $true)
        Set-Acl -LiteralPath $KeyPath -AclObject $acl -ErrorAction SilentlyContinue
    } catch {
        $null = $_
    }
}

function New-SandalLocalCaCerts {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$CertsDir = 'certs',
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = '',
        [int]$ValidityDays = 365,
        [scriptblock]$OpenSslRunner = $null
    )
    if ($ValidityDays -gt 397 -or $ValidityDays -lt 1) {
        throw [System.ArgumentOutOfRangeException]::new('ValidityDays', $ValidityDays, "ValidityDays ($ValidityDays) exceeds the 397-day maximum (STD-03).")
    }
    New-Item -ItemType Directory -Path $CertsDir -Force | Out-Null
    $sanString = Build-SandalSanString -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain
    $caKey = Join-Path $CertsDir 'ca.key'
    $caCert = Join-Path $CertsDir 'ca.crt'
    $srvKey = Join-Path $CertsDir 'server.key'
    $srvCsr = Join-Path $CertsDir 'server.csr'
    $srvCert = Join-Path $CertsDir 'server.crt'
    $extFile = Join-Path $CertsDir 'server-ext.cnf'
    if ($PSCmdlet.ShouldProcess($CertsDir, 'Generate Mode A Local ECDSA P-256 Root CA and SAN Leaf certificates')) {
        $null = Invoke-SandalOpenSsl -Arguments @('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', $caKey) -OpenSslRunner $OpenSslRunner
        $null = Invoke-SandalOpenSsl -Arguments @('req', '-x509', '-new', '-key', $caKey, '-sha256', '-days', '3650', '-subj', '/CN=SandalCore Local Root CA/O=SandalCore', '-addext', 'basicConstraints=critical,CA:TRUE,pathlen:0', '-addext', 'keyUsage=critical,keyCertSign,cRLSign', '-out', $caCert) -OpenSslRunner $OpenSslRunner
        $null = Invoke-SandalOpenSsl -Arguments @('ecparam', '-name', 'prime256v1', '-genkey', '-noout', '-out', $srvKey) -OpenSslRunner $OpenSslRunner
        $null = Invoke-SandalOpenSsl -Arguments @('req', '-new', '-key', $srvKey, '-subj', '/CN=localhost/O=SandalCore', '-out', $srvCsr) -OpenSslRunner $OpenSslRunner
        $extLines = @(
            'basicConstraints=critical,CA:FALSE',
            'keyUsage=critical,digitalSignature,keyEncipherment',
            'extendedKeyUsage=serverAuth,clientAuth',
            "subjectAltName=$sanString"
        )
        Set-Content -LiteralPath $extFile -Value $extLines -Encoding ascii
        $null = Invoke-SandalOpenSsl -Arguments @('x509', '-req', '-in', $srvCsr, '-CA', $caCert, '-CAkey', $caKey, '-CAcreateserial', '-out', $srvCert, '-days', "$ValidityDays", '-sha256', '-extfile', $extFile) -OpenSslRunner $OpenSslRunner
        foreach ($tmp in @($srvCsr, $extFile, (Join-Path $CertsDir 'ca.srl'))) {
            if (Test-Path -LiteralPath $tmp) {
                Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            }
        }
        Protect-SandalPrivateKey -KeyPath $caKey
        Protect-SandalPrivateKey -KeyPath $srvKey
    }
    return [pscustomobject]@{
        Mode           = 'local-ca'
        CaCertPath     = $caCert
        CaKeyPath      = $caKey
        ServerCertPath = $srvCert
        ServerKeyPath  = $srvKey
        SubjectAltName = $sanString
        ValidityDays   = $ValidityDays
        TrustStoreMode = 'X509ChainPolicy.CustomTrustStore'
    }
}

function Test-SandalCustomCertBundle {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$FullChainPath,
        [Parameter(Mandatory = $true)][string]$PrivKeyPath,
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = '',
        [scriptblock]$OpenSslRunner = $null
    )
    return Confirm-SandalCerts -ServerCertPath $FullChainPath -ServerKeyPath $PrivKeyPath -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain -OpenSslRunner $OpenSslRunner -SkipCaCheck
}

function Ensure-SandalCerts {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [string]$CertsDir = 'certs',
        [ValidateSet('local-ca', 'acme', 'custom')][string]$Mode = 'local-ca',
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = '',
        [switch]$ForceRegenerate,
        [scriptblock]$OpenSslRunner = $null
    )
    if ($Mode -eq 'local-ca') {
        if (-not $ForceRegenerate) {
            $check = Confirm-SandalCerts -CertsDir $CertsDir -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain -OpenSslRunner $OpenSslRunner
            if ($check.Valid) {
                return [pscustomobject]@{ Action = 'Reused'; Mode = $Mode; Validation = $check; Certs = $null }
            }
        }
        $certs = New-SandalLocalCaCerts -CertsDir $CertsDir -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain -OpenSslRunner $OpenSslRunner
        return [pscustomobject]@{ Action = 'Generated'; Mode = $Mode; Validation = $null; Certs = $certs }
    }
    $fullChainCandidate = Join-Path $CertsDir 'fullchain.pem'
    $privKeyCandidate = Join-Path $CertsDir 'privkey.pem'
    $chain = if (Test-Path -LiteralPath $fullChainCandidate) { $fullChainCandidate } else { Join-Path $CertsDir 'server.crt' }
    $key = if (Test-Path -LiteralPath $privKeyCandidate) { $privKeyCandidate } else { Join-Path $CertsDir 'server.key' }
    $check = Test-SandalCustomCertBundle -FullChainPath $chain -PrivKeyPath $key -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain -OpenSslRunner $OpenSslRunner
    if (-not $check.Valid) {
        throw "Custom/ACME certificate validation failed: $($check.Reason)"
    }
    return [pscustomobject]@{ Action = 'Validated'; Mode = $Mode; Validation = $check; Certs = $null }
}
