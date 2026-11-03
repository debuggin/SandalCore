-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

CREATE SCHEMA IF NOT EXISTS auth;
CREATE SCHEMA IF NOT EXISTS world;
CREATE SCHEMA IF NOT EXISTS realm_state;

CREATE TABLE IF NOT EXISTS auth.rbac_roles (
    role_id     SMALLINT PRIMARY KEY CHECK (role_id BETWEEN 0 AND 4),
    role_name   TEXT     NOT NULL UNIQUE,
    parent_role SMALLINT REFERENCES auth.rbac_roles(role_id)
);

CREATE INDEX IF NOT EXISTS idx_rbac_roles_parent
    ON auth.rbac_roles (parent_role);

INSERT INTO auth.rbac_roles (role_id, role_name, parent_role)
VALUES
    (0, 'player', NULL),
    (1, 'guide', 0),
    (2, 'gm', 1),
    (3, 'lead_gm', 2),
    (4, 'admin', 3)
ON CONFLICT (role_id) DO NOTHING;

CREATE TABLE IF NOT EXISTS auth.rbac_permissions (
    permission_id TEXT     PRIMARY KEY,
    min_tier      SMALLINT NOT NULL REFERENCES auth.rbac_roles(role_id),
    description   TEXT     NOT NULL DEFAULT ''
);

CREATE INDEX IF NOT EXISTS idx_rbac_permissions_min_tier
    ON auth.rbac_permissions (min_tier);

CREATE TABLE IF NOT EXISTS auth.rbac_role_permissions (
    role_id       SMALLINT NOT NULL REFERENCES auth.rbac_roles(role_id) ON DELETE CASCADE,
    permission_id TEXT     NOT NULL REFERENCES auth.rbac_permissions(permission_id) ON DELETE CASCADE,
    PRIMARY KEY (role_id, permission_id)
);

CREATE INDEX IF NOT EXISTS idx_rbac_role_permissions_perm
    ON auth.rbac_role_permissions (permission_id);

CREATE TABLE IF NOT EXISTS auth.rbac_account_permissions (
    account_id    BIGINT      NOT NULL REFERENCES accounts(id) ON DELETE CASCADE,
    role_id       SMALLINT    NOT NULL DEFAULT 0 REFERENCES auth.rbac_roles(role_id),
    permission_id TEXT        NOT NULL REFERENCES auth.rbac_permissions(permission_id) ON DELETE CASCADE,
    granted       BOOLEAN     NOT NULL DEFAULT TRUE,
    granted_by    BIGINT      REFERENCES accounts(id),
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (account_id, permission_id)
);

CREATE INDEX IF NOT EXISTS idx_rbac_account_permissions_role
    ON auth.rbac_account_permissions (role_id);

CREATE INDEX IF NOT EXISTS idx_rbac_account_permissions_perm
    ON auth.rbac_account_permissions (permission_id);

CREATE INDEX IF NOT EXISTS idx_rbac_account_permissions_granted_by
    ON auth.rbac_account_permissions (granted_by);

DO $$
BEGIN
    CREATE TABLE IF NOT EXISTS realm_state.gm_audit_log (
        audit_id           UUID        PRIMARY KEY,
        parent_audit_id    UUID        REFERENCES realm_state.gm_audit_log(audit_id),
        actor_account_id   BIGINT      NOT NULL REFERENCES accounts(id),
        actor_character_id BIGINT      REFERENCES characters(id),
        actor_role         SMALLINT    NOT NULL REFERENCES auth.rbac_roles(role_id),
        command_name       TEXT        NOT NULL,
        target_entity_id   BIGINT,
        target_coords      JSONB,
        before_state       JSONB       NOT NULL DEFAULT '{}'::jsonb,
        after_state        JSONB       NOT NULL DEFAULT '{}'::jsonb,
        client_ip          INET,
        created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
    );
EXCEPTION
    WHEN datatype_mismatch THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS realm_state.gm_audit_log (
                audit_id           UUID        PRIMARY KEY,
                parent_audit_id    UUID        REFERENCES realm_state.gm_audit_log(audit_id),
                actor_account_id   BIGINT      NOT NULL REFERENCES accounts(id),
                actor_character_id BIGINT,
                actor_role         SMALLINT    NOT NULL REFERENCES auth.rbac_roles(role_id),
                command_name       TEXT        NOT NULL,
                target_entity_id   BIGINT,
                target_coords      JSONB,
                before_state       JSONB       NOT NULL DEFAULT ''{}''::jsonb,
                after_state        JSONB       NOT NULL DEFAULT ''{}''::jsonb,
                client_ip          INET,
                created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
            )',
            'TABLE'
        );
END
$$;

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_parent
    ON realm_state.gm_audit_log (parent_audit_id);

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_actor_account
    ON realm_state.gm_audit_log (actor_account_id, created_at DESC);

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_actor_character
    ON realm_state.gm_audit_log (actor_character_id);

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_actor_role
    ON realm_state.gm_audit_log (actor_role);

CREATE OR REPLACE VIEW world.gm_audit_log AS
    SELECT * FROM realm_state.gm_audit_log;

CREATE OR REPLACE FUNCTION realm_state.fn_gm_audit_log_immutable()
RETURNS TRIGGER
LANGUAGE plpgsql
AS $$
BEGIN
    RAISE EXCEPTION 'gm_audit_log is append-only; UPDATE and DELETE are prohibited'
        USING ERRCODE = '55000';
