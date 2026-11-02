# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(navbuild navbuild_main.cpp)
target_link_libraries(navbuild PRIVATE aocnav unofficial::sqlite3::sqlite3)
set_target_properties(navbuild PROPERTIES FOLDER "Servers")
add_custom_target(aoc_navbuild DEPENDS navbuild)
add_custom_command(TARGET navbuild POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:navbuild>"
        "$<TARGET_FILE_DIR:navbuild>/aoc_navbuild$<TARGET_FILE_SUFFIX:navbuild>"
)
