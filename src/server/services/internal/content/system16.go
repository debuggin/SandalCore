// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"database/sql"
	"fmt"
)

const System16DDL = `
CREATE TABLE world_spawners (
    spawner_id TEXT PRIMARY KEY,
    spawner_category TEXT NOT NULL,
    actor_class TEXT NOT NULL,
    sublevel TEXT NOT NULL,
    npc_guid INTEGER NOT NULL DEFAULT 0,
    x REAL NOT NULL,
    y REAL NOT NULL,
    z REAL NOT NULL,
    radius REAL NOT NULL DEFAULT 0,
    min_node_stage INTEGER NOT NULL DEFAULT 0,
    max_node_stage INTEGER NOT NULL DEFAULT 4,
    time_of_day_mask INTEGER NOT NULL DEFAULT 3
);
CREATE TABLE npc_abilities (
    npc_guid INTEGER NOT NULL,
    ability_guid INTEGER NOT NULL,
    ability_name TEXT NOT NULL DEFAULT '',
    source TEXT NOT NULL DEFAULT '',
    PRIMARY KEY (npc_guid, ability_guid)
);
CREATE TABLE world_interactables (
    interactable_id TEXT PRIMARY KEY,
    actor_class TEXT NOT NULL,
    interactable_type TEXT NOT NULL,
    sublevel TEXT NOT NULL,
    x REAL NOT NULL,
    y REAL NOT NULL,
    z REAL NOT NULL,
    radius REAL NOT NULL DEFAULT 0
);
CREATE TABLE node_stage_stations (
    node_guid INTEGER NOT NULL,
    stage INTEGER NOT NULL,
    station_id TEXT NOT NULL,
    station_type TEXT NOT NULL,
    rel_x REAL NOT NULL,
    rel_y REAL NOT NULL,
    rel_z REAL NOT NULL,
    courtyard_radius_cm REAL NOT NULL DEFAULT 700,
    PRIMARY KEY (node_guid, stage, station_id)
);
CREATE TABLE zoi_foliage_spawns (
    zoi_guid INTEGER NOT NULL,
    instance_idx INTEGER NOT NULL,
    mesh_path TEXT NOT NULL,
    gatherable_guid INTEGER NOT NULL DEFAULT 0,
    x REAL NOT NULL,
    y REAL NOT NULL,
    z REAL NOT NULL,
    scale REAL NOT NULL DEFAULT 1.0,
    PRIMARY KEY (zoi_guid, instance_idx)
);
CREATE TABLE water_volumes (
    volume_id TEXT PRIMARY KEY,
    body_type TEXT NOT NULL,
    min_x REAL NOT NULL,
    min_y REAL NOT NULL,
    min_z REAL NOT NULL,
    max_x REAL NOT NULL,
    max_y REAL NOT NULL,
    max_z REAL NOT NULL
);
CREATE TABLE hazard_volumes (
    volume_id TEXT PRIMARY KEY,
    hazard_type TEXT NOT NULL,
    damage_per_tick INTEGER NOT NULL DEFAULT 0,
    tick_ms INTEGER NOT NULL DEFAULT 1000,
    min_x REAL NOT NULL,
    min_y REAL NOT NULL,
    min_z REAL NOT NULL,
    max_x REAL NOT NULL,
    max_y REAL NOT NULL,
    max_z REAL NOT NULL
);
CREATE TABLE sky_clock_config (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    day_compression REAL NOT NULL,
    night_compression REAL NOT NULL,
    cycle_length_sec REAL NOT NULL DEFAULT 7200
);
CREATE TABLE fishing_spots (
    spot_id TEXT PRIMARY KEY,
    fish_school_type TEXT NOT NULL,
    loot_table_guid INTEGER NOT NULL DEFAULT 0,
    x REAL NOT NULL,
    y REAL NOT NULL,
    z REAL NOT NULL,
    radius REAL NOT NULL DEFAULT 0
);
CREATE TABLE treasure_sockets (
    socket_id TEXT PRIMARY KEY,
    sublevel TEXT NOT NULL,
    loot_table_guid INTEGER NOT NULL DEFAULT 0,
    x REAL NOT NULL,
    y REAL NOT NULL,
    z REAL NOT NULL
);
CREATE TABLE world_event_defs (
    event_guid INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    min_node_stage INTEGER NOT NULL DEFAULT 0,
    max_node_stage INTEGER NOT NULL DEFAULT 4,
    state_graph_json TEXT NOT NULL DEFAULT '{}'
);
CREATE TABLE weather_cycles (
    zone_name TEXT NOT NULL,
    weather_state TEXT NOT NULL,
    weight REAL NOT NULL,
    duration_sec INTEGER NOT NULL DEFAULT 600,
    PRIMARY KEY (zone_name, weather_state)
);
`

var WorldSystem16Tables = []string{
	"world_spawners",
	"npc_abilities",
	"world_interactables",
	"node_stage_stations",
	"zoi_foliage_spawns",
	"water_volumes",
	"hazard_volumes",
	"sky_clock_config",
	"fishing_spots",
	"treasure_sockets",
	"world_event_defs",
	"weather_cycles",
}

func createSystem16(db *sql.DB) error {
	if _, err := db.Exec(System16DDL); err != nil {
		return fmt.Errorf("content: create the system 16 world tables: %w", err)
	}
	return nil
}

type SkyClockConfig struct {
	DayCompression   float64
	NightCompression float64
	CycleLengthSec   float64
}

func (d *DB) SkyClockConfig() (SkyClockConfig, error) {
	var c SkyClockConfig
	err := d.sql.QueryRow(`SELECT day_compression, night_compression, cycle_length_sec FROM sky_clock_config WHERE id = 1`).
		Scan(&c.DayCompression, &c.NightCompression, &c.CycleLengthSec)
	return c, err
}

type WorldEventDef struct {
	EventGuid      int64
	Name           string
	MinNodeStage   int
	MaxNodeStage   int
	StateGraphJSON string
}

func (d *DB) WorldEventDefs() ([]WorldEventDef, error) {
	rows, err := d.sql.Query(`SELECT event_guid, name, min_node_stage, max_node_stage, state_graph_json FROM world_event_defs ORDER BY event_guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []WorldEventDef
	for rows.Next() {
		var e WorldEventDef
		if err := rows.Scan(&e.EventGuid, &e.Name, &e.MinNodeStage, &e.MaxNodeStage, &e.StateGraphJSON); err != nil {
			return nil, err
		}
		out = append(out, e)
	}
	return out, rows.Err()
}
