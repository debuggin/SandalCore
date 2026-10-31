Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-SandalOpenSsl {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [scriptblock]$OpenSslRunner = $null
    )
    if ($null -ne $OpenSslRunner) {
        $res = & $OpenSslRunner $Arguments
        if ($res -is [hashtable] -and $res.ContainsKey('ExitCode')) {
            return [pscustomobject]@{ ExitCode = [int]$res['ExitCode']; Output = [string]$res['Output'] }
        }
        if ($null -ne $res -and $null -ne $res.PSObject.Properties['ExitCode']) {
            return [pscustomobject]@{ ExitCode = [int]$res.ExitCode; Output = [string]$res.Output }
        }
        $outText = (($res | ForEach-Object { [string]$_ }) -join "`n")
        return [pscustomobject]@{ ExitCode = 0; Output = $outText }
    }
    $cmd = Get-Command openssl -ErrorAction SilentlyContinue | Select-Object -First 1
    $exe = if ($null -ne $cmd) {
        $cmd.Source
    } elseif (Test-Path -LiteralPath 'C:\Program Files\Git\usr\bin\openssl.exe') {
        'C:\Program Files\Git\usr\bin\openssl.exe'
    } elseif (Test-Path -LiteralPath 'C:\Program Files\Git\mingw64\bin\openssl.exe') {
        'C:\Program Files\Git\mingw64\bin\openssl.exe'
    } else {
        throw 'OpenSSL executable not found in PATH or Git for Windows.'
    }
    $raw = & $exe @Arguments 2>&1
    $exitCode = if ($null -ne $LASTEXITCODE) { [int]$LASTEXITCODE } else { 0 }
    $output = (($raw | ForEach-Object { [string]$_ }) -join "`n")
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function ConvertFrom-SandalOpenSslDate {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][string]$DateText
    )
    $clean = ($DateText -replace '^not(Before|After)=', '' -replace '\s+GMT$', '' -replace '\s+', ' ').Trim()
    $formats = @(
        'MMM d HH:mm:ss yyyy',
        'MMM dd HH:mm:ss yyyy',
        'yyyy-MM-ddTHH:mm:ssZ',
        'yyyy-MM-dd HH:mm:ss'
    )
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    $styles = [System.Globalization.DateTimeStyles]::AssumeUniversal -bor [System.Globalization.DateTimeStyles]::AdjustToUniversal
    $parsed = [datetimeoffset]::MinValue
    foreach ($fmt in $formats) {
        if ([datetimeoffset]::TryParseExact($clean, $fmt, $culture, $styles, [ref]$parsed)) {
            return $parsed
        }
    }
    return [datetimeoffset]::Parse($clean, $culture, $styles)
}

function Test-SandalX509SanMatch {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][AllowEmptyString()][string]$SanText,
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = ''
    )
    if ([string]::IsNullOrWhiteSpace($SanText)) {
        return $false
    }
    if ($SanText -notmatch '(?:^|[\s,])DNS:localhost(?:$|[\s,])') {
        return $false
    }
    if ($SanText -notmatch '(?:^|[\s,])(?:IP Address|IP):127\.0\.0\.1(?:$|[\s,])') {
        return $false
    }
    if (-not [string]::IsNullOrWhiteSpace($AdvertiseIp)) {
        $escapedIp = [regex]::Escape($AdvertiseIp.Trim())
        if ($SanText -notmatch "(?:^|[\s,])(?:IP Address|IP|DNS):$escapedIp(?:`$|[\s,])") {
            return $false
        }
    }
    if (-not [string]::IsNullOrWhiteSpace($CustomDomain)) {
        $escapedDomain = [regex]::Escape($CustomDomain.Trim())
        if ($SanText -notmatch "(?:^|[\s,])DNS:$escapedDomain(?:`$|[\s,])") {
            return $false
        }
    }
    return $true
}

function New-SandalCertValidationResult {
    [CmdletBinding()]
    param(
        [bool]$Valid,
        [string]$Reason,
        [double]$DaysRemaining = 0.0,
        [double]$TotalValidityDays = 0.0,
        [string]$SubjectAltName = '',
        [string]$BasicConstraints = '',
        [bool]$KeyMatches = $false
    )
    return [pscustomobject]@{
        Valid             = $Valid
        Reason            = $Reason
        DaysRemaining     = $DaysRemaining
        TotalValidityDays = $TotalValidityDays
        SubjectAltName    = $SubjectAltName
        BasicConstraints  = $BasicConstraints
        KeyMatches        = $KeyMatches
    }
}

