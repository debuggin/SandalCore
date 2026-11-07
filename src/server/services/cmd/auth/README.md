# Auth Service

**Related Guides**: [Root Overview](../../../../../README.md) | [Control-Plane Services](../../../../../docs/SERVICES.md) | [Configuration Reference](../../../../../docs/CONFIGURATION.md) | [Gateway Service](../gateway/README.md) | [SandalLauncher](../../../../clients/launcher/README.md)

`src/server/services/cmd/auth` (`aoc-auth`, listening on `:50051` gRPC) is the account authentication and session ticket service for SandalCore.

---

## Core Responsibilities

- **Password Hashing and Account Registration**: Hashes account passwords with Argon2id and stores account records in PostgreSQL (`accounts`).
- **Session Ticket Issuance**: Issues and validates cryptographically random session tickets exchanged by `SandalLauncher` (`/v1/auth/login`), the `:19021` local UDP tether, and `aoc-character` (`:50061` ICS TLS 1.3).
- **Disposable Temporary Accounts**: Creates and cleans up one-click `Temp Account` sessions (`TMP <Word1><Word2><Word3>`) requested from `SandalLauncher` (`/v1/auth/temp-login` and `/v1/auth/temp-cleanup`).

---

## Configuration

| Environment Variable | Default | Description |
|:---|:---|:---|
| `AOC_AUTH_GRPC_ADDR` | `:50051` | Internal gRPC listen address for `AuthService`. |
| `AOC_AUTH_METRICS_ADDR` | `:9091` | HTTP listen address for `/livez`, `/readyz`, `/healthz`, and `/metrics`. |
| `DATABASE_URL` | `postgres://aoc:aoc@postgres:5432/aoc?sslmode=disable` | PostgreSQL connection string. |

---

## Testing

```powershell
.\sandal.ps1 test
pwsh -NoProfile -File scripts/test_functional_apis.ps1
```
