// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  world: the world server (plan A4: the connection layer, plan A5: the native bootstrap)
//
//  Registers with mesh, accepts game clients on UDP, redeems their join
//  tickets with the gateway and loads their characters, and takes them
//  through the control channel's login.  After NMT_Join it spawns the player
//  natively (session/player_session.h): the character's actors go out on actor
//  channels and the client's requests are answered.  That needs content.db
//  (--content-db, plan C1's contentc output); without it world still starts
//  but refuses every login.  Plan A6: each move is checked and a player's last good
//  position is saved through the character service on a worker thread (session/
//  position_saver.h), also when world stops.
//
//      world --bind 127.0.0.1:7777 --service-token-file dist/services/service.token
// ============================================================================
#include "login/login_service.h"
#include "login/login_worker.h"
#include "net/cookie.h"
#include "net/endpoint.h"
#include "net/tick_loop.h"
#include "net/tick_profile.h"
#include "telemetry/health_metrics_http.h"
#include "protocol/server_guid_allocator.h"
#include "session/player_world.h"
#include "session/shared_updates.h"
#include "session/gm_travel.h"
#include "quest/quest_service.h"
#include "session/position_saver.h"
#include "spawns/node_population.h"
#include "svc/city_node_feed.h"
#include "svc/grpc_services.h"
#include "svc/service_token.h"
#include "svc/world_admin.h"
#include "svc/world_stream.h"
#include "svc/city_node_control.h"
#include "world/city_nodes.h"
#include "world/node_layout_data.h"
#include "world/node_scenery.h"
#include "world/node_wire.h"
#include "world_loop.h"
#include <CLI/CLI.hpp>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <optional>
#include <thread>
#include <utility>

using namespace aoc::worldd;

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

