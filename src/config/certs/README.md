# TLS Certificates

SandalCore does not ship private keys or server certificates in the repository. TLS material is provisioned and validated through `scripts/sandal/certs.ps1` (`pwsh -File sandal.ps1 certs`), `scripts/lib/certs.ps1`, and `scripts/lib/certs-pki.ps1`.

---

## Architecture & Modules

Certificate lifecycle management spans the CLI subcommand and two PowerShell library modules:

- **`scripts/sandal/certs.ps1` (`pwsh -File sandal.ps1 certs`)**:
  - `Ensure-ServerCertPairFiles`: Reuses existing user-supplied certificates (`dist/certs/server.pem` or `server.crt` paired with `dist/certs/server.key`, or restored from `$HOME/.aoc/certs/*` / `SANDAL_CERTS_OVERRIDE_DIR` / `SANDAL_TLS_CERT_PATH` + `SANDAL_TLS_KEY_PATH`, such as ACME wildcard ECDSA P-256 certificates) untouched when `-Force` is not passed, tightens private-key ACLs (`0600`), and purges stale `ca.*` / `client.*` files across `dist/certs` and `deploy/certs` whenever `ca.crt`'s subject does not match `server.pem`'s issuer.
  - `Invoke-DotNetPkiGeneration` & `Install-SandalRootCaToCurrentUserStore`: Generates local Root CA (`ca.crt`, `ca.key`) and SAN leaf certificates (`server.crt`, `server.pem`, `server.key` covering `127.0.0.1`, `::1`, `localhost`, `gateway`, `aoc-gateway`) when no existing pair is present or `-Force` is specified, and imports `ca.crt` into `Cert:\CurrentUser\Root` (user-scoped, zero administrator privileges required).
- **`scripts/lib/certs.ps1`**:
  - `Confirm-SandalCerts`: Inspects existing certificate files (`ca.crt`, `server.crt`, `server.key`) using OpenSSL, verifying `BasicConstraints` (`CA:FALSE`), validity window (`<= 397` total days and `> 30` days remaining), Subject Alternative Names (`DNS:localhost`, `IP:127.0.0.1`, `AdvertiseIp`, and optional `CustomDomain`), and public/private key matching. Emits `INFO: Valid SAN TLS certificates already present (>30d remaining); keeping existing certs` when valid certificates are reused.
  - `Invoke-SandalOpenSsl`: Executes OpenSSL commands or a mockable `$OpenSslRunner` scriptblock.
- **`scripts/lib/certs-pki.ps1`**:
  - `New-SandalLocalCaCerts`: Generates an ECDSA P-256 Root CA and signed SAN leaf certificate.
  - `Test-SandalCustomCertBundle`: Validates custom or ACME-issued certificate bundles (`fullchain.pem` and `privkey.pem`).
  - `Ensure-SandalCerts`: Orchestrates Mode A (Local CA) and Mode B/C (ACME / Custom bundle) provisioning with graceful reuse when existing certificates have more than 30 days remaining.

---

## Provisioning Modes

### Mode A: Local Root CA & User-Scoped Trust (`scripts/sandal/certs.ps1` / `New-SandalLocalCaCerts`)

By default (`Mode = 'LocalCA'`), `scripts/sandal/certs.ps1` (`pwsh -File sandal.ps1 certs`) and `Ensure-SandalCerts` check for existing certificates in `dist/certs/` (and `$HOME/.aoc/certs/*` or `SANDAL_CERTS_OVERRIDE_DIR` / `SANDAL_TLS_CERT_PATH` + `SANDAL_TLS_KEY_PATH`) first. If `dist/certs/server.pem` + `server.key` are already present (for example an externally issued or ACME wildcard ECDSA P-256 certificate pair), they are preserved untouched and any stale `ca.*` / `client.*` files from a different issuer are removed. When generating local development certificates, the provisioning pipeline creates:

- **`certs/ca.crt` & `certs/ca.key`**: Local Root CA (`CN=SandalCore Local Root CA, O=SandalCore`) with `basicConstraints = critical, CA:TRUE, pathlen:0` and `keyUsage = critical, keyCertSign, cRLSign`.
- **`certs/server.crt`, `certs/server.key`, & `certs/server.pem`**: Leaf certificate signed by `ca.crt` with:
  - `basicConstraints = critical, CA:FALSE`
  - `keyUsage = critical, digitalSignature, keyEncipherment`
  - `extendedKeyUsage = serverAuth`
  - Total validity of `365` days (`<= 397` days to satisfy modern TLS leaf lifetime limits)
  - `subjectAltName` entries covering `DNS:localhost`, `DNS:*.localhost`, `DNS:gateway`, `DNS:aoc-gateway`, `IP:127.0.0.1`, `IP:::1`, `IP:<AdvertiseIp>`, and any configured `DNS:<CustomDomain>`

**User-Scoped `CurrentUser\Root` & `CustomTrustStore` Trust**: When a local Root CA (`ca.crt`) is generated or reused, `Install-SandalRootCaToCurrentUserStore` imports `ca.crt` into `Cert:\CurrentUser\Root` (user-scoped, requiring zero administrator elevation) and `SandalLauncher` also loads `certs/ca.crt` into `X509ChainPolicy.CustomTrustStore` with `X509ChainTrustMode.CustomRootTrust`. This allows both `SandalLauncher` and `AOCClient-Win64-Shipping.exe` (Schannel) to validate the TLS 1.3 `:50061` ICS connection with strict X.509 chain and SAN verification.

### Mode B / Mode C: ACME & Custom Certificate Bundles (`Ensure-ServerCertPairFiles` / `Test-SandalCustomCertBundle`)

When running with publicly trusted or external PKI certificates (such as ACME wildcard ECDSA P-256 certificates placed in `$HOME/.aoc/certs/` or `dist/certs/server.pem` + `dist/certs/server.key`, or `fullchain.pem` + `privkey.pem` with `Mode = 'Custom'`), `scripts/sandal/certs.ps1` (`Ensure-ServerCertPairFiles`) and `Ensure-SandalCerts` (`Test-SandalCustomCertBundle` delegating to `Confirm-SandalCerts -SkipCaCheck`) keep the external `server.pem` and `server.key` untouched, sync `server.crt`, and remove any stale local `ca.*` / `client.*` artifacts so TLS clients validate directly against the system Root CA store.
