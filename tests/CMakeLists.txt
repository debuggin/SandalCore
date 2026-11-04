# SPDX-License-Identifier: AGPL-3.0-only

get_filename_component(SANDAL_WORLD_GEN_DIR "${CMAKE_BINARY_DIR}/../gen/world" ABSOLUTE)
if(NOT EXISTS "${SANDAL_WORLD_GEN_DIR}" AND EXISTS "${CMAKE_SOURCE_DIR}/../build/gen/world")
    get_filename_component(SANDAL_WORLD_GEN_DIR "${CMAKE_SOURCE_DIR}/../build/gen/world" ABSOLUTE)
elseif(NOT EXISTS "${SANDAL_WORLD_GEN_DIR}" AND EXISTS "${CMAKE_SOURCE_DIR}/build/gen/world")
    get_filename_component(SANDAL_WORLD_GEN_DIR "${CMAKE_SOURCE_DIR}/build/gen/world" ABSOLUTE)
endif()

function(add_sandal_domain_test_exe target_name source_relpath)
    add_executable(${target_name} "${CMAKE_CURRENT_SOURCE_DIR}/${source_relpath}")
    target_include_directories(${target_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/world"
        "${SANDAL_WORLD_GEN_DIR}"
        "${SANDAL_WORLD_GEN_DIR}/tests"
        "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/server/world"
        "${PROJECT_SOURCE_DIR}/libs/aocnet"
        "${PROJECT_SOURCE_DIR}/libs/aocnav/include"
        "${PROJECT_SOURCE_DIR}/libs/aocstats/include"
    )
    target_compile_definitions(${target_name} PRIVATE
        STATS_CORPUS_PATH="${CMAKE_BINARY_DIR}/../gen/fixtures/corpus.txt"
        TALENT_FIXTURE_PATH="${SANDAL_WORLD_GEN_DIR}/tests/talent_fixture.sql"
    )
    target_link_libraries(${target_name} PRIVATE
        world_core
        aocnav
        aocstats
        aocnet
    )
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        FOLDER "Tests/domain"
    )
    if(MSVC)
        target_compile_definitions(${target_name} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            WIN32_LEAN_AND_MEAN
            NOMINMAX
        )
    endif()
    if(AOC_DATA_FREE)
        target_compile_definitions(${target_name} PRIVATE AOC_DATA_FREE=1)
    endif()
endfunction()

set(SANDAL_DOMAIN_TEST_TARGETS
    test_world_clock_and_data_free
    test_world_zoi_and_nodes
    test_region_classifier
    test_zoi_classifier
    test_nav_ground_snap
    test_nav_los_and_bvh
    test_combat_core_formulas
    test_combat_procs_and_cc
    test_combat_mob_gates_and_swings
    test_quests_dialogue_session
    test_quests_objectives_and_commissions
    test_protocol_bitstream_bounds
    test_character_inventory_and_glint
    test_character_mounts_and_talents
    test_mesh_handover_hysteresis
    test_chaos_fault_seams
    fuzz_farchive_langsec
    test_move_packed_inline_base
    test_diff_save_334_parity
    test_stop_cast_race
    test_world_config
    test_health_probes_drain
    test_canonical_json_logger
    test_net_serialize_all_12
    test_content_hotfix_reload
    test_os_pal_determinism
    test_three_ring_stress
    test_content_snapshot_rcu
    test_swarm_500_bots_30hz
    test_boundary_oscillation_10k
    test_transport_matrix_36
    test_v1_udp_jitter_loss
    test_v2_pg17_wal_stall
    test_v3_mutated_bitstreams
    test_v4_live_rcu_load
    test_v5_sigkill_handover
    test_v6_dst_buggify
)