void sleep_unless_stopped(std::chrono::milliseconds d) {
    const auto until = Clock::now() + d;
    while (!g_stop && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

struct WorldCliOptions {
    std::string bind = "127.0.0.1:7777", public_address, port_opt, admin_port_opt;
    std::string mesh = "127.0.0.1:50053", gateway = "127.0.0.1:50054", character = "127.0.0.1:50052";
    std::string citynode = "127.0.0.1:50055", citynode_seed = "config/seeds/citynode_seed.json";
    bool no_citynode = false;
    std::string world_admin = "127.0.0.1:50056", world_stream = "127.0.0.1:50057";
    bool no_world_admin = false, no_world_stream = false, gm_commands = false, no_reactions = false;
    uint64_t combat_seed = 0, loot_seed = 0, gather_seed = 0;
    std::string token_file = "dist/services/service.token", log_file;
    std::string content_db_path = "dist/content/content.db", catalog_db_path = "dist/content/catalog.db";
    std::string gamestate_mode = "tier3";
    EndpointConfig endpoint_config;
    MovementConfig movement;
    spawns::AreaConfig area;
    double area_radius_m = -1;
    bool no_zone_filter = false, gm_garden = false, no_node_scenery = false, no_node_people = false;
    std::string node_origin = "captured", roads_levels_path = "dist/content/roads_levels.tsv", area_region, area_zoi, area_exclude_zoi, area_node_plug, area_exclude_node_plug;
    std::string area_polygon, area_exclude_polygon;
    int worker_index = -1;
    double scenery_radius_m = 1450.0;
    std::string area_nav_path = "dist/content/area_nav.bin", npc_motion_path = "dist/content/npc_motion.tsv";
    bool no_npc_movement = false, no_generic_npcs = false, no_spawn_filter = false, spawn_humanoids = false;
    std::string npc_abilities_path = "dist/content/npc_abilities.tsv", npc_targeting_path = "dist/content/npc_targeting.tsv";
    std::string npc_montages_path = "dist/content/npc_montages.tsv", npc_senses_path = "dist/content/npc_senses.tsv";
    std::string npc_aggro_curves_path = "dist/content/npc_aggro_curves.tsv", mount_speeds_path = "dist/content/mount_speeds.tsv";
    std::string mount_riders_path = "dist/content/mount_riders.tsv", roads_path = "dist/content/roads.tsv";
    std::string town_platforms_path = "dist/content/town_platforms.bin", npc_exclude;
    double spawn_hour = -1, world_hour = -1;
    std::string spawn_node_level = "Wilderness";
    bool no_combat = false, no_loot = false, no_respawn = false, no_death_loss = false;
    double npc_aggro_cm = 800.0, npc_leash_cm = 3000.0, npc_damage_mult = 1.0;
    double corpse_seconds = 15.0, loot_corpse_seconds = 120.0, loot_chance_mult = 1.0, loot_rarity_boost = 1.0;
    bool no_gathering = false, gather_quality_literal = false;
    double gather_respawn_mult = 1.0, foliage_regrow_mult = 1.0;
    std::string foliage_map_path = "dist/content/region_foliage.tsv";
    int tick_hz = 30, move_ack_gap_ms = 100;
    double loop_stats_seconds = 0;
    bool no_view_spread = false;
    unsigned view_threads = 4;
    CLI::Option *view_threads_option = nullptr, *loop_stats_option = nullptr, *tick_hz_option = nullptr;
    std::string share_updates_text = "on";
};

struct NodeRuntimeState {
    std::unique_ptr<world::CityNodes> city_nodes;
    std::unique_ptr<world::NodeWire> node_wire;
    std::unique_ptr<world::NodeLayoutData> node_layout_data;
    std::unique_ptr<spawns::NodePopulationData> node_pop_data;
    std::unique_ptr<svc::GrpcNodeControl> node_control;
};

void register_service_cli_options(CLI::App& app, WorldCliOptions& o) {
    app.add_option("--bind", o.bind, "UDP address to listen on")->envname("AOC_WORLD_BIND_ADDR");
    app.add_option("--port", o.port_opt, "UDP port shorthand (0.0.0.0:<port>)");
    app.add_option("--admin-port", o.admin_port_opt, "world admin gRPC port shorthand (0.0.0.0:<port>)");
    app.add_option("--public-address", o.public_address, "address game clients dial, registered with mesh (default: --bind)");
    app.add_option("--mesh", o.mesh, "mesh gRPC address")->envname("AOC_MESH_GRPC_ADDR");
    app.add_option("--gateway", o.gateway, "gateway internal gRPC address (RedeemTicket)")->envname("AOC_GATEWAY_GRPC_ADDR");
    app.add_option("--character", o.character, "character service gRPC address")->envname("AOC_CHAR_GRPC_ADDR");
    app.add_option("--citynode", o.citynode, "citynode service gRPC address (WatchNodes)")->envname("AOC_CITYNODE_GRPC_ADDR")->capture_default_str();
    app.add_option("--citynode-seed", o.citynode_seed, "the node seed (config/seeds/citynode_seed.json): the levels world uses until the citynode service answers")->envname("AOC_CITYNODE_SEED")->capture_default_str();
    app.add_flag("--no-citynode", o.no_citynode, "do not call the citynode service: the nodes stay at the seed's levels")->envname("AOC_NO_CITYNODE");
    app.add_option("--world-admin", o.world_admin, "the address world's admin API (WorldAdminService) listens on; calls need the service token")->envname("AOC_WORLD_ADMIN_ADDR")->capture_default_str();
    app.add_flag("--no-world-admin", o.no_world_admin, "do not serve the admin API")->envname("AOC_NO_WORLD_ADMIN");
    app.add_option("--world-stream", o.world_stream, "the address world's world stream (WorldStreamService) listens on; calls need the service token")->envname("AOC_WORLD_STREAM_ADDR")->capture_default_str();
    app.add_flag("--no-world-stream", o.no_world_stream, "do not serve the world stream")->envname("AOC_NO_WORLD_STREAM");
    app.add_flag("--gm-commands", o.gm_commands, "answer the game client's own GM teleport commands (ServerTeleportXYZ, ServerTeleport, ServerTeleportPlayer); off by default")->envname("AOC_GM_COMMANDS");
    app.add_flag("--no-reactions", o.no_reactions, "the game's reactions do not run: abilities and hits land as before reactions were built")->envname("AOC_NO_REACTIONS");
    app.add_option("--combat-seed", o.combat_seed, "the number the hit, block and critical rolls start from (0: fresh seed on every start)")->envname("AOC_COMBAT_SEED")->capture_default_str();
    app.add_option("--service-token-file", o.token_file, "the internal services' shared token");
    app.add_option("--log-file", o.log_file, "also write the log to this file");
    app.add_option("--content-db", o.content_db_path, "content.db, the game numbers contentc builds; without it every login is refused")->envname("AOC_CONTENT_DB")->capture_default_str();
    app.add_option("--catalog-db", o.catalog_db_path, "optional AOCDB01 runtime design-data catalog (System 16)")->envname("AOC_CATALOG_DB")->capture_default_str();
    app.add_option("--gamestate-mode", o.gamestate_mode, "how much of the GameState a joining player gets: tier1, tier2, tier3 or captured")->envname("AOC_GAMESTATE_MODE")->transform(CLI::IsMember({"tier1", "tier2", "tier3", "captured"}, CLI::ignore_case))->capture_default_str();
    app.add_option("--max-move-speed", o.movement.limits.max_speed, "the fastest a player may move along the ground, cm/s")->envname("AOC_MAX_MOVE_SPEED")->check(CLI::PositiveNumber)->capture_default_str();
}

void register_area_cli_options(CLI::App& app, WorldCliOptions& o) {
    app.add_option("--area-radius", o.area_radius_m, "the area filled with NPCs, resource nodes and the GM vendors (-1: continent, >0: circle radius m, 0: empty)")->envname("AOC_AREA_RADIUS_M")->check(CLI::Range(-1.0, 20000.0))->capture_default_str();
    app.add_option("--area-region", o.area_region, "worker region partition (riverlands, sandsquall, jundark, anvils, turquoise_sea, continent)")->envname("AOC_WORLD_REGION");
    app.add_option("--area-zoi", o.area_zoi, "comma-separated ZOI GUID/name allowlist for per-ZOI worker partitioning")->envname("AOC_WORLD_ZOI");
    app.add_option("--area-exclude-zoi", o.area_exclude_zoi, "comma-separated ZOI GUIDs/names carved out of the worker's region")->envname("AOC_WORLD_EXCLUDE_ZOI");
    app.add_option("--area-node-plug", o.area_node_plug, "comma-separated CityNode names/GUIDs included as rotated 1 km x 1 km Node Plug OBBs")->envname("AOC_WORLD_NODE_PLUG");
    app.add_option("--area-exclude-node-plug", o.area_exclude_node_plug, "comma-separated CityNode names/GUIDs carved out of the surrounding ZOI/region worker")->envname("AOC_WORLD_EXCLUDE_NODE_PLUG");
    app.add_option("--area-polygon", o.area_polygon, "convex polygon cutout in cm (x1,y1;x2,y2;x3,y3...)")->envname("AOC_WORLD_POLYGON_CUTOUT");
    app.add_option("--area-exclude-polygon", o.area_exclude_polygon, "excluded convex polygon cutouts in cm (x1,y1;x2,y2;x3,y3|...)")->envname("AOC_WORLD_EXCLUDE_POLYGON_CUTOUT");
    app.add_option("--worker-index", o.worker_index, "worker index assigned and registered with the mesh")->envname("AOC_WORLD_WORKER_INDEX");
    app.add_flag("--no-zone-filter", o.no_zone_filter, "points have no zone of influence: a spawn filter that names one never passes, as in E1's circle")->envname("AOC_NO_ZONE_FILTER");
    app.add_flag("--gm-garden", o.gm_garden, "a GM garden near the login spot: 15 resource nodes across Mining, Lumberjacking, and Herbalism (Novice to GrandMaster)")->envname("AOC_GM_GARDEN");
    app.add_flag("--no-node-scenery", o.no_node_scenery, "no town replicators: the nodes' buildings and props are not sent (the towns are empty holes, as before stage 3a)")->envname("AOC_NO_NODE_SCENERY");
    app.add_option("--node-origin", o.node_origin, "where a town's replicator stands: captured (the default: the open locations of the 13 nodes the retail captures show) or record")
        ->envname("AOC_NODE_ORIGIN")->transform(CLI::IsMember({"captured", "record"}, CLI::ignore_case))->capture_default_str();
    app.add_flag("--no-node-people", o.no_node_people, "no town people: the layouts' and buildings' populations (vendors, clerks, guards, villagers) are not spawned")
        ->envname("AOC_NO_NODE_PEOPLE");
    app.add_option("--roads-levels", o.roads_levels_path, "the roads of every node level with their owner zones (contentc roads --levels); used instead of --roads when the file exists")
        ->envname("AOC_ROADS_LEVELS")->capture_default_str();
    app.add_option("--scenery-radius", o.scenery_radius_m, "a town's replicator is sent to a player within this many metres of it (retail opened it at 1,190 to 1,450 m)")
        ->envname("AOC_SCENERY_RADIUS_M")->check(CLI::Range(100.0, 10000.0))->capture_default_str();
    app.add_option("--area-nav", o.area_nav_path, "the ground of the area: the navmesh and the terrain tools/mapc builds (a missing file: the NPCs stand where the data puts them)")
        ->envname("AOC_AREA_NAV")->capture_default_str();
    app.add_option("--npc-motion", o.npc_motion_path, "the NPC archetypes' capsules and speeds tools/mapc reads from the game (a missing file: the Blueprint defaults)")
        ->envname("AOC_NPC_MOTION")->capture_default_str();
    app.add_option("--npc-abilities", o.npc_abilities_path, "the creature Blueprints' ability tasks tools/npcabilities reads from the game")->envname("AOC_NPC_ABILITIES")->capture_default_str();
    app.add_option("--npc-targeting", o.npc_targeting_path, "where each creature ability's hit lands, tools/npcabilities")->envname("AOC_NPC_TARGETING")->capture_default_str();
    app.add_option("--npc-montages", o.npc_montages_path, "the creature animation montages tools/npcabilities reads from the game: their lengths and hit notifies")
        ->envname("AOC_NPC_MONTAGES")->capture_default_str();
    app.add_option("--npc-senses", o.npc_senses_path, "what each creature Blueprint notices and how far it follows, tools/npcabilities")->envname("AOC_NPC_SENSES")->capture_default_str();
    app.add_option("--npc-aggro-curves", o.npc_aggro_curves_path, "the level curve that scales a creature's aggro radius, tools/npcabilities")->envname("AOC_NPC_AGGRO_CURVES")->capture_default_str();
    app.add_option("--mount-speeds", o.mount_speeds_path, "the mount Blueprints' speeds tools/mapc reads from the game")->envname("AOC_MOUNT_SPEEDS")->capture_default_str();
    app.add_option("--mount-riders", o.mount_riders_path, "the mount Blueprints' rider AnimBPs tools/mapc reads from the game")->envname("AOC_MOUNT_RIDERS")->capture_default_str();
    app.add_option("--roads", o.roads_path, "the area's roads, dist/content/roads.tsv")->envname("AOC_ROADS")->capture_default_str();
    app.add_option("--town-platforms", o.town_platforms_path, "the floor of the five city nodes' bowls, dist/content/town_platforms.bin")->envname("AOC_TOWN_PLATFORMS")->capture_default_str();
    app.add_flag("--no-npc-movement", o.no_npc_movement, "NPCs stand still where the data puts them (stage 1's behaviour)");
    app.add_flag("--no-generic-npcs", o.no_generic_npcs, "only the NPCs whose Blueprint a retail capture opened spawn (stage 1's NPCs); the rest are not made")->envname("AOC_NO_GENERIC_NPCS");
    app.add_option("--npc-exclude", o.npc_exclude, "the kill switch for one NPC type: Blueprint names or archetype paths, comma separated, that are not spawned")->envname("AOC_NPC_EXCLUDE");
    app.add_flag("--no-spawn-filter", o.no_spawn_filter, "spawn every point of the design data whose Blueprint opens, as stage 2 did")->envname("AOC_NO_SPAWN_FILTER");
    app.add_flag("--spawn-humanoids", o.spawn_humanoids, "let the humanoid Blueprints through whose body needs an NPCAppearanceComponent the open does not carry")->envname("AOC_SPAWN_HUMANOIDS");
    app.add_option("--world-hour", o.world_hour, "the game hour (0 to 24) the world clock starts at (default: 12, noon); also the hour the spawns' time-of-day conditions are judged at")
        ->envname("AOC_WORLD_HOUR")->check(CLI::Range(-1.0, 24.0));
    app.add_option("--spawn-hour", o.spawn_hour, "alias for --world-hour when --world-hour is not given (kept for scripts that passed --spawn-hour)")
        ->envname("AOC_SPAWN_HOUR")->check(CLI::Range(-1.0, 24.0));
    app.add_option("--spawn-node-level", o.spawn_node_level, "the node level the spawns' conditions are judged at: Wilderness, Crossroads, Encampment, Village, Town, City, Metropolis")
        ->envname("AOC_SPAWN_NODE_LEVEL")->capture_default_str();
}

void register_combat_and_gather_cli_options(CLI::App& app, WorldCliOptions& o) {
    app.add_flag("--no-combat", o.no_combat, "the kill switch for the fights: the NPCs do not aggro, attack or take hits (stage 2's world)")->envname("AOC_NO_COMBAT");
    app.add_option("--npc-aggro-cm", o.npc_aggro_cm, "how far (cm) an NPC whose data gives no aggro radius notices a player")->envname("AOC_NPC_AGGRO_CM")->capture_default_str();
    app.add_option("--npc-leash-cm", o.npc_leash_cm, "how far (cm) from home an NPC chases before it gives up and resets")->envname("AOC_NPC_LEASH_CM")->capture_default_str();
    app.add_option("--npc-damage-mult", o.npc_damage_mult, "multiplies the Health damage every NPC's hit does to a player: 1 (the default) is the data's own damage, 0.2 takes 80 % off")
        ->envname("AOC_NPC_DAMAGE_MULT")->check(CLI::Range(0.0, 100.0))->capture_default_str();
    app.add_flag("--no-loot", o.no_loot, "the kill switch for what follows a death: no experience, no loot, no respawn (the corpse still goes)")->envname("AOC_NO_LOOT");
    app.add_flag("--no-death-loss", o.no_death_loss, "a player's death takes no materials (retail: about half of each stack, into a pile where it fell)")->envname("AOC_NO_DEATH_LOSS");
    app.add_flag("--no-respawn", o.no_respawn, "the kill switch for respawns: a dead NPC stays dead")->envname("AOC_NO_RESPAWN");
    app.add_option("--corpse-seconds", o.corpse_seconds, "how long a corpse with nothing to loot stays (retail: 15 s)")->envname("AOC_CORPSE_SECONDS")->capture_default_str();
    app.add_option("--loot-corpse-seconds", o.loot_corpse_seconds, "how long a corpse with loot that nobody takes stays (modelled: 120 s)")->envname("AOC_LOOT_CORPSE_SECONDS")->capture_default_str();
    app.add_option("--loot-chance-mult", o.loot_chance_mult, "multiplies the chance of every RollEachByPercent loot draw (never a quantity; each chance is capped at 1): 3 makes small drop chances 3 times as likely (1: the data's own)")
        ->envname("AOC_LOOT_CHANCE_MULT")->check(CLI::Range(0.0, 1000.0))->capture_default_str();
    app.add_option("--loot-rarity-boost", o.loot_rarity_boost,
                   "TEST ONLY: multiplies the weight of every outcome above Common in the rarity rolls of a kill's gear and resource drops (never a harvest, a quantity or which Glint tier drops): "
                   "20 makes an Uncommon or better drop about 20 times as likely against a Common one (1: the game's own odds)")
        ->envname("AOC_LOOT_RARITY_BOOST")->check(CLI::Range(1.0, 1000.0))->capture_default_str();
    app.add_option("--loot-seed", o.loot_seed,
                   "the number the kills' loot rolls (and the respawn delays) start from: 0 (the default) takes a new one on every start, so each run rolls different drops; any other number repeats the same rolls on every start")
        ->envname("AOC_LOOT_SEED")->capture_default_str();
    app.add_flag("--no-gathering", o.no_gathering, "the kill switch for harvesting: the resource nodes are seen and not harvested")->envname("AOC_NO_GATHERING");
    app.add_option("--gather-respawn-mult", o.gather_respawn_mult,
                   "multiplies the time a gathered resource node takes to return (the data's: the actor def's OverrideRespawnDuration, else the foliage data's EmptyDuration; 0.02 turns a tree's 14,400 s into 288 s; at least 1 s)")
        ->envname("AOC_GATHER_RESPAWN_MULT")->check(CLI::Range(0.0001, 1000.0))->capture_default_str();
    app.add_flag("--gather-quality-literal", o.gather_quality_literal,
                 "GetRolledItemQuality() in the Quantity equation answers the literal quality (1, 6, 16 ...) instead of the index of its rarity band (0 to 6): the equation compares it with 0 to 6")
        ->envname("AOC_GATHER_QUALITY_LITERAL");
    app.add_option("--gather-seed", o.gather_seed,
                   "the number the harvest rolls start from: 0 (the default) takes a new one on every start, so each run rolls different rarities; any other number repeats the same rolls on every start")
        ->envname("AOC_GATHER_SEED")->capture_default_str();
    app.add_option("--foliage-regrow-mult", o.foliage_regrow_mult,
                   "multiplies EVERY phase of a harvested tree's, plant's or stone's regrowth (the data's: an Oak large is 300 s empty, 1800 s a sapling, 1800 s medium, then whole; a plant 1800 s empty and 300 s a sapling; 0.01 makes the Oak 3 + 18 + 18 s)")
        ->envname("AOC_FOLIAGE_REGROW_MULT")->check(CLI::Range(0.0001, 1000.0))->capture_default_str();
    app.add_option("--foliage-map", o.foliage_map_path,
                   "the painted foliage of the area, dist/content/region_foliage.tsv (tools/mapc `mapc extract`: 84,218 instances of the Riverlands; a missing file: the foliage is seen and not harvested)")
        ->envname("AOC_FOLIAGE_MAP")->capture_default_str();
    app.add_option("--max-sessions", o.endpoint_config.max_sessions, "the most client sessions at once; further handshakes are ignored (0: no limit)")
        ->capture_default_str();
}

void register_tick_cli_options(CLI::App& app, WorldCliOptions& o) {
    o.tick_hz_option = app.add_option("--tick-hz", o.tick_hz,
                   "the server's tick, times a second (10 to 120): every session's due work and every client's packets; the original server sent every 33 ms, 30 times a second. "
                   "The NPCs keep their 15 Hz step and each player's view of the world about 15 passes a second (the length of 67 ms in whole ticks: 20 a second at 20 Hz, 12.5 at 25 Hz, "
                   "10 at 10 Hz). 0: the old loop, a tick after every datagram")
        ->envname("AOC_TICK_HZ")->capture_default_str();
    app.add_flag("--no-view-spread", o.no_view_spread,
                 "the kill switch for the spread of the players' view passes: each player's passes land on the tick its first pass fell on (players who join together then pass together)")
        ->envname("AOC_NO_VIEW_SPREAD");
    o.view_threads_option = app.add_option("--view-threads", o.view_threads,
                   "the threads that run the players' view passes side by side, the tick's own included (0 to 8): with 1 or more the tick runs every session's own work, then all the due passes at once "
                   "with the world's registry frozen, then every session's flush (session/view_pool.h); 1 runs the passes in order on the tick's own thread. 0: each session's work, view and "
                   "flush in turn, as before. The default is 4. Needs --tick-hz 10 or more (--tick-hz 0, the old loop, runs no threads and refuses an asked-for number); more threads than the machine has "
                   "cores less two are not started (at least one is)")
        ->envname("AOC_VIEW_THREADS")->check(CLI::Range(0, static_cast<int>(kViewWorkers)))->capture_default_str();
    app.add_option("--share-updates", o.share_updates_text,
                   "on, off or verify: build the change of a mob once for every player who holds it in the same state, and send each of them that update (session/shared_updates.h); "
                   "verify also builds one shared update in eight the plain way and compares the bytes (a difference is logged and turns sharing off). Off sends what every player's own update builds")
        ->envname("AOC_SHARE_UPDATES")->check(CLI::IsMember({"on", "off", "verify"}))->capture_default_str();
    app.add_option("--move-ack-gap-ms", o.move_ack_gap_ms,
                   "the pace of the answers to a player's good moves: the latest good move of a tick is answered when more than this many milliseconds have gone by since the last answer, "
                   "as the original server did (about every second move of a walking player); 0 answers every move at once")
        ->envname("AOC_MOVE_ACK_GAP_MS")->check(CLI::Range(0, 1000))->capture_default_str();
    o.loop_stats_option = app.add_option("--loop-stats", o.loop_stats_seconds,
                   "every N seconds (1 or more) print a [loop] line (the ticks, the share of the time working, the p50/p99/longest tick, the datagrams and bytes each way, the sessions, the "
                   "save queue) and a [tick] line (the rate, and where the tick's time went: the world, the sessions, their views, the sends, the costliest session); 0: off")
        ->envname("AOC_LOOP_STATS_SECONDS")->capture_default_str();
}

int validate_cli_options(WorldCliOptions& o) {
    if (o.loop_stats_option && o.loop_stats_option->count() == 0) {
        if (const char* env_ls = std::getenv("AOC_LOOP_STATS"); env_ls && *env_ls) {
            o.loop_stats_seconds = std::strtod(env_ls, nullptr);
        }
    }
    if (o.view_threads > 0 && o.tick_hz == 0) {
        if (o.view_threads_option && o.view_threads_option->count() > 0) {
            std::fprintf(stderr, "--view-threads needs the fixed-rate tick (--tick-hz 10 or more): the old loop (--tick-hz 0) has no phases to run the passes in\n");
            return 2;
        }
        o.view_threads = 0;
    }
    if (o.tick_hz != 0 && (o.tick_hz < TickSchedule::kMinHz || o.tick_hz > TickSchedule::kMaxHz)) {
        std::fprintf(stderr, "--tick-hz must be 0 (the old loop) or %d to %d, not %d\n", TickSchedule::kMinHz, TickSchedule::kMaxHz, o.tick_hz);
        return 2;
    }
    if (!std::isfinite(o.loop_stats_seconds) || (o.loop_stats_seconds != 0 && o.loop_stats_seconds < 1)) {
        std::fprintf(stderr, "--loop-stats must be 0 (off) or a number of seconds, 1 or more, not %g\n", o.loop_stats_seconds);
        return 2;
    }
    // CLI11 Range lets NaN through because comparisons with NaN evaluate false.
    const std::pair<const char*, double> checked[] = {
        {"--area-radius", o.area_radius_m},         {"--scenery-radius", o.scenery_radius_m},   {"--world-hour", o.world_hour}, {"--spawn-hour", o.spawn_hour},
        {"--npc-damage-mult", o.npc_damage_mult},   {"--loot-chance-mult", o.loot_chance_mult}, {"--loot-rarity-boost", o.loot_rarity_boost},
        {"--gather-respawn-mult", o.gather_respawn_mult}, {"--foliage-regrow-mult", o.foliage_regrow_mult},
        {"--max-move-speed", o.movement.limits.max_speed}};
    for (const auto& [flag, value] : checked) {
        if (!std::isfinite(value)) {
            std::fprintf(stderr, "%s must be a number, not %g\n", flag, value);
            return 2;
        }
    }
    return 0;
}
bool init_logging_token_and_content(WorldCliOptions& o, std::string& token_out,
                                    std::optional<content::ContentDb>& content, std::string& content_problem) {
    if (!o.port_opt.empty()) o.bind = "0.0.0.0:" + o.port_opt;
    if (!o.admin_port_opt.empty()) o.world_admin = "0.0.0.0:" + o.admin_port_opt;
    if (!o.world_admin.empty() && o.world_admin[0] == ':') o.world_admin = "0.0.0.0" + o.world_admin;
    if (!o.world_stream.empty() && o.world_stream[0] == ':') o.world_stream = "0.0.0.0" + o.world_stream;
    if (const char* env_tf = std::getenv("AOC_SERVICE_TOKEN_FILE"); env_tf && *env_tf && o.token_file == "dist/services/service.token") o.token_file = env_tf;
    std::vector<spdlog::sink_ptr> sinks{std::make_shared<spdlog::sinks::stderr_color_sink_mt>()};
    auto logger = std::make_shared<spdlog::logger>("world", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
    o.area = spawns::AreaConfig::resolve_worker_area(
        spawns::WorkerAreaSpec{o.area_radius_m, o.area_region, o.area_zoi, o.area_exclude_zoi, o.area_node_plug, o.area_exclude_node_plug,
                               o.no_zone_filter, o.area_polygon, o.area_exclude_polygon, {}, {}, o.worker_index});
    const char *env_thz = std::getenv("AOC_WORLD_TICK_HZ"), *env_thz2 = std::getenv("AOC_TICK_HZ");
    if (!o.tick_hz_option || o.tick_hz_option->count() == 0) {
        if (env_thz && *env_thz) o.tick_hz = std::clamp(static_cast<int>(std::strtol(env_thz, nullptr, 10)), 1, 120);
        else if (!env_thz2 || !*env_thz2) o.tick_hz = std::clamp(o.area.default_tick_hz_for_scope(), 1, 120);
    }
#ifndef _WIN32
    if (o.tick_hz > 0) ::setenv("AOC_WORLD_TICK_HZ", std::to_string(o.tick_hz).c_str(), 1);
#endif
    if (o.area.dynamic_scaling_stub) spdlog::info("[world] dynamic worker scaling stub active (min_workers=1, dynamic_spawns=0)");
    spdlog::info("[world] worker={} index={} region={} scope_kind={} tick_hz={} data_mode={} (region_mask=0x{:02x}, zoi_filter={}, exclude_zoi={}, node_plugs={}, exclude_plugs={}, poly_pts={})",
                 o.area.worker_name, o.area.worker_index, o.area.region_label, o.area.scope_kind(), o.tick_hz, aoc::world::telemetry::ResolveDataMode(),
                 o.area.region_mask, o.area.zoi_guids.size(), o.area.exclude_zoi_guids.size(), o.area.include_node_plugs.size(), o.area.exclude_node_plugs.size(), o.area.polygon_cutout_cm.size());

    std::string error;
    auto token = svc::load_service_token(o.token_file, &error);
    if (!token) {
        if (const char* env_tok = std::getenv("AOC_SERVICE_TOKEN"); env_tok && *env_tok) {
            token = std::string(env_tok);
        } else if (!std::filesystem::exists(o.token_file)) {
            spdlog::warn("service token file {} absent; using container fallback token", o.token_file);
            token = std::string(64, '0');
        } else {
            spdlog::error("{}", error);
            return false;
        }
    }
    token_out = std::move(*token);
    content = content::ContentDb::open(o.content_db_path, error);
    if (content) {
        spdlog::info("content.db {} opened", o.content_db_path);
    } else {
        content_problem = error;
        spdlog::error("content.db {} did not open: {}", o.content_db_path, content_problem);
        spdlog::error("world refuses every login until content.db is built (inputs and flags: services/README.md, \"Content\"); restart world after building it");
    }
    return true;
}

int register_with_mesh(svc::GrpcServices& services, const Address& public_addr, const WorldCliOptions& o, svc::RegisterResult& reg) {
    for (auto backoff = std::chrono::milliseconds(500); !g_stop;) {
        reg = services.register_server(public_addr.to_string(), {o.area.region_label});
        if (reg.status == svc::CallStatus::Ok) break;
        if (reg.status == svc::CallStatus::Refused) {
            spdlog::error("mesh refused to register {}", public_addr.to_string());
            return 1;
        }
        spdlog::warn("registering with mesh at {} failed ({}); retrying", o.mesh, svc::call_status_name(reg.status));
        sleep_unless_stopped(backoff);
        backoff = std::min(backoff * 2, std::chrono::milliseconds(5000));
    }
    if (!g_stop) spdlog::info("world {} registered with mesh as server {} (region={})", public_addr.to_string(), reg.server_id, o.area.region_label);
    return 0;
}

uint64_t configure_player_world(PlayerWorld& players, const WorldCliOptions& o, CharacterSaver& saver,
                                quest::QuestWorker& quest_worker, std::shared_ptr<SharedUpdates>& shared_updates) {
    players.set_runtime_catalog_loaded(!o.catalog_db_path.empty() && std::filesystem::exists(o.catalog_db_path), 16);
    spdlog::info("[catalog] AOCDB01 schema v{} runtime catalog {}: {}", players.runtime_catalog_schema_version,
                 players.runtime_catalog_loaded ? "loaded" : "fallback (in-process tables)", o.catalog_db_path);
    if (!players.ready() && players.content()) spdlog::error("world refuses every login: {}", players.content_problem());
    players.set_movement(o.movement);
    players.set_move_ack_gap(std::chrono::milliseconds(o.move_ack_gap_ms));
    spdlog::info("[movement] {}", o.move_ack_gap_ms > 0
                                      ? "a player's good moves are answered at the original server's pace: the latest one of a tick, at most one in " + std::to_string(o.move_ack_gap_ms) + " ms"
                                      : std::string("every good move of a player is answered at once (--move-ack-gap-ms 0)"));
    if (o.tick_hz > 0) {
        ActorView::Config view = players.view_config();
        const int64_t ticks_per_pass = tick_view_timing(view.relevancy, TickSchedule::period_of(o.tick_hz));
        if (ticks_per_pass > 1 && !o.no_view_spread) view.phases = std::make_shared<ViewPhases>(static_cast<unsigned>(ticks_per_pass));
        players.set_view_config(view);
        spdlog::info("[tick] the server ticks {} times a second (every {:.2f} ms): each tick runs every session's due work and sends every client's packets; the NPCs step 15 times a "
                     "second and each player's view of the world passes every {} ms (Dormancy closes after {} ms){}",
                     o.tick_hz, std::chrono::duration<double, std::milli>(TickSchedule::period_of(o.tick_hz)).count(), view.relevancy.tick.count(), view.relevancy.dormancy_delay.count(),
                     view.phases ? ", the players' passes spread over " + std::to_string(ticks_per_pass) + " ticks" : std::string());
    } else {
        spdlog::info("[tick] --tick-hz 0: the old loop, every session's tick and sends after every datagram (and every 10 ms without one)");
    }
    const ShareMode share_mode = SharedUpdates::parse(o.share_updates_text);
    if (share_mode != ShareMode::Off) shared_updates = std::make_shared<SharedUpdates>(share_mode);
    ActorView::Config view = players.view_config();
    view.shared = shared_updates;
    players.set_view_config(view);
    spdlog::info("[share-updates] {}", share_mode == ShareMode::Off
                                           ? "off (--share-updates off): every player's view builds the changes it sends (the default, on, builds a mob's change once for the players who hold it in the same state)"
                                           : share_mode == ShareMode::On ? "on: a mob's change is built once for the players who hold it in the same state (--share-updates off: every player's view builds its own)"
                                                                         : "verify: shared, and one shared update in eight is built the plain way as well and compared; a difference turns sharing off");
    players.set_position_sink(&saver);
    players.set_gm_commands(o.gm_commands);
    players.set_reactions_enabled(!o.no_reactions);
    if (o.no_reactions) spdlog::info("[abilities] the game's reactions are OFF (--no-reactions)");
    const uint64_t dice_seed = o.combat_seed != 0 ? o.combat_seed : gather::fresh_seed();
    players.set_dice_seed(dice_seed);
    spdlog::info("[combat] the hit, block and critical rolls start from seed {}{}", dice_seed,
                 o.combat_seed != 0 ? " (--combat-seed: the same rolls for the same fights)" : " (new on every start)");
    const bool hour_asked = o.world_hour >= 0 || o.spawn_hour >= 0;
    const double start_hour = o.world_hour >= 0 ? o.world_hour : (o.spawn_hour >= 0 ? o.spawn_hour : world::kDefaultStartHour);
    const world::WorldClock day_clock = hour_asked ? world::WorldClock::starting_at(start_hour) : world::default_world_clock();
    players.set_day_clock(day_clock);
    if (hour_asked) bootstrap::set_global_time_of_day_hours(start_hour);
    spdlog::info("[clock] the world clock starts at {} (hour {:.1f}; {}): {:.0f} game seconds per real second from hour 6 up to hour 18 and {:.0f} from hour 18 up to hour 6, so a game day takes {:.0f} minutes",
                 world::hour_text(day_clock.hour(0.0)), day_clock.hour(0.0), o.world_hour >= 0 ? "--world-hour" : (o.spawn_hour >= 0 ? "--spawn-hour" : "default: noon"),
                 world::WorldClock::kDayRate, world::WorldClock::kNightRate, (12.0 * 3600.0 / world::WorldClock::kDayRate + 12.0 * 3600.0 / world::WorldClock::kNightRate) / 60.0);
    players.set_quest_worker(&quest_worker);
    spdlog::info("[gm] the client's own GM teleport commands are {}",
                 o.gm_commands ? "ON (--gm-commands)" : "off (--gm-commands, or AOC_GM_COMMANDS=1, turns them on); each one the client sends is logged");
    return dice_seed;
}

void load_city_nodes_and_scenery(PlayerWorld& players, const WorldCliOptions& o, const std::string& token,
                                 NodeRuntimeState& nodes, std::unique_ptr<svc::CityNodeFeed>& city_feed) {
    std::string nodes_error;
    nodes.city_nodes = world::CityNodes::load(o.content_db_path, nodes_error);
    if (!nodes.city_nodes) {
        spdlog::error("[nodes] no city nodes ({}): the loot tables' node reads answer as in a world with no nodes", nodes_error);
        return;
    }
    size_t applied = 0, skipped = 0;
    std::string seed_error;
    if (nodes.city_nodes->apply_seed_file(o.citynode_seed, seed_error, &applied, &skipped)) {
        spdlog::info("[nodes] {} city nodes, {} zones of influence; the seed {} gives {} levels{}: {}", nodes.city_nodes->size(), nodes.city_nodes->zones(), o.citynode_seed, applied,
                     skipped > 0 ? " (" + std::to_string(skipped) + " entries skipped)" : "", nodes.city_nodes->describe_levels());
    } else {
        spdlog::warn("[nodes] {} city nodes, {} zones of influence; no seed levels ({}): every node is at Wilderness until the citynode service answers", nodes.city_nodes->size(),
                     nodes.city_nodes->zones(), seed_error);
    }
    players.set_city_nodes(nodes.city_nodes.get());
    std::string wire_error;
    nodes.node_wire = world::NodeWire::load(o.content_db_path, *nodes.city_nodes, wire_error);
    if (!nodes.node_wire) {
        spdlog::error("[nodes] no node maps for the client ({}): the GameState goes without them", wire_error);
    } else {
        players.set_node_wire(nodes.node_wire.get());
        spdlog::info("[nodes] the client is told of {} nodes (retail's list), {} layouts' plots and {} estates; GameState mode {}", nodes.node_wire->roster().size(),
                     nodes.node_wire->layouts(), nodes.node_wire->estates(), o.gamestate_mode);
        if (!o.no_node_people) {
            std::string people_error;
            nodes.node_pop_data = spawns::NodePopulationData::load(o.content_db_path, people_error);
            if (!nodes.node_pop_data) spdlog::error("[people] no town people ({}): the towns have no NPCs", people_error);
            else {
                players.set_node_population_data(nodes.node_pop_data.get());
                spdlog::info("[people] {} layouts with population sets, {} buildings with populations", nodes.node_pop_data->layouts(), nodes.node_pop_data->buildings());
            }
        } else {
            spdlog::info("[people] --no-node-people: no town people");
        }
        if (!o.no_node_scenery) {
            std::string layout_error;
            nodes.node_layout_data = world::NodeLayoutData::load(o.content_db_path, layout_error);
            if (!nodes.node_layout_data) spdlog::error("[scenery] no town scenery ({}): the towns are empty holes", layout_error);
            else {
                players.set_node_layout_data(nodes.node_layout_data.get());
                spdlog::info("[scenery] {} layouts, {} asset sets, {} asset definitions, {} buildings", nodes.node_layout_data->layouts(), nodes.node_layout_data->asset_sets(),
                             nodes.node_layout_data->definitions(), nodes.node_layout_data->buildings());
            }
        } else {
            spdlog::info("[scenery] --no-node-scenery: no town replicators");
        }
    }
    if (o.no_citynode) {
        spdlog::info("[nodes] --no-citynode: the nodes stay at the seed's levels");
    } else {
        nodes.node_control = std::make_unique<svc::GrpcNodeControl>(svc::GrpcNodeControl::Options{o.citynode, token});
        players.set_node_control(nodes.node_control.get());
        city_feed = std::make_unique<svc::CityNodeFeed>(*nodes.city_nodes, svc::CityNodeFeed::Options{o.citynode, token});
        city_feed->start();
    }
}

void configure_area_spawns_and_combat(PlayerWorld& players, const WorldCliOptions& o, uint64_t dice_seed) {
    spawns::GenericConfig generic;
    generic.enabled = !o.no_generic_npcs;
    generic.hunting = !o.no_gathering;
    for (size_t start = 0; start <= o.npc_exclude.size();) {
        const size_t comma = o.npc_exclude.find(',', start);
        std::string one = o.npc_exclude.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!one.empty() && std::isspace(static_cast<unsigned char>(one.front()))) one.erase(one.begin());
        while (!one.empty() && std::isspace(static_cast<unsigned char>(one.back()))) one.pop_back();
        if (!one.empty()) generic.exclude.push_back(one);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    players.set_generic_npcs(std::move(generic));
    spawns::SpawnContext context;
    context.enabled = !o.no_spawn_filter;
    context.humanoids = o.spawn_humanoids;
    if (o.area.whole_world) { context.biome = "*"; context.culture = "*"; }
    context.node_level = o.spawn_node_level;
    if (spawns::SpawnFilter::node_level_number(o.spawn_node_level) < 0 || spawns::SpawnFilter::node_level_number(o.spawn_node_level) > 6) {
        spdlog::warn("[area] --spawn-node-level {} is no node level: Wilderness is used", o.spawn_node_level);
        context.node_level = "Wilderness";
    }
    context.hour = players.day_clock().hour(0.0);
    players.set_spawn_context(std::move(context));
    combat::CombatWorldConfig combat_config;
    combat_config.enabled = !o.no_combat;
    combat_config.seed = dice_seed;
    combat_config.default_aggro_cm = o.npc_aggro_cm;
    combat_config.default_leash_cm = o.npc_leash_cm;
    combat_config.npc_damage_mult = o.npc_damage_mult;
    players.set_combat_config(std::move(combat_config));
    combat::AftermathConfig aftermath;
    aftermath.enabled = !o.no_loot;
    aftermath.respawn = !o.no_respawn;
    aftermath.death_loss = !o.no_death_loss;
    aftermath.corpse_seconds = o.corpse_seconds;
    aftermath.loot_corpse_seconds = o.loot_corpse_seconds;
    aftermath.loot_chance_mult = o.loot_chance_mult;
    aftermath.loot_rarity_boost = o.loot_rarity_boost;
    aftermath.seed = o.loot_seed != 0 ? o.loot_seed : gather::fresh_seed();
    spdlog::info("[loot] the kills' loot rolls start from seed {}{}", aftermath.seed, o.loot_seed != 0 ? " (--loot-seed: the same rolls on every start)" : " (new on every start)");
    players.set_aftermath_config(std::move(aftermath));
    std::string table_error;
    if (!players.load_blueprints(o.npc_motion_path, table_error)) {
        spdlog::warn("[area] no Blueprint table ({}): the NPCs no retail capture opened cannot be made (run tools/mapc: mapc build)", table_error);
    }
}

void load_npc_and_mount_tables(PlayerWorld& players, WorldCliOptions& o) {
    std::string err;
    if (!players.load_npc_abilities(o.npc_abilities_path, o.npc_montages_path, o.npc_targeting_path, err)) {
        spdlog::warn("[combat] no mob attack animations ({}): the mobs swing without an animation (run scripts/extract_game_data.ps1)", err);
    }
    if (!players.load_npc_senses(o.npc_senses_path, o.npc_aggro_curves_path, err)) {
        spdlog::warn("[combat] no mob senses ({}): every mob notices within {} cm and leashes at {} cm (run scripts/extract_game_data.ps1)", err, o.npc_aggro_cm, o.npc_leash_cm);
    }
    if (players.mounts() && !players.load_mount_speeds(o.mount_speeds_path, err)) {
        spdlog::warn("[content] no mount speeds ({}): a mount runs at 600 cm/s times its item's ground percentage when it is desummoned (run tools/mapc: mapc mount-speeds)", err);
    }
    if (players.mounts() && !players.load_mount_riders(o.mount_riders_path, err)) {
        spdlog::warn("[content] no mount riders ({}): each mount names the rider AnimBP of its kind's retail donor (run tools/mapc: mapc mount-speeds)", err);
    }
    const bool by_level = std::filesystem::exists(o.roads_levels_path);
    players.set_road_default_level(std::max(0, spawns::SpawnFilter::node_level_number(o.spawn_node_level)));
    if (by_level) o.roads_path = o.roads_levels_path;
    else if (players.mounts()) {
        spdlog::warn("[content] no {} (contentc roads --levels writes it): the roads keep the tiers of {} and do not follow the city nodes' levels", o.roads_levels_path, o.roads_path);
    }
    if (players.mounts() && !players.load_roads(o.roads_path, err)) {
        spdlog::warn("[content] no roads ({}): a mount rides at its own speed everywhere (run tools/mapc: mapc roads, then contentc roads)", err);
    } else if (players.mounts() && players.roads().node_level() >= 0 && players.roads().node_level() != spawns::SpawnFilter::node_level_number(o.spawn_node_level)) {
        spdlog::warn("[content] {} resolved the roads' tiers at node level {}, the world runs at {} ({}): run contentc roads --node-level {}", o.roads_path, players.roads().node_level(),
                     spawns::SpawnFilter::node_level_number(o.spawn_node_level), o.spawn_node_level, o.spawn_node_level);
    }
}

void load_living_area_and_gathering(PlayerWorld& players, WorldCliOptions& o, uint64_t dice_seed) {
    if (!players.ready() || o.area_radius_m == 0) {
        if (o.area_radius_m == 0) spdlog::info("--area-radius 0: players see no other actors");
        return;
    }
    players.set_gm_garden(o.gm_garden);
    const auto started = std::chrono::steady_clock::now();
    configure_area_spawns_and_combat(players, o, dice_seed);
    load_npc_and_mount_tables(players, o);
    std::string area_error;
    if (!players.load_area(o.area, area_error)) {
        spdlog::error("the living area is not loaded (players see no other actors): {}", area_error);
    } else {
        spdlog::info("the living area loaded in {} ms: {} actors",
                     std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), players.registry().size());
        if (o.no_npc_movement) {
            spdlog::info("[nav] --no-npc-movement: the NPCs stand where the data puts them");
        } else if (std::string ground_error; !players.load_ground(o.area_nav_path, o.npc_motion_path, ground_error)) {
            spdlog::error("[nav] no ground, the NPCs stand where the data puts them and do not move: {} (build it: dotnet build/dotnet/bin/mapc/Release/net10.0/mapc.dll build)", ground_error);
        }
    }
    if (players.area_report() && players.gm_garden_requested() && !players.garden_report()) players.place_gm_garden();
    if (std::string platforms_error; !players.load_town_platforms(o.town_platforms_path, platforms_error)) {
        spdlog::warn("[floor] no town platforms ({}): the floor of a town's bowl is the interim model's, and a player put where the client has no floor yet falls through to the kill plane at -10,000 cm (run tools/mapc: mapc town-floors)", platforms_error);
    }
    players.start_scenery(world::NodeScenery::Config{!o.no_node_scenery, o.scenery_radius_m * 100.0, o.node_origin != "record"});
    players.start_node_people(spawns::NodePopulations::Config{!o.no_node_people});
    if (o.no_gathering) {
        spdlog::info("[gather] --no-gathering: the resource nodes are seen and not harvested");
        return;
    }
    gather::Config gathering;
    gathering.board.respawn_mult = o.gather_respawn_mult;
    gathering.quality_literal = o.gather_quality_literal;
    gathering.seed = o.gather_seed != 0 ? o.gather_seed : gather::fresh_seed();
    spdlog::info("[gather] the harvest rolls start from seed {}{}", gathering.seed, o.gather_seed != 0 ? " (--gather-seed: the same rolls on every start)" : " (new on every start)");
    if (std::string gather_error; !players.start_gathering(gathering, gather_error)) {
        spdlog::warn("[gather] harvesting is off: {} (the resource nodes are seen and not harvested)", gather_error);
    } else if (std::string foliage_error; !players.start_foliage(foliage::Config{o.foliage_map_path, o.foliage_regrow_mult}, foliage_error)) {
        spdlog::warn("[foliage] harvesting is off: {} (the trees, plants and stones of the world are seen and not harvested)", foliage_error);
    }
}

void configure_view_threads(WorldCliOptions& o) {
    const unsigned cores = std::thread::hardware_concurrency();
    const unsigned room = cores > 3 ? cores - 2 : 1;
    o.endpoint_config.view_threads = std::min(o.view_threads, std::max(room, 1u));
    if (o.endpoint_config.view_threads != o.view_threads) {
        if (o.view_threads_option && o.view_threads_option->count() > 0) {
            spdlog::warn("[view-threads] {} asked for, {} used: this machine has {} cores", o.view_threads, o.endpoint_config.view_threads, cores);
        } else {
            spdlog::info("[view-threads] the default of {} is lowered to {}: this machine has {} cores, and the passes get no more threads than its cores less two (at least one)",
                         o.view_threads, o.endpoint_config.view_threads, cores);
        }
    }
    if (o.endpoint_config.view_threads == 0) {
        spdlog::info("[view-threads] off: each session's work, view and flush run in turn on the network thread (--view-threads 0, or --tick-hz 0; --view-threads N runs the passes of the players' views on N threads)");
    } else {
        spdlog::info("[view-threads] {}: the tick runs every session's own work, then all the due passes of the players' views at once on {} thread(s), the network thread's included, then every flush{}",
                     o.endpoint_config.view_threads, o.endpoint_config.view_threads, o.endpoint_config.view_threads == 1 ? " (one thread: the passes run in order here)" : "");
    }
}

std::unique_ptr<svc::WorldAdminServer> start_admin_server(const WorldCliOptions& o, const std::string& token,
                                                          const PlayerWorld& players, GmTravel& travel, svc::AdminQueue& admin_queue) {
    if (o.no_world_admin) { spdlog::info("[admin] --no-world-admin: the admin API is not served"); return nullptr; }
    auto admin = std::make_unique<svc::WorldAdminServer>(travel, admin_queue, svc::WorldAdminServer::Options{o.world_admin, token});
    if (std::string admin_error; !admin->start(&admin_error)) {
        spdlog::error("[admin] the admin API is not served: {}", admin_error);
        return nullptr;
    }
    const bool loopback = o.world_admin.rfind("127.", 0) == 0 || o.world_admin.rfind("localhost", 0) == 0 || o.world_admin.rfind("[::1]", 0) == 0;
    spdlog::info("[admin] the admin API listens on {} ({} places to teleport to; worldctl players | places | tp | speed){}",
                 o.world_admin, players.places().all().size(), loopback ? "" : "; NOT loopback: anyone who can reach it and has the service token can move players");
    return admin;
}

std::unique_ptr<svc::WorldStreamServer> start_stream_server(const WorldCliOptions& o, const std::string& token,
                                                            PlayerWorld& players, Endpoint& endpoint, svc::AdminQueue& admin_queue) {
    if (o.no_world_stream) { spdlog::info("[stream] --no-world-stream: the world stream is not served"); return nullptr; }
    auto stream = std::make_unique<svc::WorldStreamServer>(players, [&] { return endpoint.players(); }, admin_queue, svc::WorldStreamServer::Options{o.world_stream, token});
    if (std::string stream_error; !stream->start(&stream_error)) {
        spdlog::error("[stream] the world stream is not served: {}", stream_error);
        return nullptr;
    }
    const bool loopback = o.world_stream.rfind("127.", 0) == 0 || o.world_stream.rfind("localhost", 0) == 0 || o.world_stream.rfind("[::1]", 0) == 0;
    spdlog::info("[stream] the world stream listens on {} (a snapshot of the world five times a second; services/cmd/worldctl: watch){}",
                 o.world_stream, loopback ? "" : "; NOT loopback: anyone who can reach it and has the service token can watch the world");
    return stream;
}

std::unique_ptr<aoc::world::telemetry::HealthMetricsHttpServer> start_metrics_server(
    const WorldCliOptions& o, const PlayerWorld& players, const Endpoint& endpoint, const LoopStats& loop_stats) {
    const auto* rep = players.area_report();
    const uint64_t monsters = rep ? rep->data_npcs : 0, resources = rep ? rep->gatherables : 0;
    const uint64_t npcs = players.registry().size() > resources ? players.registry().size() - resources : monsters;
    const auto started = std::chrono::steady_clock::now();
    return std::make_unique<aoc::world::telemetry::HealthMetricsHttpServer>([&, monsters, resources, npcs, started] {
        const auto up = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        return aoc::world::telemetry::HealthMetricsState{o.area.region_label, players.ready(), true, g_stop.load(),
            static_cast<uint32_t>(endpoint.session_count()), static_cast<uint32_t>(TickLoop::effective_tick_hz(o.tick_hz)),
            static_cast<uint64_t>(std::max<int64_t>(0, up)), loop_stats.datagrams, loop_stats.packets_out, npcs, monsters, resources,
            aoc::worldd::npc::NpcWorld::global_simulate_all_npcs(), static_cast<uint32_t>(std::max(0, o.area.worker_index)),
            o.area.worker_name, std::string(o.area.scope_kind()), std::string(aoc::world::telemetry::ResolveDataMode()),
            o.area.dynamic_scaling_stub, bootstrap::has_global_time_of_day_override() ? bootstrap::global_time_of_day_hours() : players.day_clock().hour(static_cast<double>(up) / 1000.0), 37.5};
    });
}

std::thread start_heartbeat_thread(svc::GrpcServices& services, const svc::RegisterResult& reg, std::atomic<int>& exit_code) {
    return std::thread([&] {
        const auto interval = std::chrono::milliseconds(std::max<int64_t>(reg.heartbeat_interval_ms, 100));
        for (;;) {
            sleep_unless_stopped(interval);
            if (g_stop) return;
            const auto s = services.heartbeat(reg.server_id);
            if (s == svc::CallStatus::NotFound) {
                spdlog::error("mesh no longer knows server {}; stopping", reg.server_id);
                exit_code = 1;
                g_stop = true;
                return;
            }
            if (s != svc::CallStatus::Ok) spdlog::warn("heartbeat to mesh failed ({})", svc::call_status_name(s));
        }
    });
}

bootstrap::GameStateMode parse_gamestate_mode(std::string_view mode_str) {
    if (mode_str == "captured") return bootstrap::GameStateMode::Captured;
    if (mode_str == "tier3") return bootstrap::GameStateMode::Tier3;
    return mode_str == "tier2" ? bootstrap::GameStateMode::Tier2 : bootstrap::GameStateMode::Tier1;
}

int parse_and_validate_cli(int argc, char** argv, WorldCliOptions& o) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && (std::string_view(argv[i]) == "--healthcheck" || std::string_view(argv[i]) == "-healthcheck")) return -1;
    }
    CLI::App app{"world: the AoC world server"};
    register_service_cli_options(app, o);
    register_area_cli_options(app, o);
    register_combat_and_gather_cli_options(app, o);
    register_tick_cli_options(app, o);
    CLI11_PARSE(app, argc, argv);
    return validate_cli_options(o);
}

