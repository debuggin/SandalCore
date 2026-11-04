from __future__ import annotations

import json
import os
from pathlib import Path
import re
import sqlite3
import unittest

REPO_ROOT = Path(__file__).resolve().parents[2]
HOTFIX_SQL_PATH = REPO_ROOT / "sql" / "hotfixes" / "0001_initial_overlay.sql"
SERVER_JSON_PATH = REPO_ROOT / "config" / "server.json"
REALM_JSON_PATH = REPO_ROOT / "config" / "realm.json"

REDACTION_PATTERNS: tuple[tuple[re.Pattern[str], str], ...] = (
    (re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"), "[REDACTED]"),
    (re.compile(r"(?i)\b(Bearer\s+)[A-Za-z0-9\-._~+/]+=*"), r"\1[REDACTED]"),
    (
        re.compile(
            r'(?i)\b(ics_token|game_connection_token|AOC_MASTER_KEY|POSTGRES_PASSWORD)\s*([:=])\s*["\']?([^\r\n"\'\s,;]+)["\']?'
        ),
        r"\1\2[REDACTED]",
    ),
    (re.compile(r"(?i)((?:postgres(?:ql)?|redis)://[^:\s/@]+:)([^@\s]+)(@)"), r"\1[REDACTED]\3"),
)


def redact_sensitive_text(text: str) -> str:
    scrubbed = text
    for pattern, replacement in REDACTION_PATTERNS:
        scrubbed = pattern.sub(replacement, scrubbed)
    return scrubbed


def generate_uuid_v7(timestamp_ms: int, rand_a: int, rand_b: int) -> str:
    if timestamp_ms < 0 or timestamp_ms >= (1 << 48):
        raise ValueError("timestamp_ms out of 48-bit range")
    ts48 = timestamp_ms & ((1 << 48) - 1)
    ver_and_a = (0x7 << 12) | (rand_a & 0x0FFF)
    var_and_b = (0b10 << 62) | (rand_b & ((1 << 62) - 1))
    high64 = (ts48 << 16) | ver_and_a
    full128 = (high64 << 64) | var_and_b
    hex_str = f"{full128:032x}"
    return f"{hex_str[0:8]}-{hex_str[8:12]}-{hex_str[12:16]}-{hex_str[16:20]}-{hex_str[20:32]}"


def parse_and_validate_uuid_v7(uuid_text: str) -> tuple[int, int, int]:
    cleaned = uuid_text.strip().replace("-", "")
    if len(cleaned) != 32:
        raise ValueError("UUID must contain 32 hex chars")
    raw = int(cleaned, 16)
    timestamp_ms = (raw >> 80) & ((1 << 48) - 1)
    version = (raw >> 76) & 0xF
    variant = (raw >> 62) & 0b11
    if version != 7 or variant != 2:
        raise ValueError(f"Invalid UUIDv7 fields: version={version}, variant={variant}")
    return timestamp_ms, version, variant


def validate_server_and_realm_contracts(server: dict[str, object], realm: dict[str, object]) -> None:
    for port_key in ("ics_port", "tether_port", "game_port", "auth_server_port"):
        port_val = server.get(port_key)
        if not isinstance(port_val, int) or not (1 <= port_val <= 65535):
            raise ValueError(f"Invalid port for {port_key}: {port_val}")
    if not isinstance(server.get("max_players"), int) or int(server["max_players"]) <= 0:
        raise ValueError("max_players must be positive int")
    conn_str = str(realm.get("game_connection_string", ""))
    if not conn_str.endswith(f":{server['game_port']}"):
        raise ValueError("realm game_connection_string port mismatch")
    if not str(realm.get("game_connection_token", "")).strip():
        raise ValueError("game_connection_token must be non-empty")


class PolyglotContractsTests(unittest.TestCase):
    def test_hotfix_sql_overlay_in_memory_idempotency_and_monotonicity(self) -> None:
        sql_script = HOTFIX_SQL_PATH.read_text(encoding="utf-8")
        with sqlite3.connect(":memory:") as conn:
            conn.executescript(sql_script)
            conn.executescript(sql_script)

            meta_rows = conn.execute(
                "SELECT version, sha256_digest, applied_at FROM content_schema_meta"
            ).fetchall()
            self.assertEqual(len(meta_rows), 1)
            self.assertEqual(meta_rows[0][0], 1)
            self.assertEqual(len(meta_rows[0][1]), 64)

            ledger_rows = conn.execute(
                "SELECT hotfix_id, version, sha256_digest FROM content_hotfix_ledger"
            ).fetchall()
            self.assertEqual(ledger_rows, [("0001_initial_overlay", 1, meta_rows[0][1])])

            conn.execute(
                "INSERT INTO content_hotfix_ledger (hotfix_id, version, sha256_digest, applied_at) VALUES (?, ?, ?, ?)",
                ("0002_balance_patch", 2, "a" * 64, "2026-01-02T00:00:00Z"),
            )
            versions = [
                row[0]
                for row in conn.execute(
                    "SELECT version FROM content_hotfix_ledger ORDER BY hotfix_id ASC"
                ).fetchall()
            ]
            self.assertEqual(versions, [1, 2])
            self.assertTrue(all(versions[i] < versions[i + 1] for i in range(len(versions) - 1)))

            with self.assertRaises(sqlite3.IntegrityError):
                conn.execute(
                    "INSERT INTO content_schema_meta (version, sha256_digest, applied_at) VALUES (2, NULL, '2026-01-03T00:00:00Z')"
                )

    def test_server_and_realm_json_cross_language_contracts(self) -> None:
        server = json.loads(SERVER_JSON_PATH.read_text(encoding="utf-8"))
        realm = json.loads(REALM_JSON_PATH.read_text(encoding="utf-8"))
        validate_server_and_realm_contracts(server, realm)
        self.assertEqual(server["tether_port"], 19021)
        self.assertEqual(server["game_port"], 7777)
        self.assertIs(realm["world_available"], True)

        with self.assertRaises(ValueError):
            validate_server_and_realm_contracts({**server, "game_port": 70000}, realm)
        with self.assertRaises(ValueError):
            validate_server_and_realm_contracts(server, {**realm, "game_connection_string": "127.0.0.1:9999"})

    def test_rfc9562_uuid_v7_bit_layout_and_monotonic_ordering(self) -> None:
        rand_seed = int.from_bytes(os.urandom(8), "big")
        u1 = generate_uuid_v7(1_700_000_000_000, 0x101, rand_seed)
        u2 = generate_uuid_v7(1_700_000_000_001, 0x002, rand_seed)
        u3 = generate_uuid_v7(1_700_000_005_000, 0xabc, rand_seed)

        ts1, ver1, var1 = parse_and_validate_uuid_v7(u1)
        self.assertEqual((ts1, ver1, var1), (1_700_000_000_000, 7, 2))
        self.assertLess(u1, u2)
        self.assertLess(u2, u3)

        with self.assertRaises(ValueError):
            generate_uuid_v7(-1, 0, 0)
        with self.assertRaises(ValueError):
            parse_and_validate_uuid_v7("550e8400-e29b-41d4-a716-446655440000")

    def test_secret_redaction_regex_parity(self) -> None:
        raw = (
            "status=ok node=verra-1\n"
            "Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VyIjoiY3liIn0.sig\n"
            'ics_token="zenith-secret-token-7788"\n'
            "dsn=postgresql://aoc_admin:super_secret_pw@127.0.0.1:5432/sandal\n"
        )
        redacted = redact_sensitive_text(raw)
        self.assertIn("status=ok node=verra-1", redacted)
        self.assertIn("Bearer [REDACTED]", redacted)
        self.assertIn("ics_token=[REDACTED]", redacted)
        self.assertIn("postgresql://aoc_admin:[REDACTED]@127.0.0.1:5432/sandal", redacted)
        for leaked in ("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9", "zenith-secret-token-7788", "super_secret_pw"):
            self.assertNotIn(leaked, redacted)


if __name__ == "__main__":
    unittest.main()
