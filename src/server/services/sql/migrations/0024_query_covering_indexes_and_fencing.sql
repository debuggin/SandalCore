-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

SET LOCAL lock_timeout = '10s';

ALTER TABLE characters ADD COLUMN IF NOT EXISTS fencing_token BIGINT NOT NULL DEFAULT 0;

CREATE INDEX IF NOT EXISTS idx_characters_account_created_id
    ON characters (account_id, created_at, id);

CREATE INDEX IF NOT EXISTS idx_characters_ungranted_starter_set
    ON characters (created_at, id)
    WHERE gm_starter_set_granted_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_characters_ungranted_starter_mounts
    ON characters (created_at, id)
    WHERE gm_starter_set_granted_at IS NOT NULL
      AND gm_starter_mounts_granted_at IS NULL;

CREATE INDEX IF NOT EXISTS idx_world_servers_heartbeat_server_id
    ON world_servers (last_heartbeat DESC, server_id DESC)
    INCLUDE (address);

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_items_char_order_id
        ON character_items (character_id, bag_slot_type, equip_slot, inventory_y, inventory_x, id);
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_items_char_order_id ON character_items (character_id, container, slot)',
            'INDEX'
        );
END
$$;

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_resource_items_char_order_id
        ON character_resource_items (character_id, bag_slot_type, equip_slot, inventory_y, inventory_x, id);
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_resource_items_char_order_id ON character_resource_items (character_id, container, slot)',
            'INDEX'
        );
END
$$;

DO $$
BEGIN
    CREATE INDEX IF NOT EXISTS idx_character_skill_ranks_tree
        ON character_skill_ranks (character_id, tree_id, skill_guid)
        INCLUDE (rank);
EXCEPTION
    WHEN undefined_column THEN
        EXECUTE format(
            'CREATE %s IF NOT EXISTS idx_character_skill_ranks_tree ON character_skill_ranks (character_id, tree_guid, ord) INCLUDE (rank_guid)',
            'INDEX'
        );
END
$$;

CREATE INDEX IF NOT EXISTS idx_character_known_recipes_taught
    ON character_known_recipes (character_id, taught_at, recipe_guid);

CREATE INDEX IF NOT EXISTS idx_hotfix_overlays_active_order
    ON world.world_hotfix_overlays (target_table, record_guid, field_path, overlay_id)
    INCLUDE (min_server_build, expires_at)
    WHERE enabled = TRUE;

CREATE INDEX IF NOT EXISTS idx_hotfix_audit_log_recorded_at
    ON world.hotfix_audit_log (recorded_at DESC);

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_created_at
    ON realm_state.gm_audit_log (created_at DESC);

CREATE INDEX IF NOT EXISTS idx_gm_audit_log_command_created
    ON realm_state.gm_audit_log (command_name, created_at DESC);
