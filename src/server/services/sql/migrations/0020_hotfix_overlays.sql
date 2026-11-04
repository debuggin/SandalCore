-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

CREATE SCHEMA IF NOT EXISTS world;
CREATE SCHEMA IF NOT EXISTS realm_state;

CREATE INDEX IF NOT EXISTS idx_sessions_account_id
    ON sessions (account_id);

CREATE INDEX IF NOT EXISTS idx_city_node_state_parent_guid
    ON city_node_state (parent_guid);

CREATE TABLE IF NOT EXISTS world.world_hotfix_overlays (
    overlay_id       BIGSERIAL    PRIMARY KEY,
    target_table     VARCHAR(64)  NOT NULL CHECK (target_table <> ''),
    record_guid      BIGINT       NOT NULL CHECK (record_guid > 0),
    field_path       VARCHAR(128) NOT NULL CHECK (field_path <> ''),
    override_json    JSONB        NOT NULL,
    min_server_build INT          NOT NULL DEFAULT 0 CHECK (min_server_build >= 0),
    enabled          BOOLEAN      NOT NULL DEFAULT TRUE,
    reason           TEXT         NOT NULL CHECK (reason <> ''),
    created_by       VARCHAR(64)  NOT NULL CHECK (created_by <> ''),
    created_at       TIMESTAMPTZ  NOT NULL DEFAULT NOW(),
    expires_at       TIMESTAMPTZ  NULL,
    CONSTRAINT world_hotfix_overlays_target_record_field_key
        UNIQUE (target_table, record_guid, field_path)
);

CREATE INDEX IF NOT EXISTS idx_hotfix_overlays_active
    ON world.world_hotfix_overlays (target_table, record_guid)
    WHERE enabled = TRUE;

CREATE TABLE IF NOT EXISTS world.hotfix_audit_log (
    audit_id    BIGSERIAL   PRIMARY KEY,
    overlay_id  BIGINT      NOT NULL REFERENCES world.world_hotfix_overlays (overlay_id) ON DELETE CASCADE,
    action      VARCHAR(32) NOT NULL CHECK (action IN ('CREATE', 'UPDATE', 'ENABLE', 'DISABLE', 'EXPIRE', 'DELETE')),
    actor       VARCHAR(64) NOT NULL CHECK (actor <> ''),
    payload     JSONB       NOT NULL DEFAULT '{}'::jsonb,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_hotfix_audit_log_overlay_id
    ON world.hotfix_audit_log (overlay_id);

CREATE TABLE IF NOT EXISTS realm_state.hotfix_registry (
    hotfix_id         INT         PRIMARY KEY,
    record_guid       BIGINT      NOT NULL CHECK (record_guid > 0),
    type_id           BIGINT      NOT NULL CHECK (type_id > 0),
    patch_json        JSONB       NOT NULL,
    raw_override_blob BYTEA,
    enabled           BOOLEAN     NOT NULL DEFAULT TRUE,
    applied_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    author            TEXT        NOT NULL CHECK (author <> ''),
    reason            TEXT        NOT NULL CHECK (reason <> '')
);

CREATE INDEX IF NOT EXISTS idx_hotfix_registry_enabled
    ON realm_state.hotfix_registry (enabled, record_guid)
    WHERE enabled = TRUE;

CREATE OR REPLACE FUNCTION realm_state.notify_hotfix_applied()
RETURNS TRIGGER LANGUAGE plpgsql AS $$
BEGIN
    PERFORM pg_notify('sandal_hotfix_applied', json_build_object(
        'hotfix_id', NEW.hotfix_id,
        'record_guid', NEW.record_guid,
        'type_id', NEW.type_id,
        'enabled', NEW.enabled
    )::text);
    RETURN NEW;
END;
$$;

DROP TRIGGER IF EXISTS trg_hotfix_applied ON realm_state.hotfix_registry;
CREATE TRIGGER trg_hotfix_applied
    AFTER INSERT OR UPDATE ON realm_state.hotfix_registry
    FOR EACH ROW EXECUTE FUNCTION realm_state.notify_hotfix_applied();

CREATE OR REPLACE VIEW realm_state.world_hotfix_overlays AS
    SELECT * FROM world.world_hotfix_overlays;

CREATE OR REPLACE VIEW realm_state.hotfix_audit_log AS
    SELECT * FROM world.hotfix_audit_log;

CREATE OR REPLACE VIEW world.hotfix_registry AS
    SELECT * FROM realm_state.hotfix_registry;

CREATE OR REPLACE VIEW public.world_hotfix_overlays AS
    SELECT * FROM world.world_hotfix_overlays;

CREATE OR REPLACE VIEW public.hotfix_audit_log AS
    SELECT * FROM world.hotfix_audit_log;

CREATE OR REPLACE VIEW public.hotfix_registry AS
    SELECT * FROM realm_state.hotfix_registry;
