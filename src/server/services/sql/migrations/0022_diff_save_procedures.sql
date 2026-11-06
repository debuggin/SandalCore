-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
--
-- Direct covering indexes on 0001..0021 domain columns (replacing legacy drafts:
-- CREATE INDEX IF NOT EXISTS idx_character_items_covering_equip ON character_items (character_id, equip_slot) INCLUDE (item_guid, charges, durability, condition_state);
-- CREATE INDEX IF NOT EXISTS idx_character_resource_items_covering ON character_resource_items (character_id, bag_slot_type) INCLUDE (item_guid, charges, inventory_x, inventory_y);
-- CREATE INDEX IF NOT EXISTS idx_character_quest_commands_covering ON character_quest_commands (character_id, quest_guid) INCLUDE (command_guid, current_progress, "UpdateCounter");
-- ).

CREATE SCHEMA IF NOT EXISTS "character";
CREATE SCHEMA IF NOT EXISTS characters;

CREATE INDEX IF NOT EXISTS idx_character_items_covering_equip
    ON character_items (character_id, container, slot)
    INCLUDE (item_record_id, instance_guid, quantity, quality, durability);

CREATE INDEX IF NOT EXISTS idx_character_items_char_bag_slot_cover
    ON character_items (character_id, container, slot)
    INCLUDE (item_record_id, instance_guid, quantity, quality, durability);

CREATE INDEX IF NOT EXISTS idx_character_resource_items_covering
    ON character_resource_items (character_id, container, slot)
    INCLUDE (item_record_id, instance_guid, quantity, quality);

CREATE INDEX IF NOT EXISTS idx_character_quest_commands_covering
    ON character_quest_commands (character_id, id)
    INCLUDE (kind, quest, name, value);

CREATE INDEX IF NOT EXISTS idx_character_talents_char_tree_cover
    ON character_skill_ranks (character_id, tree_guid, ord)
    INCLUDE (rank_guid);

