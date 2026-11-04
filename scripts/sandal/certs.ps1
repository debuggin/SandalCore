#Requires -Version 7.2
[CmdletBinding()]
param(
    [Alias('CertsDir')]
    [string]$OutputDir = '',
    [switch]$Force,
    [switch]$DryRun,
    [switch]$Json,
    [switch]$NoColor,
    [string]$RepoRoot = '',
    [string[]]$Arguments = @(),
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-CertsOptions {
    param(
        [string]$BaseOutDir,
        [bool]$BaseForce,
        [bool]$BaseDryRun,
        [bool]$BaseJson,
        [string[]]$RawArgs
    )
    $out = $BaseOutDir
    $frc = $BaseForce
    $dry = $BaseDryRun
    $jsn = $BaseJson
    for ($i = 0; $i -lt $RawArgs.Count; $i++) {
        $arg = $RawArgs[$i]
        switch -Regex ($arg) {
            '^(-OutputDir|--output-dir|-o|-CertsDir|--certs-dir)$' {
                if (($i + 1) -lt $RawArgs.Count) { $i++; $out = $RawArgs[$i] }
            }
            '^(-Force|--force|-f)$'   { $frc = $true }
            '^(-DryRun|--dry-run)$'   { $dry = $true }
            '^(-Json|--json)$'        { $jsn = $true }
        }
    }
    return [pscustomobject]@{ OutputDir = $out; Force = $frc; DryRun = $dry; Json = $jsn }
}

function Write-CertAndKeyPem {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
        [System.Security.Cryptography.ECDsa]$PrivateKey,
        [string]$TargetDir,
        [string]$Stem
    )
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    $certPem = [string]::new([System.Security.Cryptography.PemEncoding]::Write('CERTIFICATE', $Certificate.RawData)) + "`n"
    $keyPem = [string]::new([System.Security.Cryptography.PemEncoding]::Write('PRIVATE KEY', $PrivateKey.ExportPkcs8PrivateKey())) + "`n"
    $crtPath = Join-Path $TargetDir "${Stem}.crt"
    $pemPath = Join-Path $TargetDir "${Stem}.pem"
    $keyPath = Join-Path $TargetDir "${Stem}.key"
    [System.IO.File]::WriteAllText($crtPath, $certPem, $utf8NoBom)
    [System.IO.File]::WriteAllText($pemPath, $certPem, $utf8NoBom)
    [System.IO.File]::WriteAllText($keyPath, $keyPem, $utf8NoBom)
    $acl = Set-SandalSecretAcl -LiteralPath $keyPath
    return [pscustomobject]@{
        stem     = $Stem
        crt_path = $crtPath
        pem_path = $pemPath
        key_path = $keyPath
        key_acl  = $acl.Mode
    }
}

function New-DotNetLeafCertificate {
    param(
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$IssuerCert,
        [string]$CommonName,
        [string]$EkuOid,
        [string]$TargetDir,
        [string]$Stem
    )
    $leafKey = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    $leafCert = $null
    try {
        $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            "CN=$CommonName, O=SandalCore",
            $leafKey,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256
        )
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
        $kuFlags = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($kuFlags, $true))
        $oids = [System.Security.Cryptography.OidCollection]::new()
        $null = $oids.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.1'))
        $null = $oids.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.2'))
        if ($EkuOid -and $EkuOid -notin @('1.3.6.1.5.5.7.3.1', '1.3.6.1.5.5.7.3.2')) {
            $null = $oids.Add([System.Security.Cryptography.Oid]::new($EkuOid))
        }
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($oids, $false))
        $san = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
        $san.AddDnsName('localhost')
        $san.AddIpAddress([System.Net.IPAddress]::Parse('127.0.0.1'))
        $req.CertificateExtensions.Add($san.Build())
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($req.PublicKey, $false))
        $serial = [System.Security.Cryptography.RandomNumberGenerator]::GetBytes(16)
        $serial[0] = [byte]($serial[0] -band 0x7F)
        $now = [DateTimeOffset]::UtcNow.AddMinutes(-5)
        $leafCert = $req.Create($IssuerCert, $now, $now.AddDays(365), $serial)
        return (Write-CertAndKeyPem -Certificate $leafCert -PrivateKey $leafKey -TargetDir $TargetDir -Stem $Stem)
    } finally {
        if ($null -ne $leafCert) { $leafCert.Dispose() }
        $leafKey.Dispose()
    }
}

