-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

SET LOCAL lock_timeout = '10s';

CREATE TABLE IF NOT EXISTS world_simulation_state (
    id BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (id),
    time_of_day_anchor_hours DOUBLE PRECISION NOT NULL DEFAULT 12.0,
    time_of_day_anchor_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    calendar_anchor_year INTEGER NOT NULL DEFAULT 1,
    calendar_anchor_day INTEGER NOT NULL DEFAULT 1,
    time_scale DOUBLE PRECISION NOT NULL DEFAULT 9.0,
    simulate_all_npcs BOOLEAN NOT NULL DEFAULT TRUE,
    worker_tick_mode TEXT NOT NULL DEFAULT 'dynamic',
    worker_tick_hz JSONB NOT NULL DEFAULT '{}'::jsonb,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO world_simulation_state (
    id, time_of_day_anchor_hours, time_of_day_anchor_at,
    calendar_anchor_year, calendar_anchor_day, time_scale,
    simulate_all_npcs, worker_tick_mode, worker_tick_hz
) VALUES (
    TRUE, 12.0, NOW(), 1, 1, 9.0, TRUE, 'dynamic', '{}'::jsonb
) ON CONFLICT (id) DO NOTHING;

CREATE TABLE IF NOT EXISTS city_node_state (
    node_guid BIGINT PRIMARY KEY,
    node_id TEXT NOT NULL UNIQUE,
    level INTEGER NOT NULL DEFAULT 1 CHECK (level BETWEEN 0 AND 6),
    experience BIGINT NOT NULL DEFAULT 0,
    mayor_character_id TEXT NOT NULL DEFAULT '',
    mayor_name TEXT NOT NULL DEFAULT '',
    tax_rate_pct DOUBLE PRECISION NOT NULL DEFAULT 5.0,
    treasury_gold BIGINT NOT NULL DEFAULT 0,
    buildings JSONB NOT NULL DEFAULT '[]'::jsonb,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
