Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Describe 'SandalCore SAN TLS PKI & Certificate Trust Module (Epic 10)' {
    BeforeAll {
        $repoRoot = $PSScriptRoot
        while ($repoRoot -and -not (Test-Path -LiteralPath (Join-Path $repoRoot 'sandal.ps1'))) {
            $parent = Split-Path -Parent $repoRoot
            if ($parent -eq $repoRoot) { break }
            $repoRoot = $parent
        }
        $script:RepoRoot = $repoRoot
        foreach ($rel in @('scripts/lib/certs.ps1', 'scripts/lib/certs-pki.ps1')) {
            $modPath = Join-Path $repoRoot $rel
            if (Test-Path -LiteralPath $modPath) { . $modPath }
        }
        if ((Get-Module Pester).Version.Major -lt 5) {
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
        function New-MockOpenSslRunner {
            param(
                [string]$BasicConstraints = 'CA:FALSE',
                [int]$TotalDays = 365,
                [int]$DaysRemaining = 180,
                [string]$San = 'DNS:localhost, DNS:*.localhost, IP Address:127.0.0.1, IP Address:192.168.1.50',
                [string]$CertPubKey = '-----BEGIN PUBLIC KEY-----MOCKKEY1-----END PUBLIC KEY-----',
                [string]$KeyPubKey = '-----BEGIN PUBLIC KEY-----MOCKKEY1-----END PUBLIC KEY-----'
            )
            $sb = {
                param([string[]]$Arguments)
                if ($Arguments -contains '-dates' -or $Arguments -contains '-ext') {
                    $nb = ([datetimeoffset]::UtcNow.AddDays($DaysRemaining - $TotalDays)).ToString('MMM dd HH:mm:ss yyyy', [System.Globalization.CultureInfo]::InvariantCulture) + ' GMT'
                    $na = ([datetimeoffset]::UtcNow.AddDays($DaysRemaining)).ToString('MMM dd HH:mm:ss yyyy', [System.Globalization.CultureInfo]::InvariantCulture) + ' GMT'
                    $text = "notBefore=$nb`nnotAfter=$na`nX509v3 Basic Constraints:`n    $BasicConstraints`nX509v3 Subject Alternative Name:`n    $San"
                    return [pscustomobject]@{ ExitCode = 0; Output = $text }
                }
                if ($Arguments -contains '-pubkey') { return [pscustomobject]@{ ExitCode = 0; Output = $CertPubKey } }
                if ($Arguments -contains '-pubout') { return [pscustomobject]@{ ExitCode = 0; Output = $KeyPubKey } }
                $outIdx = [array]::IndexOf($Arguments, '-out')
                if ($outIdx -ge 0 -and ($outIdx + 1) -lt $Arguments.Count) {
                    $outPath = $Arguments[$outIdx + 1]
                    $parent = Split-Path -Parent $outPath
                    if ($parent -and -not (Test-Path -LiteralPath $parent)) { $null = New-Item -ItemType Directory -Path $parent -Force }
                    Set-Content -LiteralPath $outPath -Value '-----BEGIN MOCK PEM-----' -Encoding ascii
                }
                return [pscustomobject]@{ ExitCode = 0; Output = 'OK' }
            }
            return $sb.GetNewClosure()
        }
    }

    Context 'Legacy make_certs.ps1 Removal' {
        It 'Verifies scripts/make_certs.ps1 does not exist' {
            $legacyScript = Join-Path $script:RepoRoot 'scripts/make_certs.ps1'
            Test-Path -LiteralPath $legacyScript | Should -BeFalse
        }
    }

    Context 'Confirm-SandalCerts Graceful Certificate Reuse (UX-07, STD-03)' {
        It 'Reuses valid SAN TLS certificates when > 30 days remain' {
            $runner = New-MockOpenSslRunner -BasicConstraints 'CA:FALSE' -TotalDays 365 -DaysRemaining 180
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Valid | Should -BeTrue
            $res.KeyMatches | Should -BeTrue
            $res.BasicConstraints | Should -Be 'CA:FALSE'
        }

        It 'Returns Action Reused in Ensure-SandalCerts local-ca mode when Confirm-SandalCerts passes' {
            $runner = New-MockOpenSslRunner -BasicConstraints 'CA:FALSE' -TotalDays 365 -DaysRemaining 180
            $res = Ensure-SandalCerts -Mode 'local-ca' -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Action | Should -Be 'Reused'
            $res.Validation.Valid | Should -BeTrue
        }
    }

    Context 'Automatic Regeneration Triggers (AdvertiseIp Change & <30d Expiry)' {
        It 'Rejects certificate when AdvertiseIp changes and is missing from SAN' {
            $runner = New-MockOpenSslRunner -DaysRemaining 180
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '10.20.30.40' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.Reason | Should -Match 'Subject Alternative Name'
        }

        It 'Rejects certificate when remaining validity is <= 30 days' {
            $runner = New-MockOpenSslRunner -TotalDays 365 -DaysRemaining 15
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.Reason | Should -Match '30 days'
        }

        It 'Rejects certificate when CustomDomain is specified but missing from SAN' {
            $runner = New-MockOpenSslRunner -DaysRemaining 180
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -CustomDomain 'play.sandal.example.com' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.Reason | Should -Match 'Subject Alternative Name'
        }

        It 'Triggers Action Generated in Ensure-SandalCerts local-ca mode when existing cert fails Confirm-SandalCerts' {
            $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-ep10-gen-" + [guid]::NewGuid().ToString('N'))
            try {
                $runner = New-MockOpenSslRunner -TotalDays 365 -DaysRemaining 15
                $res = Ensure-SandalCerts -Mode 'local-ca' -CertsDir $tempDir -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
                $res.Action | Should -Be 'Generated'
                $res.Certs.Mode | Should -Be 'local-ca'
            } finally {
                if (Test-Path -LiteralPath $tempDir) {
                    Remove-Item -LiteralPath $tempDir -Recurse -Force
                }
            }
        }
    }

    Context 'V5 Certificate Misuse Vector Rejection (STD-03, SEC-04)' {
        It 'Rejects leaf certificate with BasicConstraints CA:TRUE' {
            $runner = New-MockOpenSslRunner -BasicConstraints 'CA:TRUE' -TotalDays 365 -DaysRemaining 180
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.BasicConstraints | Should -Be 'CA:TRUE'
        }

        It 'Rejects leaf certificate with total validity > 397 days' {
            $runner = New-MockOpenSslRunner -BasicConstraints 'CA:FALSE' -TotalDays 825 -DaysRemaining 400
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.Reason | Should -Match '397'
        }

        It 'Rejects certificate with mismatched private key' {
            $runner = New-MockOpenSslRunner -CertPubKey '-----BEGIN PUBLIC KEY-----MOCKKEY1-----END PUBLIC KEY-----' -KeyPubKey '-----BEGIN PUBLIC KEY-----MOCKKEY2-----END PUBLIC KEY-----'
            $res = Confirm-SandalCerts -CertsDir 'certs' -AdvertiseIp '192.168.1.50' -OpenSslRunner $runner
            $res.Valid | Should -BeFalse
            $res.KeyMatches | Should -BeFalse
        }

        It 'Throws when New-SandalLocalCaCerts requests ValidityDays > 397' {
            $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-ep10-days-" + [guid]::NewGuid().ToString('N'))
            { New-SandalLocalCaCerts -CertsDir $tempDir -ValidityDays 400 } | Should -Throw
        }

        It 'Rejects invalid custom bundles in Test-SandalCustomCertBundle and Ensure-SandalCerts custom mode' {
            $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-ep10-custom-" + [guid]::NewGuid().ToString('N'))
            try {
                $null = New-Item -ItemType Directory -Path $tempDir -Force
                $chain = Join-Path $tempDir 'fullchain.pem'
                $priv = Join-Path $tempDir 'privkey.pem'
                Set-Content -LiteralPath $chain -Value '-----BEGIN CERTIFICATE-----' -Encoding ascii
                Set-Content -LiteralPath $priv -Value ('-----BEGIN ' + 'PRIVATE KEY-----') -Encoding ascii

                $caRunner = New-MockOpenSslRunner -BasicConstraints 'CA:TRUE'
                $bundleCheck = Test-SandalCustomCertBundle -FullChainPath $chain -PrivKeyPath $priv -AdvertiseIp '192.168.1.50' -OpenSslRunner $caRunner
                $bundleCheck.Valid | Should -BeFalse
                { Ensure-SandalCerts -Mode 'custom' -CertsDir $tempDir -AdvertiseIp '192.168.1.50' -OpenSslRunner $caRunner } | Should -Throw

                $mismatchRunner = New-MockOpenSslRunner -CertPubKey '-----BEGIN PUBLIC KEY-----A-----END PUBLIC KEY-----' -KeyPubKey '-----BEGIN PUBLIC KEY-----B-----END PUBLIC KEY-----'
                $mismatchCheck = Test-SandalCustomCertBundle -FullChainPath $chain -PrivKeyPath $priv -AdvertiseIp '192.168.1.50' -OpenSslRunner $mismatchRunner
                $mismatchCheck.Valid | Should -BeFalse
                $mismatchCheck.KeyMatches | Should -BeFalse
                { Ensure-SandalCerts -Mode 'custom' -CertsDir $tempDir -AdvertiseIp '192.168.1.50' -OpenSslRunner $mismatchRunner } | Should -Throw
            } finally {
                if (Test-Path -LiteralPath $tempDir) {
                    Remove-Item -LiteralPath $tempDir -Recurse -Force
                }
            }
        }
    }

    Context 'Real OpenSSL End-to-End ECDSA P-256 SAN Generation & Inspection' {
        It 'Generates ECDSA P-256 Root CA and SAN leaf certificate that pass Confirm-SandalCerts' {
            $hasOpenSsl = ($null -ne (Get-Command openssl -ErrorAction SilentlyContinue)) -or
                (Test-Path -LiteralPath 'C:\Program Files\Git\usr\bin\openssl.exe') -or
                (Test-Path -LiteralPath 'C:\Program Files\Git\mingw64\bin\openssl.exe')
            if (-not $hasOpenSsl) {
                return
            }
            $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("sandal-ep10-" + [guid]::NewGuid().ToString('N'))
            try {
                $gen = New-SandalLocalCaCerts -CertsDir $tempDir -AdvertiseIp '10.0.0.99' -CustomDomain 'shard.local' -ValidityDays 365
                $gen.Mode | Should -Be 'local-ca'
                $verify = Confirm-SandalCerts -CertsDir $tempDir -AdvertiseIp '10.0.0.99' -CustomDomain 'shard.local'
                $verify.Valid | Should -BeTrue
                $verify.KeyMatches | Should -BeTrue
                $verify.BasicConstraints | Should -Match 'CA:FALSE'
                $verify.SubjectAltName | Should -Match 'DNS:localhost'
                $verify.SubjectAltName | Should -Match '127\.0\.0\.1'
                $verify.SubjectAltName | Should -Match '10\.0\.0\.99'
                $verify.SubjectAltName | Should -Match 'DNS:shard\.local'
            } finally {
                if (Test-Path -LiteralPath $tempDir) {
                    Remove-Item -LiteralPath $tempDir -Recurse -Force
                }
            }
        }
    }
}
