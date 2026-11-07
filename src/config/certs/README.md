# TLS Certificates

**Related Guides**: [Root Overview](../../../README.md) | [Configuration Reference](../../../docs/CONFIGURATION.md) | [Container Deployment](../../deploy/README.md) | [SandalLauncher](../../clients/launcher/README.md)

`src/config/certs` and `scripts/sandal/certs.ps1` manage the TLS 1.3 X.509 certificates (`dist/certs/server.pem`, `dist/certs/server.crt`, and `dist/certs/server.key`) used by `aoc-character` (`:50061` ICS TLS 1.3) and `aoc-ingress-proxy` (`:8443` HTTPS).

---

## Certificate Preservation and Automatic Backup

SandalCore never overwrites existing valid certificates in `dist/certs/`:

1. **Automatic Host Backup (`$HOME/.aoc/certs/`)**: Whenever `dist/certs/server.pem` and `dist/certs/server.key` exist (including custom Let's Encrypt or wildcard certificates), `scripts/sandal/certs.ps1` and `scripts/sandal/deploy.ps1` back them up to `$HOME/.aoc/certs/`.
2. **Automatic Restore After Clean Builds**: If `dist/` is deleted by `.\sandal.ps1 clean`, the next `build.bat` or `start.bat` run automatically restores your certificates from `$HOME/.aoc/certs/` into `dist/certs/` and syncs `server.crt` to `server.pem`.
3. **Self-Signed Fallback for Fresh Setups**: Only when both `dist/certs/` and `$HOME/.aoc/certs/` are empty does `scripts/sandal/certs.ps1` generate a local self-signed development certificate (`CN=localhost`, SANs `DNS:localhost`, `IP:127.0.0.1`).

---

## Wildcard Certificate Host Resolution

When `dist/certs/server.pem` carries a wildcard Subject Alternative Name (`*.<domain>`), `SandalConfigLoader.cs` (in `SandalLauncher`) and `scripts/lib/Env.psm1` automatically map `localhost` and `127.0.0.1` to `localhost.<domain>` (`127.0.0.1`) so TLS 1.3 hostname verification succeeds without manual `.env` edits.