function Confirm-SandalCerts {
    [CmdletBinding()]
    param(
        [string]$CertsDir = 'certs',
        [string]$AdvertiseIp = '127.0.0.1',
        [string]$CustomDomain = '',
        [scriptblock]$OpenSslRunner = $null,
        [string]$ServerCertPath = '',
        [string]$ServerKeyPath = '',
        [switch]$SkipCaCheck
    )
    $caPath = Join-Path $CertsDir 'ca.crt'
    $certPath = if ($ServerCertPath) { $ServerCertPath } else { Join-Path $CertsDir 'server.crt' }
    $keyPath = if ($ServerKeyPath) { $ServerKeyPath } else { Join-Path $CertsDir 'server.key' }
    if ($null -eq $OpenSslRunner) {
        $hasFiles = (Test-Path -LiteralPath $certPath) -and (Test-Path -LiteralPath $keyPath) -and ($SkipCaCheck -or (Test-Path -LiteralPath $caPath))
        if (-not $hasFiles) {
            return New-SandalCertValidationResult -Valid $false -Reason 'Missing certificate or key files'
        }
    }
    $certInfo = Invoke-SandalOpenSsl -Arguments @('x509', '-in', $certPath, '-noout', '-ext', 'subjectAltName,basicConstraints', '-dates') -OpenSslRunner $OpenSslRunner
    if ($certInfo.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($certInfo.Output)) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Missing or unreadable certificate files'
    }
    $out = $certInfo.Output
    $bc = if ($out -match 'CA:TRUE') { 'CA:TRUE' } elseif ($out -match 'CA:FALSE') { 'CA:FALSE' } else { '' }
    if ($out -notmatch 'CA:FALSE' -or $out -match 'CA:TRUE') {
        return New-SandalCertValidationResult -Valid $false -Reason 'Leaf certificate must have BasicConstraints CA:FALSE' -BasicConstraints $bc
    }
    $nbMatch = [regex]::Match($out, '(?m)^notBefore=([^\r\n]+)')
    $naMatch = [regex]::Match($out, '(?m)^notAfter=([^\r\n]+)')
    if (-not $nbMatch.Success -or -not $naMatch.Success) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Certificate missing validity dates' -BasicConstraints $bc
    }
    $notBefore = ConvertFrom-SandalOpenSslDate -DateText $nbMatch.Groups[1].Value
    $notAfter = ConvertFrom-SandalOpenSslDate -DateText $naMatch.Groups[1].Value
    $totalDays = [math]::Round(($notAfter - $notBefore).TotalDays, 2)
    $daysRemaining = [math]::Round(($notAfter - [datetimeoffset]::UtcNow).TotalDays, 2)
    if ($totalDays -gt 397.0 -or $totalDays -le 0.0) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Certificate total validity exceeds 397 days' -DaysRemaining $daysRemaining -TotalValidityDays $totalDays -BasicConstraints $bc
    }
    if ($daysRemaining -le 30.0) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Certificate has 30 days or fewer remaining' -DaysRemaining $daysRemaining -TotalValidityDays $totalDays -BasicConstraints $bc
    }
    if (-not (Test-SandalX509SanMatch -SanText $out -AdvertiseIp $AdvertiseIp -CustomDomain $CustomDomain)) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Certificate missing required Subject Alternative Name (SAN)' -DaysRemaining $daysRemaining -TotalValidityDays $totalDays -SubjectAltName $out -BasicConstraints $bc
    }
    $certPub = Invoke-SandalOpenSsl -Arguments @('x509', '-in', $certPath, '-noout', '-pubkey') -OpenSslRunner $OpenSslRunner
    $keyPub = Invoke-SandalOpenSsl -Arguments @('pkey', '-in', $keyPath, '-pubout') -OpenSslRunner $OpenSslRunner
    $keyMatch = ($certPub.ExitCode -eq 0) -and ($keyPub.ExitCode -eq 0) -and (-not [string]::IsNullOrWhiteSpace($certPub.Output)) -and ($certPub.Output.Trim() -eq $keyPub.Output.Trim())
    if (-not $keyMatch) {
        return New-SandalCertValidationResult -Valid $false -Reason 'Private key does not match certificate public key' -DaysRemaining $daysRemaining -TotalValidityDays $totalDays -SubjectAltName $out -BasicConstraints $bc -KeyMatches $false
    }
    Write-Information 'INFO: Valid SAN TLS certificates already present (>30d remaining); keeping existing certs' -InformationAction Continue
    return New-SandalCertValidationResult -Valid $true -Reason 'Valid' -DaysRemaining $daysRemaining -TotalValidityDays $totalDays -SubjectAltName $out -BasicConstraints $bc -KeyMatches $true
}

if (-not (Get-Command New-SandalLocalCaCerts -ErrorAction SilentlyContinue)) {
    $pkiModule = Join-Path $PSScriptRoot 'certs-pki.ps1'
    if (Test-Path -LiteralPath $pkiModule) {
        . $pkiModule
    }
}
