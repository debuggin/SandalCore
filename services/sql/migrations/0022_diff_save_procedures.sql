-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

CREATE SCHEMA IF NOT EXISTS "character";
CREATE SCHEMA IF NOT EXISTS characters;

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_items_covering_equip ON character_items (character_id, equip_slot) INCLUDE (item_guid, charges, durability, condition_state);
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_items_covering_equip ON character_items (character_id, container, slot) INCLUDE (item_record_id, instance_guid, quantity, quality, durability)',
            'INDEX'
        );
END
$$;

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_resource_items_covering ON character_resource_items (character_id, bag_slot_type) INCLUDE (item_guid, charges, inventory_x, inventory_y);
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_resource_items_covering ON character_resource_items (character_id, container, slot) INCLUDE (item_record_id, instance_guid, quantity, quality)',
            'INDEX'
        );
END
$$;

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_quest_commands_covering ON character_quest_commands (character_id, quest_guid) INCLUDE (command_guid, current_progress, "UpdateCounter");
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_quest_commands_covering ON character_quest_commands (character_id, id) INCLUDE (kind, quest, name, value)',
            'INDEX'
        );
END
$$;

DO $$
DECLARE
    src_schema TEXT := current_schema();
BEGIN
    BEGIN
        CREATE TABLE IF NOT EXISTS characters.character_save_fencing (
            character_id  BIGINT      PRIMARY KEY REFERENCES characters(id) ON DELETE CASCADE,
            fencing_token BIGINT      NOT NULL DEFAULT 0,
            updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
    EXCEPTION
        WHEN datatype_mismatch THEN
            EXECUTE format(
                'CREATE %s IF NOT EXISTS characters.character_save_fencing (character_id BIGINT PRIMARY KEY, fencing_token BIGINT NOT NULL DEFAULT 0, updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW())',
                'TABLE'
            );
    END;

    DELETE FROM characters.character_save_fencing;

    BEGIN
        ALTER TABLE characters.character_save_fencing
            DROP CONSTRAINT IF EXISTS character_save_fencing_character_id_fkey;
        EXECUTE format(
            'ALTER TABLE characters.character_save_fencing ADD CONSTRAINT character_save_fencing_character_id_fkey FOREIGN KEY (character_id) REFERENCES %I.characters (id) ON DELETE CASCADE',
            src_schema
        );
    EXCEPTION
        WHEN datatype_mismatch THEN
            NULL;
    END;
END
$$;

CREATE OR REPLACE VIEW "character".character_save_fencing AS
    SELECT * FROM characters.character_save_fencing;

CREATE OR REPLACE FUNCTION characters.fn_check_and_advance_fencing_token(
    p_character_id  BIGINT,
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
    p_character_id  BIGINT,
    p_fencing_token BIGINT
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_items(
    p_character_id     BIGINT,
    p_fencing_token    BIGINT,
    p_deleted_ids      BIGINT[],
    p_item_ids         BIGINT[],
    p_item_guids       BIGINT[],
    p_equip_slots      INT[],
    p_charges          INT[],
    p_durabilities     INT[],
    p_condition_states INT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_deleted_ids IS NOT NULL AND cardinality(p_deleted_ids) > 0 THEN
        DELETE FROM characters.character_items
         WHERE character_id = p_character_id
           AND id = ANY(p_deleted_ids);
    END IF;

    IF p_item_ids IS NOT NULL AND cardinality(p_item_ids) > 0 THEN
        INSERT INTO characters.character_items (
            id,
            character_id,
            item_guid,
            equip_slot,
            charges,
            durability,
            condition_state,
            updated_at
        )
        SELECT
            u.item_id,
            p_character_id,
            u.item_guid,
            u.equip_slot,
            u.charges,
            u.durability,
            u.condition_state,
            NOW()
        FROM unnest(
            p_item_ids,
            p_item_guids,
            p_equip_slots,
            p_charges,
            p_durabilities,
            p_condition_states
        ) AS u(
            item_id,
            item_guid,
            equip_slot,
            charges,
            durability,
            condition_state
        )
        ON CONFLICT (id) DO UPDATE
            SET item_guid       = EXCLUDED.item_guid,
                equip_slot      = EXCLUDED.equip_slot,
                charges         = EXCLUDED.charges,
                durability      = EXCLUDED.durability,
                condition_state = EXCLUDED.condition_state,
                updated_at      = NOW();
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_items(
    p_character_id     BIGINT,
    p_fencing_token    BIGINT,
    p_deleted_ids      BIGINT[],
    p_item_ids         BIGINT[],
    p_item_guids       BIGINT[],
    p_equip_slots      INT[],
    p_charges          INT[],
    p_durabilities     INT[],
    p_condition_states INT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_items(
        p_character_id,
        p_fencing_token,
        p_deleted_ids,
        p_item_ids,
        p_item_guids,
        p_equip_slots,
        p_charges,
        p_durabilities,
        p_condition_states
    );
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_quests(
    p_character_id          BIGINT,
    p_fencing_token         BIGINT,
    p_deleted_quest_guids   BIGINT[],
    p_quest_guids           BIGINT[],
    p_quest_states          INT[],
    p_quest_steps           INT[],
    p_completed_quest_guids BIGINT[],
    p_completed_timestamps  TIMESTAMPTZ[],
    p_var_quest_guids       BIGINT[],
    p_var_names             TEXT[],
    p_var_values            TEXT[],
    p_cmd_quest_guids       BIGINT[],
    p_command_guids         BIGINT[],
    p_cmd_progress          INT[],
    p_cmd_update_counters   INT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_deleted_quest_guids IS NOT NULL AND cardinality(p_deleted_quest_guids) > 0 THEN
        DELETE FROM characters.character_quests
         WHERE character_id = p_character_id
           AND quest_guid = ANY(p_deleted_quest_guids);
    END IF;

    IF p_quest_guids IS NOT NULL AND cardinality(p_quest_guids) > 0 THEN
        INSERT INTO characters.character_quests (
            character_id,
            quest_guid,
            quest_state,
            quest_step,
            updated_at
        )
        SELECT
            p_character_id,
            q.quest_guid,
            q.quest_state,
            q.quest_step,
            NOW()
        FROM unnest(p_quest_guids, p_quest_states, p_quest_steps)
            AS q(quest_guid, quest_state, quest_step)
        ON CONFLICT (character_id, quest_guid) DO UPDATE
            SET quest_state = EXCLUDED.quest_state,
                quest_step  = EXCLUDED.quest_step,
                updated_at  = NOW();
    END IF;

    IF p_completed_quest_guids IS NOT NULL AND cardinality(p_completed_quest_guids) > 0 THEN
        INSERT INTO characters.character_quests_completed (
            character_id,
            quest_guid,
            completed_at
        )
        SELECT
            p_character_id,
            c.quest_guid,
            c.completed_at
        FROM unnest(p_completed_quest_guids, p_completed_timestamps)
            AS c(quest_guid, completed_at)
        ON CONFLICT (character_id, quest_guid) DO UPDATE
            SET completed_at = EXCLUDED.completed_at;
    END IF;

    IF p_var_quest_guids IS NOT NULL AND cardinality(p_var_quest_guids) > 0 THEN
        INSERT INTO characters.character_quest_variables (
            character_id,
            quest_guid,
            var_name,
            var_value,
            updated_at
        )
        SELECT
            p_character_id,
            v.quest_guid,
            v.var_name,
            v.var_value,
            NOW()
        FROM unnest(p_var_quest_guids, p_var_names, p_var_values)
            AS v(quest_guid, var_name, var_value)
        ON CONFLICT (character_id, quest_guid, var_name) DO UPDATE
            SET var_value  = EXCLUDED.var_value,
                updated_at = NOW();
    END IF;

    IF p_cmd_quest_guids IS NOT NULL AND cardinality(p_cmd_quest_guids) > 0 THEN
        INSERT INTO characters.character_quest_commands (
            character_id,
            quest_guid,
            command_guid,
            current_progress,
            "UpdateCounter",
            updated_at
        )
        SELECT
            p_character_id,
            cmd.quest_guid,
            cmd.command_guid,
            cmd.current_progress,
            cmd.update_counter,
            NOW()
        FROM unnest(
            p_cmd_quest_guids,
            p_command_guids,
            p_cmd_progress,
            p_cmd_update_counters
        ) AS cmd(
            quest_guid,
            command_guid,
            current_progress,
            update_counter
        )
        ON CONFLICT (character_id, quest_guid, command_guid) DO UPDATE
            SET current_progress = EXCLUDED.current_progress,
                "UpdateCounter"  = EXCLUDED."UpdateCounter",
                updated_at       = NOW();
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_quests(
    p_character_id          BIGINT,
    p_fencing_token         BIGINT,
    p_deleted_quest_guids   BIGINT[],
    p_quest_guids           BIGINT[],
    p_quest_states          INT[],
    p_quest_steps           INT[],
    p_completed_quest_guids BIGINT[],
    p_completed_timestamps  TIMESTAMPTZ[],
    p_var_quest_guids       BIGINT[],
    p_var_names             TEXT[],
    p_var_values            TEXT[],
    p_cmd_quest_guids       BIGINT[],
    p_command_guids         BIGINT[],
    p_cmd_progress          INT[],
    p_cmd_update_counters   INT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_quests(
        p_character_id,
        p_fencing_token,
        p_deleted_quest_guids,
        p_quest_guids,
        p_quest_states,
        p_quest_steps,
        p_completed_quest_guids,
        p_completed_timestamps,
        p_var_quest_guids,
        p_var_names,
        p_var_values,
        p_cmd_quest_guids,
        p_command_guids,
        p_cmd_progress,
        p_cmd_update_counters
    );
END;
$$;

CREATE OR REPLACE FUNCTION characters.sp_diff_save_character_wallet_and_progress(
    p_character_id      BIGINT,
    p_fencing_token     BIGINT,
    p_currency_ids      INT[],
    p_currency_amounts  BIGINT[],
    p_xp_types          INT[],
    p_xp_amounts        BIGINT[],
    p_xp_levels         INT[],
    p_profession_ids    INT[],
    p_profession_levels INT[],
    p_profession_xp     BIGINT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.fn_check_and_advance_fencing_token(p_character_id, p_fencing_token);

    IF p_currency_ids IS NOT NULL AND cardinality(p_currency_ids) > 0 THEN
        INSERT INTO characters.character_wallet (
            character_id,
            currency_id,
            amount,
            updated_at
        )
        SELECT
            p_character_id,
            w.currency_id,
            w.amount,
            NOW()
        FROM unnest(p_currency_ids, p_currency_amounts)
            AS w(currency_id, amount)
        ON CONFLICT (character_id, currency_id) DO UPDATE
            SET amount     = EXCLUDED.amount,
                updated_at = NOW();
    END IF;

    IF p_xp_types IS NOT NULL AND cardinality(p_xp_types) > 0 THEN
        INSERT INTO characters.character_experience (
            character_id,
            xp_type,
            experience,
            level,
            updated_at
        )
        SELECT
            p_character_id,
            x.xp_type,
            x.experience,
            x.level,
            NOW()
        FROM unnest(p_xp_types, p_xp_amounts, p_xp_levels)
            AS x(xp_type, experience, level)
        ON CONFLICT (character_id, xp_type) DO UPDATE
            SET experience = EXCLUDED.experience,
                level      = EXCLUDED.level,
                updated_at = NOW();
    END IF;

    IF p_profession_ids IS NOT NULL AND cardinality(p_profession_ids) > 0 THEN
        INSERT INTO characters.character_professions (
            character_id,
            profession_id,
            skill_level,
            skill_xp,
            updated_at
        )
        SELECT
            p_character_id,
            pr.profession_id,
            pr.skill_level,
            pr.skill_xp,
            NOW()
        FROM unnest(p_profession_ids, p_profession_levels, p_profession_xp)
            AS pr(profession_id, skill_level, skill_xp)
        ON CONFLICT (character_id, profession_id) DO UPDATE
            SET skill_level = EXCLUDED.skill_level,
                skill_xp    = EXCLUDED.skill_xp,
                updated_at  = NOW();
    END IF;
END;
$$;

CREATE OR REPLACE FUNCTION "character".sp_diff_save_character_wallet_and_progress(
    p_character_id      BIGINT,
    p_fencing_token     BIGINT,
    p_currency_ids      INT[],
    p_currency_amounts  BIGINT[],
    p_xp_types          INT[],
    p_xp_amounts        BIGINT[],
    p_xp_levels         INT[],
    p_profession_ids    INT[],
    p_profession_levels INT[],
    p_profession_xp     BIGINT[]
) RETURNS VOID
LANGUAGE plpgsql
AS $$
BEGIN
    PERFORM characters.sp_diff_save_character_wallet_and_progress(
        p_character_id,
        p_fencing_token,
        p_currency_ids,
        p_currency_amounts,
        p_xp_types,
        p_xp_amounts,
        p_xp_levels,
        p_profession_ids,
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
        characters.fn_check_and_advance_fencing_token(BIGINT, BIGINT),
        "character".fn_check_and_advance_fencing_token(BIGINT, BIGINT),
        characters.sp_diff_save_character_items(BIGINT, BIGINT, BIGINT[], BIGINT[], BIGINT[], INT[], INT[], INT[], INT[]),
        "character".sp_diff_save_character_items(BIGINT, BIGINT, BIGINT[], BIGINT[], BIGINT[], INT[], INT[], INT[], INT[]),
        characters.sp_diff_save_character_quests(BIGINT, BIGINT, BIGINT[], BIGINT[], INT[], INT[], BIGINT[], TIMESTAMPTZ[], BIGINT[], TEXT[], TEXT[], BIGINT[], BIGINT[], INT[], INT[]),
        "character".sp_diff_save_character_quests(BIGINT, BIGINT, BIGINT[], BIGINT[], INT[], INT[], BIGINT[], TIMESTAMPTZ[], BIGINT[], TEXT[], TEXT[], BIGINT[], BIGINT[], INT[], INT[]),
        characters.sp_diff_save_character_wallet_and_progress(BIGINT, BIGINT, INT[], BIGINT[], INT[], BIGINT[], INT[], INT[], INT[], BIGINT[]),
        "character".sp_diff_save_character_wallet_and_progress(BIGINT, BIGINT, INT[], BIGINT[], INT[], BIGINT[], INT[], INT[], INT[], BIGINT[])
        TO sandal_char, sandal_admin;
EXCEPTION
    WHEN undefined_object OR insufficient_privilege THEN
        NULL;
END
$$;
