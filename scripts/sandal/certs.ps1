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
        [System.Security.Cryptography.RSA]$PrivateKey,
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
    $leafKey = [System.Security.Cryptography.RSA]::Create(2048)
    $leafCert = $null
    try {
        $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            "CN=$CommonName, O=SandalCore",
            $leafKey,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
        )
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
        $kuFlags = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($kuFlags, $true))
        $oids = [System.Security.Cryptography.OidCollection]::new()
        $null = $oids.Add([System.Security.Cryptography.Oid]::new($EkuOid))
        $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($oids, $false))
        if ($Stem -eq 'server') {
            $san = [System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder]::new()
            $san.AddIpAddress([System.Net.IPAddress]::Parse('127.0.0.1'))
            $san.AddIpAddress([System.Net.IPAddress]::Parse('::1'))
            foreach ($dns in @('localhost', 'gateway', 'aoc-gateway')) { $san.AddDnsName($dns) }
            $req.CertificateExtensions.Add($san.Build())
        }
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
    $caKey = [System.Security.Cryptography.RSA]::Create(2048)
    $caCert = $null
    try {
        $caReq = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            'CN=SandalCore Local Root CA, O=SandalCore',
            $caKey,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
        )
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($true, $true, 0, $true))
        $caKu = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyCertSign -bor [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::CrlSign
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($caKu, $true))
        $caReq.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($caReq.PublicKey, $false))
        $now = [DateTimeOffset]::UtcNow.AddMinutes(-10)
        $caCert = $caReq.CreateSelfSigned($now, $now.AddDays(3650))
        $caRecord = Write-CertAndKeyPem -Certificate $caCert -PrivateKey $caKey -TargetDir $TargetDir -Stem 'ca'
        $srvRecord = New-DotNetLeafCertificate -IssuerCert $caCert -CommonName '127.0.0.1' -EkuOid '1.3.6.1.5.5.7.3.1' -TargetDir $TargetDir -Stem 'server'
        $cliRecord = New-DotNetLeafCertificate -IssuerCert $caCert -CommonName 'sandal-client' -EkuOid '1.3.6.1.5.5.7.3.2' -TargetDir $TargetDir -Stem 'client'
        return @($caRecord, $srvRecord, $cliRecord)
    } finally {
        if ($null -ne $caCert) { $caCert.Dispose() }
        $caKey.Dispose()
    }
}

function Ensure-ServerCertPairFiles {
    param([string]$TargetDir, [string]$FallbackCertsDir)
    if (-not (Test-Path -LiteralPath $TargetDir)) {
        $null = New-Item -ItemType Directory -Path $TargetDir -Force
    }
    $serverPem = Join-Path $TargetDir 'server.pem'
    $serverCrt = Join-Path $TargetDir 'server.crt'
    $serverKey = Join-Path $TargetDir 'server.key'
    if (-not (Test-Path -LiteralPath $serverPem -PathType Leaf) -and (Test-Path -LiteralPath $serverCrt -PathType Leaf)) {
        Copy-Item -LiteralPath $serverCrt -Destination $serverPem -Force
    }
    if (-not (Test-Path -LiteralPath $serverPem -PathType Leaf) -and $FallbackCertsDir -and (Test-Path -LiteralPath $FallbackCertsDir -PathType Container)) {
        foreach ($name in @('server.pem', 'server.crt', 'server.key', 'ca.crt', 'ca.pem')) {
            $src = Join-Path $FallbackCertsDir $name
            $dst = Join-Path $TargetDir $name
            if ((Test-Path -LiteralPath $src -PathType Leaf) -and -not (Test-Path -LiteralPath $dst -PathType Leaf)) {
                Copy-Item -LiteralPath $src -Destination $dst -Force
            }
        }
    }
    $hasPair = (Test-Path -LiteralPath $serverPem -PathType Leaf) -and (Test-Path -LiteralPath $serverKey -PathType Leaf)
    if (-not $hasPair) {
        return $false
    }
    Copy-Item -LiteralPath $serverPem -Destination $serverCrt -Force
    $caCrtPath = Join-Path $TargetDir 'ca.crt'
    $isStaleCa = $false
    if (Test-Path -LiteralPath $caCrtPath -PathType Leaf) {
        $srvCert = $null
        $caCert = $null
        try {
            $srvCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem((Get-Content -LiteralPath $serverPem -Raw))
            $caCert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::CreateFromPem((Get-Content -LiteralPath $caCrtPath -Raw))
            $isStaleCa = ($srvCert.Issuer -ne $caCert.Subject)
        } catch {
        } finally {
            if ($null -ne $srvCert) { $srvCert.Dispose() }
            if ($null -ne $caCert) { $caCert.Dispose() }
        }
    }
    if ($isStaleCa) {
        $deployCertsDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\deploy\certs'))
        foreach ($dir in @($TargetDir, $deployCertsDir)) {
            if (-not (Test-Path -LiteralPath $dir -PathType Container)) { continue }
            foreach ($staleName in @('ca.crt', 'ca.pem', 'ca.key', 'client.crt', 'client.pem', 'client.key')) {
                Remove-Item -LiteralPath (Join-Path $dir $staleName) -Force -ErrorAction SilentlyContinue
            }
        }
    }
    return $true
}