END;
$$;

CREATE TABLE IF NOT EXISTS realm_state.command_idempotency_ledger (
    command_id       UUID        PRIMARY KEY CHECK (substring(command_id::text from 15 for 1) = '7'),
    actor_account_id BIGINT      NOT NULL REFERENCES accounts(id),
    command_name     TEXT        NOT NULL,
    payload_sha256   BYTEA       NOT NULL CHECK (octet_length(payload_sha256) = 32),
    status           SMALLINT    NOT NULL DEFAULT 1,
    response_payload JSONB       NOT NULL DEFAULT '{}'::jsonb,
    audit_id         UUID        REFERENCES realm_state.gm_audit_log(audit_id),
    created_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    expires_at       TIMESTAMPTZ NOT NULL DEFAULT (NOW() + INTERVAL '24 hours')
);

CREATE INDEX IF NOT EXISTS idx_command_idempotency_actor_account
    ON realm_state.command_idempotency_ledger (actor_account_id);

CREATE INDEX IF NOT EXISTS idx_command_idempotency_audit_id
    ON realm_state.command_idempotency_ledger (audit_id);

CREATE INDEX IF NOT EXISTS idx_command_idempotency_expires_at
    ON realm_state.command_idempotency_ledger (expires_at);

CREATE OR REPLACE VIEW world.command_idempotency_ledger AS
    SELECT * FROM realm_state.command_idempotency_ledger;

DO $$
DECLARE
    src_schema TEXT := current_schema();
BEGIN
    DROP TRIGGER IF EXISTS trg_gm_audit_log_immutable ON realm_state.gm_audit_log;

    DELETE FROM realm_state.command_idempotency_ledger;
    DELETE FROM realm_state.gm_audit_log;
    DELETE FROM auth.rbac_account_permissions;

    ALTER TABLE auth.rbac_account_permissions
        DROP CONSTRAINT IF EXISTS rbac_account_permissions_account_id_fkey,
        DROP CONSTRAINT IF EXISTS rbac_account_permissions_granted_by_fkey;
    EXECUTE format(
        'ALTER TABLE auth.rbac_account_permissions ADD CONSTRAINT rbac_account_permissions_account_id_fkey FOREIGN KEY (account_id) REFERENCES %I.accounts (id) ON DELETE CASCADE',
        src_schema
    );
    EXECUTE format(
        'ALTER TABLE auth.rbac_account_permissions ADD CONSTRAINT rbac_account_permissions_granted_by_fkey FOREIGN KEY (granted_by) REFERENCES %I.accounts (id) ON DELETE SET NULL',
        src_schema
    );

    ALTER TABLE realm_state.gm_audit_log
        DROP CONSTRAINT IF EXISTS gm_audit_log_actor_account_id_fkey,
        DROP CONSTRAINT IF EXISTS gm_audit_log_actor_character_id_fkey;
    EXECUTE format(
        'ALTER TABLE realm_state.gm_audit_log ADD CONSTRAINT gm_audit_log_actor_account_id_fkey FOREIGN KEY (actor_account_id) REFERENCES %I.accounts (id)',
        src_schema
    );
    BEGIN
        EXECUTE format(
            'ALTER TABLE realm_state.gm_audit_log ADD CONSTRAINT gm_audit_log_actor_character_id_fkey FOREIGN KEY (actor_character_id) REFERENCES %I.characters (id)',
            src_schema
        );
    EXCEPTION
        WHEN datatype_mismatch THEN
            NULL;
    END;

    ALTER TABLE realm_state.command_idempotency_ledger
        DROP CONSTRAINT IF EXISTS command_idempotency_ledger_actor_account_id_fkey;
    EXECUTE format(
        'ALTER TABLE realm_state.command_idempotency_ledger ADD CONSTRAINT command_idempotency_ledger_actor_account_id_fkey FOREIGN KEY (actor_account_id) REFERENCES %I.accounts (id)',
        src_schema
    );
END
$$;

DROP TRIGGER IF EXISTS trg_gm_audit_log_immutable ON realm_state.gm_audit_log;
CREATE TRIGGER trg_gm_audit_log_immutable
    BEFORE UPDATE OR DELETE ON realm_state.gm_audit_log
    FOR EACH ROW EXECUTE FUNCTION realm_state.fn_gm_audit_log_immutable();

DO $$
BEGIN
    GRANT SELECT, INSERT, UPDATE, DELETE
        ON auth.rbac_roles, auth.rbac_permissions, auth.rbac_role_permissions, auth.rbac_account_permissions
        TO sandal_auth, sandal_admin;

    GRANT SELECT
        ON auth.rbac_roles, auth.rbac_permissions, auth.rbac_role_permissions, auth.rbac_account_permissions
        TO sandal_world;

    GRANT SELECT, INSERT ON realm_state.gm_audit_log, world.gm_audit_log TO sandal_world, sandal_admin;
    REVOKE UPDATE, DELETE, TRUNCATE ON realm_state.gm_audit_log, world.gm_audit_log FROM PUBLIC, sandal_auth, sandal_char, sandal_world;

    GRANT SELECT, INSERT, UPDATE
        ON realm_state.command_idempotency_ledger, world.command_idempotency_ledger
        TO sandal_world, sandal_admin;
EXCEPTION
    WHEN undefined_object OR insufficient_privilege THEN
        NULL;
END
$$;