add_sandal_domain_test_exe(test_world_clock_and_data_free world/test_world_clock_and_data_free.cpp)
add_sandal_domain_test_exe(test_world_config world/test_world_config.cpp)
target_sources(test_world_config PRIVATE "${PROJECT_SOURCE_DIR}/libs/aoccore/config/world_config.cpp")
add_test(NAME world.config COMMAND test_world_config)
set_tests_properties(world.config PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_health_probes_drain world/test_health_probes_drain.cpp)
add_test(NAME world.health_probes_drain COMMAND test_health_probes_drain)
set_tests_properties(world.health_probes_drain PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_canonical_json_logger world/test_canonical_json_logger.cpp)
add_test(NAME world.canonical_json_logger COMMAND test_canonical_json_logger)
set_tests_properties(world.canonical_json_logger PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_os_pal_determinism world/test_os_pal_determinism.cpp)
add_test(NAME world.os_pal_determinism COMMAND $<TARGET_FILE:test_os_pal_determinism> WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(world.os_pal_determinism PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_test(NAME world.test_world_ability_content COMMAND $<TARGET_FILE:test_world_ability_content> WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(world.test_world_ability_content PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_world_zoi_and_nodes world/test_world_zoi_and_nodes.cpp)
add_sandal_domain_test_exe(test_region_classifier world/test_region_classifier.cpp)
add_test(
    NAME world.test_region_classifier
    COMMAND $<TARGET_FILE:test_region_classifier>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(world.test_region_classifier PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_zoi_classifier world/test_zoi_classifier.cpp)
add_test(
    NAME world.test_zoi_classifier
    COMMAND $<TARGET_FILE:test_zoi_classifier>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(world.test_zoi_classifier PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_nav_ground_snap nav/test_nav_ground_snap.cpp)
add_sandal_domain_test_exe(test_nav_los_and_bvh nav/test_nav_los_and_bvh.cpp)
add_sandal_domain_test_exe(test_combat_core_formulas combat/test_combat_core_formulas.cpp)
add_sandal_domain_test_exe(test_combat_procs_and_cc combat/test_combat_procs_and_cc.cpp)
add_sandal_domain_test_exe(test_combat_mob_gates_and_swings combat/test_combat_mob_gates_and_swings.cpp)
add_sandal_domain_test_exe(test_quests_dialogue_session quests/test_quests_dialogue_session.cpp)
add_sandal_domain_test_exe(test_quests_objectives_and_commissions quests/test_quests_objectives_and_commissions.cpp)
add_sandal_domain_test_exe(test_protocol_bitstream_bounds protocol/test_protocol_bitstream_bounds.cpp)
add_sandal_domain_test_exe(test_character_inventory_and_glint character/test_character_inventory_and_glint.cpp)
add_sandal_domain_test_exe(test_character_mounts_and_talents character/test_character_mounts_and_talents.cpp)
add_sandal_domain_test_exe(test_mesh_handover_hysteresis mesh/test_mesh_handover_hysteresis.cpp)
add_sandal_domain_test_exe(test_chaos_fault_seams chaos/test_chaos_fault_seams.cpp)
add_sandal_domain_test_exe(fuzz_farchive_langsec protocol/fuzz_farchive_langsec.cpp)
target_include_directories(fuzz_farchive_langsec PRIVATE "${PROJECT_SOURCE_DIR}/libs/aocnet")
target_link_libraries(fuzz_farchive_langsec PRIVATE aocnet)
add_sandal_domain_test_exe(test_move_packed_inline_base protocol/test_move_packed_inline_base.cpp)
target_include_directories(test_move_packed_inline_base PRIVATE "${PROJECT_SOURCE_DIR}/libs/aocnet")
target_link_libraries(test_move_packed_inline_base PRIVATE aocnet)
add_sandal_domain_test_exe(test_diff_save_334_parity character/test_diff_save_334_parity.cpp)
add_sandal_domain_test_exe(test_stop_cast_race combat/test_stop_cast_race.cpp)
add_sandal_domain_test_exe(test_net_serialize_all_12 protocol/test_net_serialize_all_12.cpp)
add_test(
    NAME protocol.test_net_serialize_all_12
    COMMAND $<TARGET_FILE:test_net_serialize_all_12>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(protocol.test_net_serialize_all_12 PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_content_hotfix_reload world/test_content_hotfix_reload.cpp)
add_test(
    NAME content_hotfix_reload
    COMMAND $<TARGET_FILE:test_content_hotfix_reload>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(content_hotfix_reload PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_test(
    NAME world.content_hotfix_reload
    COMMAND $<TARGET_FILE:test_content_hotfix_reload>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(world.content_hotfix_reload PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_sandal_domain_test_exe(test_three_ring_stress world/test_three_ring_stress.cpp)
add_sandal_domain_test_exe(test_content_snapshot_rcu world/test_content_snapshot_rcu.cpp)
add_sandal_domain_test_exe(test_swarm_500_bots_30hz mesh/test_swarm_500_bots_30hz.cpp)
add_sandal_domain_test_exe(test_boundary_oscillation_10k mesh/test_boundary_oscillation_10k.cpp)
add_sandal_domain_test_exe(test_transport_matrix_36 mesh/test_transport_matrix_36.cpp)
add_sandal_domain_test_exe(test_v1_udp_jitter_loss chaos/test_v1_udp_jitter_loss.cpp)
add_sandal_domain_test_exe(test_v2_pg17_wal_stall chaos/test_v2_pg17_wal_stall.cpp)
add_sandal_domain_test_exe(test_v3_mutated_bitstreams chaos/test_v3_mutated_bitstreams.cpp)
add_sandal_domain_test_exe(test_v4_live_rcu_load chaos/test_v4_live_rcu_load.cpp)
add_sandal_domain_test_exe(test_v5_sigkill_handover chaos/test_v5_sigkill_handover.cpp)
add_sandal_domain_test_exe(test_v6_dst_buggify chaos/test_v6_dst_buggify.cpp)

get_property(SANDAL_WORLD_TEST_TARGETS GLOBAL PROPERTY SANDAL_WORLD_TEST_TARGETS)

set(SANDAL_AOCNET_TEST_TARGETS
    test_cs_packet_info
    test_level_visibility
    test_pc_bootstrap
    test_possession
    test_rep_layout_writer
    test_sc_packet_info
    test_hit_confirmation
    test_pkt22_round_trip
    test_pkt104_round_trip
    test_name_update_bunch
    test_pawn_spawn_diff
    test_pc_spawn_diff
    test_package_map_export
    test_packet_notify
    test_handshake
    test_packet_framing
    test_packet_reader
    test_server_guid_allocator
    test_generated_schemas
    test_repl_codecs
    test_repl_rep_block
    test_repl_fast_array
    test_repl_aoc_codecs
    test_repl_rpc
    test_repl_move_packed
    test_repl_ability_rpc
    test_repl_items
    test_repl_item_rpc
    test_repl_hits
    test_repl_leveling
    test_repl_actor_open
    test_repl_npc
    test_repl_send_ledger
    test_repl_vendor
    test_repl_loot
    test_repl_mount
    test_repl_nodes
    test_repl_gather
    test_repl_resource
    test_repl_foliage
    test_repl_hunting
    test_repl_node_layout
    test_repl_dialogue
    test_repl_quest
    test_repl_warehouse
)

set(SANDAL_ALL_CPP_TEST_TARGETS
    ${SANDAL_AOCNET_TEST_TARGETS}
    test_ics_token_file
    test_world_stats
    test_world_effects
    test_world_ability_content
    ${SANDAL_DOMAIN_TEST_TARGETS}
    ${SANDAL_WORLD_TEST_TARGETS}
)
if(WIN32 AND TARGET test_launcher_saved_sign_in)
    list(APPEND SANDAL_ALL_CPP_TEST_TARGETS test_launcher_saved_sign_in)
endif()
foreach(t IN LISTS SANDAL_ALL_CPP_TEST_TARGETS)
    if(TARGET ${t})
        set_target_properties(${t} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${AOC_DIST_ROOT}/tests/Debug"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${AOC_DIST_ROOT}/tests/Release"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${AOC_DIST_ROOT}/tests/RelWithDebInfo"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${AOC_DIST_ROOT}/tests/MinSizeRel"
        )
    endif()
endforeach()

add_custom_target(sandal_ctest_suite
    DEPENDS
        ${SANDAL_AOCNET_TEST_TARGETS}
        test_ics_token_file
        $<$<BOOL:${WIN32}>:test_launcher_saved_sign_in>
        test_world_stats
        test_world_effects
        test_world_ability_content
        ${SANDAL_DOMAIN_TEST_TARGETS}
        ${SANDAL_WORLD_TEST_TARGETS}
)
set_target_properties(sandal_ctest_suite PROPERTIES FOLDER "Tests")

if(SANDAL_WORLD_TEST_TARGETS)
    foreach(t IN LISTS SANDAL_WORLD_TEST_TARGETS)
        add_test(
            NAME world.${t}
            COMMAND $<TARGET_FILE:${t}>
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        )
        set_tests_properties(world.${t} PROPERTIES
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
        )
    endforeach()
endif()

foreach(t IN LISTS SANDAL_AOCNET_TEST_TARGETS)
    add_test(
        NAME aocnet.${t}
        COMMAND $<TARGET_FILE:${t}>
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(aocnet.${t} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
    )
endforeach()

add_test(
    NAME tools.test_ics_token_file
    COMMAND $<TARGET_FILE:test_ics_token_file>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(tools.test_ics_token_file PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
if(WIN32)
    add_test(
        NAME tools.test_launcher_saved_sign_in
        COMMAND $<TARGET_FILE:test_launcher_saved_sign_in>
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(tools.test_launcher_saved_sign_in PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
    )
endif()

add_test(
    NAME world_stats.test_world_stats
    COMMAND $<TARGET_FILE:test_world_stats>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(world_stats.test_world_stats PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)
add_test(
    NAME world_stats.test_world_effects
    COMMAND $<TARGET_FILE:test_world_effects>
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(world_stats.test_world_effects PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
)

function(add_sandal_domain_case_test test_name target_name case_name)
    add_test(
        NAME ${test_name}
        COMMAND $<TARGET_FILE:${target_name}> --data-free --case ${case_name}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    )
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${AOC_DIST_ROOT}/$<CONFIG>"
    )
endfunction()

add_sandal_domain_case_test(world.manual_clock_tick_barriers test_world_clock_and_data_free manual_clock_tick_barriers)
add_sandal_domain_case_test(world.data_free_standin_tooltip_lookup test_world_clock_and_data_free data_free_standin_tooltip_lookup)
add_sandal_domain_case_test(world.zoi_spatial_cell_partition test_world_zoi_and_nodes zoi_spatial_cell_partition)
add_sandal_domain_case_test(world.city_node_stage_progression test_world_zoi_and_nodes city_node_stage_progression)
add_sandal_domain_case_test(world.json_fixture_matches_generated_region_data test_region_classifier json_fixture_matches_generated_region_data)
add_sandal_domain_case_test(world.half_open_rect_boundaries_and_index_lookup test_region_classifier half_open_rect_boundaries_and_index_lookup)
add_sandal_domain_case_test(world.area_config_riverlands_and_non_finite_guards test_region_classifier area_config_riverlands_and_non_finite_guards)
add_sandal_domain_case_test(world.json_zoi_fixture_matches_generated_region_data test_zoi_classifier json_zoi_fixture_matches_generated_region_data)
add_sandal_domain_case_test(world.zoi_surface_table_agrees_with_zoi_rects_fixture test_zoi_classifier zoi_surface_table_agrees_with_zoi_rects_fixture)
add_sandal_domain_case_test(world.zoi_surface_boundary_and_adversarial_inputs test_zoi_classifier zoi_surface_boundary_and_adversarial_inputs)
add_sandal_domain_case_test(nav.heightfield_ground_snap_no_z test_nav_ground_snap heightfield_ground_snap_no_z)
add_sandal_domain_case_test(nav.water_floor_clamp_corridor test_nav_ground_snap water_floor_clamp_corridor)
add_sandal_domain_case_test(nav.bvh_chunk_bounds_query test_nav_los_and_bvh bvh_chunk_bounds_query)
add_sandal_domain_case_test(nav.detour_polyref64_los_raycast test_nav_los_and_bvh detour_polyref64_los_raycast)
add_sandal_domain_case_test(combat.mitigation_and_crit_scaling test_combat_core_formulas mitigation_and_crit_scaling)
add_sandal_domain_case_test(combat.npc_ability_known_table_standin test_combat_core_formulas npc_ability_known_table_standin)
add_sandal_domain_case_test(combat.proc_chain_depth_guard test_combat_procs_and_cc proc_chain_depth_guard)
add_sandal_domain_case_test(combat.cc_diminishing_returns_clock test_combat_procs_and_cc cc_diminishing_returns_clock)
add_sandal_domain_case_test(combat.mob_aggro_and_leash_evade test_combat_mob_gates_and_swings mob_aggro_and_leash_evade)
add_sandal_domain_case_test(combat.mob_swing_timer_cadence test_combat_mob_gates_and_swings mob_swing_timer_cadence)
add_sandal_domain_case_test(quests.npc_dialogue_prerequisite_gate test_quests_dialogue_session npc_dialogue_prerequisite_gate)
add_sandal_domain_case_test(quests.dialogue_choice_branch_transition test_quests_dialogue_session dialogue_choice_branch_transition)
add_sandal_domain_case_test(quests.quest_objective_counter_step test_quests_objectives_and_commissions quest_objective_counter_step)
add_sandal_domain_case_test(quests.commission_board_reward_payout test_quests_objectives_and_commissions commission_board_reward_payout)
add_sandal_domain_case_test(protocol.bit_reader_varint_and_tail_bounds test_protocol_bitstream_bounds bit_reader_varint_and_tail_bounds)
add_sandal_domain_case_test(protocol.bunch_header_framing_roundtrip test_protocol_bitstream_bounds bunch_header_framing_roundtrip)
add_sandal_domain_case_test(character.inventory_bag_slot_stacking test_character_inventory_and_glint inventory_bag_slot_stacking)
add_sandal_domain_case_test(character.vendor_glint_denomination_change test_character_inventory_and_glint vendor_glint_denomination_change)
add_sandal_domain_case_test(character.mount_cooldown_manual_clock test_character_mounts_and_talents mount_cooldown_manual_clock)
add_sandal_domain_case_test(character.talent_tree_rank_allocation test_character_mounts_and_talents talent_tree_rank_allocation)
add_sandal_domain_case_test(mesh.hysteresis_band_200m_authority test_mesh_handover_hysteresis hysteresis_band_200m_authority)
add_sandal_domain_case_test(mesh.mesh_forward_header_12b_epoch test_mesh_handover_hysteresis mesh_forward_header_12b_epoch)
add_sandal_domain_case_test(chaos.udp_jitter_loss_reorder_seam test_chaos_fault_seams udp_jitter_loss_reorder_seam)
add_sandal_domain_case_test(chaos.rcu_snapshot_generation_fence test_chaos_fault_seams rcu_snapshot_generation_fence)
add_sandal_domain_case_test(protocol.farchive_varint_and_bit_bounds_fuzz fuzz_farchive_langsec farchive_varint_and_bit_bounds_fuzz)
add_sandal_domain_case_test(protocol.bunch_and_move_packed_langsec_fuzz fuzz_farchive_langsec bunch_and_move_packed_langsec_fuzz)
add_sandal_domain_case_test(protocol.object_id_1_inline_movement_base_roundtrip test_move_packed_inline_base object_id_1_inline_movement_base_roundtrip)
add_sandal_domain_case_test(protocol.inline_movement_base_invalid_flags_rejected test_move_packed_inline_base inline_movement_base_invalid_flags_rejected)
add_sandal_domain_case_test(character.dirty_slot_diff_save_10k_parity test_diff_save_334_parity dirty_slot_diff_save_10k_parity)
add_sandal_domain_case_test(character.diff_save_fencing_epoch_and_rollback test_diff_save_334_parity diff_save_fencing_epoch_and_rollback)
add_sandal_domain_case_test(combat.mob_stop_cast_target_death_and_cc_race test_stop_cast_race mob_stop_cast_target_death_and_cc_race)
add_sandal_domain_case_test(combat.mob_stop_cast_generation_token_immunity test_stop_cast_race mob_stop_cast_generation_token_immunity)
add_sandal_domain_case_test(world.three_ring_stress test_three_ring_stress three_ring_stress)
add_sandal_domain_case_test(world.content_snapshot_rcu test_content_snapshot_rcu content_snapshot_rcu)
add_sandal_domain_case_test(mesh.swarm_500_bots_30hz test_swarm_500_bots_30hz swarm_500_bots_30hz)
add_sandal_domain_case_test(mesh.boundary_oscillation_10k test_boundary_oscillation_10k boundary_oscillation_10k)
add_sandal_domain_case_test(mesh.transport_matrix_36 test_transport_matrix_36 transport_matrix_36)
add_sandal_domain_case_test(chaos.v1_udp_jitter_loss test_v1_udp_jitter_loss v1_udp_jitter_loss)
add_sandal_domain_case_test(chaos.v2_pg17_wal_stall test_v2_pg17_wal_stall v2_pg17_wal_stall)
add_sandal_domain_case_test(chaos.v3_mutated_bitstreams test_v3_mutated_bitstreams v3_mutated_bitstreams)
add_sandal_domain_case_test(chaos.v4_live_rcu_load test_v4_live_rcu_load v4_live_rcu_load)
add_sandal_domain_case_test(chaos.v5_sigkill_handover test_v5_sigkill_handover v5_sigkill_handover)
add_sandal_domain_case_test(chaos.v6_dst_buggify test_v6_dst_buggify v6_dst_buggify)

find_program(SANDAL_PWSH_EXECUTABLE NAMES pwsh powershell REQUIRED)
add_test(
    NAME powershell.public_check
    COMMAND "${SANDAL_PWSH_EXECUTABLE}" -NoProfile -File "${PROJECT_SOURCE_DIR}/tests/powershell/public_check.Tests.ps1"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
)
