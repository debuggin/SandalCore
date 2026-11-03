# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

set(RECAST_DIR "${PROJECT_SOURCE_DIR}/third_party/recastnavigation")

add_library(recast_detour STATIC
    "${RECAST_DIR}/Recast/Source/Recast.cpp"
    "${RECAST_DIR}/Recast/Source/RecastAlloc.cpp"
    "${RECAST_DIR}/Recast/Source/RecastArea.cpp"
    "${RECAST_DIR}/Recast/Source/RecastAssert.cpp"
    "${RECAST_DIR}/Recast/Source/RecastContour.cpp"
    "${RECAST_DIR}/Recast/Source/RecastFilter.cpp"
    "${RECAST_DIR}/Recast/Source/RecastLayers.cpp"
    "${RECAST_DIR}/Recast/Source/RecastMesh.cpp"
    "${RECAST_DIR}/Recast/Source/RecastMeshDetail.cpp"
    "${RECAST_DIR}/Recast/Source/RecastRasterization.cpp"
    "${RECAST_DIR}/Recast/Source/RecastRegion.cpp"
    "${RECAST_DIR}/Detour/Source/DetourAlloc.cpp"
    "${RECAST_DIR}/Detour/Source/DetourAssert.cpp"
    "${RECAST_DIR}/Detour/Source/DetourCommon.cpp"
    "${RECAST_DIR}/Detour/Source/DetourNavMesh.cpp"
    "${RECAST_DIR}/Detour/Source/DetourNavMeshBuilder.cpp"
    "${RECAST_DIR}/Detour/Source/DetourNavMeshQuery.cpp"
    "${RECAST_DIR}/Detour/Source/DetourNode.cpp"
)
target_include_directories(recast_detour PUBLIC
    "${RECAST_DIR}/Recast/Include"
    "${RECAST_DIR}/Detour/Include"
)
target_compile_definitions(recast_detour PUBLIC DT_POLYREF64=1)
if(MSVC)
    target_compile_options(recast_detour PRIVATE /W0)
else()
    target_compile_options(recast_detour PRIVATE -w)
endif()
set_target_properties(recast_detour PROPERTIES FOLDER "Servers/world")

add_library(aocnav STATIC
    src/area_nav.cpp
    src/nav_build.cpp
    src/nav_file.cpp
    src/tile_repair.cpp
    src/town_geometry.cpp
    src/town_platform.cpp
)
add_library(world_nav ALIAS aocnav)
target_include_directories(aocnav PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_definitions(aocnav PUBLIC DT_POLYREF64=1)
target_link_libraries(aocnav PUBLIC recast_detour)
set_target_properties(aocnav PROPERTIES FOLDER "Servers/world")