std::unique_ptr<UdpSocket> bind_world_socket(WorldCliOptions& o, Address& public_addr) {
    if (o.public_address.empty() && o.bind.rfind("0.0.0.0:", 0) == 0) o.public_address = "127.0.0.1:" + o.bind.substr(8);
    const auto bind_addr = parse_address(o.bind), pub = parse_address(o.public_address.empty() ? o.bind : o.public_address);
    if (!bind_addr || !pub || pub->ip == 0 || pub->port == 0) {
        spdlog::error("--bind and --public-address must be a.b.c.d:port, and the public address dialable");
        return nullptr;
    }
    public_addr = *pub;
    std::string error;
    auto socket = UdpSocket::open(*bind_addr, &error);
    if (!socket) spdlog::error("{}", error);
    return socket;
}

void shutdown_world_services(UdpSocket& socket, Endpoint& endpoint, CharacterSaver& saver, quest::QuestWorker& quest_worker, std::thread& heartbeat) {
    spdlog::info("stopping: telling {} connected clients", endpoint.session_count());
    g_stop = true;
    endpoint.kick_all(login_text::kShuttingDown, Clock::now());
    drain_shutdown_window(socket, endpoint, std::chrono::milliseconds(500));
    endpoint.finish_players("world is stopping");
    if (!saver.flush(std::chrono::seconds(5))) spdlog::warn("stopping: {} position(s) were not saved in 5 s", saver.queued());
    if (!quest_worker.flush(std::chrono::seconds(5))) spdlog::warn("stopping: a quest log was not saved in 5 s");
    heartbeat.join();
}

}  // namespace