CREATE TABLE IF NOT EXISTS characters.character_save_fencing (
    character_id  TEXT        PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,
    fencing_token BIGINT      NOT NULL DEFAULT 0,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

DO $$
DECLARE
    src_schema TEXT := current_schema();
BEGIN
    DELETE FROM characters.character_save_fencing;

    ALTER TABLE characters.character_save_fencing
        DROP CONSTRAINT IF EXISTS character_save_fencing_character_id_fkey;

    DROP VIEW IF EXISTS "character".character_save_fencing;

    ALTER TABLE IF EXISTS characters.character_save_fencing
        ALTER COLUMN character_id TYPE TEXT USING character_id::text;

    EXECUTE format(
        'ALTER TABLE characters.character_save_fencing ADD CONSTRAINT character_save_fencing_character_id_fkey FOREIGN KEY (character_id) REFERENCES %I.characters (id) ON DELETE CASCADE',
        src_schema
    );
END
$$;

CREATE OR REPLACE VIEW "character".character_save_fencing AS
    SELECT * FROM characters.character_save_fencing;

DROP FUNCTION IF EXISTS "character".fn_check_and_advance_fencing_token(BIGINT, BIGINT);
DROP FUNCTION IF EXISTS characters.fn_check_and_advance_fencing_token(BIGINT, BIGINT);
DROP FUNCTION IF EXISTS "character".sp_diff_save_character_items(BIGINT, BIGINT, BIGINT[], BIGINT[], BIGINT[], INT[], INT[], INT[], INT[]);
DROP FUNCTION IF EXISTS characters.sp_diff_save_character_items(BIGINT, BIGINT, BIGINT[], BIGINT[], BIGINT[], INT[], INT[], INT[], INT[]);
DROP FUNCTION IF EXISTS "character".sp_diff_save_character_quests(BIGINT, BIGINT, BIGINT[], BIGINT[], INT[], INT[], BIGINT[], TIMESTAMPTZ[], BIGINT[], TEXT[], TEXT[], BIGINT[], BIGINT[], INT[], INT[]);
DROP FUNCTION IF EXISTS characters.sp_diff_save_character_quests(BIGINT, BIGINT, BIGINT[], BIGINT[], INT[], INT[], BIGINT[], TIMESTAMPTZ[], BIGINT[], TEXT[], TEXT[], BIGINT[], BIGINT[], INT[], INT[]);
DROP FUNCTION IF EXISTS "character".sp_diff_save_character_wallet_and_progress(BIGINT, BIGINT, INT[], BIGINT[], INT[], BIGINT[], INT[], INT[], INT[], BIGINT[]);
DROP FUNCTION IF EXISTS characters.sp_diff_save_character_wallet_and_progress(BIGINT, BIGINT, INT[], BIGINT[], INT[], BIGINT[], INT[], INT[], INT[], BIGINT[]);

CREATE OR REPLACE FUNCTION characters.fn_check_and_advance_fencing_token(
    p_character_id  TEXT,
    p_fencing_token BIGINT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
DECLARE
    v_existing_token BIGINT;
BEGIN
    SELECT fencing_token
      INTO v_existing_token
      FROM characters.character_save_fencing
     WHERE character_id = p_character_id
       FOR UPDATE;

    IF FOUND AND p_fencing_token < v_existing_token THEN
        RAISE EXCEPTION SQLSTATE '55000'
            USING MESSAGE = format(
                'stale fencing token %s for character %s (current %s)',
                p_fencing_token, p_character_id, v_existing_token
            );
    END IF;

    INSERT INTO characters.character_save_fencing (character_id, fencing_token, updated_at)
    VALUES (p_character_id, p_fencing_token, NOW())
    ON CONFLICT (character_id) DO UPDATE
        SET fencing_token = EXCLUDED.fencing_token,
            updated_at    = NOW()
      WHERE EXCLUDED.fencing_token >= characters.character_save_fencing.fencing_token;

    IF NOT FOUND THEN
        RAISE EXCEPTION SQLSTATE '55000'
            USING MESSAGE = format(
                'stale fencing token %s for character %s',
                p_fencing_token, p_character_id
            );
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".fn_check_and_advance_fencing_token(
    p_character_id  TEXT,
    p_fencing_token BIGINT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_items(
    p_character_id      TEXT,
    p_fencing_token     BIGINT,
    p_deleted_guids     TEXT[],
    p_containers        TEXT[],
    p_slots             INT[],
    p_item_record_ids   BIGINT[],
    p_instance_guids    TEXT[],
    p_quantities        INT[],
    p_qualities         INT[],
    p_durabilities      REAL[],
    p_persisted_healths REAL[] DEFAULT NULL,
    p_persisted_manas   REAL[] DEFAULT NULL,
    p_lock_expirations  BIGINT[] DEFAULT NULL
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_deleted_guids IS NOT NULL AND cardinality(p_deleted_guids) > 0 THEN
        DELETE FROM character_items
         WHERE character_id = p_character_id
           AND instance_guid = ANY(p_deleted_guids);
    END IF;

    IF p_containers IS NOT NULL AND cardinality(p_containers) > 0 THEN
        INSERT INTO character_items (
            character_id,
            container,
            slot,
            item_record_id,
            instance_guid,
            quantity,
            quality,
            durability,
            persisted_health,
            persisted_mana,
            lock_expiration
        )
        SELECT
            p_character_id,
            u.container,
            u.slot,
            u.item_record_id,
            u.instance_guid,
            u.quantity,
            u.quality,
            u.durability,
            u.persisted_health,
            u.persisted_mana,
            COALESCE(u.lock_expiration, 0)
        FROM unnest(
            p_containers,
            p_slots,
            p_item_record_ids,
            p_instance_guids,
            p_quantities,
            p_qualities,
            p_durabilities,
            COALESCE(p_persisted_healths, array_fill(NULL::real, ARRAY[cardinality(p_containers)])),
            COALESCE(p_persisted_manas, array_fill(NULL::real, ARRAY[cardinality(p_containers)])),
            COALESCE(p_lock_expirations, array_fill(0::bigint, ARRAY[cardinality(p_containers)]))
        ) AS u(
            container,
            slot,
            item_record_id,
            instance_guid,
            quantity,
            quality,
            durability,
            persisted_health,
            persisted_mana,
            lock_expiration
        )
        ON CONFLICT (character_id, container, slot) DO UPDATE
            SET item_record_id   = EXCLUDED.item_record_id,
                instance_guid    = EXCLUDED.instance_guid,
                quantity         = EXCLUDED.quantity,
                quality          = EXCLUDED.quality,
                durability       = EXCLUDED.durability,
                persisted_health = EXCLUDED.persisted_health,
                persisted_mana   = EXCLUDED.persisted_mana,
                lock_expiration  = EXCLUDED.lock_expiration;
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_items(
    p_character_id      TEXT,
    p_fencing_token     BIGINT,
    p_deleted_guids     TEXT[],
    p_containers        TEXT[],
    p_slots             INT[],
    p_item_record_ids   BIGINT[],
    p_instance_guids    TEXT[],
    p_quantities        INT[],
    p_qualities         INT[],
    p_durabilities      REAL[],
    p_persisted_healths REAL[] DEFAULT NULL,
    p_persisted_manas   REAL[] DEFAULT NULL,
    p_lock_expirations  BIGINT[] DEFAULT NULL
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_items(
        p_character_id,
        p_fencing_token,
        p_deleted_guids,
        p_containers,
        p_slots,
        p_item_record_ids,
        p_instance_guids,
        p_quantities,
        p_qualities,
        p_durabilities,
        p_persisted_healths,
        p_persisted_manas,
        p_lock_expirations
    );
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_quests(
    p_character_id        TEXT,
    p_fencing_token       BIGINT,
    p_deleted_quest_guids TEXT[],
    p_instance_guids      TEXT[],
    p_record_ids          BIGINT[],
    p_statuses            SMALLINT[],
    p_start_times         DOUBLE PRECISION[],
    p_source_guids        BIGINT[],
    p_positions           INT[],
    p_objectives          JSONB[],
    p_completed_records   BIGINT[],
    p_completions         INT[],
    p_var_names           TEXT[],
    p_var_values          BIGINT[],
    p_deleted_ids         BIGINT[] DEFAULT NULL,
    p_cmd_ids             BIGINT[] DEFAULT NULL,
    p_cmd_kinds           TEXT[] DEFAULT NULL,
    p_cmd_quests          TEXT[] DEFAULT NULL,
    p_cmd_names           TEXT[] DEFAULT NULL,
    p_cmd_values          BIGINT[] DEFAULT NULL
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_deleted_quest_guids IS NOT NULL AND cardinality(p_deleted_quest_guids) > 0 THEN
        DELETE FROM character_quests
         WHERE character_id = p_character_id
           AND instance_guid = ANY(p_deleted_quest_guids);
    END IF;

    IF p_instance_guids IS NOT NULL AND cardinality(p_instance_guids) > 0 THEN
        INSERT INTO character_quests (
            character_id,
            instance_guid,
            record_id,
            status,
            start_time,
            source_guid,
            position,
            objectives
        )
        SELECT
            p_character_id,
            q.instance_guid,
            q.record_id,
            q.status,
            q.start_time,
            q.source_guid,
            q.position,
            COALESCE(q.objectives, '[]'::jsonb)
        FROM unnest(
            p_instance_guids,
            p_record_ids,
            p_statuses,
            p_start_times,
            p_source_guids,
            p_positions,
            p_objectives
        ) AS q(
            instance_guid,
            record_id,
            status,
            start_time,
            source_guid,
            position,
            objectives
        )
        ON CONFLICT (character_id, instance_guid) DO UPDATE
            SET record_id   = EXCLUDED.record_id,
                status      = EXCLUDED.status,
                start_time  = EXCLUDED.start_time,
                source_guid = EXCLUDED.source_guid,
                position    = EXCLUDED.position,
                objectives  = EXCLUDED.objectives;
    END IF;

    IF p_completed_records IS NOT NULL AND cardinality(p_completed_records) > 0 THEN
        INSERT INTO character_quests_completed (
            character_id,
            record_id,
            completions
        )
        SELECT
            p_character_id,
            c.record_id,
            c.completions
        FROM unnest(p_completed_records, p_completions)
            AS c(record_id, completions)
        ON CONFLICT (character_id, record_id) DO UPDATE
            SET completions = EXCLUDED.completions;
    END IF;

    IF p_var_names IS NOT NULL AND cardinality(p_var_names) > 0 THEN
        INSERT INTO character_quest_variables (
            character_id,
            name,
            value
        )
        SELECT
            p_character_id,
            v.name,
            v.value
        FROM unnest(p_var_names, p_var_values)
            AS v(name, value)
        ON CONFLICT (character_id, name) DO UPDATE
            SET value = EXCLUDED.value;
    END IF;

    IF p_deleted_ids IS NOT NULL AND cardinality(p_deleted_ids) > 0 THEN
        DELETE FROM character_quest_commands
         WHERE character_id = p_character_id
           AND id = ANY(p_deleted_ids);
    END IF;

    IF p_cmd_ids IS NOT NULL AND cardinality(p_cmd_ids) > 0 THEN
        INSERT INTO character_quest_commands (
            id,
            character_id,
            kind,
            quest,
            name,
            value,
            created_at
        )
        SELECT
            cmd.id,
            p_character_id,
            cmd.kind,
            COALESCE(cmd.quest, ''),
            COALESCE(cmd.name, ''),
            COALESCE(cmd.value, 0),
            NOW()
        FROM unnest(
            p_cmd_ids,
            p_cmd_kinds,
            p_cmd_quests,
            p_cmd_names,
            p_cmd_values
        ) AS cmd(
            id,
            kind,
            quest,
            name,
            value
        )
        ON CONFLICT (id) DO UPDATE
            SET kind  = EXCLUDED.kind,
                quest = EXCLUDED.quest,
                name  = EXCLUDED.name,
                value = EXCLUDED.value;
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_quests(
    p_character_id        TEXT,
    p_fencing_token       BIGINT,
    p_deleted_quest_guids TEXT[],
    p_instance_guids      TEXT[],
    p_record_ids          BIGINT[],
    p_statuses            SMALLINT[],
    p_start_times         DOUBLE PRECISION[],
    p_source_guids        BIGINT[],
    p_positions           INT[],
    p_objectives          JSONB[],
    p_completed_records   BIGINT[],
    p_completions         INT[],
    p_var_names           TEXT[],
    p_var_values          BIGINT[],
    p_deleted_ids         BIGINT[] DEFAULT NULL,
    p_cmd_ids             BIGINT[] DEFAULT NULL,
    p_cmd_kinds           TEXT[] DEFAULT NULL,
    p_cmd_quests          TEXT[] DEFAULT NULL,
    p_cmd_names           TEXT[] DEFAULT NULL,
    p_cmd_values          BIGINT[] DEFAULT NULL
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_quests(
        p_character_id,
        p_fencing_token,
        p_deleted_quest_guids,
        p_instance_guids,
        p_record_ids,
        p_statuses,
        p_start_times,
        p_source_guids,
        p_positions,
        p_objectives,
        p_completed_records,
        p_completions,
        p_var_names,
        p_var_values,
        p_deleted_ids,
        p_cmd_ids,
        p_cmd_kinds,
        p_cmd_quests,
        p_cmd_names,
        p_cmd_values
    );
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_wallet_and_progress(
    p_character_id        TEXT,
    p_fencing_token       BIGINT,
    p_currency_guids      BIGINT[],
    p_currency_amounts    BIGINT[],
    p_leveling_type_guids BIGINT[],
    p_xp_amounts          BIGINT[],
    p_xp_debts            BIGINT[],
    p_profession_guids    BIGINT[],
    p_certification_tiers SMALLINT[],
    p_profession_levels   SMALLINT[],
    p_profession_xp       BIGINT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_currency_guids IS NOT NULL AND cardinality(p_currency_guids) > 0 THEN
        INSERT INTO character_wallet (
            character_id,
            currency_guid,
            amount,
            updated_at
        )
        SELECT
            p_character_id,
            w.currency_guid,
            w.amount,
            NOW()
        FROM unnest(p_currency_guids, p_currency_amounts)
            AS w(currency_guid, amount)
        ON CONFLICT (character_id, currency_guid) DO UPDATE
            SET amount     = EXCLUDED.amount,
                updated_at = NOW();
    END IF;

    IF p_leveling_type_guids IS NOT NULL AND cardinality(p_leveling_type_guids) > 0 THEN
        INSERT INTO character_experience (
            character_id,
            leveling_type_guid,
            xp,
            debt,
            updated_at
        )
        SELECT
            p_character_id,
            x.leveling_type_guid,
            x.xp,
            COALESCE(x.debt, 0),
            NOW()
        FROM unnest(p_leveling_type_guids, p_xp_amounts, p_xp_debts)
            AS x(leveling_type_guid, xp, debt)
        ON CONFLICT (character_id, leveling_type_guid) DO UPDATE
            SET xp         = EXCLUDED.xp,
                debt       = EXCLUDED.debt,
                updated_at = NOW();
    END IF;

    IF p_profession_guids IS NOT NULL AND cardinality(p_profession_guids) > 0 THEN
        INSERT INTO character_professions (
            character_id,
            profession_guid,
            certification_tier,
            level,
            xp,
            updated_at
        )
        SELECT
            p_character_id,
            pr.profession_guid,
            pr.certification_tier,
            pr.level,
            pr.xp,
            NOW()
        FROM unnest(p_profession_guids, p_certification_tiers, p_profession_levels, p_profession_xp)
            AS pr(profession_guid, certification_tier, level, xp)
        ON CONFLICT (character_id, profession_guid) DO UPDATE
            SET certification_tier = EXCLUDED.certification_tier,
                level              = EXCLUDED.level,
                xp                 = EXCLUDED.xp,
                updated_at         = NOW();
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_wallet_and_progress(
    p_character_id        TEXT,
    p_fencing_token       BIGINT,
    p_currency_guids      BIGINT[],
    p_currency_amounts    BIGINT[],
    p_leveling_type_guids BIGINT[],
    p_xp_amounts          BIGINT[],
    p_xp_debts            BIGINT[],
    p_profession_guids    BIGINT[],
    p_certification_tiers SMALLINT[],
    p_profession_levels   SMALLINT[],
    p_profession_xp       BIGINT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_wallet_and_progress(
        p_character_id,
        p_fencing_token,
        p_currency_guids,
        p_currency_amounts,
        p_leveling_type_guids,
        p_xp_amounts,
        p_xp_debts,
        p_profession_guids,
        p_certification_tiers,
        p_profession_levels,
        p_profession_xp
    );
END;
$$;

DO $$
BEGIN
    GRANT SELECT, INSERT, UPDATE, DELETE
        ON characters.character_save_fencing, "character".character_save_fencing
        TO sandal_char, sandal_admin;

    GRANT EXECUTE ON FUNCTION
        characters.fn_check_and_advance_fencing_token(TEXT, BIGINT),
        "character".fn_check_and_advance_fencing_token(TEXT, BIGINT),
        characters.sp_diff_save_character_items(TEXT, BIGINT, TEXT[], TEXT[], INT[], BIGINT[], TEXT[], INT[], INT[], REAL[], REAL[], REAL[], BIGINT[]),
        "character".sp_diff_save_character_items(TEXT, BIGINT, TEXT[], TEXT[], INT[], BIGINT[], TEXT[], INT[], INT[], REAL[], REAL[], REAL[], BIGINT[]),
        characters.sp_diff_save_character_quests(TEXT, BIGINT, TEXT[], TEXT[], BIGINT[], SMALLINT[], DOUBLE PRECISION[], BIGINT[], INT[], JSONB[], BIGINT[], INT[], TEXT[], BIGINT[], BIGINT[], BIGINT[], TEXT[], TEXT[], TEXT[], BIGINT[]),
        "character".sp_diff_save_character_quests(TEXT, BIGINT, TEXT[], TEXT[], BIGINT[], SMALLINT[], DOUBLE PRECISION[], BIGINT[], INT[], JSONB[], BIGINT[], INT[], TEXT[], BIGINT[], BIGINT[], BIGINT[], TEXT[], TEXT[], TEXT[], BIGINT[]),
        characters.sp_diff_save_character_wallet_and_progress(TEXT, BIGINT, BIGINT[], BIGINT[], BIGINT[], BIGINT[], BIGINT[], BIGINT[], SMALLINT[], SMALLINT[], BIGINT[]),
        "character".sp_diff_save_character_wallet_and_progress(TEXT, BIGINT, BIGINT[], BIGINT[], BIGINT[], BIGINT[], BIGINT[], BIGINT[], SMALLINT[], SMALLINT[], BIGINT[])
        TO sandal_char, sandal_admin;
EXCEPTION
    WHEN undefined_object OR insufficient_privilege THEN
        NULL;
END
$$;
