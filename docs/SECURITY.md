# Security Policy

**Related Guides**: [Root Overview](../README.md) | [Contributing Guide](CONTRIBUTING.md) | [Configuration Reference](CONFIGURATION.md)

We take the security of SandalCore seriously. If you discover a security vulnerability or credential exposure, please report it privately rather than opening a public issue.

---

## Reporting a Vulnerability

Report security issues through **GitHub Private Vulnerability Reporting**:

1. Open the repository's **Security** tab on GitHub.
2. Click **Report a vulnerability** under *Vulnerability reporting*.
3. Submit the form with a clear description and reproduction steps.

### What to Include

- A summary of the vulnerability and affected component (`src/server/world`, `src/server/services`, `src/libs/aocnet`, `src/clients/*`, or deployment scripts).
- Step-by-step reproduction instructions or a minimal proof of concept.
- The potential impact and any suggested fix.

---

## Security Boundaries

- **Never Commit Secrets or Game Assets**: `scripts/checks/public_check.ps1` blocks private keys (`*.key`, `*.pfx`), real environment files (`.env`), packet captures (`*.pcap`, `*.pcapng`), and extracted client data (`content.db`, `*.pak`, `*.utoc`, `*.ucas`).
- **Local Host Overrides Outside Git**: Custom TLS certificates (`$HOME/.aoc/certs/server.pem` and `server.key`) and local Unreal mappings (`$HOME/.aoc/aoc.usmap`, or `~/.aoc/aoc.usmap`) are stored in your home directory outside the repository so private keys and local overrides are never tracked by Git, while keeping `src/config/layouts/aoc.usmap` unmodified.
- **Single-Boot Web Admin Passphrase and Scoped JWTs**: Every `start.bat` boot generates a fresh passphrase (`deploy/.admin_passphrase`) and a fresh `AOC_ADMIN_BOOT_ID` UUID, invalidating tokens from earlier server boots.
- **Cluster-Internal Metrics Endpoints**: Internal telemetry endpoints (`http://mesh-gateway:8082/metrics`, `http://<worker>:8080/metrics`, and `http://<service>:18080/metrics`) are reachable only inside the container network and are summarized for authenticated operators holding `metrics:read` scope in the Web Admin **Observability** tab.
- **Read-Only Container Filesystems**: All runtime containers in `src/deploy/compose.yml` run with `read_only: true` root filesystems and RAM-backed `tmpfs` scratch mounts.