function Test-SandalRootCaThumbprintTrusted {
    param([string]$Thumbprint)
    foreach ($loc in @([System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser, [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine)) {
        $store = [System.Security.Cryptography.X509Certificates.X509Store]::new([System.Security.Cryptography.X509Certificates.StoreName]::Root, $loc)
        try {
            $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadOnly)
            $found = $store.Certificates.Find([System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint, $Thumbprint, $false)
            if ($found.Count -gt 0) { return $true }
        } catch {
        } finally {
            $store.Close()
            $store.Dispose()
        }
    }
    return $false
}

function Install-SandalRootCaToCurrentUserStore {
    param([string]$CaCertPath)
    if (-not $IsWindows -or -not (Test-Path -LiteralPath $CaCertPath -PathType Leaf)) {
        return $false
    }
    $ca = $null
    $rwStore = $null
    try {
        $ca = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($CaCertPath)
        if (Test-SandalRootCaThumbprintTrusted -Thumbprint $ca.Thumbprint) {
            return $true
        }
        if ($env:CI -eq 'true') {
            return $false
        }
        [Console]::Error.WriteLine('Adding the SandalCore local CA to your CurrentUser Trusted Root store (choose Yes if Windows prompts to confirm)...')
        $rwStore = [System.Security.Cryptography.X509Certificates.X509Store]::new(
            [System.Security.Cryptography.X509Certificates.StoreName]::Root,
            [System.Security.Cryptography.X509Certificates.StoreLocation]::CurrentUser
        )
        $rwStore.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        foreach ($oldCert in @($rwStore.Certificates | Where-Object { $_.Subject -match 'CN=Sandal\s*Core Local (Root )?CA' -and $_.Thumbprint -ne $ca.Thumbprint })) {
            try { $rwStore.Remove($oldCert) } catch {}
        }
        $rwStore.Add($ca)
        $rwStore.Close()
        if (Test-SandalRootCaThumbprintTrusted -Thumbprint $ca.Thumbprint) {
            return $true
        }
        $null = & certutil.exe -user -addstore -f Root $CaCertPath *> $null
        return ($LASTEXITCODE -eq 0)
    } catch {
        $null = & certutil.exe -user -addstore -f Root $CaCertPath *> $null
        return ($LASTEXITCODE -eq 0)
    } finally {
        if ($null -ne $rwStore) { $rwStore.Close(); $rwStore.Dispose() }
        if ($null -ne $ca) { $ca.Dispose() }
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
$legacyCertsDir = Join-Path $rootPath 'certs'
$opts = Resolve-CertsOptions -BaseOutDir $OutputDir -BaseForce ([bool]$Force) -BaseDryRun ([bool]($DryRun -or $inheritedDryRun)) -BaseJson ([bool]($Json -or $inheritedJson)) -RawArgs (@($Arguments) + @($RemainingArgs))
$targetDir = if ($opts.OutputDir) { [System.IO.Path]::GetFullPath($opts.OutputDir) } else { $defaultCertsDir }

$action = 'dry_run'
$artifacts = @()
$caCertPath = Join-Path $targetDir 'ca.crt'
if (-not $opts.DryRun) {
    $hasServerPair = if ($opts.Force) { $false } else { Ensure-ServerCertPairFiles -TargetDir $targetDir -FallbackCertsDir $legacyCertsDir }
    if ($hasServerPair -and -not $opts.Force) {
        $action = 'reused'
        foreach ($keyName in @('ca.key', 'server.key', 'client.key')) {
            $keyCandidate = Join-Path $targetDir $keyName
            if (Test-Path -LiteralPath $keyCandidate -PathType Leaf) {
                $null = Set-SandalSecretAcl -LiteralPath $keyCandidate
            }
        }
        if (Test-Path -LiteralPath $caCertPath -PathType Leaf) {
            $null = Install-SandalRootCaToCurrentUserStore -CaCertPath $caCertPath
        }
    } else {
        $action = 'generated'
        $artifacts = @(Invoke-DotNetPkiGeneration -TargetDir $targetDir)
        $null = Install-SandalRootCaToCurrentUserStore -CaCertPath $caCertPath
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
    algorithm  = 'RSA-2048'
    san        = @('IP:127.0.0.1', 'IP:::1', 'DNS:localhost', 'DNS:gateway', 'DNS:aoc-gateway')
    key_acl    = '0600'
    artifacts  = $artifacts
}

if ($opts.Json) {
    Write-Output ($payload | ConvertTo-Json -Depth 6)
} else {
    Write-Output ("SandalCore 'certs' ({0}, dir={1}, dry_run={2})" -f $action, $targetDir, $opts.DryRun)
}
