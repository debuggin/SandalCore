-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
--
-- PostgreSQL LISTEN/NOTIFY payload contract:
--   LISTEN content_hotfix_applied;
--   NOTIFY content_hotfix_applied, '{"hotfix_id":"0001_initial_overlay","version":1,"sha256_digest":"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}';

CREATE TABLE IF NOT EXISTS content_schema_meta (
    version INTEGER PRIMARY KEY,
    sha256_digest TEXT NOT NULL,
    applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS content_hotfix_ledger (
    hotfix_id TEXT PRIMARY KEY,
    version INTEGER NOT NULL,
    sha256_digest TEXT NOT NULL,
    applied_at TEXT NOT NULL
);

INSERT INTO content_schema_meta (version, sha256_digest, applied_at)
VALUES (
    1,
    'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
    '2026-01-01T00:00:00Z'
)
ON CONFLICT(version) DO UPDATE SET
    sha256_digest = excluded.sha256_digest,
    applied_at = excluded.applied_at;

INSERT INTO content_hotfix_ledger (hotfix_id, version, sha256_digest, applied_at)
VALUES (
    '0001_initial_overlay',
    1,
    'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
    '2026-01-01T00:00:00Z'
)
ON CONFLICT(hotfix_id) DO UPDATE SET
    version = excluded.version,
    sha256_digest = excluded.sha256_digest,
    applied_at = excluded.applied_at;
