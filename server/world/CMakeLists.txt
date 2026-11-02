# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# ============================================================================
#  world: the C++ world server (plan A4: the connection layer; plan A5: the native bootstrap, bootstrap/, and the
#  player session after NMT_Join, session/; plan A6: the move checks and the saving of positions, session/)
#
#  world_core holds everything but main() so the tests can link it.
#  PROTOC and GRPC_CPP_PLUGIN come from the top-level CMakeLists.
#
#  content/content_db reads content.db (plan C1's SQLite file) read-only: the vcpkg sqlite3 port
#  (vcpkg.json) gives unofficial::sqlite3::sqlite3.
# ============================================================================

find_package(unofficial-sqlite3 CONFIG REQUIRED)

# ── gRPC stubs come from unified libs/aocproto (world_protos ALIAS aocproto) ──
# ── stats & nav come from libs/aocstats (world_stats ALIAS aocstats) & libs/aocnav (world_nav ALIAS aocnav) ──

# ── ability_content: the ability tables of content.db (plan D2), a connection of its own ───────────────────
add_library(world_ability_content STATIC
    content/ability_content.cpp
    content/ability_content.h
)
target_include_directories(world_ability_content PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(world_ability_content PUBLIC
    world_stats
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
)
set_target_properties(world_ability_content PROPERTIES FOLDER "Servers/world")

# ── world_core ──────────────────────────────────────────────────────────────
add_library(world_core STATIC
    bootstrap/ability_replication.h
    bootstrap/bootstrap.cpp
    bootstrap/bootstrap.h
    bootstrap/bootstrap_abilities.cpp
    bootstrap/bootstrap_economy.cpp
    bootstrap/bootstrap_internal.h
    bootstrap/bootstrap_join.cpp
    bootstrap/bootstrap_world.cpp
    bootstrap/captured_abilities.h
    bootstrap/captured_bits.h
    bootstrap/captured_character.h
    bootstrap/captured_gamestate.h
    bootstrap/captured_level_list.h
    bootstrap/captured_replies.h
    bootstrap/captured_setup.cpp
    bootstrap/captured_setup.h
    bootstrap/captured_stats.h
    bootstrap/character_setup.cpp
    bootstrap/character_setup.h
    bootstrap/character_setup_appearance.cpp
    bootstrap/constants.cpp
    bootstrap/constants.h
    bootstrap/gather_wire.cpp
    bootstrap/mount_ride.cpp
    bootstrap/player_setup.cpp
    bootstrap/player_setup.h
    combat/aftermath.cpp
    combat/aftermath.h
    combat/combat_data.cpp
    combat/combat_data.h
    combat/combat_world.cpp
    combat/combat_world.h
    combat/combat_world_ai.cpp
    combat/combat_world_attacks.cpp
    combat/fighter.h
    combat/hit_context.cpp
    combat/hit_context.h
    combat/hit_resolver.cpp
    combat/hit_resolver.h
    combat/npc_kits.h
    combat/npc_replicator.cpp
    combat/npc_replicator.h
    combat/npc_stats.cpp
    combat/npc_stats.h
    combat/rewards.cpp
    combat/rewards.h
    content/content_db.cpp
    content/content_db.h
    content/content_db_craft.cpp
    content/content_db_economy.cpp
    content/content_db_internal.h
    content/content_db_loot_mounts.cpp
    content/content_db_npcs.cpp
    content/content_db_progression.cpp
    content/content_db_quests.cpp
    content/content_db_talents.cpp
    foliage/foliage.cpp
    foliage/foliage.h
    foliage/foliage_cells.cpp
    foliage/foliage_cells.h
    foliage/foliage_map.cpp
    foliage/foliage_map.h
    foliage/foliage_store.cpp
    foliage/foliage_store.h
    gather/gathering.cpp
    gather/gathering.h
    gather/node_board.cpp
    gather/node_board.h
    gather/node_wire.cpp
    gather/node_wire.h
    gather/rules.cpp
    gather/rules.h
    items/inventory.cpp
    items/inventory.h
    items/inventory_ops.cpp
    items/resource_grid.cpp
    items/resource_grid.h
    login/login_service.cpp
    login/login_service.h
    login/login_worker.cpp
    login/login_worker.h
    net/connection.cpp
    net/connection.h
    net/control_channel.cpp
    net/control_channel.h
    net/cookie.cpp
    net/cookie.h
    net/endpoint.cpp
    net/endpoint.h
    net/udp_socket.cpp
    net/udp_socket.h
    quest/bootstrap_objects.cpp
    quest/quest_log.cpp
    quest/quest_log.h
    quest/quest_service.cpp
    quest/quest_service.h
    quest/quest_wire.cpp
    quest/quest_wire.h
    quest/session_givers.cpp
    quest/session_handin.cpp
    quest/session_objects.cpp
    quest/session_quest_effects.cpp
    respawn/bootstrap_embersprings.cpp
    respawn/session_embersprings.cpp
    session/abilities.cpp
    session/abilities.h
    session/actor_view.cpp
    session/actor_view.h
    session/ability_catalog.cpp
    session/ability_catalog.h
    session/ability_pipeline.cpp
    session/ability_pipeline.h
    session/ability_pipeline_cast.cpp
    session/ability_pipeline_damage.cpp
    session/ability_pipeline_hits.cpp
    session/ability_pipeline_internal.h
    session/ability_pipeline_zones.cpp
    session/ability_reactions.cpp
    session/action_bar.cpp
    session/action_bar.h
    session/gm_travel.cpp
    session/gm_travel.h
    session/item_use.cpp
    session/item_use.h
    session/leveling.cpp
    session/leveling.h
    session/mounts.cpp
    session/mounts.h
    session/move_checker.cpp
    session/move_checker.h
    session/player_foliage.cpp
    session/player_gather.cpp
    session/player_session.cpp
    session/player_session.h
    session/player_session_artisan.cpp
    session/player_session_combat.cpp
    session/player_session_guild.cpp
    session/player_session_internal.h
    session/player_session_items.cpp
    session/player_session_lifecycle.cpp
    session/player_session_loot.cpp
    session/player_session_mount.cpp
    session/player_session_movement.cpp
    session/player_session_quests.cpp
    session/player_session_quick_craft.cpp
    session/player_session_state.h
    session/player_session_types.h
    session/player_session_vendor.cpp
    session/player_session_zone.cpp
    session/player_world.cpp
    session/player_world.h
    session/player_world_relevancy.cpp
    session/player_world_spawns.cpp
    session/position_saver.cpp
    session/position_saver.h
    session/professions.cpp
    session/professions.h
    session/riding.cpp
    session/riding.h
    session/talents.cpp
    session/talents.h
    svc/city_node_control.cpp
    svc/city_node_control.h
    svc/city_node_feed.cpp
    svc/city_node_feed.h
    svc/grpc_services.cpp
    svc/grpc_services.h
    svc/service_token.cpp
    svc/service_token.h
    svc/services.h
    svc/world_admin.cpp
    svc/world_admin.h
    spawns/area.h
    spawns/area_loader.cpp
    spawns/area_loader.h
    spawns/area_spawns.h
    spawns/captured_spawns.cpp
    spawns/captured_spawns.h
    spawns/corpse_exports.h
    spawns/generic_npc.cpp
    spawns/gm_garden.cpp
    spawns/gm_garden.h
    spawns/garden_defs.h
    spawns/generic_npc.h
    spawns/mount_donors.h
    spawns/mount_spawn.cpp
    spawns/mount_spawn.h
    spawns/mount_state.cpp
    spawns/mount_state.h
    spawns/node_population.cpp
    spawns/node_population.h
    spawns/node_populations.cpp
    spawns/node_populations.h
    spawns/population.cpp
    spawns/population.h
    spawns/region_data.h
    spawns/region_index.cpp
    spawns/region_index.h
    spawns/spawn_filter.cpp
    spawns/spawn_filter.h
    spawns/spawn_util.h
    vendor/gm_vendor.cpp
    vendor/gm_vendor.h
    vendor/node_tax.cpp
    vendor/node_tax.h
    vendor/node_tax_table.h
    warehouse/player_warehouse.cpp
    world/city_nodes.cpp
    world/city_nodes.h
    world/node_control.h
    world/node_layout_data.cpp
    world/node_layout_data.h
    world/node_origins.h
    world/node_scenery.cpp
    world/node_scenery.h
    world/node_wire.cpp
    world/node_wire.h
    world/town_floor.cpp
    world/town_floor.h
    world/corpse.cpp
    world/corpse.h
    world/actor_registry.cpp
    world/actor_registry.h
    world/interaction_state.cpp
    world/interaction_state.h
    world/places.cpp
    world/places.h
    world/relevancy.h
    world/roads.cpp
    world/roads.h
    world/zoi_surface.cpp
    world/zoi_surface.h
    world/zoi_surface_table.h
    npc/ai/npc_ai.cpp
    npc/ai/npc_ai.h
    npc/ai/threat.cpp
    npc/ai/threat.h
    npc/area_npcs.cpp
    npc/area_npcs.h
    npc/motion_table.cpp
    npc/motion_table.h
    npc/mover.cpp
    npc/mover.h
    npc/npc_world.cpp
    npc/npc_world.h
    npc/npc_world_orders.cpp
)
target_include_directories(world_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(world_core PUBLIC
    aocnet
    aocproto
    aocnav
    aocstats
    world_ability_content
    spdlog::spdlog
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    unofficial::sqlite3::sqlite3
    $<$<PLATFORM_ID:Windows>:ws2_32>
)
if(MSVC)
    target_compile_definitions(world_core PUBLIC
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()
set_target_properties(world_core PROPERTIES FOLDER "Servers/world")

# ── world ───────────────────────────────────────────────────────────────────
add_executable(world main.cpp)
target_link_libraries(world PRIVATE world_core CLI11::CLI11)
set_target_properties(world PROPERTIES FOLDER "Servers")

# ── tests: plain executables that print [ok ]/[FAIL], like libaocnet's ──────
set(WORLD_LEGACY_TAG "d")
function(world_resolve_test_src out_var rel_dir name)
    set(candidate "${rel_dir}/${name}.cpp")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${candidate}")
        string(REGEX REPLACE "^test_world_" "test_world${WORLD_LEGACY_TAG}_" legacy_name "${name}")
        set(candidate "${rel_dir}/${legacy_name}.cpp")
    endif()
    set(${out_var} "${candidate}" PARENT_SCOPE)
endfunction()

function(world_add_test name)
    if(ARGC GREATER 1)
        set(src_file "${ARGV1}")
    else()
        world_resolve_test_src(src_file "tests" "${name}")
    endif()
    add_executable(${name} ${src_file})
    target_link_libraries(${name} PRIVATE world_core)
    # capture_packets.h lives with the libaocnet tests.
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/libs/aocnet/tests)
    # the shared stat corpus (plan B3): the tests that build a content.db fixture take the stat program from it
    target_compile_definitions(${name} PRIVATE
        STATS_CORPUS_PATH="${PROJECT_SOURCE_DIR}/libs/aocstats/testdata/corpus.txt"
        TALENT_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/talent_fixture.sql")
    set_target_properties(${name} PROPERTIES FOLDER "Tests/world")
endfunction()

foreach(t
    test_world_abilities
    test_world_ability_session
    test_world_actor_nak
    test_world_actor_view
    test_world_area_session
    test_world_bootstrap
    test_world_channels
    test_world_character_setup
    test_world_connection
    test_world_content_db
    test_world_control_channel
    test_world_cookie
    test_world_endpoint
    test_world_items
    test_world_items_session
    test_world_leveling
    test_world_login
    test_world_move_checker
    test_world_movement
    test_world_npc_ai
    test_world_nav
    test_world_nav_window
    test_world_npc_move
    test_world_town_walk
    test_world_repl_acks
    test_world_services
    test_world_session
    test_world_talents
    test_world_udp_socket
)
    world_add_test(${t})
endforeach()
target_sources(test_world_area_session PRIVATE
    tests/test_worldd_area_dialogue.cpp
    tests/test_worldd_quest_offer_lifecycle.cpp
    tests/test_worldd_quest_progression_handin.cpp
)
target_sources(test_world_character_setup PRIVATE tests/test_worldd_character_setup_stats.cpp)
target_sources(test_world_movement PRIVATE tests/test_worldd_movement_saver.cpp)
target_sources(test_world_session PRIVATE
    tests/test_worldd_session_relog.cpp
    tests/test_worldd_session_lifecycle.cpp
)
world_add_test(test_world_abilities_cooldowns tests/test_worldd_abilities_cooldowns.cpp)
world_add_test(test_world_abilities_retail tests/test_worldd_abilities_retail.cpp)
world_add_test(test_world_items_bootstrap tests/test_worldd_items_bootstrap.cpp)
world_add_test(test_world_items_fuzz tests/test_worldd_items_fuzz.cpp)
# Gathering stage 2: the session tests of the professions and the GM artisan scrolls run on the real content.db of the working tree (they skip without it)
world_add_test(test_world_items_session_persistence tests/test_worldd_items_session_persistence.cpp)
world_add_test(test_world_items_session_stats tests/test_worldd_items_session_stats.cpp)
world_add_test(test_world_items_session_scrolls tests/test_worldd_items_session_scrolls.cpp)
world_add_test(test_world_items_session_talents tests/test_worldd_items_session_talents.cpp)
world_add_test(test_world_items_session_weapons_resources tests/test_worldd_items_session_weapons_resources.cpp)
foreach(t
    test_world_items_session
    test_world_items_session_persistence
    test_world_items_session_stats
    test_world_items_session_scrolls
    test_world_items_session_talents
    test_world_items_session_weapons_resources
)
    target_compile_definitions(${t} PRIVATE
        PROFESSIONS_REAL_DB="${PROJECT_SOURCE_DIR}/dist/content/content.db")
endforeach()

# the ability tables' reader (plan D2) on the content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB), with the stat engine
world_resolve_test_src(TEST_WORLD_ABILITY_CONTENT_SRC "tests" "test_world_ability_content")
add_executable(test_world_ability_content ${TEST_WORLD_ABILITY_CONTENT_SRC})
target_link_libraries(test_world_ability_content PRIVATE world_ability_content world_stats)
target_compile_definitions(test_world_ability_content PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
set_target_properties(test_world_ability_content PROPERTIES FOLDER "Tests/world")

# the living area (plan E1) on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
world_add_test(test_world_area)
target_sources(test_world_area PRIVATE tests/test_worldd_area_vendors_ground.cpp)
target_compile_definitions(test_world_area PRIVATE
    AREA_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# the harvest of the resource nodes (Gathering stage 2, "Harvesting"): the node state, the wire against the retail capture, the requirements and the rolls, on the real content.db of the working tree
world_add_test(test_world_gather)
target_compile_definitions(test_world_gather PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
world_add_test(test_world_gather_session)
target_compile_definitions(test_world_gather_session PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
world_add_test(test_world_gather_session_bag tests/test_worldd_gather_session_bag.cpp)
target_compile_definitions(test_world_gather_session_bag PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# Resource storage (Gathering stage 3): the grid against the captured slots, the routing, the top-up, the full container, the moves and the login's repair on a fixture, and the flags of every
# item of the real content.db of the working tree
world_add_test(test_world_resource)
target_compile_definitions(test_world_resource PRIVATE
    RESOURCE_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# Foliage gathering (Gathering stage 3): the map, the store with a fake clock, the cells' state actors and the foliage tables of the real content.db and region_foliage.tsv of the working tree
world_add_test(test_world_foliage)
target_compile_definitions(test_world_foliage PRIVATE
    FOLIAGE_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    FOLIAGE_TSV_PATH="${PROJECT_SOURCE_DIR}/dist/content/region_foliage.tsv")

world_add_test(test_world_foliage_session)
target_compile_definitions(test_world_foliage_session PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    FOLIAGE_TSV_PATH="${PROJECT_SOURCE_DIR}/dist/content/region_foliage.tsv")
world_add_test(test_world_foliage_session_world tests/test_worldd_foliage_session_world.cpp)
target_compile_definitions(test_world_foliage_session_world PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    FOLIAGE_TSV_PATH="${PROJECT_SOURCE_DIR}/dist/content/region_foliage.tsv")

# Hunting (Gathering stage 3): the huntable creature's template, the 53 hunting interactions and their rewards, and the corpse's wire against the retail capture, on the real content.db of the working tree
world_add_test(test_world_hunting)
target_compile_definitions(test_world_hunting PRIVATE
    HUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

world_add_test(test_world_hunting_session)
target_sources(test_world_hunting_session PRIVATE tests/test_worldd_hunting_session_refusals.cpp)
target_compile_definitions(test_world_hunting_session PRIVATE
    GATHER_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# the professions and the GM artisan items (Gathering stage 2): the rules on a small book and the 67 scrolls on the real content.db of the working tree
world_add_test(test_world_professions)
target_compile_definitions(test_world_professions PRIVATE
    PROFESSIONS_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# the Riverlands as the area (G1 stage 1, Part B): the index, the population against an independent oracle, the region's load, the GM garden
world_add_test(test_world_region)
target_compile_definitions(test_world_region PRIVATE
    REGION_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    REGION_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# the scale test (G1 stage 1, Part B): the full region and synthetic sessions crossing it; needs the chunked area_nav.bin (it skips without), run it alone
world_add_test(test_world_region_load)
target_compile_definitions(test_world_region_load PRIVATE
    LOAD_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    LOAD_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv"
    LOAD_NAV_PATH="${PROJECT_SOURCE_DIR}/dist/content/area_nav.bin")

# the mounts (the mounts milestone, stage 2): the mount actor, its controller and the session on the real content.db of the working tree
world_add_test(test_world_mounts)
target_sources(test_world_mounts PRIVATE tests/test_worldd_mounts_summon.cpp)
target_compile_definitions(test_world_mounts PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv"
    MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")
world_add_test(test_world_mounts_session)
target_compile_definitions(test_world_mounts_session PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")
world_add_test(test_world_mounts_fly)
target_compile_definitions(test_world_mounts_fly PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")
world_add_test(test_world_mounts_ride)
world_add_test(test_world_mounts_ride_combat tests/test_worldd_mounts_ride_combat.cpp)
world_add_test(test_world_mounts_ride_flight_water tests/test_worldd_mounts_ride_flight_water.cpp)
foreach(t
    test_world_mounts_ride
    test_world_mounts_ride_combat
    test_world_mounts_ride_flight_water
)
    target_compile_definitions(${t} PRIVATE
        MOUNT_ROADS_PATH="${PROJECT_SOURCE_DIR}/dist/content/roads.tsv"
        MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
        MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv"
        MOUNT_RIDERS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_riders.tsv")
endforeach()

# the GM travel tools (teleport and run speed): the session, the client's GM RPCs and the admin API on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
world_add_test(test_world_gm_travel)
target_compile_definitions(test_world_gm_travel PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")
world_add_test(test_world_gm_travel_admin tests/test_worldd_gm_travel_admin.cpp)
target_compile_definitions(test_world_gm_travel_admin PRIVATE
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")

# the city nodes (Nodes 1, stage 1): the registry and the whole-world ZOI lookup on the real content.db of the working tree, the node reads against the shared corpus's
# nodehost blocks, the seed fallback, and the citynode feed against an in-process fake of the service
world_add_test(test_world_city_nodes)
target_compile_definitions(test_world_city_nodes PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json")

# the nodes the client is told of (Nodes 1, stage 2): the maps, summaries and sieges of the registry's levels against the retail capture's, the session's answers (their order, the
# live update, the GM console) and the GM control over gRPC against an in-process fake of the node service
world_add_test(test_world_node_state)
target_compile_definitions(test_world_node_state PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json"
    MOUNT_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    MOUNT_SPEEDS_PATH="${PROJECT_SOURCE_DIR}/dist/content/mount_speeds.tsv")

# the towns' scenery (Nodes 1, stage 3a): the items of each town's replicator built from the real content.db against the 18 retail payloads of the captures, the replicator actors
# (their place, a level change as a new full payload), the actor view opening and closing them by range, and the world following the node levels
world_add_test(test_world_node_layout)
target_compile_definitions(test_world_node_layout PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json")
world_add_test(test_world_node_layout_view tests/test_worldd_node_layout_view.cpp)
target_compile_definitions(test_world_node_layout_view PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json")

# the towns' people (Nodes 1, stage 3b): the selection of each node's NPCs at each level against the Python oracle's golden counts (tests/node_pop_golden.h), the live manager (a level change
# spawns and despawns the right sets, a respawn follows the level, the ground's pending rules), the any-of siege rule and the roads by node level, on the real content.db
world_add_test(test_world_node_pop)
world_add_test(test_world_node_pop_roads tests/test_worldd_node_pop_roads.cpp)
world_add_test(test_world_node_pop_buildings tests/test_worldd_node_pop_buildings.cpp)
foreach(t
    test_world_node_pop
    test_world_node_pop_roads
    test_world_node_pop_buildings
)
    target_compile_definitions(${t} PRIVATE
        NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
        NODES_SEED_PATH="${PROJECT_SOURCE_DIR}/deploy/citynode_seed.json"
        NODES_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv"
        NODES_ROADS_LEVELS_PATH="${PROJECT_SOURCE_DIR}/dist/content/roads_levels.tsv")
endforeach()

# the towns' real ground: the platform reader and sampler on bytes made in the test, the frames, and - when dist/content/town_platforms.bin exists (mapc town-floors) - the real
# platforms at the real node records and the world's ground queries on them; AOC_REAL_GROUND=1 adds the baked pit
world_add_test(test_world_town_platform)
target_compile_definitions(test_world_town_platform PRIVATE
    NODES_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NODES_NAV_PATH="${PROJECT_SOURCE_DIR}/dist/content/area_nav.bin"
    TOWN_PLATFORMS_PATH="${PROJECT_SOURCE_DIR}/dist/content/town_platforms.bin")

# the spawn filter (plan E1, stage 3) on the same content.db and dist/content/npc_motion.tsv
world_add_test(test_world_spawn_filter)
target_compile_definitions(test_world_spawn_filter PRIVATE
    AREA_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")

# the combat core (plan E1, stage 3): NPC stats, damage and the hit rules on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
world_add_test(test_world_combat)
target_sources(test_world_combat PRIVATE
    tests/test_worldd_combat_cast_grem.cpp
    tests/test_worldd_combat_gates.cpp
    tests/test_worldd_combat_lunge_chase.cpp
    tests/test_worldd_combat_projectiles.cpp
    tests/test_worldd_combat_generic.cpp
)
target_compile_definitions(test_world_combat PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# what follows a death (plan E1, stage 3, part 2): the experience, the loot, the corpse and the respawn on the real content.db
world_add_test(test_world_aftermath)
target_compile_definitions(test_world_aftermath PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# what a kill hands out (plan E1, stage 3, part 2): the reward tables of the real content.db and dist/content/npc_motion.tsv
world_add_test(test_world_rewards)
target_compile_definitions(test_world_rewards PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")

# the generic ability runtime (plan D2) on the real content.db of the working tree (dist/content/content.db, or AOC_CONTENT_DB)
world_add_test(test_world_ability_pipeline)
target_compile_definitions(test_world_ability_pipeline PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
world_add_test(test_world_ability_pipeline_chains tests/test_worldd_ability_pipeline_chains.cpp)
target_compile_definitions(test_world_ability_pipeline_chains PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
world_add_test(test_world_ability_retail)
target_compile_definitions(test_world_ability_retail PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
world_add_test(test_world_ability_all)
target_compile_definitions(test_world_ability_all PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
# every ability of every class against the client's tooltip (plan E1, stage 3 fix, Part C): tests/tooltip_key.h is written by tools/capture/gen_tooltips.py
world_add_test(test_world_tooltips)
target_compile_definitions(test_world_tooltips PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db")
# the class abilities that need another actor against the retail captures, on a creature of the world (plan E1, stage 3, part 2)
world_add_test(test_world_ability_npc)
target_compile_definitions(test_world_ability_npc PRIVATE
    ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
    NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")
world_add_test(test_world_ability_session_endpoint tests/test_worldd_ability_session_endpoint.cpp)
world_add_test(test_world_ability_session_casts tests/test_worldd_ability_session_casts.cpp)
world_add_test(test_world_ability_session_combat tests/test_worldd_ability_session_combat.cpp)
world_add_test(test_world_ability_session_targets tests/test_worldd_ability_session_targets.cpp)
world_add_test(test_world_ability_session_death_loot tests/test_worldd_ability_session_death_loot.cpp)
foreach(t
    test_world_ability_session
    test_world_ability_session_endpoint
    test_world_ability_session_casts
    test_world_ability_session_combat
    test_world_ability_session_targets
    test_world_ability_session_death_loot
)
    target_compile_definitions(${t} PRIVATE
        ABILITY_CONTENT_DB_PATH="${PROJECT_SOURCE_DIR}/dist/content/content.db"
        NPC_MOTION_PATH="${PROJECT_SOURCE_DIR}/dist/content/npc_motion.tsv")
endforeach()