int main(int argc, char** argv) {
    WorldCliOptions o;
    if (const int rc = parse_and_validate_cli(argc, argv, o); rc != 0) return rc < 0 ? 0 : rc;
    std::string token, content_problem;
    std::optional<content::ContentDb> content;
    if (!init_logging_token_and_content(o, token, content, content_problem)) return 1;
    Address public_addr{};
    auto socket = bind_world_socket(o, public_addr);
    if (!socket) return 1;
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
    svc::GrpcServices services({o.gateway, o.character, o.mesh}, token);
    svc::RegisterResult reg;
    if (const int rc = register_with_mesh(services, public_addr, o, reg); rc != 0 || g_stop) return rc;
    uint32_t randomizer = 0;
    while (randomizer == 0) secure_random(reinterpret_cast<uint8_t*>(&randomizer), sizeof(randomizer));
    aoc::protocol::ServerGuidAllocator guids(reg.server_id, randomizer);
    spdlog::info("GameState mode {}", o.gamestate_mode);
    NodeRuntimeState nodes;
    CharacterSaver saver(services, CharacterSaver::Config{.destructor_wait = std::chrono::milliseconds(0)});
    const char* quests_env = std::getenv("AOC_QUESTS_GRPC_ADDR");
    quest::GrpcQuestApi quest_api(quests_env && *quests_env ? quests_env : "127.0.0.1:50058", token);
    quest::QuestWorker quest_worker(quest_api);
    PlayerWorld players(content ? &*content : nullptr, content_problem, parse_gamestate_mode(o.gamestate_mode), reg.server_id, guids);
    std::shared_ptr<SharedUpdates> shared_updates;
    const uint64_t dice_seed = configure_player_world(players, o, saver, quest_worker, shared_updates);
    std::unique_ptr<svc::CityNodeFeed> city_feed;
    if (content && players.ready()) load_city_nodes_and_scenery(players, o, token, nodes, city_feed);
    load_living_area_and_gathering(players, o, dice_seed);
    spdlog::info("moves are checked: at most {:.0f} cm/s along the ground; positions are saved every {} s and when a session ends", o.movement.limits.max_speed, o.movement.save_interval.count());
    o.endpoint_config.players = &players;
    configure_view_threads(o);
    std::atomic<int> exit_code{0};
    std::thread heartbeat = start_heartbeat_thread(services, reg, exit_code);
    CookieSigner signer;
    LoginService login(services, services, &quest_api);
    Endpoint* endpoint_ptr = nullptr;
    LoginWorker worker(login, reg.server_id, [&](uint64_t id, LoginResult r) { endpoint_ptr->post_login_result(id, std::move(r)); });
    LoopStats loop_stats;
    Endpoint endpoint(o.endpoint_config, signer, worker, [&](const Address& to, const std::vector<uint8_t>& pkt) { loop_stats.bytes_out += pkt.size(); ++loop_stats.packets_out; socket->send(to, pkt.data(), pkt.size()); }, secure_random, Clock::now());
    endpoint_ptr = &endpoint;
    endpoint.publish_token_snapshot(TokenSnapshot{1, token, {}, Clock::now()});
    GmTravel travel(players, [&] { return endpoint.players(); }, [] { return Clock::now(); });
    players.set_gm_port(&travel);
    svc::AdminQueue admin_queue;
    auto admin = start_admin_server(o, token, players, travel, admin_queue);
    auto stream = start_stream_server(o, token, players, endpoint, admin_queue);
    auto metrics = start_metrics_server(o, players, endpoint, loop_stats);
    TickLoopBudget tick_budget;
    WorldLoopContext loop_ctx{*socket, endpoint, saver, admin_queue, stream.get(), shared_updates.get(), loop_stats, g_stop, exit_code, o.tick_hz, o.loop_stats_seconds, &tick_budget};
    run_world_loop(loop_ctx);
    if (metrics) metrics->stop();
    if (stream) stream->stop();
    if (admin) admin->stop();
    players.set_gm_port(nullptr);
    shutdown_world_services(*socket, endpoint, saver, quest_worker, heartbeat);
    return exit_code;
}
