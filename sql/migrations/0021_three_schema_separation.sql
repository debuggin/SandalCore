-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

CREATE SCHEMA IF NOT EXISTS auth;
CREATE SCHEMA IF NOT EXISTS "character";
CREATE SCHEMA IF NOT EXISTS characters;
CREATE SCHEMA IF NOT EXISTS world;
CREATE SCHEMA IF NOT EXISTS realm_state;

CREATE INDEX IF NOT EXISTS idx_sessions_account_id
    ON sessions (account_id);

CREATE INDEX IF NOT EXISTS idx_city_node_state_parent_guid
    ON city_node_state (parent_guid);

CREATE TABLE IF NOT EXISTS realm_state.spawner_state (
    spawner_guid       BIGINT      PRIMARY KEY,
    node_guid          BIGINT      NOT NULL REFERENCES city_node_state (node_guid) ON DELETE CASCADE,
    respawn_at_unix_ms BIGINT      NOT NULL DEFAULT 0,
    population_counter INT         NOT NULL DEFAULT 0 CHECK (population_counter >= 0),
    active_wave_idx    INT         NOT NULL DEFAULT 0 CHECK (active_wave_idx >= 0),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_spawner_state_node_guid
    ON realm_state.spawner_state (node_guid, respawn_at_unix_ms);

CREATE TABLE IF NOT EXISTS realm_state.buy_orders (
    order_id           BIGSERIAL   PRIMARY KEY,
    node_guid          BIGINT      NOT NULL REFERENCES city_node_state (node_guid) ON DELETE CASCADE,
    buyer_char_id      TEXT        NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    item_guid          BIGINT      NOT NULL CHECK (item_guid > 0),
    unit_price_copper  BIGINT      NOT NULL CHECK (unit_price_copper > 0),
    quantity_remaining INT         NOT NULL CHECK (quantity_remaining >= 0),
    escrow_copper      BIGINT      NOT NULL CHECK (escrow_copper >= 0),
    status             TEXT        NOT NULL DEFAULT 'OPEN'
        CHECK (status IN ('OPEN', 'FILLED', 'CANCELLED', 'EXPIRED')),
    created_at         TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at         TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_buy_orders_node_item_status
    ON realm_state.buy_orders (node_guid, item_guid, status, unit_price_copper DESC);

CREATE INDEX IF NOT EXISTS idx_buy_orders_buyer_char_id
    ON realm_state.buy_orders (buyer_char_id);

CREATE OR REPLACE VIEW world.spawner_state AS
    SELECT * FROM realm_state.spawner_state;

CREATE OR REPLACE VIEW world.buy_orders AS
    SELECT * FROM realm_state.buy_orders;

CREATE OR REPLACE VIEW public.spawner_state AS
    SELECT * FROM realm_state.spawner_state;

CREATE OR REPLACE VIEW public.buy_orders AS
    SELECT * FROM realm_state.buy_orders;

DO $$
DECLARE
    src_schema TEXT := current_schema();
    tbl        TEXT;
    r           TEXT;
BEGIN
    FOREACH r IN ARRAY ARRAY['sandal_auth', 'sandal_char', 'sandal_world', 'sandal_admin'] LOOP
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = r) THEN
                EXECUTE format('CREATE ROLE %I NOINHERIT NOLOGIN', r);
            END IF;
        EXCEPTION
            WHEN duplicate_object OR insufficient_privilege THEN
                NULL;
        END;
    END LOOP;

    DELETE FROM realm_state.buy_orders;
    DELETE FROM realm_state.spawner_state;

    ALTER TABLE realm_state.spawner_state
        DROP CONSTRAINT IF EXISTS spawner_state_node_guid_fkey;
    EXECUTE format(
        'ALTER TABLE realm_state.spawner_state ADD CONSTRAINT spawner_state_node_guid_fkey FOREIGN KEY (node_guid) REFERENCES %I.city_node_state (node_guid) ON DELETE CASCADE',
        src_schema
    );

    ALTER TABLE realm_state.buy_orders
        DROP CONSTRAINT IF EXISTS buy_orders_node_guid_fkey,
        DROP CONSTRAINT IF EXISTS buy_orders_buyer_char_id_fkey;
    EXECUTE format(
        'ALTER TABLE realm_state.buy_orders ADD CONSTRAINT buy_orders_node_guid_fkey FOREIGN KEY (node_guid) REFERENCES %I.city_node_state (node_guid) ON DELETE CASCADE',
        src_schema
    );
    EXECUTE format(
        'ALTER TABLE realm_state.buy_orders ADD CONSTRAINT buy_orders_buyer_char_id_fkey FOREIGN KEY (buyer_char_id) REFERENCES %I.characters (id) ON DELETE CASCADE',
        src_schema
    );

    FOREACH tbl IN ARRAY ARRAY['accounts', 'login_tokens', 'sessions'] LOOP
        EXECUTE format('CREATE OR REPLACE VIEW auth.%I AS SELECT * FROM %I.%I', tbl, src_schema, tbl);
    END LOOP;

    FOREACH tbl IN ARRAY ARRAY[
        'characters',
        'character_items',
        'character_resource_items',
        'character_warehouse_items',
        'character_wallet',
        'character_processing_jobs',
        'character_known_recipes',
        'character_experience',
        'character_skill_ranks',
        'character_action_bar',
        'character_professions',
        'character_completed_ftues',
        'character_attunements',
        'character_home_attunement',
        'character_quests',
        'character_quests_completed',
        'character_quest_variables',
        'character_quest_commands'
    ] LOOP
        EXECUTE format('CREATE OR REPLACE VIEW "character".%I AS SELECT * FROM %I.%I', tbl, src_schema, tbl);
        EXECUTE format('CREATE OR REPLACE VIEW characters.%I AS SELECT * FROM %I.%I', tbl, src_schema, tbl);
    END LOOP;

    FOREACH tbl IN ARRAY ARRAY['world_servers', 'city_node_state', 'city_node_clock', 'city_node_seed'] LOOP
        EXECUTE format('CREATE OR REPLACE VIEW world.%I AS SELECT * FROM %I.%I', tbl, src_schema, tbl);
        EXECUTE format('CREATE OR REPLACE VIEW realm_state.%I AS SELECT * FROM %I.%I', tbl, src_schema, tbl);
    END LOOP;

    BEGIN
        GRANT USAGE ON SCHEMA auth TO sandal_auth, sandal_admin;
        GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA auth TO sandal_auth, sandal_admin;

        GRANT USAGE ON SCHEMA "character", characters TO sandal_char, sandal_world, sandal_admin;
        GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA "character", characters TO sandal_char, sandal_world, sandal_admin;

        GRANT USAGE ON SCHEMA world, realm_state TO sandal_world, sandal_admin;
        GRANT SELECT, INSERT, UPDATE, DELETE ON ALL TABLES IN SCHEMA world, realm_state TO sandal_world, sandal_admin;
        GRANT USAGE, SELECT ON ALL SEQUENCES IN SCHEMA world, realm_state TO sandal_world, sandal_admin;

        GRANT sandal_auth, sandal_char, sandal_world TO sandal_admin;
    EXCEPTION
        WHEN undefined_object OR insufficient_privilege THEN
            NULL;
    END;
END
$$;
