# TLS Certificates

SandalCore does not ship private keys or server certificates in the repository. TLS material is provisioned and validated through `scripts/lib/certs.ps1` and `scripts/lib/certs-pki.ps1`.

---

## Architecture & Modules

Certificate lifecycle management is split across two PowerShell modules:

- **`scripts/lib/certs.ps1`**:
  - `Confirm-SandalCerts`: Inspects existing certificate files (`ca.crt`, `server.crt`, `server.key`) using OpenSSL, verifying `BasicConstraints` (`CA:FALSE`), validity window (`<= 397` total days and `> 30` days remaining), Subject Alternative Names (`DNS:localhost`, `IP:127.0.0.1`, `AdvertiseIp`, and optional `CustomDomain`), and public/private key matching. Emits `INFO: Valid SAN TLS certificates already present (>30d remaining); keeping existing certs` when valid certificates are reused.
  - `Invoke-SandalOpenSsl`: Executes OpenSSL commands or a mockable `$OpenSslRunner` scriptblock.
- **`scripts/lib/certs-pki.ps1`**:
  - `New-SandalLocalCaCerts`: Generates an ECDSA P-256 Root CA and signed SAN leaf certificate.
  - `Test-SandalCustomCertBundle`: Validates custom or ACME-issued certificate bundles (`fullchain.pem` and `privkey.pem`).
  - `Ensure-SandalCerts`: Orchestrates Mode A (Local CA) and Mode B/C (ACME / Custom bundle) provisioning with graceful reuse when existing certificates have more than 30 days remaining.

---

## Provisioning Modes

### Mode A: Local ECDSA P-256 Root CA (`New-SandalLocalCaCerts`)

By default (`Mode = 'LocalCA'`), `Ensure-SandalCerts` checks existing certificates via `Confirm-SandalCerts` and, if absent or expiring within 30 days, invokes `New-SandalLocalCaCerts` to create:

- **`certs/ca.crt` & `certs/ca.key`**: ECDSA `prime256v1` (P-256) Root CA with `basicConstraints = critical, CA:TRUE, pathlen:0` and `keyUsage = critical, keyCertSign, cRLSign`.
- **`certs/server.crt`, `certs/server.key`, & `certs/server.pem`**: ECDSA `prime256v1` leaf certificate signed by `ca.crt` with:
  - `basicConstraints = critical, CA:FALSE`
  - `keyUsage = critical, digitalSignature, keyEncipherment`
  - `extendedKeyUsage = serverAuth`
  - Total validity of `365` days (`<= 397` days to satisfy modern TLS leaf lifetime limits)
  - `subjectAltName` entries covering `DNS:localhost`, `DNS:*.localhost`, `IP:127.0.0.1`, `IP:<AdvertiseIp>`, and any configured `DNS:<CustomDomain>`

**Zero-Admin Trust**: Mode A does not mutate the Windows OS certificate store (`certutil` is never invoked). Instead, the Avalonia launcher loads `certs/ca.crt` directly into `X509ChainPolicy.CustomTrustStore` with `X509ChainTrustMode.CustomRootTrust`, requiring zero administrator privileges or OS security prompts.

### Mode B / Mode C: ACME & Custom Certificate Bundles (`Test-SandalCustomCertBundle`)

When running with publicly trusted or external PKI certificates (`Mode = 'Custom'`), `Ensure-SandalCerts` validates the provided `fullchain.pem` and `privkey.pem` via `Test-SandalCustomCertBundle` (delegating to `Confirm-SandalCerts -SkipCaCheck`) and copies them into `certs/server.crt`, `certs/server.key`, and `certs/server.pem`.

---

## Why Legacy `scripts/make_certs.ps1` Was Removed

The legacy `scripts/make_certs.ps1` script and manual CN-only RSA one-liners (`-subj "/CN=localhost"`) were removed because:
1. Certificates relying on Common Name (`CN`) without RFC 5280 Subject Alternative Name (`SAN`) extensions are rejected by modern Go (`crypto/tls`) and .NET X.509 chain validation.
2. Leaf certificates generated with multi-year validity (`825` days) violate the 397-day maximum leaf validity standard (`STD-03`).
3. Mutating the Windows user/machine root store via `certutil -user -addstore Root` triggered modal OS elevation/security dialogs and failed in non-interactive sessions.
