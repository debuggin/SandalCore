# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
cmake_minimum_required(VERSION 3.22)

if(EXISTS "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows")
    list(APPEND CMAKE_PREFIX_PATH "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows")
    list(APPEND CMAKE_PROGRAM_PATH
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/tools/protobuf"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/tools/grpc")
elseif(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
endif()

project(AshesOfCreation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC AND MSVC_VERSION LESS 1950)
    file(GLOB _vs18_lib_dirs "C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Tools/MSVC/*/lib/x64")
    if(_vs18_lib_dirs)
        list(GET _vs18_lib_dirs -1 _vs18_lib_dir)
        link_directories(BEFORE "${_vs18_lib_dir}")
    endif()
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ── Output directory ────────────────────────────────────────────────────────
get_filename_component(SANDAL_REPO_ROOT "${CMAKE_SOURCE_DIR}/.." ABSOLUTE)
set(AOC_DIST_ROOT "${SANDAL_REPO_ROOT}/dist" CACHE PATH
    "Canonical output root: binaries land in <AOC_DIST_ROOT>/<config>/")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY                "${AOC_DIST_ROOT}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${AOC_DIST_ROOT}/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${AOC_DIST_ROOT}/Release")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${AOC_DIST_ROOT}/RelWithDebInfo")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${AOC_DIST_ROOT}/MinSizeRel")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY                "${CMAKE_BINARY_DIR}/lib")
message(STATUS "AshesOfCreation dist/ root: ${AOC_DIST_ROOT}")

# ── Design-data runtime catalog (AOCDB01) ───────────────────────────────────
option(AOC_DESIGN_DATA_RUNTIME_CATALOG "Load design-data tables at runtime via AOCDB01 catalog instead of requiring configure-time generated headers" ON)
message(STATUS "AshesOfCreation design-data runtime catalog (AOCDB01): ${AOC_DESIGN_DATA_RUNTIME_CATALOG}")

# ── Dependencies ────────────────────────────────────────────────────────────
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
    message(FATAL_ERROR "grpc_cpp_plugin not found (install via vcpkg grpc)")
endif()
find_program(PROTOC protoc)
if(NOT PROTOC)
    message(FATAL_ERROR "protoc not found (install via vcpkg protobuf)")
endif()

# ── Proto codegen ───────────────────────────────────────────────────────────
# world generates the gRPC stubs of the internal services it calls (server/world/CMakeLists.txt, from proto/internal) with the PROTOC
# and GRPC_CPP_PLUGIN found above.  The Go services generate theirs from proto/ (services/buf.gen.yaml).

# ── libaocproto: unified C++ protobuf & gRPC stubs ──────────────────────────
add_subdirectory(libs/aocproto)

# ── libaocnet: protocol library (wire / emit / schema / NetGUIDs) ──────────
add_subdirectory(libs/aocnet)

# ── libaocstats: stat engine & damage evaluator (world_stats ALIAS aocstats) ─
add_subdirectory(libs/aocstats)

# ── libaocnav & navbuild: Recast/Detour navigation mesh library & tool ─────
add_subdirectory(libs/aocnav)
add_subdirectory(tools/navbuild)

# ── world: the C++ world server (plan A4) ──────────────────────────────────
add_subdirectory(server/world)
add_custom_target(worldd DEPENDS world)

# ── Tether server (standalone UDP ARQ executable) ───────────────────────────
add_executable(tether
    server/tether/tether_server_main.cpp
    server/tether/tether_service.h
)
add_custom_target(tether_server DEPENDS tether)

target_include_directories(tether PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/server/tether"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/aoccore"
)

target_link_libraries(tether
    PRIVATE
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        ws2_32
)

target_compile_features(tether PRIVATE cxx_std_20)

if(MSVC)
    target_compile_definitions(tether PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

set_target_properties(tether PROPERTIES FOLDER "Servers")

# ── Launcher GUI application ─────────────────────────────────────────────────
# Self-contained Win32 GUI.  Reads config\paths.json for the game exe path
# and config\server.json for auth_server_host/auth_server_port/tether_port.
# No gRPC or protobuf dependency — only WinInet (wininet.lib) + nlohmann_json.
if(WIN32)
    add_executable(launcher WIN32
        clients/launcher/launcher_main.cpp
        clients/launcher/launcher_auth.cpp
        clients/launcher/launcher_ui.cpp
        libs/aoccore/auth/ics_token_acl.cpp
    )
    add_custom_target(SandalLauncher DEPENDS launcher)
    target_include_directories(launcher PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/clients/launcher"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/aoccore"
    )
    target_link_libraries(launcher
        PRIVATE
            nlohmann_json::nlohmann_json
            wininet
            comdlg32
            advapi32
            shell32
    )
    target_compile_features(launcher PRIVATE cxx_std_20)
    target_compile_definitions(launcher PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    set_target_properties(launcher PROPERTIES FOLDER "Tools")
endif()

add_custom_target(LauncherBuild
    COMMAND dotnet build "${CMAKE_CURRENT_SOURCE_DIR}/clients/launcher/SandalLauncher.csproj" -c $<CONFIG>
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building Avalonia 11 C# Desktop Shell (SandalLauncher.csproj)"
    VERBATIM
)
set_target_properties(LauncherBuild PROPERTIES FOLDER "Tools")

# test_ics_token_file: the launcher/tether login-token hand-off (plan A3)
add_executable(test_ics_token_file
    tests/cpp/protocol/test_ics_token_file.cpp
    libs/aoccore/auth/ics_token_acl.cpp
)
target_include_directories(test_ics_token_file PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/aoccore"
)
target_compile_features(test_ics_token_file PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(test_ics_token_file PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(test_ics_token_file PROPERTIES FOLDER "Tools")

# test_launcher_saved_sign_in: "Keep me signed in" for launcher.exe (clients/launcher/launcher_sign_in.h): the Windows Credential Manager
# round trip under a test-only target name, how the auth server's answers are read, and the automatic sign-in at start
# (fake clock, fake auth server)
if(WIN32)
    add_executable(test_launcher_saved_sign_in
        tests/cpp/protocol/test_launcher_saved_sign_in.cpp
    )
    target_include_directories(test_launcher_saved_sign_in PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/clients/launcher"
    )
    target_link_libraries(test_launcher_saved_sign_in PRIVATE nlohmann_json::nlohmann_json)
    target_compile_features(test_launcher_saved_sign_in PRIVATE cxx_std_20)
    target_compile_definitions(test_launcher_saved_sign_in PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    set_target_properties(test_launcher_saved_sign_in PROPERTIES FOLDER "Tools")
endif()

option(AOC_DATA_FREE "Enable --data-free synthetic fixtures for hermetic CTest runs" ON)
enable_testing()
add_subdirectory(tests)

