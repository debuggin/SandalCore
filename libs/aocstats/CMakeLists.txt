# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

add_library(aocstats STATIC
    src/stat_program.cpp
    src/stat_engine.cpp
    src/gear.cpp
    src/character_stats.cpp
    src/damage.cpp
)
add_library(world_stats ALIAS aocstats)
target_include_directories(aocstats PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_executable(test_world_stats
    tests/test_worldd_stats.cpp
    tests/test_worldd_stats_combat.cpp
)
target_link_libraries(test_world_stats PRIVATE aocstats)
target_compile_definitions(test_world_stats PRIVATE
    STATS_CORPUS_PATH="${CMAKE_BINARY_DIR}/../gen/fixtures/corpus.txt"
)

add_executable(test_world_effects tests/test_worldd_effects.cpp)
target_link_libraries(test_world_effects PRIVATE aocstats)
target_compile_definitions(test_world_effects PRIVATE
    STATS_CORPUS_PATH="${CMAKE_BINARY_DIR}/../gen/fixtures/corpus.txt"
)