function Invoke-DotNetPkiGeneration {
    param([string]$TargetDir)
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        $null = New-Item -ItemType Directory -Path $TargetDir -Force
    }
    $caKey = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    $caCert = $null
    try {
        $caReq = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            'CN=SandalCore Local Root CA, O=SandalCore',
            $caKey,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256
        )
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $true, 0, $true))
        $caKu = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($caKu, $true))
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($caReq.PublicKey, $false))
        $now = [DateTimeOffset]::UtcNow.AddMinutes(-10)
        $caCert = $caReq.CreateSelfSigned($now, $now.AddDays(3650))
        $caRecord = Write-CertAndKeyPem -Certificate $caCert -PrivateKey $caKey -TargetDir $TargetDir -Stem 'ca'
        $srvRecord = New-DotNetLeafCertificate -IssuerCert $caCert -CommonName 'localhost' -EkuOid '1.3.6.1.5.5.7.3.1' -TargetDir $TargetDir -Stem 'server'
        $cliRecord = New-DotNetLeafCertificate -IssuerCert $caCert -CommonName 'sandal-client' -EkuOid '1.3.6.1.5.5.7.3.2' -TargetDir $TargetDir -Stem 'client'
        return @($caRecord, $srvRecord, $cliRecord)
    } finally {
        if ($null -ne $caCert) { $caCert.Dispose() }
        $caKey.Dispose()
    }
}

function Test-ExistingCertsValid {
    param([string]$TargetDir)
    foreach ($name in @('ca.crt', 'ca.key', 'server.crt', 'server.key', 'client.crt', 'client.key')) {
        if (-not (Test-Path -LiteralPath (Join-Path $TargetDir $name) -PathType Leaf)) {
            return $false
        }
    }
    $cert = $null
    try {
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPemFile(
            (Join-Path $TargetDir 'server.crt'),
            (Join-Path $TargetDir 'server.key')
        )
        $daysLeft = ($cert.NotAfter.ToUniversalTime() - [DateTime]::UtcNow).TotalDays
        return ($daysLeft -gt 30.0)
    } catch {
        return $false
    } finally {
        if ($null -ne $cert) { $cert.Dispose() }
    }
}

$inheritedJson = [bool](Get-Variable -Name AsJson -ValueOnly -ErrorAction SilentlyContinue)
$inheritedDryRun = [bool](Get-Variable -Name IsDryRun -ValueOnly -ErrorAction SilentlyContinue)
$inheritedNoColor = [bool](Get-Variable -Name UseNoColor -ValueOnly -ErrorAction SilentlyContinue)
$useNoColor = [bool]($NoColor -or $inheritedNoColor -or -not [string]::IsNullOrEmpty($env:NO_COLOR))

$rootPath = if ($RepoRoot) { [System.IO.Path]::GetFullPath($RepoRoot) } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
if (-not (Get-Command Set-SandalSecretAcl -ErrorAction SilentlyContinue)) {
    Import-Module (Join-Path $rootPath 'scripts\lib\SandalCore.psd1') -Force -DisableNameChecking
}

$defaultCertsDir = Join-Path $rootPath 'dist\certs'
$opts = Resolve-CertsOptions -BaseOutDir $OutputDir -BaseForce ([bool]$Force) -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -RawArgs (@($Arguments) + @($RemainingArgs))
$targetDir = if ($opts.OutputDir) { [System.IO.Path]::GetFullPath($opts.OutputDir) } else { $defaultCertsDir }

$action = 'dry_run'
$artifacts = @()
if (-not $opts.DryRun) {
    if (-not $opts.Force -and (Test-ExistingCertsValid -TargetDir $targetDir)) {
        $action = 'reused'
        foreach ($keyName in @('ca.key', 'server.key', 'client.key')) {
            $null = Set-SandalSecretAcl -LiteralPath (Join-Path $targetDir $keyName)
        }
    } else {
        $action = 'generated'
        $artifacts = @(Invoke-DotNetPkiGeneration -TargetDir $targetDir)
    }
}

$payload = [ordered]@{
    ok         = $true
    command    = 'certs'
    action     = $action
    output_dir = $targetDir
    force      = [bool]$opts.Force
    dry_run    = [bool]$opts.DryRun
    no_color   = $useNoColor
    algorithm  = 'ECDSA-P256'
    san        = @('DNS:localhost', 'IP:127.0.0.1')
    key_acl    = '0600'
    artifacts  = $artifacts
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    Write-Output ("SandalCore 'certs' ({0}, dir={1}, dry_run={2})" -f $action, $targetDir, $opts.DryRun)
}
